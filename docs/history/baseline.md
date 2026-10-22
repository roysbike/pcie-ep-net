# Baseline — 2026-10-05

State of `claude/driver-development` at commit `6a77b6c` before any
driver change, measured on the live Cluster Box. Everything below was
observed on the hardware; nothing is carried over from earlier notes
without being re-checked.

## Hosts

| Alias | Role | Kernel | OS |
| --- | --- | --- | --- |
| `clusterbox` | Root complex (MT7620A) + ASM2824 switch | `5.15.150 #0` mips | OpenWrt 23.05-SNAPSHOT (`BUILD_ID=MIXTILE`) |
| `blade160` | Endpoint, switch port `02:00.0` → bus 03, slot 3 | `6.1.99 #2 SMP` aarch64 (vendor) | Debian 12 |
| `blade173` | Endpoint, switch port `02:04.0` → bus 04, slot 4 | `6.1.99 #2 SMP` aarch64 (vendor) | Debian 12 |

Blades: 8 cores, 31 GiB RAM, `linux-headers-6.1-rockchip` installed,
`gcc` 12.2, `iperf3`, `python3`. No `ethtool` on blade160, no
`modinfo`, no sysstat. The Cluster Box has `lspci`/`setpci`, no
`devmem`.

## PCIe topology

```
-[0000:00]---00.0-[01-06]----00.0-[02-06]--+-00.0-[03]----00.0  [1d87:4f4d]  blade160
                                           +-04.0-[04]----00.0  [1d87:4f4d]  blade173
                                           +-08.0-[05]--
                                           \-0c.0-[06]--
```

| BDF | ID | Role | LnkCap | LnkSta |
| --- | --- | --- | --- | --- |
| 00:00.0 | `1814:0801` | MT7620A root port | 2.5 GT/s x1 | 2.5 GT/s x1 |
| 01:00.0 | `1b21:2824` | ASM2824 upstream | 8 GT/s x8 | 2.5 GT/s x1 (downgraded) |
| 02:00.0 | `1b21:2824` | downstream → bus 03 | 8 GT/s x4 | **8 GT/s x2**, EQ phases 1–3 complete |
| 02:04.0 | `1b21:2824` | downstream → bus 04 | 8 GT/s x4 | **8 GT/s x2**, EQ phases 1–3 complete |
| 03:00.0 | `1d87:4f4d` class 0200 | blade160 EP | 8 GT/s x4 | 8 GT/s x2 (downgraded) |
| 04:00.0 | `1d87:4f4d` class 0200 | blade173 EP | 8 GT/s x4 | 8 GT/s x2 (downgraded) |

Why x2: on both blades `fe160000.pcie` (pcie3x2) is an active root
complex with the Samsung NVMe at `0001:11:00.0`, so the PCIe3 PHY is
bifurcated. The driver programs width x2 (`lanes=2` default).

MPS: root port and switch upstream 128 B; switch downstream ports and
EPs 256 B. Peer TLPs only cross downstream ports, so 256 B applies.

No ACS capability on any ASM2824 port. AER on the root port shows
`CmpltTO+` and `RxErr+` latched; the switch upstream shows `UnsupReq+`.
These are sticky and their age is unknown.

### Endpoint config space (from the RC)

| Item | 03:00.0 | 04:00.0 |
| --- | --- | --- |
| BAR0 | 64-bit pref 16 MiB @ `0x21000000` | 64-bit pref 16 MiB @ `0x22000000` |
| BAR2–5 | 16 MiB each, shown `[virtual]` @ `0x23..0x26000000` | 16 MiB each @ `0x28..0x2b000000` |
| Expansion ROM | 64 KiB @ `0x27000000` | 64 KiB @ `0x27800000` |
| COMMAND | `Mem+ BusMaster-` | `Mem+ BusMaster-` |
| MSI / MSI-X | 1/32 / 128, both disabled | same |

Every blade consumes 16 + 4×16 MiB + 64 KiB ≈ 80 MiB of the 256 MiB
MT7620A window (`0x20000000–0x2fffffff`). Three blades fit, four do
not.

The EPs do DMA with `BusMaster-`. It works on this hardware, but it is
not PCIe-compliant behaviour to rely on.

## Driver as found

* Both blades had `openmiop_ep` loaded, but **not the same build**:
  blade160 was running a module without the `lanes` parameter, older
  than the `/usr/local/lib/openmiop-ep.ko` on disk. blade173 matched
  the disk copy. The source in `~/openmiop` on blade173 is byte-identical
  to repo HEAD (`md5 4428e086…` for `openmiop-ep.c`).
* `openmiop.service` is enabled on both blades (active on blade160,
  inactive on blade173). `/etc/init.d/openmiop` is enabled on the RC.
