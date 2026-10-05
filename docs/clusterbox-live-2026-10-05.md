# Cluster Box live snapshot — 2026-10-05

Collected over SSH as `mixtile@192.168.70.6` (password auth).
Kernel: `Linux ClusterBox 5.15.150 #0 Sun Oct 4 19:01:58 2026 mips GNU/Linux`.

## Topology

```
-[0000:00]---00.0-[01-06]----00.0-[02-06]--+-00.0-[03]--
                                           +-04.0-[04]--
                                           +-08.0-[05]--
                                           \-0c.0-[06]--
```

| BDF | ID | Role |
| --- | --- | --- |
| 00:00.0 | `1814:0801` | MT7620A / Ralink root port |
| 01:00.0 | `1b21:2824` | ASM2824 upstream |
| 02:00.0 | `1b21:2824` | ASM2824 downstream → bus 03 |
| 02:04.0 | `1b21:2824` | ASM2824 downstream → bus 04 |
| 02:08.0 | `1b21:2824` | ASM2824 downstream → bus 05 |
| 02:0c.0 | `1b21:2824` | ASM2824 downstream → bus 06 |

`nodectl` slot → bus / port map (from
`mixtile-clusterbox-mt7620a-openwrt/package/nodectl/src/nodectl.c`):

| Slot | PCI bus | Downstream BDF |
| --- | --- | --- |
| 1 | 6 | 02:0c.0 |
| 2 | 5 | 02:08.0 |
| 3 | 3 | 02:00.0 |
| 4 | 4 | 02:04.0 |

## Link

- Root port `LnkCap` / `LnkSta`: **2.5 GT/s, x1**.
- ASM2824 upstream `LnkCap`: 8 GT/s x8; `LnkSta`: **2.5 GT/s x1 (downgraded)**.
- Downstream `LnkCap`: 8 GT/s x4; `LnkSta` today: 2.5 GT/s x1 (no EP training).

## Resources

`/proc/iomem`:

```
20000000-2fffffff : pcie@10140000
  20000000-2000ffff : 0000:00:00.0
```

Matches `mt7620a.dtsi` `ranges`:

```
0x02000000 0 0x00000000 0x20000000 0 0x10000000 /* pci memory */
```

All ASM2824 bridges currently report `Memory behind bridge: [disabled]`.
That is expected with empty downstream buses; Linux will program windows
only after an Endpoint presents BARs.

## Capabilities observed on ASM2824

Present: PM, MSI, PCIe, AER, Power Budgeting, LTR, **Multicast**
(MaxGroups 64, currently disabled), Secondary PCIe, Vendor-Specific.

**Not present: ACS.** `lspci -nnvv` lists no Access Control Services
capability on 01:00.0 or 02:00.0/04.0/08.0/0c.0.

`AtomicOpsCap: Routing-` on the upstream port.

## MIOP host driver

```
module:   /lib/modules/5.15.150/miop.ko
license:  MIXTILE
name:     miop
vermagic: 5.15.150 mod_unload MIPS32_R2 32BIT
```

`dmesg`: `miop: module license 'MIXTILE' taints kernel.`

`pci0` netdev is **not** created. `ls /sys/class/net` is only
`lo`, `eth0`, `eth0.1`, `eth0.2`, `br-lan`.

`/usr/share/hwdata/pci.ids`:

```
4586  Mixtile Limited
        b6f2  Blade 3
```

## Software on the blades

Blades currently run the project's custom Talos image. That image
intentionally does **not** include MIOP EP support, which is why
buses 03–06 are empty. This matches
`mixtile-talos/README.md` ("MIOP/Cluster Box endpoint support is
intentionally not included").
