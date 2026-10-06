# pcie-ep-net (openmiop)

Ethernet between the Mixtile Blade 3 boards of a Mixtile Cluster Box, over
the Cluster Box's internal PCIe switch. Each blade gets a network
interface `omi0`; frames between blades are DMA writes from one blade's
memory straight into another blade's PCIe BAR. They cross the switch and
never pass through the Cluster Box BMC.

The implementation is called **openmiop** (`omi0`, on-wire magic `OMI1`).
It is an independent implementation, not Mixtile's MIOP driver, and
contains no Mixtile code or firmware. License: GPL-2.0-or-later
([LICENSE](LICENSE)).

Current release: **v0.1.0-rc.2** (protocol v4). See
[CHANGELOG.md](CHANGELOG.md) and [Compatible releases](#compatible-releases).

## Architecture

```
        Cluster Box BMC (MT7620A, OpenWrt)
        openmiop-rc helper, omi0 10.20.0.1
                   |
              PCIe Gen1 x1   (control + slow gateway only)
                   |
            ASMedia ASM2824 PCIe switch
         /        /          \          \
    Gen3 x2   Gen3 x2      Gen3 x2    Gen3 x2
       |         |            |          |
     Blade     Blade        Blade      Blade      Mixtile Blade 3 (RK3588)
     slot 1    slot 2       slot 3     slot 4     PCIe endpoint, openmiop-ep.ko
     node 3    node 2       node 0     node 1     Debian or Talos, omi0
```

* **Blade side:** `openmiop-ep.ko` turns the RK3588 PCIe3 controller
  `fe150000` into a PCIe endpoint (PCI ID `1d87:4f4d`) with one 16 MiB
  BAR and registers the netdev `omi0`. It programs the controller
  directly (DBI, iATU, eDMA); it does not need the kernel PCI endpoint
  framework.
* **BMC side:** `openmiop-rc` runs on the Cluster Box (MT7620A root
  complex). It finds the endpoints, restores BAR/command/MPS settings,
  numbers the blades by switch slot, publishes the peer table into every
  BAR, and bridges a slow gateway so the BMC itself is on `omi0`
  (10.20.0.1). It is a userspace program using sysfs; **no BMC kernel
  module is needed** (only `kmod-tun`).
* **Data path:** blade-to-blade frames are posted `MemWr` TLPs from the
  sender's eDMA engine into a per-sender ring in the receiver's BAR. The
  switch routes them by address (the ASM2824 has no ACS). The receiver
  polls its rings (NAPI + GRO) and returns credits with posted writes.
  Endpoints never read each other.

Details: [docs/architecture.md](docs/architecture.md).

### Protocol v4 in short

* Peer discovery: the BMC helper enumerates the endpoints behind the
  switch and writes a peer table (epoch, BAR address, MAC per node) into
  every BAR. Up to 8 nodes in the protocol, 4 receive rings per BAR in
  this layout (one per possible sender in a four-slot box).
* Node index = switch slot, stable across reloads and reboots.
* Stable MAC: derived from the SoC serial number U-Boot puts in the
  device tree (locally administered, `02:…`); random only if there is no
  serial number.
* BAR layout (16 MiB): header, RC control line, peer table, per-sender
  producer and credit lines, BMC gateway rings, P2P rings (4 senders ×
  256 slots × 10 KiB) at `0x200000`, eDMA scratch at `0xf00000`.
* Write-only endpoint-to-endpoint control: connect tokens, acks and
  credits are posted writes; per-node epochs reject stale state.
* L2: unicast by peer table and learned MACs, broadcast/multicast to all
  members, ARP, IPv4, IPv6 ND. MTU up to 9246; 9000 is used.
* Recovery: leave handshake on module unload/reboot/poweroff, host link
  reset recovery, stalled-peer detection, BMC re-enumeration from the
  PCIe root port when a blade appears without a BAR address.

## Supported hardware and software

| Component | Supported / tested |
| --- | --- |
| Chassis | Mixtile Cluster Box (ASM2824 switch, MT7620A BMC), 4 slots |
| Blade | Mixtile Blade 3 (RK3588), PCIe link Gen3 x2 to the switch |
| Blade OS | Debian 12 with the Mixtile vendor kernel 6.1.99; Talos v1.14.2 (6.18.54-talos) via [mixtile-talos](https://github.com/roysbike/mixtile-talos) |
| BMC | OpenWrt 23.05 (kernel 5.15.150) on the Cluster Box; packaged in [mixtile-clusterbox-mt7620a-openwrt](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt) |

The blade's PCIe3 PHY is split in two x2 halves: lanes 0-1 go to the
Cluster Box (endpoint), lanes 2-3 to the M.2 NVMe (host). Both train
Gen3 x2.

## Install

### Cluster Box (BMC)

Recommended: install the ClusterBox firmware
[mixtile-clusterbox-mt7620a-openwrt v0.1.0-rc.1](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt/releases/tag/v0.1.0-rc.1),
which contains the helper as the `openmiop` package and starts it at boot.

On other Cluster Box firmware, use the release assets of this repository
(`openmiop-rc`, `openmiop.init`, `SHA256SUMS`):

```sh
sha256sum -c SHA256SUMS
ssh <bmc> 'cat > /tmp/openmiop-rc' < openmiop-rc
ssh <bmc> 'cat > /tmp/openmiop.init' < openmiop.init
# as root on the BMC (prefix each command with sudo when logged in as a user):
ssh <bmc> 'cp /tmp/openmiop-rc /usr/bin/openmiop-rc && chmod 755 /usr/bin/openmiop-rc &&
  cp /tmp/openmiop.init /etc/init.d/openmiop && chmod 755 /etc/init.d/openmiop &&
  /etc/init.d/openmiop enable && /etc/init.d/openmiop start'
```

`chmod` after copying matters: copying over an existing file on the BMC
overlay can drop the execute bit.

The BMC gets `omi0` = 10.20.0.1/24, MTU 9000. Log: `/tmp/openmiop-rc.log`.

### Blade 3 with Talos

Use the [mixtile-talos v0.1.0-rc.2](https://github.com/roysbike/mixtile-talos/releases/tag/v0.1.0-rc.2)
installer image. The module is built into it as a system extension;
nothing has to be compiled. The release notes there have the machine
configuration (`LinkAliasConfig` for `omi0`) and upgrade commands.

### Blade 3 with Debian (Mixtile vendor kernel)

The vendor kernel headers ship only in the Mixtile Debian image
(`/usr/src/linux-headers-6.1-rockchip`), so the module is built on the
blade itself:

```sh
sudo apt-get install -y build-essential git
git clone --branch v0.1.0-rc.2 https://github.com/roysbike/pcie-ep-net.git
cd pcie-ep-net
make -C drivers/openmiop KDIR=/usr/src/linux-headers-6.1-rockchip
sudo install -m 0644 drivers/openmiop/openmiop-ep.ko /usr/local/lib/openmiop-ep.ko
sudo install -m 0755 scripts/openmiop-ep-start.sh /usr/local/sbin/openmiop-ep-start
sudo install -m 0644 scripts/openmiop.service /etc/systemd/system/openmiop.service
echo 10.20.0.<last octet of the management address>/24 | sudo tee /etc/openmiop.addr
sudo systemctl daemon-reload && sudo systemctl enable --now openmiop
```

Choose a distinct 10.20.0.x address per blade (10.20.0.1 is the BMC).
If NetworkManager manages new interfaces, install
`scripts/unmanaged-omi0.conf` to `/etc/NetworkManager/conf.d/`.

## Verify

On a blade:

```sh
ip -d link show omi0          # UP, LOWER_UP, mtu 9000
dmesg | grep openmiop         # "link up", "node N", "peer M up"
ethtool -S omi0               # per-peer counters, tx_stalled, rx_dropped
ping -c3 10.20.0.1            # BMC over the gateway
ping -c3 -M do -s 8972 10.20.0.<peer>   # jumbo frame to another blade
```

On the BMC:

```sh
tail /tmp/openmiop-rc.log     # "node N active" per blade
ping -c3 10.20.0.<blade>
```

From a workstation with SSH access to all members,
`scripts/matrix-test.sh` pings every pair (normal and jumbo) and checks
IPv6 multicast; `scripts/bench.sh` runs iperf3 and SHA-256 transfers.

## Upgrade

* BMC: upgrade the ClusterBox firmware, or replace `/usr/bin/openmiop-rc`
  and restart `/etc/init.d/openmiop`. Blades reconnect by themselves.
* Talos: `talosctl upgrade` to the matching mixtile-talos installer.
* Debian: rebuild and install the module, then
  `sudo rmmod openmiop_ep && sudo systemctl restart openmiop`. The module
  performs the leave handshake on unload; peers pause for a few seconds
  and reconnect.

All members must speak the same protocol version (v4 here). The header
carries the version; a v3 endpoint is not activated by a v4 helper.

## Troubleshooting

| Symptom | Check / fix |
| --- | --- |
| `omi0` exists but no carrier | Is the helper running on the BMC (`pgrep openmiop-rc`, log)? Does `lspci` on the BMC show `1d87:4f4d` behind the switch? |
| Blade visible on the BMC but without BAR address | The helper re-enumerates from the root port automatically (at most once a minute); force with `touch /var/run/openmiop-reenumerate` on the BMC. P2P pauses ~1-2 s. |
| Pings up to ~100 bytes work, larger fail | MPS mismatch; the helper sets one MPS for all endpoints — restart it. |
| On Talos the link is called `enx…` | Add the `LinkAliasConfig` from the mixtile-talos release notes. |
| BMC log shows completion timeouts / nothing answers behind the switch | The fabric wedged (seen when an endpoint vanished during an RC read). Stop the helper, reboot the BMC (blade power stays on). |
| Throughput low | Check `LnkSta` on the BMC (`lspci -vv`): Gen3 x2 expected; MTU 9000 on both ends. |

More: [docs/operations.md](docs/operations.md).

## Tested configuration and results (2026-10-06)

Cluster Box, four Blade 3: two Debian 12 (vendor 6.1.99), two Talos
v1.14.2, the BMC: five members on one `omi0` segment. MTU 9000, all
links Gen3 x2.

| Test | Result |
| --- | --- |
| Debian ↔ Debian TCP, 1 stream | 8.0-8.3 Gbit/s each way, 0 retransmits |
| Debian ↔ Debian bidirectional | ~7.9 + 7.9 Gbit/s (10 min soak 7.96 + 7.96) |
| Debian → Talos / Talos → Debian | 6.4-6.6 / 7.4-7.5 Gbit/s |
| Talos ↔ Talos | 6.48 / 6.73 Gbit/s |
| SHA-256 integrity | 1 GiB and 512 MiB transfers in all tested directions identical |
| Five-member matrix | ping + jumbo between all members, IPv6 `ff02::1` |
| Blade ↔ BMC gateway | management-grade, ~12 Mbit/s towards the BMC |

Full list and recovery tests: [CHANGELOG.md](CHANGELOG.md), raw reports
in `docs/bench/`.

## Known limitations

* RX is polled by a kernel thread; interrupt-driven RX is not
  implemented or tested.
* No multiqueue; one TX queue and one NAPI context.
* TX polls the eDMA done bit (one core busy while sending).
* Talos receives ~20 % slower than Debian (6.4 vs 8.0 Gbit/s); not
  investigated.
* BMC root-port re-enumeration pauses all fabric traffic ~1-2 s when a
  blade appears without a BAR address (in rc.2 peers then reconnect by
  themselves; in rc.1 a moved peer could stay "connecting"). A blade that disappears without
  the leave handshake (crash, power loss) can race a BMC read and wedge
  the fabric until the BMC reboots.
* The BMC gateway is slow (management only).
* Not tested: more than four blades, MPS other than 128/256, helper
  restart under load with four endpoints.

## Compatible releases

| OpenMIOP Stack v0.1.0-rc.2 | |
| --- | --- |
| Protocol | OpenMIOP v4 (wire format unchanged since rc.1) |
| Blade driver | [pcie-ep-net v0.1.0-rc.2](https://github.com/roysbike/pcie-ep-net/releases/tag/v0.1.0-rc.2) |
| Blade OS | [mixtile-talos v0.1.0-rc.2](https://github.com/roysbike/mixtile-talos/releases/tag/v0.1.0-rc.2) |
| ClusterBox BMC | [mixtile-clusterbox-mt7620a-openwrt v0.1.0-rc.1](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt/releases/tag/v0.1.0-rc.1) (the BMC helper did not change in rc.2) |

## Build

```sh
make -C drivers/openmiop KDIR=<kernel build tree>     # endpoint module
make -C userspace                                     # openmiop-rc, omi-peek (needs mipsel-linux-gnu-gcc)
```

`openmiop-rc` and `omi-peek` are freestanding MIPS32 soft-float binaries
(the MT7620A has no FPU; a glibc build is hard-float and dies with
SIGILL). GitHub Actions builds them on every push and attaches them to
tagged releases; it also compiles the module against upstream
`linux-6.1.y` as a build check (that module is not shipped).

## Documents

* [docs/architecture.md](docs/architecture.md): topology, BAR layout,
  control plane, data path, memory ordering, L2, limitations
* [docs/operations.md](docs/operations.md): install, addresses, health,
  recovery
* [docs/talos-port.md](docs/talos-port.md): what Talos 1.14.2 needed
* [docs/baseline.md](docs/baseline.md), [docs/phase1-runbook.md](docs/phase1-runbook.md),
  [docs/feasibility.md](docs/feasibility.md): history
* `docs/bench/`: benchmark reports (`scripts/bench.sh`)
