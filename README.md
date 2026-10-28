# pcie-ep-net (openmiop)

[English](README.md) · [Русский](README.ru.md)

**Ethernet between the Mixtile Blade 3 boards of a Mixtile Cluster Box,
over the box's internal PCIe switch.** Each blade gets a network interface
`omi0`. A frame from one blade to another is a DMA write from the
sender's memory straight into the receiver's PCIe BAR: it crosses the
switch and never passes through the Cluster Box BMC.

The implementation is called **openmiop** (interface `omi0`, on-wire magic
`OMI1`). It is an independent implementation, not Mixtile's MIOP driver,
and contains no Mixtile code or firmware. License: GPL-2.0-or-later
([LICENSE](LICENSE)).

**Current release: [v0.7.0](https://github.com/roysbike/pcie-ep-net/releases/tag/v0.7.0)**
(2026-10-24), part of OpenMIOP Stack v0.7.0.
Release notes: [v0.7.0](docs/release-notes/v0.7.0.md) ·
[v0.6.1](docs/release-notes/v0.6.1.md) · [CHANGELOG.md](CHANGELOG.md).

## Contents

* [The OpenMIOP Stack](#the-openmiop-stack)
* [Features](#features)
* [Hardware](#hardware)
* [How it works](#how-it-works)
* [Requirements](#requirements)
* [Install](#install)
* [Verify](#verify)
* [Configuration](#configuration)
* [Upgrade and mixed versions](#upgrade-and-mixed-versions)
* [Performance](#performance)
* [Known limitations](#known-limitations)
* [Troubleshooting](#troubleshooting)
* [Build](#build)
* [Repository layout](#repository-layout)

## The OpenMIOP Stack

Mixtile ships the Cluster Box with its own BMC firmware and its own blade
network driver (MIOP). The **OpenMIOP Stack** is an independent, open
replacement made of three repositories that are released together:

| Repository | What it gives you | Stack v0.7.0 |
| --- | --- | --- |
| **pcie-ep-net** (this one) | the blade driver `openmiop-ep` (`omi0`), the BMC module `omi-rc`, the Blade 3 board modules | [v0.7.0](https://github.com/roysbike/pcie-ep-net/releases/tag/v0.7.0) |
| [mixtile-talos](https://github.com/roysbike/mixtile-talos) | Talos Linux (Kubernetes) for Blade 3 with openmiop built in | [v0.7.0](https://github.com/roysbike/mixtile-talos/releases/tag/v0.7.0) (Talos v1.14.4) |
| [mixtile-clusterbox-mt7620a-openwrt](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt) | OpenWrt for the BMC: blade power and consoles, the BMC side of openmiop, the cluster's network edge, web UI, updates | [v0.4.1](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt/releases/tag/v0.4.1) (OpenWrt 25.12.6) |

Protocol of Stack v0.7.0: OpenMIOP v4 framing between blades with the
per-connection features RX doorbell, multiqueue and checksum offload;
push mode, gateway pull and zero-copy towards the BMC.

## Features

* **Fast blade-to-blade network.** ~8.5 Gbit/s for one TCP flow,
  ~11.8 Gbit/s with four flows between two blades, ~25 Gbit/s total with
  all four blades sending. MTU 9000 (up to 9246).
* **Plain Ethernet for the OS.** `omi0` is a normal Linux netdev: ARP,
  IPv4, IPv6 with neighbour discovery, broadcast and multicast, MAC
  learning (works behind a Linux bridge, Kubernetes CNIs, VXLAN/Geneve).
* **The BMC on the same segment.** The BMC is `10.20.0.1` on `omi0` and
  routes the blades to the LAN and the internet (~220-280 Mbit/s, limited
  by the BMC CPU).
* **Multiqueue and interrupts.** 4 queue pairs with RSS, an RX doorbell
  interrupt and NAPI context per queue, TX threads and eDMA interrupts on
  the Cortex-A76 cores.
* **Checksum offload between blades.** The receiver gets frames with
  `CHECKSUM_PARTIAL`, as with virtio-net; the payload only crosses PCIe
  links (LCRC-protected) and DRAM.
* **Survives resets.** Blade reboots, module reloads and a BMC reboot
  recover by themselves: leave handshake on unload, per-node epochs, link
  reset recovery, blades reconnect to each other without the BMC.
* **Safe for the root complex.** In push mode the BMC never reads a blade
  BAR, so a blade that disappears cannot wedge the PCIe fabric.
* **Stable identity.** Node number = switch slot; MAC address derived
  from the RK3588 serial number.
* **Rolling upgrades.** Features are negotiated per pair, so blades on
  different releases (v0.1 to v0.6) keep talking.
* **Blade 3 board modules** (`drivers/blade3`): `blade3-nvme-recover`
  brings the M.2 NVMe back after a Cluster Box PCIe reset, `blade3-leds`
  drives the 2.5G port LEDs.

## Hardware

```
        Cluster Box BMC (MT7620A, OpenWrt)
        omi-rc module, omi0 10.20.0.1
                   |
              PCIe Gen1 x1   (control + gateway to the LAN)
                   |
            ASMedia ASM2824 PCIe switch     <- blade-to-blade traffic stays here
         /        /          \          \
    Gen3 x2   Gen3 x2      Gen3 x2    Gen3 x2
       |         |            |          |
     slot 1    slot 2       slot 3     slot 4     Mixtile Blade 3 (RK3588)
     node 3    node 2       node 0     node 1     PCIe endpoint, openmiop-ep, omi0 10.20.0.x
```

* **Mixtile Cluster Box:** chassis for four Blade 3. Its BMC (MediaTek
  MT7620A, MIPS 580 MHz) switches the blades on and off, gives access to
  their serial consoles and is the PCIe root complex of the box. An
  ASMedia ASM2824 Gen3 switch connects each blade slot (Gen3 x2) and the
  BMC (Gen1 x1).
* **Mixtile Blade 3:** Rockchip RK3588 (4× Cortex-A76 + 4× Cortex-A55),
  eMMC, M.2 NVMe, two 2.5G Ethernet ports. Its PCIe 3.0 x4 controller is
  split in two: lanes 0-1 go to the Cluster Box switch (endpoint, this
  driver), lanes 2-3 to the M.2 NVMe (host).

Without a Cluster Box there is no PCIe fabric, so a standalone Blade 3
has no use for `openmiop-ep`.

## How it works

* **Blade side.** `openmiop-ep` turns the RK3588 PCIe3 controller
  `fe150000` into a PCIe endpoint (PCI ID `1d87:4f4d`) with one 16 MiB
  BAR and registers `omi0`. It programs the controller directly (DBI,
  iATU, eDMA) and does not use the kernel PCI endpoint framework.
* **BMC side.** The `omi-rc` kernel module finds the endpoints behind the
  switch, restores their BAR, command and MPS settings, numbers them by
  switch slot, publishes the peer table (epoch, BAR address, MAC per
  node) into every BAR and puts the BMC itself on `omi0`. Push mode: the
  blades write their state into BMC memory, the BMC never reads a blade
  ([docs/push-mode.md](docs/push-mode.md)). The older userspace helper
  `openmiop-rc` does the same from sysfs and is the fallback.
* **Data path.** The sender's eDMA engine writes frames as posted `MemWr`
  TLPs into a per-sender, per-queue ring in the receiver's BAR; the switch
  routes them by address (the ASM2824 has no ACS). Then the sender writes
  a doorbell word that the receiver maps onto a GIC ITS interrupt, which
  runs the queue's NAPI context (GRO). Credits go back as posted writes.
  Endpoints never read each other.
* **Gateway.** Frames to the BMC go into the gateway ring; frames from
  the BMC are pulled by the blade's eDMA straight out of the BMC's socket
  buffers (gateway pull, zero-copy).

Full description (BAR layout, control plane, memory ordering, L2):
[docs/architecture.md](docs/architecture.md).

## Requirements

| Component | Supported / tested |
| --- | --- |
| Chassis | Mixtile Cluster Box (ASM2824 switch, MT7620A BMC), 4 slots |
| Blade | Mixtile Blade 3 (RK3588), link Gen3 x2 to the switch |
| Blade OS, tested on hardware | Talos v1.14.4 (kernel 6.18.57-talos) via [mixtile-talos](https://github.com/roysbike/mixtile-talos), the reference platform; Debian 12 with the Mixtile vendor kernel 6.1.99 (tested with v0.1) |
| Blade OS, built in CI | DKMS package on Debian 12/13 and Ubuntu 22.04 (HWE)/24.04 arm64 kernels; any kernel 6.1 or newer with its headers. RX interrupts need Linux 6.10+ (older kernels poll) |
| Device tree | `pcie@fe150000` as PCIe endpoint: `openmiop,rk3588-pcie-ep` (mixtile-talos) or `mixtile,miop-ep-rk3588` (Mixtile U-Boot) |
| BMC | ClusterBox firmware v0.4.1 (OpenWrt 25.12.6, Linux 6.12) or v0.3.0 (OpenWrt 23.05, Linux 5.15) with `omi-rc` built in |

## Install

### 1. The BMC

Install the ClusterBox firmware
[v0.4.1](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt/releases/tag/v0.4.1).
It loads `omi-rc` at boot (`kmod-omi-rc`) and falls back to the helper
(`openmiop` package). Nothing else is needed on the BMC.

The BMC gets `omi0` = 10.20.0.1/24, MTU 9000.

<details>
<summary>Other BMC firmware: the userspace helper</summary>

Use the release asset `openmiop-<version>-clusterbox-bmc-mipsel.tar.gz`
(`openmiop-rc`, `omi-peek`, `openmiop.init`):

```sh
sha256sum -c --ignore-missing SHA256SUMS
tar xzf openmiop-0.7.0-clusterbox-bmc-mipsel.tar.gz
cd openmiop-0.7.0-clusterbox-bmc-mipsel
scp -O openmiop-rc openmiop.init root@<bmc>:/tmp/
ssh root@<bmc> 'cp /tmp/openmiop-rc /usr/bin/openmiop-rc && chmod 755 /usr/bin/openmiop-rc &&
  cp /tmp/openmiop.init /etc/init.d/openmiop && chmod 755 /etc/init.d/openmiop &&
  /etc/init.d/openmiop enable && /etc/init.d/openmiop start'
```

`chmod` after copying matters: copying over an existing file on the BMC
overlay can drop the execute bit. Log: `/tmp/openmiop-rc.log`.
</details>

### 2. The blades: Talos

Use the [mixtile-talos v0.7.0](https://github.com/roysbike/mixtile-talos/releases/tag/v0.7.0)
image or installer. The module and the board modules are built in as a
system extension; nothing has to be compiled. The mixtile-talos README
has the machine configuration (`LinkAliasConfig` for `omi0`, address)
and the upgrade procedure.

### 2. The blades: Debian or Ubuntu

Install the DKMS package from the
[release](https://github.com/roysbike/pcie-ep-net/releases/tag/v0.7.0).
DKMS builds the module for the running kernel (its headers must be
installed; on the Mixtile Debian 12 image the package finds
`/usr/src/linux-headers-6.1-rockchip` by itself).

```sh
sha256sum -c --ignore-missing SHA256SUMS
sudo apt install ./openmiop-dkms_0.7.0_all.deb
echo 10.20.0.<N>/24 | sudo tee /etc/openmiop.addr    # one address per blade; .1 is the BMC
sudo reboot
```

The package enables `openmiop.service` (it brings up `omi0` once
`/etc/openmiop.addr` exists) and disables Mixtile's `load-miop.service`:
the vendor MIOP stack drives the same controller and cannot be unloaded
on a running system, hence the reboot. The board modules in
`drivers/blade3` are not part of the DKMS package.

## Verify

On a blade:

```sh
ip -d link show omi0          # UP, LOWER_UP, mtu 9000
dmesg | grep openmiop         # "node N", "peer M up, 4 queues, doorbell, checksum offload"
ethtool -S omi0               # counters, rx_q<n>_packets per queue
ethtool -l omi0               # queue pairs
ping -c3 10.20.0.1            # the BMC
ping -c3 -M do -s 8972 10.20.0.<peer>   # jumbo frame to another blade
```

On Talos: `talosctl -n <node> dmesg | grep openmiop`.

On the BMC:

```sh
dmesg | grep omi0             # "push mode", "node N active" per blade
nodectl status --all          # every blade powered, LINK Gen3 x2
ping -c3 10.20.0.<blade>
```

From a workstation that reaches all members: `scripts/matrix-test.sh`
pings every pair (normal and jumbo frames) and checks IPv6 multicast;
`scripts/bench.sh` runs iperf3 and SHA-256 transfers.

## Configuration

**Addresses.** One /24 for the fabric: the BMC is 10.20.0.1, each blade
picks its own address (the lab uses the last octet of the blade's
management address). MTU 9000 on every member.

**Driver parameters** of `openmiop-ep` (`modprobe openmiop-ep name=value`;
on Talos `machine.kernel.modules[].parameters`). The defaults are what
the releases are tested with.

| Parameter | Default | What it does |
| --- | --- | --- |
| `queues` | 4 | TX/RX queue pairs (1-4); peers with the same count use one ring per queue |
| `rx_doorbell` | on | RX interrupt raised by the sender (Linux 6.10+ with the GIC ITS; off: poll) |
| `tx_irq` | on | wait for eDMA runs on the done interrupt (off: poll) |
| `tx_chain` | on | build the next eDMA run while one is on the wire and start it at its done; writable at runtime |
| `tx_batch` | 16 | frames per eDMA run at most (1-32); writable at runtime |
| `tx_cpus` | empty | CPUs for the TX threads and eDMA interrupts, e.g. `4-7`; empty: the biggest cores (Cortex-A76 on RK3588) |
| `p2p_csum` | on | offer and use checksum offload between blades |
| `gw_pull`, `gw_zc` | on | read BMC gateway frames with eDMA; zero-copy on the BMC when it offers it |
| `autonomy` | on | after a fabric reset reconnect to the last peers without waiting for the BMC |
| `use_edma` | on | off: everything goes over the BMC gateway (debugging only) |

## Upgrade and mixed versions

* **BMC:** update the ClusterBox firmware (web UI or `mixtile-update`).
  Blades reconnect by themselves.
* **Talos:** `talosctl upgrade` to the matching mixtile-talos installer,
  one node at a time.
* **Debian/Ubuntu:** install the new `.deb`, then reboot (or
  `sudo rmmod openmiop_ep && sudo systemctl restart openmiop`). The module
  does the leave handshake on unload; peers pause for a few seconds and
  reconnect.

All members must speak protocol v4 (the header carries the version).
Blades from v0.1 to v0.6 mix: interrupts, queues and checksum offload are
negotiated per pair, so a rolling upgrade keeps every pair working; a
pair gets the offload once both ends run v0.6. `omi-rc` (push mode) needs
v0.3 or later on every blade; older blades need the helper
(`touch /etc/openmiop.use-helper` on the BMC).

## Performance

Lab Cluster Box, four Blade 3 on Talos, MTU 9000, under cluster load:

| Path | Throughput |
| --- | --- |
| Blade -> blade, 1 TCP flow | 8.5 Gbit/s (v0.6: 7.8-8.0) |
| Blade -> blade, 4 / 8 TCP flows | 11.8-11.9 / 11.5-11.9 Gbit/s (v0.6: 10.6-11.3) |
| Ping under 8-flow load | ~1.4 ms average, < 5 ms max (v0.6: 1.6-1.9 / 6.6-7.6 ms) |
| All four blades sending at once | ~25 Gbit/s total (measured with v0.3) |
| Pod -> pod over Kube-OVN Geneve, 1 / 4 flows | 5.3-5.6 / 8.6 Gbit/s |
| Blade <-> LAN through the BMC | ~220-280 Mbit/s (BMC CPU) |

Data integrity is checked with SHA-256 transfers in every release. All
results, older releases and the gateway history:
[docs/performance.md](docs/performance.md).

## Known limitations

* **One flow = one queue.** A single TCP flow uses one queue and one eDMA
  channel (~8.5 Gbit/s); more flows spread over the four queues.
* **No TSO/GSO.** The stack segments to the MTU. Checksum offload works
  only between blades, not towards the BMC. Slots are a fixed 10 KiB, so
  small frames waste ring space.
* **Latency under bidirectional load.** With full traffic in both
  directions between two blades, a ping between them takes 1-2 ms
  (median), with tails of tens of ms and up to ~130 ms on a node whose
  CPU is busy (measured 2026-10-28 with iperf3 `-P 4 --bidir` on a
  loaded cluster; idle 0.2-0.3 ms). Small frames (ping, ACKs, etcd) wait
  behind the bulk frames in the same queues and for the CPU of the
  receiving node. Pinning iperf3 to other cores did not help; a smaller
  `tx_batch` lowers the median but costs throughput, so the default stays
  16.
* **The gateway is slow.** Traffic to the LAN goes through the MT7620A
  CPU: ~220-280 Mbit/s. Blade-to-blade traffic is not affected.
  When several blades send into one blade at full speed, gateway traffic
  to that blade queues behind them (pings up to ~200 ms, some loss).
* **A BMC reboot resets the fabric.** Every blade link drops until
  `omi-rc` is loaded again (~150 s on OpenWrt 25.12); blades reconnect to
  each other after ~12 s by themselves. On Blade 3 the same reset hits the
  M.2 NVMe (shared PERST#); `blade3-nvme-recover` brings it back. With
  DRBD 9.3.4, check that `out-of-sync` is 0 before a planned BMC reboot
  ([LINBIT/drbd#153](https://github.com/LINBIT/drbd/issues/153)).
* **Helper only:** the userspace helper reads blade BARs and can wedge
  the fabric if a blade vanishes without the leave handshake; `omi-rc`
  never reads a blade. Root-port re-enumeration (helper only) pauses all
  traffic for ~1-2 s.
* **Not tested:** more than four blades, other PCIe switches, MPS other
  than 128/256, the current release on Debian hardware.

## Troubleshooting

| Symptom | Check / fix |
| --- | --- |
| `omi0` exists but has no carrier | On the BMC: `omi-rc` loaded (`dmesg \| grep omi0`) or the helper running (`pgrep openmiop-rc`)? `lspci -d 1d87:4f4d` shows the blade? |
| Blade visible on the BMC without a BAR address | Helper: it re-enumerates from the root port by itself (at most once a minute); force with `touch /var/run/openmiop-reenumerate` |
| Pings up to ~100 bytes work, larger ones fail | MPS mismatch: restart `omi-rc` / the helper, it sets one MPS for all endpoints |
| On Talos the link is called `enx…` | Add the `LinkAliasConfig` from the mixtile-talos README |
| Completion timeouts on the BMC, nothing answers behind the switch | The fabric wedged: reboot the BMC (blade power stays on) |
| Throughput low | `lspci -vv` on the BMC: `LnkSta` Gen3 x2; MTU 9000 on both ends; dmesg shows `4 queues, doorbell, checksum offload` for the peer |

More: [docs/operations.md](docs/operations.md).

## Build

```sh
make -C drivers/openmiop KDIR=<kernel build tree>     # openmiop-ep.ko
make -C drivers/blade3 KDIR=<kernel build tree>       # board modules (need a full build tree: private DesignWare header)
make -C userspace                                     # openmiop-rc, omi-peek (mipsel-linux-gnu-gcc)
make -C <openwrt kernel tree> M=$PWD/drivers/omi-rc ARCH=mips CROSS_COMPILE=mipsel-openwrt-linux- modules
```

`openmiop-rc` and `omi-peek` are freestanding MIPS32 soft-float binaries
(the MT7620A has no FPU). The ClusterBox firmware builds `omi-rc` as
`kmod-omi-rc`; mixtile-talos builds `openmiop-ep` and the board modules
against the Talos kernel.

GitHub Actions on every push: the endpoint module against `linux-6.1.y`
and `linux-6.18.y`, the board modules, the BMC binaries, and the DKMS
package installed on Debian 12/13 and Ubuntu 22.04/24.04. A `v*` tag
publishes the release (DKMS `.deb`, BMC tarball, `SHA256SUMS`,
`BUILD-INFO.txt`).

## Repository layout

| Path | Content |
| --- | --- |
| `drivers/openmiop/` | blade endpoint driver `openmiop-ep` and the shared protocol header `openmiop.h` |
| `drivers/omi-rc/` | BMC kernel module `omi-rc` |
| `drivers/blade3/` | Blade 3 board modules `blade3-nvme-recover`, `blade3-leds` |
| `userspace/` | BMC helper `openmiop-rc` and the BAR inspector `omi-peek` |
| `packaging/debian-dkms/` | DKMS package and `openmiop.service` |
| `scripts/` | tests (`matrix-test.sh`, `bench.sh`, `net-test.sh`), init scripts, build helpers |
| `docs/` | [architecture](docs/architecture.md), [push mode](docs/push-mode.md), [operations](docs/operations.md), [performance](docs/performance.md), [release notes](docs/release-notes/), [history](docs/history/) |
