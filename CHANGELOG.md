# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.2.0-rc.1] - 2026-10-08

Interrupts, multiqueue (RSS) and two eDMA channels. Wire format still
protocol v4: the new features are negotiated per connection, so v0.1
blades and the BMC helper keep working with v0.2 blades. Part of
OpenMIOP Stack v0.2.0-rc.1.

### Added

- **TX completion interrupt.** eDMA runs end with the done interrupt of
  their channel (line `dma<c>`) instead of a busy-wait: the TX thread
  slept ~92 % of a core away before, ~13-18 % per channel now
  (`tx_irq=0` restores polling).
- **RX doorbell (interrupt-driven RX).** After its frames and head
  writes, the sender's eDMA writes one word into a 64 KiB window of the
  receiver's BAR that an inbound iATU region maps onto the GIC ITS
  translater, so the write raises an MSI on the receiver and NAPI runs
  there. The receiver creates one ITS vector per possible sender bus
  (DeviceID = requester ID through the controller's `msi-map`) and per
  queue. The ctl thread then polls only every 1-2 ms (gateway ring,
  lost doorbells): `omi-ctl` ~6 % -> ~1 % of a core. Needs Linux 6.10+
  and the GIC ITS; otherwise, or with `rx_doorbell=0`, RX is polled as
  before.
- **Multiqueue / RSS.** `queues=` (default 4; a power of two, at most
  one per CPU) queue pairs. The stack picks the TX queue from the flow
  hash, so a flow stays on one queue; each eDMA write channel (2 on
  RK3588) has its own thread and serves half of the queues, so both run
  in parallel. The receiver splits each sender's ring area into one
  ring per queue, has one NAPI context and one doorbell vector per
  queue, and pins (as a hint) queue q's vector to the q-th fastest CPU
  (the A76 cores). The flow hash travels in the slot header and becomes
  the skb hash (RPS/RFS). `ethtool -l` shows the queues, `ethtool -S`
  `rx_q<n>_packets`.
- `ethtool -S`: `tx_dma_irq_lost`, `tx_doorbells`,
  `ctl_doorbell_windows`.

### Changed

- Protocol (still v4 framing, `OMI_FEAT_*`): the sender announces
  `features` and `nq` with its connect token; the receiver offers its
  doorbell (`db_off`, `db_data`) and the same `nq` with its ack, plus a
  head and a credit word per queue. All in the existing 64-byte
  producer and credit lines. Pairs that do not agree use one ring with
  all slots; frames that can reach such a peer (and floods, multicast,
  the gateway) always take queue 0.
- Inbound iATU: the BAR match moved from region 0 to region 1; region 0
  is the doorbell window (the lower region wins where they overlap).
- Locking: one lock per TX queue, `tx_lock` for peer state only, a
  separate lock for the gateway TX ring; statistics per queue/channel.

### Tested

On the lab Cluster Box: four Blade 3 on Talos v1.14.2 (6.18.54) with
this driver (mixtile-talos development builds), BMC on ClusterBox
firmware v0.1.0-rc.3, MTU 9000, all links Gen3 x2, a loaded Cozystack
cluster (Cilium, DRBD) on top:

- Every ordered pair, 4 TCP streams: 7.9-8.4 Gbit/s (two pairs ~4.3-4.7
  when the four flows hashed onto one or two queues). With one queue
  (development build with the interrupts, same cluster, Gen3 x2):
  4.3-4.4 Gbit/s; rc.2 one stream on Gen3 x2: ~5 Gbit/s.
- Ring of four senders at once, 4 streams each: 22.2 Gbit/s total
  (rc.2: 11.7).
- One TCP stream: 4.1-4.8 Gbit/s, about as before (one flow = one queue
  = one eDMA channel).
- No driver errors or drops (`tx_dma_errors`, `rx_resyncs`,
  `rx_bad_len`, `tx_dma_irq_lost`: 0).
- Rolling upgrades with mixed peers (rc.2 polled <-> dev doorbell, one
  queue <-> four queues) during the rollout; BMC reboot (firmware
  update) and hard resets of all four blades afterwards: all twelve
  directions came back with 4 queues and doorbells.
- Debian/Ubuntu: module builds and the DKMS package installs in CI
  (6.1, 6.12, Ubuntu kernels); not run on hardware with v0.2 (on 6.1
  kernels the doorbell is compiled out, RX is polled).

### Known limitations

- A single flow is limited to one queue and one eDMA channel (~4.7
  Gbit/s here); TX waits for the done interrupt and a thread wakeup
  between runs.
- No checksum or GSO offload yet; fixed 10 KiB slots (64 per queue with
  4 queues).
- A BMC reboot takes the whole fabric down for minutes (the BMC is the
  PCIe root and resets the switch). On the lab cluster DRBD 9.3.4
  deadlocked afterwards (`__drbd_md_sync` waiting on md_buffer) until
  every node was rebooted; this is outside the driver.

## [0.1.0-rc.2] - 2026-10-06

Driver fix and packaging release. Protocol v4 is unchanged; the BMC
helper source is unchanged. Part of OpenMIOP Stack v0.1.0-rc.2.

### Added

- `openmiop-dkms` Debian package for Debian 12/13 and Ubuntu 22.04/24.04
  (arm64, kernel 6.1 or newer): DKMS builds the module for the running
  kernel, `openmiop.service` brings up `omi0` from `/etc/openmiop.addr`,
  the vendor `load-miop.service` is disabled. Finds the Mixtile vendor
  headers by itself. CI installs the package in each of these
  distributions and checks that the module builds for its kernel.

### Changed

- Release assets have one naming scheme and no build leftovers:
  `openmiop-dkms_<version>_all.deb`,
  `openmiop-<version>-clusterbox-bmc-mipsel.tar.gz` (helper, `omi-peek`,
  init script), `BUILD-INFO.txt`, `SHA256SUMS`.

### Fixed

- Peers could stay "connecting" after the BMC re-enumerated the fabric
  and the endpoint BAR addresses moved (seen on hardware when blades were
  removed and new ones added: four blades → two → three → four). A
  receiver answered a connect through its outbound window to the peer's
  *old* BAR address, so the ack was lost (or landed in whichever node
  now owned that address), and a repeated connect with the same token
  was never acked again. Traffic in that direction fell back to the slow
  BMC gateway. Now:
  - acks and credits wait until the window points at the peer's current
    BAR (`ack_ok`, cleared when the peer leaves, set when the new peer
    table entry has been programmed);
  - a sender that gets no ack for 2 s connects again with a new token,
    which the receiver acks as a new connection (recovers from any lost
    ack). Counted in `ethtool -S` as `ctl_connect_renew`.
  rc.1 workaround (no longer needed): `touch /var/run/openmiop-reenumerate`
  on the BMC.

### Tested

- Four Talos blades on this driver: rolling upgrade, then BARs moved on
  purpose twice (graceful shutdown of one blade + BMC re-enumeration, and
  its return): all 12 peer directions reconnected without the workaround.
- DKMS package built and installed in Debian 12/13 and Ubuntu 22.04
  (HWE)/24.04 arm64 containers (CI); no Debian/Ubuntu hardware run in rc.2.

## [0.1.0-rc.1] - 2026-10-06

First release candidate of openmiop: Ethernet (`omi0`) over the PCIe
fabric of the Mixtile Cluster Box. Mixtile Blade 3 (RK3588) boards are
PCIe endpoints behind the ASM2824 switch; the MT7620A BMC is the root
complex. Part of OpenMIOP Stack v0.1.0-rc.1 (protocol v4).

The driver source (`drivers/openmiop/`) is identical to commit `a7a8177`,
the build that was hardware-tested on Debian and Talos below.

### Added

- Endpoint driver `openmiop-ep.ko` for the RK3588 PCIe3 controller
  `fe150000` (platform driver, programs DBI/iATU/eDMA directly; PCI ID
  `1d87:4f4d`, one 16 MiB 64-bit prefetchable BAR, link Gen3 x2).
  Builds unchanged for the Mixtile vendor kernel 6.1.99 (Debian) and
  6.18.54-talos.
- Protocol v4:
  - any number of endpoints behind the switch (up to 8 nodes, 4 receive
    rings per BAR in this layout);
  - node index = switch slot, stable across reloads and reboots;
  - peer discovery: the BMC publishes a peer table (epoch, BAR address,
    MAC) into every BAR; full mesh between blades;
  - write-only endpoint-to-endpoint control (connect tokens, acks and
    credits are posted writes; no endpoint reads a peer);
  - per-node epochs; the BMC session is bound to the epoch it activated.
- Stable MAC address derived from the SoC serial number in the device
  tree (locally administered); random only without a serial number.
- Data path: eDMA writes from the sender's DRAM into the peer's BAR
  (payload never crosses the BMC); scatter-gather, pipelined TX with
  two alternating eDMA lists; NAPI + GRO RX; `ethtool -S` counters.
- L2: unicast by peer table and learned source MACs, broadcast and
  multicast to all members, unknown-unicast flooding, BMC relay for
  peers still connecting. ARP, IPv4, IPv6 ND and multicast.
- Jumbo frames: MTU up to 9246; 9000 used everywhere, including the BMC.
- BMC helper `openmiop-rc` (freestanding MIPS32 soft-float) and
  `/etc/init.d/openmiop`:
  - restores BAR0, Memory Space + Bus Master and one common MPS for all
    endpoints;
  - re-enumerates from the PCIe root port when an endpoint has no BAR
    address (bridge windows are sized at first assignment), at most
    once a minute, after quiescing P2P traffic;
    `/var/run/openmiop-reenumerate` forces one;
  - relays gateway frames by MAC; TAP `omi0` on the BMC, 10.20.0.1/24.
- `omi-peek` BAR diagnostic for the BMC.
- Scripts: `matrix-test.sh` (all-pairs ping/jumbo/IPv6), `bench.sh`
  (iperf3 + SHA-256 transfers), Debian systemd unit and start script.
- CI: helper built with `gcc-mipsel-linux-gnu`, module compile check
  against upstream `linux-6.1.y`; tagged builds publish the helper with
  `BUILD-INFO.txt` and `SHA256SUMS` as release assets.

### Changed

- Protocol v3 (two fixed endpoints) replaced by v4; v3 and v4 members
  do not interoperate. The helper only activates endpoints whose header
  says version 4.

### Fixed

- MPS mismatch after a rescan (TLPs over 128 B dropped): the helper
  enforces one MPS.
- Host link reset left BAR0 at 1 GiB and stale iATU targets: the driver
  restores the resizable-BAR size and every iATU region.
- BMC completion-timeout wedge during back-to-back reloads: leave
  handshake on unload/reboot/poweroff (`.shutdown`), quiet periods
  after detach, config-space liveness check before MMIO.
- Lockless TX queue read (oops), RCU stall under sustained TX, PHY GRF
  mapping too small (oops).
- Stalled-peer detection missed the queue-stopped case; frames for a
  stalled peer are no longer relayed through the gateway.
- Re-enumeration started at the switch instead of the root port, which
  left the root window too small for a fourth blade.

### Tested

Cluster Box with four Blade 3: blade160, blade173 (Debian 12, vendor
6.1.99), two Talos v1.14.2 (6.18.54-talos) nodes, plus the BMC — five
members on one segment. All endpoint links 8 GT/s x2, MTU 9000.

| Test | Result |
| --- | --- |
| Debian ↔ Debian TCP, 1 stream | 8.03-8.26 Gbit/s each way, 0 retransmits |
| Debian ↔ Debian bidirectional | ~7.9 + 7.9 Gbit/s; 10 min soak 7.96 + 7.96 |
| Debian ↔ Debian 4 / 8 streams | ~7.4 Gbit/s |
| Debian → Talos / Talos → Debian | 6.4-6.6 / 7.4-7.5 Gbit/s |
| Talos ↔ Talos | 6.48 / 6.73 Gbit/s, bidirectional 4.45 + 4.51 |
| SHA-256 transfers | 1 GiB Debian↔Debian both ways, 1 GiB Debian→Talos, 512 MiB Talos→Debian and Talos→Talos: identical |
| Five-member matrix | ping + jumbo between all members, TCP to Talos apid, IPv6 `ff02::1` |
| Idle cost | 0.6-0.8 % of 8 cores; RTT 0.3 ms busy / 0.86 ms after idle |
| Recovery | reload under load, 8 back-to-back reload cycles, peer reboot, link disable/enable, interface down under load, BMC re-enumeration (forced, shrink, growth) |
| BMC gateway | ~12 Mbit/s blade→BMC, ~88 Mbit/s BMC→blade |

The release helper binary (CI build of this source) ran on the BMC with
all four endpoints; the Debian module built from this source repeated
8.40 / 8.20 Gbit/s with identical SHA-256.

### Known limitations

- RX is polled by a kthread (20-50 µs busy, 200-400 µs idle);
  interrupt-driven RX (GIC ITS doorbell or vendor-defined message) is
  not implemented or tested.
- No multiqueue / RPS / XPS work; one TX queue, one NAPI context.
- TX polls the eDMA done bit: one core busy while sending.
- Talos receives ~20 % slower than Debian in these tests (6.4 vs 8.0
  Gbit/s); not investigated.
- The BMC gateway is management-grade (~12 Mbit/s towards the BMC).
- Root-port re-enumeration pauses all P2P traffic ~1-2 s when an
  endpoint appears without an address. An endpoint that crashes without
  the leave handshake can race a BMC read and wedge the fabric until the
  BMC reboots (not tested on purpose).
- Debian: no prebuilt module (the vendor headers are only in the
  Mixtile image); build on the blade.
- Not hardware-tested: more than four blades, slot orders other than
  this box, MPS other than 128/256, helper restart under load with four
  endpoints.

[0.2.0-rc.1]: https://github.com/roysbike/pcie-ep-net/releases/tag/v0.2.0-rc.1
[0.1.0-rc.2]: https://github.com/roysbike/pcie-ep-net/releases/tag/v0.1.0-rc.2
[0.1.0-rc.1]: https://github.com/roysbike/pcie-ep-net/releases/tag/v0.1.0-rc.1
