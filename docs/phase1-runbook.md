# Phase 1 runbook — enumerate one Blade 3 as a PCIe Endpoint

Do this on **one** slot after Ubuntu/Debian is installed on that
blade. The other three can stay on Talos.

Do **not** start the Ethernet driver at the end of this document.
The exit criterion is `lspci` on the Cluster Box.

## 0. What you need

On the Blade:

- Linux **6.12 or newer** (6.18 preferred). Stock Mixtile vendor
  6.1 + `pcie-ep-rk35.ko` is the proprietary path and is not this
  work.
- Kernel built with [../phase1/config/linux-pcie-ep.config](../phase1/config/linux-pcie-ep.config).
- DTB that applies
  [../phase1/dts/rk3588-mixtile-blade3-pcie-ep.dtso](../phase1/dts/rk3588-mixtile-blade3-pcie-ep.dtso).
- U-Boot that does **not** rewrite `/pcie@fe150000` to
  `mixtile,miop-ep-rk3588`. See [../phase1/uboot/README.md](../phase1/uboot/README.md).

On the Cluster Box (already true today):

- OpenWrt with `miop.ko` loaded is fine. We use PCI IDs that it
  will **not** bind (`1d87:3588`).
- `lspci`, `nodectl`, SSH as `mixtile`.

## 1. Confirm the slot is empty before the experiment

From your laptop:

```bash
ssh mixtile@192.168.70.6 'lspci -tvnn; echo; lspci -nn'
```

Expected (Talos blades, no EP):

```
-[0000:00]---00.0-[01-06]----00.0-[02-06]--+-00.0-[03]--
                                           +-04.0-[04]--
                                           +-08.0-[05]--
                                           \-0c.0-[06]--
```

and **no** `03:00.0` / `04:00.0` / `05:00.0` / `06:00.0`.

Slot → bus (from `nodectl.c`):

| Physical slot | Bus | Downstream port |
| --- | --- | --- |
| 1 | 06 | 02:0c.0 |
| 2 | 05 | 02:08.0 |
| 3 | 03 | 02:00.0 |
| 4 | 04 | 02:04.0 |

## 2. Boot the Ubuntu blade

UART (1500000 8n1) should show U-Boot printing `PWRDIS` / `IFDET`
and, in the Cluster Box, `4 EP`.

If U-Boot prints that it set `compatible = "mixtile,miop-ep-rk3588"`,
**stop** and fix U-Boot first. Linux will not bind the upstream EPC.

On the blade, after login:

```bash
uname -r                          # 6.12+ 
zcat /proc/config.gz | grep -E 'PCIE_ROCKCHIP_DW_EP|PCI_ENDPOINT|PCI_EPF_TEST'
ls /sys/class/pci_epc
ls /sys/firmware/devicetree/base | grep -i pcie
```

`/sys/class/pci_epc/` must contain one controller
(`fe150000.pcie-ep` or similar). If it is empty, the EPC driver did
not probe — check `dmesg | grep -i pcie` and the live DT:

```bash
find /sys/firmware/devicetree/base -name compatible | while read f; do
  tr '\0' ' ' < "$f" | grep -q pcie && echo "$f: $(tr '\0' ' ' < "$f")"
done
```

You want `rockchip,rk3588-pcie-ep` with `status = okay`, and
`pcie@fe150000` either missing or `disabled`.

## 3. Bind `pci_epf_test`

As root on the blade:

```bash
./phase1/scripts/ep-start-pci-epf-test.sh
```

or the equivalent commands in that script. Then:

```bash
dmesg | tail -50
```

Look for LTSSM / `link up` from `rockchip-dw-pcie`.

## 4. Rescan on the Cluster Box

```bash
ssh mixtile@192.168.70.6
sudo pci-rescan   # or: echo 1 | sudo tee /sys/bus/pci/rescan
sudo lspci -tvnn
sudo lspci -nnvv -s 03:00.0   # adjust bus to the slot
```

[../phase1/scripts/rc-watch-enum.sh](../phase1/scripts/rc-watch-enum.sh)
does the dump.

Success looks like:

```
02:00.0-[03]----00.0  [1d87:3588]
```

and `lspci -nnvv -s 03:00.0` showing:

- Type 0 Endpoint
- vendor `1d87`, device `3588`
- BAR0 (and usually BAR1/2/3/5) assigned in `0x20000000–0x2fffffff`
- MSI capability present
- **no** BAR4 (reserved on RK3588)

Downstream port `LnkSta` should move from `2.5 GT/s, x1` toward
`8 GT/s, Width x4`. If it stays Gen1 x1, the PHY/SRIS settings are
wrong — that is still a Phase 1 bug, not a reason to write a
netdev.

`nodectl list` will stay empty: it looks for MIOP devices, not
`1d87:3588`. That is expected.

## 5. Optional BAR smoke test (still Phase 1/2)

If `pcimem` / `busybox devmem` can map BAR0 on the Cluster Box:

```bash
# on RC, after noting BAR0 from lspci
sudo pcimem /sys/bus/pci/devices/0000:03:00.0/resource0 0 w 0x12345678
```

On the EP, `pci_epf_test` owns that BAR; a cleaner check is to
install `pci_endpoint_test` on a custom OpenWrt later. Do not
fight `miop.ko` — it must not bind this ID.

## 6. Failure checklist

| Symptom | Likely cause |
| --- | --- |
| No `/sys/class/pci_epc` | `pcie3x4_ep` disabled, or U-Boot stole the node |
| `mixtile,miop-ep-rk3588` in live DT | Vendor U-Boot fixup; patch U-Boot |
| EPC probes, no link | SRNS property missing; PERST still driven; PHY not aggregated |
| Link up on EP, nothing on RC | RC needs `rescan`; or EP `start` not written |
| EP appears, BAR is `???` / size 0 | BAR4 used, or 64-bit BAR on 32-bit RC |
| EP appears, `miop` binds | Wrong IDs (`4586:b6f2`); rebuild with `1d87:3588` |
| Downstream stays Gen1 x1 after EP | SRIS / `app_sris_mode` experiment (see feasibility §10.9) |
| MT7620A hangs on rescan | Too-large BAR; drop to 64 KiB / 1 MiB |

## 7. Stop

When the Type-0 function is visible, come back. Phase 2 is BAR
read/write, MSI, and a 1 MiB memcpy benchmark — still no
`net_device`.