* `/usr/bin/openmiop-rc` on the RC is the build of repo
  `userspace/openmiop-rc.c`.
* The RC's sysfs `enable` count for both EPs was **1114**: the helper
  writes `enable` on every scan and never drops it.

For the baseline both blades were reloaded with the same repo-HEAD
build (below). Nothing persistent was changed: the module is loaded
from `/tmp`; `/usr/local/lib`, systemd units and the RC init script
are untouched.

## Vendor kernel facts relevant to the driver

`/proc/config.gz` on the blades:

```
CONFIG_PCIE_DW=y  CONFIG_PCIE_DW_HOST=y  CONFIG_PCIE_DW_ROCKCHIP=y
# CONFIG_PCIE_DW_ROCKCHIP_EP is not set
# CONFIG_PCI_ENDPOINT is not set
# CONFIG_DW_EDMA is not set
CONFIG_PHY_ROCKCHIP_SNPS_PCIE3=y
```

There is no EPC framework and no dw-edma dmaengine on this kernel.
That is why the driver programs DBI, iATU and eDMA registers itself.

Live DT node `/pcie@fe150000` (U-Boot rewrote it):

| Property | Value |
| --- | --- |
| compatible | `mixtile,miop-ep-rk3588` |
| reg | apb `0xfe150000` 64 KiB, dbi `0xa40000000` 4 MiB, config `0xf0000000` 1 MiB |
| ranges | … 64-bit pref `0x9_00000000` 1 GiB (the driver's `ob_base`) |
| interrupts | SPI 263/262/261/260/259 = sys, pmc, msg, legacy, err |
| memory-region | `miop_dma@0x0e000000` (`0x0e000000–0x1fffffff` reserved) |

The eDMA interrupt lines (SPI 269–272 in mainline `pcie3x4_ep`) are
not in this node.

## Build

```sh
# on a blade (no aarch64 cross compiler on the workstation)
make -C drivers/openmiop          # KDIR=/usr/src/linux-headers-6.1-rockchip
```

Builds with no warnings other than the compiler-version notice (kernel
built with gcc 10.3, module with Debian gcc 12.2).

checkpatch (`--strict`, current mainline script):
`0 errors, 11 warnings, 7 checks`. Ten warnings are
"memory barrier without comment".

## Load / reload procedure

`scripts/dev-reload.sh path/to/openmiop-ep.ko` does all of it:

1. Stop `openmiop-rc` on the RC.
2. On each blade: `rmmod`, `insmod`, MTU 9000, up, address from
   `/etc/openmiop.addr`.
3. On the RC, rewrite BAR0 and set `PCI_COMMAND` Memory Space.
4. Start `openmiop-rc`.

Step 3 is required. Probe asserts the controller reset, which clears
the EP's BAR0 register and `PCI_COMMAND`:

```
03:00.0 BAR0=0000000c BAR1=00000000 CMD=0002   # after reload, before fix
```

The helper then reads magic `0x00000000` and loops on
`skip wrong version`. `echo 1 > enable` is a no-op because the count
is already non-zero. The driver comment "Keep an address the host
already assigned" in `program_config_space()` does not hold across a
reload.

Probe log (blade173, same on blade160):

```
openmiop-ep fe150000.pcie: BAR dma 0xe000000 (raw 0xe000000) size 16777216 cacheable
openmiop-ep fe150000.pcie: resizable BAR limited to 16 MiB (6)
openmiop-ep fe150000.pcie: controller link width x2
openmiop-ep fe150000.pcie: target link Gen3 x2 LNKCAP 0x426823
openmiop-ep fe150000.pcie: hid ext cap 0xf at 0x168
openmiop-ep fe150000.pcie: hid ext cap 0x13 at 0x178
openmiop-ep fe150000.pcie: hid ext cap 0x15 at 0x2e8
openmiop-ep fe150000.pcie: config id 1d87:4f4d class 020000
openmiop-ep fe150000.pcie: iATU unroll, target 0xe000000
openmiop-ep fe150000.pcie: link up, LTSSM 0x230011
openmiop-ep fe150000.pcie: eDMA ctrl 0x20002, write IRQ masked
openmiop-ep fe150000.pcie: omi netdev omi0 mac ea:73:70:73:60:16
openmiop-ep fe150000.pcie: outbound window 0x900000000 -> peer BAR 0x21000000
openmiop-ep fe150000.pcie: peer BAR magic 0x31494d4f
openmiop-ep fe150000.pcie: P2P on, peer 5a:da:70:55:99:ce gen 1
```

No error lines in dmesg on either blade during any test below.

## Results

Addresses: blade160 `10.20.0.13`, blade173 `10.20.0.14`, RC `10.20.0.1`.
`omi0` MTU 9000 on the blades, 1500 on the RC TAP.

### Latency (blade160)

| Target | Path | Result |
| --- | --- | --- |
| 10.20.0.14, 100 × 0.01 s | P2P eDMA | 0 % loss, rtt 0.300 / 0.427 / 0.801 ms |
| 10.20.0.1, 20 × 0.05 s | gateway ring + RC helper | 0 % loss, rtt 0.571 / 2.119 / 18.236 ms |

### MTU

| Test | Result |
| --- | --- |
| peer, DF, payload 1472 / 8972 | pass |
| gateway, payload 1472 | pass |
| gateway, payload 1473 / 4000 | **100 % loss, no error** |

`omi0` advertises MTU 9000, but frames for the RC above 1514 bytes are
dropped in `omi_xmit()` without a counter or ICMP. One interface, two
effective MTUs.

### Throughput (iperf3 3.12, 10 s, blade160 client)

| Test | Result | Retr |
| --- | --- | --- |
| TCP 160 → 173 | 7.26 Gbit/s | 0 |
| TCP 173 → 160 (`-R`) | 6.91 Gbit/s | 110 |
| TCP bidir | 6.75 + 6.85 Gbit/s | 500 + 483 |
| TCP 160 → 173, `-P 4` | 8.05 Gbit/s sum | 222 |
| TCP 160 → 173, 60 s | 7.21 Gbit/s (6.51–7.49 per 10 s) | 160 |
| UDP 8972 B, 3 Gbit/s | 2.7 % loss | – |
| UDP 8972 B, unlimited | 6.89 sent / 6.20 recv Gbit/s, 10 % loss | – |
| UDP 1400 B, 1 Gbit/s | 0.059 % loss | – |

UDP loss is socket receive-buffer overflow, not the driver:
`UdpRcvbufErrors` on blade173 = 54494 = sum of the three runs, and the
sender's `omi0` TX packet count equals the receiver's RX count (minus
gateway frames). `rx_dropped`/`rx_errors` stayed 0. blade160 shows
`tx_dropped 6` (from the earlier gateway MTU test).

### Reordering

TCP retransmits on a lossless link come from reordering. During the
60 s run:

| Counter | Value |
| --- | --- |
| sender `TcpExtTCPSACKReorder` | 7100 |
| sender `TcpExtTCPDSACKRecv` (spurious retransmits) | 737 |
| receiver `TcpExtTCPOFOQueue` | 108 |

The receive poll thread migrated across all 8 CPUs during the run
(30 samples: CPU0 ×8, CPU1 ×7, others 1–4 each). `rx_deliver()` uses
`gro_cells_receive()`, which queues to the current CPU's cell. Frames
queued on two CPUs are drained independently, so they reach TCP out of
order. To be confirmed by the fix.

### Data integrity

The receive path sets `CHECKSUM_UNNECESSARY`, so TCP will not detect a
corrupted payload. `scripts/xfer-check.py` hashes the bytes on both
ends instead: 4 runs × 1 GiB random data (2 per direction), sha256
matched every time, 1.6–2.0 s per GiB.

### CPU and interrupts (`scripts/cpu-sample.sh`)

| Condition | Host | Total busy | `openmiop-tx` | `openmiop-ep` (poll) | IRQ/s |
| --- | --- | --- | --- | --- | --- |
| idle | blade160 | 2.1 % | 0 % | 19.0 % | 15 974 |
| idle | blade173 | 1.3 % | 0 % | 14.4 % | 17 165 |
| TCP 160→173 | sender 160 | 2.12 cores | **100.8 %** | 29.2 % | 15 595 |
| TCP 160→173 | receiver 173 | 1.44 cores | 14.1 % | 69.6 % | 21 727 |
| TCP bidir | 160 | 2.29 cores | 100.4 % | 55.4 % | 16 414 |
| TCP bidir | 173 | 1.93 cores | 97.4 % | 48.4 % | 18 315 |

* The sender's TX thread is pinned at 100 %: `edma_wait_ch()` spins on
  the eDMA status register.
* Idle cost is 14–19 % of a core and ~16 k timer interrupts/s from
  `usleep_range(20, 50)` in the poll thread.
* The poll thread sleeps in `D` state, so load average is a constant
  1.00 on an idle blade.
* No PCIe/eDMA interrupt is used: the eDMA IRQ is masked and `pcie-sys`
  stays at 2.

## Other observations

* MACs are random on every load (`eth_hw_addr_random()`), and the RC
  helper picks a random MAC on every start. After a helper restart,
  blade160 still had the old RC MAC in ARP; the first 3 gateway pings
  failed until ARP re-resolved.
* `iperf3 -s -D` servers were left running on both blades, bound to
  the `omi0` addresses, for later runs.
