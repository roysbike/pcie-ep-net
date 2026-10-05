# U-Boot: do not let vendor fixup steal the EP controller

## What vendor / mixtile-talos U-Boot does

`board_fdt_fixup()` → `fdt_fixup_pcie()` reads Cluster Box sense
pins labelled `PWRDIS` and `IFDET`. When the board is in the box
(`status == 1`, printed as `4 EP`) it:

1. Sets `pcie30phy` `rockchip,pcie30-phymode` to aggregation (x4).
2. Looks up **`/pcie@fe150000`** (the RC node, not `pcie-ep@`).
3. Overwrites `compatible` with `mixtile,miop-ep-rk3588`.
4. Deletes `reset-gpios`.
5. Sets `status = okay`.

Source in this workspace:
`mixtile-talos/patch/uboot/002-add-mixtile-blade3-board.patch`.

`mixtile,miop-ep-rk3588` has **no** in-tree Linux driver. Official
Mixtile images bind `pcie-ep-rk35.ko` (proprietary).

## What we need instead

Linux 6.12+ binds `rockchip,rk3588-pcie-ep` on
`/pcie-ep@fe150000` (`pcie3x4_ep`). The RC node `/pcie@fe150000`
must stay `disabled`.

## Preferred fix (small)

In `fdt_fixup_pcie()`, case “4 EP”:

```c
/* PHY stays aggregated — already done. */

pcie3x4_node = fdt_path_offset(blob, "/pcie@fe150000");
if (pcie3x4_node >= 0)
	fdt_setprop_string(blob, pcie3x4_node, "status", "disabled");

ep_node = fdt_path_offset(blob, "/pcie-ep@fe150000");
if (ep_node >= 0) {
	fdt_setprop_string(blob, ep_node, "compatible",
			   "rockchip,rk3588-pcie-ep");
	fdt_setprop_string(blob, ep_node, "status", "okay");
	/* SRNS: Cluster Box and Blade do not share refclk */
	/* Prefer to set rockchip,rx-common-refclk-mode on the PHY
	 * node from the kernel DTB; U-Boot does not have to. */
}
```

Do **not** write `mixtile,miop-ep-rk3588`.

## Alternative

Use a mainline U-Boot that never calls this fixup, and ship the
Phase 1 overlay in the kernel DTB. The PHY still defaults to
lane aggregation, which is what Cluster Box wants.

## How to see which path you are on

UART at 1500000 8n1 during power-on:

```
PWRDIS: n, IFDET: n
4 EP
```

Then in Linux:

```
tr '\0' ' ' < /proc/device-tree/pcie@fe150000/compatible
tr '\0' ' ' < /proc/device-tree/pcie-ep@fe150000/compatible
```

Phase 1 needs the second node to be `rockchip,rk3588-pcie-ep`
and the first node disabled or absent.
