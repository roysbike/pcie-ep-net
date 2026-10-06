# Changelog

## v0.1.0-rc.1 — 2026-10-06

First release candidate of openmiop: Ethernet (`omi0`) over the PCIe fabric
of the Mixtile Cluster Box, RK3588 Blade 3 as PCIe endpoint, ASM2824 switch,
MT7620A root complex (BMC).

The driver source (`drivers/openmiop/`) is identical to commit `a7a8177`,
which is the build that was hardware-tested on Debian and Talos below.

### Components

| Component | Where | Runs on |
| --- | --- | --- |
| `openmiop-ep.ko` | `drivers/openmiop/` | Blade 3, Debian vendor 6.1.99 and Talos 6.18.54-talos (same source) |
| `openmiop-rc` | `userspace/` | Cluster Box BMC (OpenWrt, MT7620A, freestanding MIPS32 soft-float) |
| `/etc/init.d/openmiop` | `scripts/openmiop.init` | Cluster Box BMC |
| `omi-peek` | `userspace/` | Cluster Box BMC (diagnostics) |

### Protocol v4 (new)

* Any number of endpoints behind the switch: up to 8 nodes, 4 receive rings
  per BAR in this layout. Node index = position of the switch port, stable
  across reloads and reboots.
* Peer table published by the root complex into every endpoint BAR; full
  mesh between blades.
* Endpoint-to-endpoint control is write-only (connect tokens, acks,
  credits are posted writes); no endpoint ever reads a peer.
* Per-node epochs; the RC session is bound to the epoch it activated.

### Data path

* Blade-to-blade frames: eDMA writes from the sender's DRAM straight into
  the peer's BAR; payload never crosses the root complex.
* TX: scatter-gather, one eDMA linked list per run, pipelined (frames are
  mapped while the previous run is on the wire), `cond_resched()` under
  sustained load.
* RX: NAPI + GRO, driven by a polling kthread (no RX interrupt).
* L2: unicast by peer table and learned source MACs, broadcast/multicast
  to every peer and the RC, unknown unicast flooded, RC relay for peers
  that are still connecting. ARP, IPv4, IPv6 ND and multicast work.
* MTU up to 9246; 9000 used everywhere, including the RC gateway.
* `ethtool -S` counters per context.

### Recovery

* Module reload, reboot and poweroff: leave handshake with the RC
  (`.shutdown` hook), peers detach before the link drops.
* Host link reset (`LINK_REQ_RST_NOT`, `APP_DLY2`): non-sticky config,
  resizable-BAR size and every iATU region are restored, new epoch.
* Peer that stops consuming: detected after 100 ms with the same test xmit
  uses, its frames are dropped, the shared queue keeps moving, no gateway
  flooding.
* eDMA abort/timeout: engine reset, only the run is dropped.

### Root-complex helper

* Restores BAR0, Memory Space + Bus Master and one MPS for all endpoints
  after a reload.
* Re-enumerates from the PCIe root port when an endpoint has no BAR
  address (bridge windows are sized at first assignment), at most once a
  minute, after quiescing all P2P traffic; `/var/run/openmiop-reenumerate`
  forces one.
* Never reads an endpoint whose link may be dropping (quiet periods,
  config-space liveness check before MMIO).
* Relays gateway frames by MAC; TAP `omi0` on the BMC, 10.20.0.1/24.

### Tested on hardware (2026-10-06)

Cluster Box with four Blade 3: blade160, blade173 (Debian 12, vendor
6.1.99), .201, .204 (Talos v1.14.2, 6.18.54-talos). All links 8 GT/s x2.

| Test | Result |
| --- | --- |
| Debian ↔ Debian TCP, 1 stream | 8.03-8.26 Gbit/s each way, 0 retransmits |
| Debian ↔ Debian bidirectional | ~7.9 + 7.9 Gbit/s; 10 min soak 7.96 + 7.96 |
| Debian ↔ Debian 4 / 8 streams | ~7.4 Gbit/s |
| Debian → Talos / Talos → Debian | 6.4-6.6 / 7.4-7.5 Gbit/s |
| Talos ↔ Talos | 6.48 / 6.73 Gbit/s, bidirectional 4.45 + 4.51 |
| SHA-256 transfers | 1 GiB Debian↔Debian both ways, 1 GiB Debian→Talos, 512 MiB Talos→Debian and Talos→Talos: identical |
| Five-member matrix | ping + jumbo between all members, TCP to Talos apid, IPv6 `ff02::1` |
| Idle cost | 0.6-0.8 % of 8 cores, RTT 0.3 ms busy / 0.86 ms after idle |
| Recovery | reload under load, 8 back-to-back reload cycles, peer reboot, link disable/enable, interface down under load, RC re-enumeration (forced, shrink, growth) |

### Known limitations

* RX is polled by a kthread; interrupt-driven RX (GIC ITS doorbell or
  vendor-defined message) is not implemented or tested.
* No multiqueue / RPS / XPS work.
* TX polls the eDMA done bit: one core busy while sending.
* Talos receives ~20 % slower than Debian in these tests (not investigated).
* The RC gateway is management-grade (~12 Mbit/s towards the BMC).
* Root-port re-enumeration pauses all P2P traffic for ~1-2 s when an
  endpoint appears without an address; an endpoint that crashes without
  the leave handshake can still race an RC read (not tested on purpose).
* Not hardware-tested: more than four blades (layout supports 4 rings),
  blades in mixed slot orders other than this box, MPS other than
  128/256, RC helper restart under load with four endpoints.
