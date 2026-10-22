# Performance and test results

All numbers are from the lab Cluster Box: four Mixtile Blade 3 (RK3588),
links Gen3 x2 to the ASM2824 switch, MTU 9000, a loaded Kubernetes
(Cozystack) cluster running on the blades. Tools: `iperf3` and
`scripts/bench.sh` (SHA-256 transfers); raw reports in [bench/](bench/).

## Current: v0.6.x

v0.6.1 has the same driver as v0.6.0. Measured 2026-10-20 with two blades
on v0.6.0 and two on v0.5.0, Talos v1.14.2; after the v0.6.1 roll
(Talos v1.14.4, 2026-10-21) one flow between every pair was again
7.8-8.0 Gbit/s.

| Test | Result |
| --- | --- |
| v0.6 <-> v0.6, 1 TCP stream | 7.82-7.98 Gbit/s, 0 retransmits (v0.5.0: 5.0-5.5) |
| v0.6 <-> v0.6, 4 TCP streams | 11.1-11.3 Gbit/s |
| v0.6 <-> v0.5, 1 TCP stream | 5.9-7.2 Gbit/s |
| Pod to pod over Kube-OVN Geneve, 1 / 4 streams | 5.3-5.6 / 8.6 Gbit/s |
| Integrity (SHA-256; TCP/UDP, IPv4/IPv6, pods, through the BMC gateway) | all identical, `InCsumErrors` 0 |
| 1 h soak: load plus 1 GiB integrity loops | 1 232 transfers identical, no driver messages |

### Through the BMC (gateway)

The BMC (MT7620A, 580 MHz MIPS) forwards between the blades and the LAN.
Its CPU is the limit, not PCIe.

| BMC firmware / driver | LAN -> blade | BMC -> blade | blade -> BMC |
| --- | --- | --- | --- |
| OpenWrt 23.05, v0.3.0 (BMC writes) | ~92 Mbit/s | ~130 Mbit/s | ~320 Mbit/s |
| OpenWrt 23.05, v0.4.0 (gateway pull) | 193-198 Mbit/s | 189-204 Mbit/s | ~320 Mbit/s |
| OpenWrt 23.05, v0.5.0 (zero-copy) | 275-281 Mbit/s | ~275 Mbit/s | ~320 Mbit/s |

On OpenWrt 25.12.6 (Linux 6.12, ClusterBox firmware v0.4.0) blade <-> LAN
is ~220-245 Mbit/s each way, 10-20 % below 23.05.

## Earlier releases

### v0.3.0 (2026-10-12)

Four Blade 3 on Talos v1.14.2, BMC with `omi-rc` (push mode).

| Test | Result |
| --- | --- |
| Every ordered pair, 1 TCP stream | 4.3-5.1 Gbit/s, 0 retransmits |
| One pair, 4 TCP streams | 8.1 Gbit/s |
| One pair bidirectional, 4 streams each way | 6.9 + 7.0 Gbit/s |
| Four senders at once (ring), 4 streams each | 25.0 Gbit/s total |
| Cluster Box boot | all blades active 1.6 s after `omi-rc` loads |
| Blade reset (`nodectl` with release) | active again 14.5 s after the reset |

### v0.2.0-rc.1 (2026-10-08)

Four Blade 3 on Talos v1.14.2 (6.18.54).

| Test | Result |
| --- | --- |
| Every ordered pair, 4 TCP streams | 7.9-8.4 Gbit/s (4.3-4.7 when the flows hashed onto one or two queues) |
| Four senders at once (ring), 4 streams each | 22.2 Gbit/s total (rc.2: 11.7) |
| One TCP stream | 4.1-4.8 Gbit/s |
| CPU under load | `omi-ctl` ~1 %, TX thread ~13-18 % per channel (rc.2: ~6 % and ~92 %) |
| Errors / drops | none |

### v0.1.0-rc.2 (2026-10-06)

Two blades on Debian 12 (Mixtile vendor kernel 6.1.99), two on Talos
v1.14.2, plus the BMC: five members on one `omi0` segment.

| Test | Result |
| --- | --- |
| Debian <-> Debian TCP, 1 stream | 8.0-8.3 Gbit/s each way, 0 retransmits |
| Debian <-> Debian bidirectional | ~7.9 + 7.9 Gbit/s (10 min soak 7.96 + 7.96) |
| Debian -> Talos / Talos -> Debian | 6.4-6.6 / 7.4-7.5 Gbit/s |
| Talos <-> Talos | 6.48 / 6.73 Gbit/s |
| SHA-256 integrity | 1 GiB and 512 MiB transfers in all tested directions identical |
| Five-member matrix | ping + jumbo between all members, IPv6 `ff02::1` |

Recovery tests (blade reset, BMC reboot, module reload) are listed per
release in [../CHANGELOG.md](../CHANGELOG.md).
