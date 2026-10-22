# Porting openmiop to Talos 1.14.2 (Linux 6.18.54-talos, arm64)

The Debian blades (vendor 6.1.99) run openmiop; the Talos blades did
not have it. This is what the port needed, measured against the running
systems on 2026-10-06. Build and deployment live in
`mixtile-talos` (`artifacts/talos-kernel`, `artifacts/openmiop`,
`build.sh`, `docs/openmiop.md` there).

## Components

| Component | Debian 6.1.99 | Talos 6.18.54 | Category |
| --- | --- | --- | --- |
| `openmiop-ep.ko` (platform driver, programs DBI/iATU/eDMA itself) | out of tree | out of tree, built per kernel | implemented by pcie-ep-net |
| RK3588 PCIe3 PHY driver | built in (vendor) | built in (`PHY_ROCKCHIP_SNPS_PCIE3=y`) | already present |
| PHY bifurcation (two x2) | vendor DT says aggregation; driver writes the GRF | `data-lanes = <1 1 2 2>` in DT | DT change |
| RX refclk mode off (separate refclk) | lane CON1 = 0x60 (vendor) | `rockchip,rx-common-refclk-mode = <0 0 0 0>` | DT change |
| Endpoint DT node | U-Boot rewrites `/pcie@fe150000` to `mixtile,miop-ep-rk3588` | upstream `pcie-ep@fe150000` with compatible `openmiop,rk3588-pcie-ep`, host node disabled | DT change |
| NVMe on pcie3x2 with PERST GPIO4_B6 | vendor DT | moved from pcie3x4 | DT change |
| PCIe SMMU (MMU-600) | disabled | was enabled; disabled | DT change |
| `CONFIG_PCI_ENDPOINT`, `PCIE_ROCKCHIP_DW_EP`, `DW_EDMA` | not set / not used | `PCI_ENDPOINT=y`, others not set; not used | not needed |
| Reserved memory `miop_dma` | present, optional | absent, not needed | not needed |
| Firmware, configfs, userspace on the blade | none | none | — |
| Module signature | not enforced | `module.sig_enforce=1`, throw-away build key | image drops the argument |
| Network config | `/etc/openmiop.addr` + service | `LinkConfig omi0` in the machine config | Talos machine config |

## Kernel API differences 6.1 → 6.18

The driver source builds unchanged for both:

* `platform_driver.remove` returns `void` since 6.11
  (`LINUX_VERSION_CODE` switch, the only compatibility code).
* Register resources: the vendor node names them `pcie-dbi`/`pcie-apb`
  (4 MiB DBI with iATU at +3 MiB); the upstream node has `dbi`, `dbi2`,
  `apb`, `addr_space` and `atu`. The driver accepts both and takes the
  outbound window base from `addr_space` when present. The eDMA block is
  at iATU + 0x80000 in both layouts.

Compile check: `drivers/openmiop/openmiop-ep.c` builds with clang 22.1.8
against the 6.18.54-talos tree without warnings.

## Module build

Talos publishes the kernel image but not its build tree, and signs its
modules with a key generated during the build and then discarded
("Build time throw-away kernel key"). So:

1. `talos-kernel-build` rebuilds the tree exactly: linux-6.18.54, the
   16 siderolabs/pkgs patches at 6c312e4, `config-arm64` (byte-identical
   to `/proc/config.gz` on the nodes), the siderolabs LLVM image
   (clang 22.1.8), `make vmlinux modules_prepare`.
2. `openmiop` builds the module against it. Required:
   `vermagic=6.18.54-talos SMP mod_unload modversions aarch64` and
   symbol CRCs equal to the running kernel's.
3. The CRCs are checked against the 7295 symbol versions recorded in
   the 418 modules of the official kernel image
   (`modprobe --dump-modversions`): every symbol openmiop imports must
   appear there with the same CRC.
4. The image passes `--extra-kernel-arg -module.sig_enforce` (Talos
   supports deleting this default argument). The module is unsigned and
   taints the kernel (`E`); nothing else is weakened.

## Device tree

See `mixtile-talos/artifacts/dtb/blade3/rk3588-mixtile-blade3.dts`
(commit "talos: enable the RK3588 PCIe endpoint for openmiop") for the
reasoning per node. Effects on a Talos node:

* `fe150000` becomes the openmiop endpoint (x2, Gen3) instead of a host.
* The NVMe moves from PCI domain 0000 (pcie3x4) to 0001 (pcie3x2) and
  should train 8 GT/s x2 instead of 2.5 GT/s x2. Disks are found by
  serial/WWID, so ZFS imports are unaffected; anything that pinned a
  PCI path would change.
* PCIe devices DMA without the SMMU.

## Rollback

* The kernel and modules live in the A/B boot slots; the overlay
  installer also writes the DTB and U-Boot to places both slots use.
  `talosctl rollback` therefore returns to the previous Talos image but
  keeps the new DTB. That still boots (same kernel; without the module
  the endpoint node is simply not bound).
* Full revert: `talosctl upgrade --image <previous installer>`
  (on .201: `ghcr.io/roysbike/sbc-mixtile-blade3:installer-v1.14.2-v0.3.2`)
  and remove the `openmiop_ep` module and `omi0` link from the machine
  config.
* If a node does not boot: the Cluster Box BMC has its serial console
  (`nodectl console -n <slot>`), where the GRUB menu offers the
  previous slot.
