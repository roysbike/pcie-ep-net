# openmiop feasibility report

Date: 2026-10-05
Target: Linux 6.18.x (Talos 1.14 later), Ubuntu/Debian ARM64 first
Hardware: Mixtile Cluster Box (MT7620A + ASM2824) + 4 × Blade 3 (RK3588)

Legend used below:

- **Fact** — observed on this Cluster Box, or present in a cited
  upstream file / public document.
- **Assumption** — reasonable, but not proven on this hardware.
- **Unknown** — requires a Blade running Endpoint firmware.

Proprietary MIOP sources were not copied. Externally observable
behaviour (PCI IDs, module names, dmesg strings, forum posts, U-Boot
fixups already in `mixtile-talos`) is used only as a constraint list.

**Live update 2026-10-05:** Debian Blade `192.168.70.173` (slot 4)
with vendor MIOP is up. After `pci rescan` the Cluster Box sees
`04:00.0 [4586:b6f2]`, BAR0=32 MiB @ `0x22000000`, BAR4=1 MiB,
link **8 GT/s x2**, `pci0` UP on both sides, ping RC↔EP 2.6–5.4 ms.
Details: [live-miop-2026-10-05.md](live-miop-2026-10-05.md).

---

## Executive summary

Phase 1 is feasible on upstream Linux 6.12+ without a new kernel
driver. The RK3588 PCIe3 x4 dual-mode controller already has an
in-tree Endpoint driver (`PCIE_ROCKCHIP_DW_EP`), a SoC DT node
(`pcie3x4_ep`), and a standard test function (`pci_epf_test`).

The hard problems are not “does RK3588 EP exist?”. They are:

1. **Boot firmware** — vendor U-Boot rewrites `/pcie@fe150000` to
   `compatible = "mixtile,miop-ep-rk3588"` when the board is in the
   Cluster Box. That fights the upstream EPC driver.
2. **Separate refclk** — Cluster Box and Blade 3 do not share a
   reference clock. ASM2824 advertises SRIS; the RK3588 PHY must run
   with `rockchip,rx-common-refclk-mode = <0 0 0 0>`. Vendor kernels
   also poke an extra SRIS bit that mainline does not.
3. **32-bit RC aperture** — MT7620A only decodes **256 MiB** of PCI
   MMIO at `0x20000000–0x2fffffff`. All four Endpoint BARs must fit.
4. **P2P is plausible but unproven here** — ASM2824 does not advertise
   ACS, Mixtile claims ~19 Gbit/s blade-to-blade, and a PCIe switch
   normally routes peer TLPs by address. Linux `pci_p2pdma` will
   **not** be the data-path API on the blades. Direct proof still
   needs two live Endpoints.

Do not start the Ethernet driver until one Ubuntu Blade appears as
`0x:00.0` behind an ASM2824 downstream port and a BAR write is
visible on both sides.

---

## 1. Which RK3588 PCIe controller should operate in EP mode?

**Fact.** Blade 3’s Cluster Box connector is the RK3588 **PCIe3 x4**
controller at `fe150000` (`pcie3x4` / `pcie3x4_ep`).

Evidence:

- SoC DT, Linux 6.18
  `arch/arm64/boot/dts/rockchip/rk3588-extra.dtsi`:
  `pcie3x4: pcie@fe150000` (RC) and
  `pcie3x4_ep: pcie-ep@fe150000` (EP), both
  `num-lanes = <4>`, `max-link-speed = <3>`, `phys = <&pcie30phy>`.
- Vendor / mixtile-talos U-Boot fixup writes
  `compatible = "mixtile,miop-ep-rk3588"` onto
  `/pcie@fe150000` when IFDET/PWRDIS say “in Cluster Box”
  (`mixtile-talos/patch/uboot/002-add-mixtile-blade3-board.patch`,
  `fdt_fixup_pcie()`, case `1`: “4 EP”).
- Public MIOP logs:
  `miop-ep fe150000.pcie: PCIe Link up, LTSSM is 0x230011`
  (Mixtile forum, topic 810).
- The other RK3588 PCIe controllers on Blade 3 are local:
  `pcie2x1l0` → onboard ASM1182e + two RTL8125,
  `pcie2x1l1` → miniPCIe. They must stay Root Complex.

**Do not** put `pcie3x2` (`fe160000`) in EP mode for Cluster Box.
That controller is the 2-lane sibling used when the PHY is
bifurcated; Cluster Box wants PHY aggregation (x4).

---

## 2. Does Linux 6.18 already contain the RK3588 EPC driver?

**Fact. Yes.** Merged with the Niklas Cassel series (PCI/rockchip EP)
and present in v6.12 already; 6.18 still has it.

| Piece | Path (v6.18) |
| --- | --- |
| Glue driver | `drivers/pci/controller/dwc/pcie-dw-rockchip.c` |
| Kconfig | `CONFIG_PCIE_ROCKCHIP_DW_EP` |
| Compatible | `rockchip,rk3588-pcie-ep` |
| DT node | `pcie3x4_ep` in `rk3588-extra.dtsi` |
| Binding | `Documentation/devicetree/bindings/pci/rockchip-dw-pcie-ep.yaml` |
| Host test ID | `drivers/misc/pci_endpoint_test.c` (`1d87:3588`) |
| Example overlay | `arch/arm64/boot/dts/rockchip/rk3588-rock-5b-pcie-ep.dtso` |

`rockchip_pcie_configure_ep()` sets
`PCIE_CLIENT_SET_MODE(PCIE_CLIENT_MODE_EP)`, calls
`dw_pcie_ep_init()` / `dw_pcie_ep_init_registers()`, and
`pci_epc_init_notify()`.

The controller is **not** enabled on Blade 3 by default. Board DT
must disable `pcie3x4` and enable `pcie3x4_ep`.

---

## 3. Required kernel CONFIG options

Phase 1, Blade (EP), Linux 6.12+ / 6.18:

```
CONFIG_PCI=y
CONFIG_PCI_MSI=y
CONFIG_PCI_ENDPOINT=y
CONFIG_PCI_ENDPOINT_CONFIGFS=y
CONFIG_PCIE_ROCKCHIP_DW_EP=y
CONFIG_PCI_EPF_TEST=m
CONFIG_PHY_ROCKCHIP_NANENG_COMBO_PHY=y   # pcie2x1 local NICs, not EP
CONFIG_PHY_ROCKCHIP_SNPS_PCIE3=y         # pcie30phy
CONFIG_CONFIGFS_FS=y
```

Useful, not mandatory for first link-up:

```
CONFIG_PCI_ENDPOINT_MSI_DOORBELL=y
CONFIG_PCIE_DW_DEBUGFS=y
CONFIG_PCI_ENDPOINT_TEST=n   # host-side misc driver; not on the EP
```

Cluster Box (RC), current OpenWrt 5.15.150, already has:

```
CONFIG_PCI=y
# plus out-of-tree miop.ko
```

`CONFIG_PCI_ENDPOINT_TEST` is **not** on the stock Cluster Box image.
Phase 1 success is `lspci` seeing the Endpoint, not running
`pci_endpoint_test`. Adding that module to OpenWrt is Phase 2.

---

## 4. Exact DTS changes

Minimum overlay, modelled on
`arch/arm64/boot/dts/rockchip/rk3588-rock-5b-pcie-ep.dtso`:

```dts
&pcie30phy {
	rockchip,rx-common-refclk-mode = <0 0 0 0>;
};

&pcie3x4 {
	status = "disabled";
};

&pcie3x4_ep {
	vpcie3v3-supply = <&vcc3v3_pcie30>;
	/* no reset-gpios — the EP must not pulse PERST toward the switch */
	status = "okay";
};

&mmu600_pcie {
	status = "disabled";
};
```

Why each line:

| Change | Why | Source |
| --- | --- | --- |
| Disable `pcie3x4` | Same MMIO as `pcie3x4_ep`; RC and EP are mutually exclusive | Cassel series commit message; `rk3588-rock-5b-pcie-ep.dtso` |
| Enable `pcie3x4_ep` | Binds `rockchip,rk3588-pcie-ep` | `pcie-dw-rockchip.c` `of_device_id` |
| `vpcie3v3-supply` | Blade 3 3.3 V PCIe rail, GPIO1_B2 | `rk3588-mixtile-blade3.dts` |
| No `reset-gpios` | Vendor U-Boot deletes `reset-gpios` in EP mode | `fdt_fixup_pcie()` case 1 |
| `rx-common-refclk-mode = <0 0 0 0>` | SRNS/SRIS; common-refclk mode makes LTSSM bounce L0↔Recovery | `phy-rockchip-snps-pcie3.c`, `rk3588-rock-5b-pcie-ep.dtso` |
| Disable `mmu600_pcie` | Official Rock 5B EP overlay does this; inbound ATU vs SMMU is a known footgun | same dtso |

Board DTS must **keep** `&pcie2x1l0` / `&pcie2x1l1` as RC so the
onboard 2.5 GbE NICs still work.

PHY aggregation (x4) is the reset default of
`rockchip_p3phy_rk3588_init()` (`RK3588_LANE_AGGREGATION`). Do not
set a bifurcated `data-lanes` property for Cluster Box.

See [../phase1/dts/rk3588-mixtile-blade3-pcie-ep.dtso](../phase1/dts/rk3588-mixtile-blade3-pcie-ep.dtso).

---

## 5. Can `pci_epf_test` be used for initial enumeration?

**Fact. Yes, and it should be.**

`Documentation/PCI/endpoint/pci-test-howto.rst` is the recipe.
On the Blade:

```
mount -t configfs none /sys/kernel/config
mkdir /sys/kernel/config/pci_ep/functions/pci_epf_test/func1
echo 0x1d87 > .../vendorid
echo 0x3588 > .../deviceid
echo 8     > .../msi_interrupts
ln -s functions/pci_epf_test/func1 controllers/<epc>/
echo 1 > controllers/<epc>/start
```

`0x1d87:0x3588` is the ID pair `pci_endpoint_test` already matches
(`PCI_VENDOR_ID_ROCKCHIP` / `PCI_DEVICE_ID_ROCKCHIP_RK3588` in
`drivers/misc/pci_endpoint_test.c`).

**Do not** use `0x4586:0xb6f2` in Phase 1. That is the MIOP alias
(`modinfo` on Blade images; `pci.ids` on this Cluster Box). The
loaded `miop.ko` would bind and we could not do clean BAR tests.

Phase 1 pass/fail on the Cluster Box:

```
02:00.0-[03]----00.0  [1d87:3588]
# or 02:04.0 / 02:08.0 / 02:0c.0 depending on slot
```

and `lspci -nnvv -s 0N:00.0` showing a Type 0 Endpoint.

---

## 6. What BAR sizes are practical?

**Fact, controller side (RK3588 EP):**

From `rockchip_pcie_epc_features_rk3588` in `pcie-dw-rockchip.c`:

| BAR | Type | Notes |
| --- | --- | --- |
| 0–3, 5 | `BAR_RESIZABLE` | 64 KiB alignment (`align = SZ_64K`) |
| 4 | `BAR_RESERVED` | Exposes iATU / DMA port logic. Host writes corrupt all BARs. |

`pci_epf_test` defaults (howto, current mainline):

```
bar0–bar3: 128 KiB
bar5:      1 MiB
```

Those defaults are the right Phase 1 / Phase 2 sizes.

**Fact, RC side (MT7620A):**

`target/linux/ramips/dts/mt7620a.dtsi` and live `/proc/iomem`:

```
20000000-2fffffff : pcie@10140000   /* 256 MiB, 32-bit */
```

All four Endpoint BARs **plus** any switch windows must fit in that
aperture. 256 MiB / 4 blades = 64 MiB/blade if we ever map packet
buffers as BARs.

Practical policy:

| Phase | BAR0 | BAR2 | BAR5 | BAR4 |
| --- | --- | --- | --- | --- |
| 1–2 (`pci_epf_test`) | 128 KiB | 128 KiB | 1 MiB | disabled |
| 3–5 (netdev) | 64 KiB control | 4–16 MiB rings | optional | never |

Use **32-bit** BARs. The RC is MIPS32 and the live root port only
shows a 32-bit non-prefetchable region.

256 MiB / 64 MiB / 16 MiB bulk copies in Phase 2 must be **chunked**
through the small BAR, or done with EP-side DMA into a peer BAR, not
by asking the RC to map a 256 MiB BAR.

Vendor MIOP reserved `miop_dma@0x0e000000`, 32 MiB
(`shared-dma-pool`) on the Blade. That is EP-local DRAM, not a BAR
size. We can do the same later with a `reserved-memory` node; it is
not required for `pci_epf_test`.

---

## 7. Is DMA supported?

**Fact, hardware / DT:** `pcie3x4_ep` lists `dma0`–`dma3` interrupts
(`GIC_SPI 271, 272, 269, 270`). That is the DesignWare eDMA block.

**Fact, software:**

- `rockchip_pcie_configure_ep()` calls
  `dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64))`.
- `pci_epf_test` can use a DMA engine (`pci_epf_test_init_dma_chan()`
  in `drivers/pci/endpoint/functions/pci-epf-test.c`) and falls back
  to memcpy.
- Cassel’s public test logs show both `DMA: NO` and `DMA: YES` paths
  on RK3588 EP.

**Assumption:** eDMA works for EP inbound/outbound once the link is
up, same as Rock 5B. Not verified on Blade 3.

**Unknown:** whether Cluster Box SRIS / 32-bit RC addressing restricts
eDMA source/destination. Phase 2 must measure memcpy vs DMA.

Do not depend on `CONFIG_SKB_DMA_FRAG` (vendor MIOP patch to
`sk_buff`). That is out of tree and not needed for correctness.

---

## 8. Are MSI / MSI-X supported?

**Fact, EPC:**

```c
static const struct pci_epc_features rockchip_pcie_epc_features_rk3588 = {
	.linkup_notifier = true,
	.msi_capable = true,
	.msix_capable = true,
	...
};
```

`rockchip_pcie_raise_irq()` implements INTX, MSI and MSI-X via
`dw_pcie_ep_raise_*`.

**Fact, RC:** MT7620A root port advertises MSI (`64bit+`) but MSI is
**disabled** today (`Enable-`). The Cluster Box has 256 MiB RAM and a
simple PIC; MSI from four EPs may or may not be usable.

**Assumption:** MSI from EP → RC works after the host driver enables
it (MIOP clearly does *something* for notifications). MSI-X to a
MIPS32 RC is more questionable.

**Unknown:** can one EP send MSI to a sibling EP? That needs the
sibling’s MSI address programmed into an outbound iATU window, and
the switch to route that address. Not assumed.

Phase 2 plan: test MSI EP→RC first; use a BAR doorbell as fallback.

INTX: one Cassel revision log said “EP cannot raise INTX IRQs” on
early bring-up. Treat INTX as best-effort only.

---

## 9. Can sibling RK3588 endpoints behind ASM2824 perform PCIe P2P?

**Not verified on this box.** Current blades are Talos and do not
enumerate, so there is no pair of BARs to test.

What we *can* say:

| Claim | Class | Evidence |
| --- | --- | --- |
| ASM2824 is a packet switch, not an Ethernet switch | Fact | ASMedia product page; `lspci` class `0604` |
| Downstream ports are independent Gen3 x4 links | Fact | `LnkCap` Width x4, Speed 8 GT/s on 02:00/04/08/0c |
| Upstream to MT7620A is Gen1 x1 | Fact | live `LnkSta` 2.5 GT/s x1 on 00:00.0 and 01:00.0 |
| Switch does not advertise ACS | Fact | `lspci -nnvv` on this box; no ACS cap |
| PCIe spec routes peer memory TLPs inside a switch when ACS redirect is off / absent | Fact | PCIe Base Spec; `Documentation/driver-api/pci/p2pdma.rst` |
| Mixtile claims ~19 Gbit/s iperf3 and ~15.5 Gbit/s bidirectional “P2P” on Gen3 x4 | Fact (vendor claim) | Mixtile community topic 897, 2026-05 driver note |
| That throughput is impossible if payload hairpins through MT7620A Gen1 x1 (~2 Gbit/s raw) | Fact | link math |
| Therefore **MIOP’s data path almost certainly does EP↔EP through the switch** | Assumption (strong) | above two rows |
| **Our** stack can do the same | Unknown | needs two EPs + a write from EP1 outbound ATU into EP2 BAR |

So: P2P is the architecture we should design for, and hardware does
not show an ACS blocker. It is still a Phase 2/5 experiment, not a
Phase 1 deliverable.

---

## 10. What prevents or complicates P2P?

Ordered by likelihood:

1. **RC memory windows.** Today every ASM2824 bridge has
   `Memory behind bridge: [disabled]`. A switch can only forward a
   TLP to a downstream port if that port’s memory window covers the
   address. The MT7620A PCI core must assign non-overlapping 32-bit
   windows inside `0x20000000–0x2fffffff`. If assignment fails, P2P
   and RC MMIO both fail.

2. **256 MiB total aperture.** Four blades × large BARs will not fit.
   Packet rings must stay small or live in EP DRAM and be reached
   through a modest inbound BAR / ATU.

3. **EP outbound ATU (`addr_space`).** `pcie3x4_ep` has a 1 GiB
   `addr_space` at `0x9_00000000`. An EP programs iATU so that a CPU
   or eDMA write to that window becomes a TLP to a peer BAR. This is
   standard DW EP; it is the real P2P data path. It is *not*
   automatic — our later EPF must program it.

4. **IOMMU on the EP.** `mmu600_pcie` must stay off until inbound
   mappings are proven. The Rock 5B overlay disables it.

5. **ATS is broken on RK3588 EP.** `pcie-dw-rockchip.c` explicitly
   hides the ATS extended capability:
   “After the host has enabled ATS … RK3588 will never send a
   completion … IOTLB_INV_TIMEOUT”. Do not enable ATS.

6. **No ACS to disable.** Good (nothing redirects upstream) and bad
   (we cannot *see* a knob; if the switch silently drops peer TLPs
   we only learn it by test).

7. **No AtomicOps routing** on the ASM2824 upstream port. Irrelevant
   unless we invent atomics-based doorbells.

8. **Cache coherency.** EP DRAM behind a BAR is not automatically
   coherent with the peer’s CPU. Phase 2 must use
   `dma_alloc_coherent` / `dma_wmb` / explicit `wmb()` and measure.

9. **Vendor SRIS extra bits.** Vendor kernel writes PHY
   `sris_mode_en` (GRF 0x1004/1104/2004/2104 bit 6) and
   `app_sris_mode` in the PCIe client register. Mainline only
   clears `rxX_cmn_refclk_mode`. If LTSSM is unstable, that is the
   first one-line experiment — not a reason to fork the whole
   driver.

10. **U-Boot compatible rewrite.** If Linux binds
    `mixtile,miop-ep-rk3588` (no in-tree driver) instead of
    `rockchip,rk3588-pcie-ep`, the EPC never appears and P2P is
    academic.

---

## 11. Can Linux `pci_p2pdma` help?

**Mostly no, not in the way the name suggests.**

`drivers/pci/p2pdma.c` and
`Documentation/driver-api/pci/p2pdma.rst` are written for a **single
Linux that is the Root Complex**, mapping one device’s BAR as a DMA
target for another device on that same OS.

In this box:

- The only shared RC Linux is OpenWrt on MT7620A.
- Each Blade is its own OS and is an **Endpoint**, not an RC device
  with a `struct pci_dev` for the sibling.
- `pci_p2pdma_add_resource()` would run on the Cluster Box, on the
  Type-0 functions it enumerated. That can help a **host** driver
  DMA between blade BARs (hairpin through the RC IOMMU/CPU), which
  is exactly the path we do **not** want for payload.

What we *can* reuse from that code is the **routing analysis**:
same switch, ACS absent ⇒ `PCI_P2PDMA_MAP_BUS_ADDR` in the host
view. That is a hint, not an API we will call on the blades.

Blade-to-blade copies will use:

```
EP1 eDMA / CPU  →  outbound iATU  →  TLP  →  ASM2824  →  EP2 inbound BAR
```

plus a small control protocol so each EP learns the PCI bus address
of its peers (the RC can publish that; it assigned the BARs).

---

## 12. What role must the MT7620A RC retain?

Even if payload never touches it:

| Role | Why |
| --- | --- |
| Link training / PERST / slot power | Physical owner of the hierarchy |
| Enumeration | Assigns bus numbers and BAR addresses the switch uses to route |
| Memory window programming | Without this, P2P has no addresses |
| Optional control plane | Publish BAR map, MAC↔slot FDB, bring-up, `nodectl` |
| Optional slow path | DHCP-like setup, crash dump, “peer not ready” |
| **Not** payload switch | Gen1 x1, 256 MiB RAM, 32-bit MIPS |

`nodectl` already treats “device on bus 03/04/05/06” as “blade
present”. Official MIOP IDs show up as
`Network controller: Mixtile Limited Blade 3`. Our Phase 1 IDs will
show as `1d87:3588` (Rockchip test). `nodectl list` will stay empty
until we either teach it the new ID or we later adopt `4586:b6f2`
on purpose.

---

## 13. Simplest architecture for true L2 between four blades

After Phase 1–2 succeed, the smallest *correct* design is:

```
 each Blade
   pci_epf_openmiop  (new EPF, not written yet)
     BAR0  64 KiB   little-endian control + MAC + ring indexes
     BAR2  4–16 MiB TX/RX rings (or one BAR, two halves)
     MSI   doorbell to the RC (bring-up) and/or BAR doorbell
     outbound iATU window per peer BAR2

 Cluster Box
   enumerates 4 EPs
   writes a directory into each BAR0:
     magic, version, my_slot, peer[4].bar2_pci_addr, peer[4].mac
   does not copy frames

 datapath
   unicast   : look up MAC → slot → DMA into that peer BAR2
   unknown / broadcast / multicast : flood to the other 3 BAR2s
   ASM2824 Multicast cap (64 groups) is a later optimisation
```

This is an **EPF + net_device on each Blade**. No RC netdev is
required for blade-to-blade L2. A Cluster Box `pci0` (as MIOP
does) is optional and should stay a control/management interface
if we add it at all.

Why not `pci_epf_ntb` / `pci_epf_vntb`? Those model one NTB pair
(RC↔EP or virtio). Four siblings behind a switch are a different
shape; we would still invent a multi-peer FDB on top.

Why not `pci_epf_vhost` / virtio-net? Useful later for guests, not
for the host `pci0` we need for Cilium/Kube-OVN.

---

## 14. What needs new kernel code?

| Component | New? | When |
| --- | --- | --- |
| RK3588 EPC | No | — |
| PHY SRNS property | No | — |
| `pci_epf_test` / `pci_endpoint_test` | No | Phase 1–2 |
| Blade DT overlay | Yes (data, not C) | Phase 1 |
| U-Boot fixup so vendor bootloader does not steal `pcie@fe150000` | Yes (small) | Phase 1 |
| Optional PHY `sris_mode_en` / `app_sris_mode` | Maybe 10 lines | only if link flaps |
| `pci_epf_openmiop` (BARs, rings, iATU, MSI) | Yes | Phase 3 |
| `net_device` (`pci0`) on the EPF | Yes | Phase 4 |
| Tiny RC helper (BAR directory, maybe netdev) | Yes, can be userspace first | Phase 5 |
| OpenWrt `pci_endpoint_test` package | Packaging only | Phase 2 |
| Talos kernel fragment + DTB overlay / system extension | Packaging | Phase 7 |

No large out-of-tree fork of `pcie-dw-rockchip.c` should be needed.

---

## 15. What already exists upstream?

- PCI Endpoint Framework, configfs, `pci_epc_*`
- DesignWare EP core (`pcie-designware-ep.c`) including iATU
- Rockchip DW EP glue + RK3588 features (BAR4 reserved, ATS hidden)
- `pci_epf_test` / `pci_endpoint_test` / `tools/testing/selftests/pci_endpoint`
- Rock 5B EP + SRNS overlays (the template we copy)
- `phy-rockchip-snps-pcie3` `rockchip,rx-common-refclk-mode`
- `pci_p2pdma` (host-side analysis only)
- `pci_epf_ntb`, `pci_epf_vntb`, `pci_epf_mhi` (not our data path)

---

## Bootloader

Vendor and mixtile-talos U-Boot read two Cluster Box sense pins
(`PWRDIS`, `IFDET`) and, when the board is a 4-lane EP, do:

```c
fdt_setprop_string(blob, pcie3x4_node, "compatible", "mixtile,miop-ep-rk3588");
fdt_delprop(blob, pcie3x4_node, "reset-gpios");
fdt_setprop_string(blob, pcie3x4_node, "status", "okay");
```

Path used: `/pcie@fe150000` (the **RC** node), not
`/pcie-ep@fe150000`.

Consequences:

- A mainline DTB that still contains `pcie3x4` will have its RC
  node rewritten to a compatible **no in-tree driver binds**.
- If our overlay also enables `pcie3x4_ep`, two platform devices
  claim the same APB/DBI.
- Official Mixtile Ubuntu/Debian already ships `pcie-ep-rk35.ko`
  for that compatible; that is the proprietary path.

Phase 1 therefore requires one of:

1. Mainline / mixtile-talos U-Boot **without** that rewrite, plus
   our overlay; or
2. A patched `fdt_fixup_pcie()` that sets
   `rockchip,rk3588-pcie-ep` on `/pcie-ep@fe150000` and leaves
   `/pcie@fe150000` `disabled`; or
3. A kernel DTB that **omits** the `pcie@fe150000` node name so the
   lookup fails (fragile; do not do this).

See [../phase1/uboot/README.md](../phase1/uboot/README.md).

---

## Answers in one page

| # | Answer | Class |
| --- | --- | --- |
| 1 | `pcie3x4` / `pcie3x4_ep` @ `fe150000`, PHY aggregation x4 | Fact |
| 2 | Yes, `CONFIG_PCIE_ROCKCHIP_DW_EP` in 6.12+ / 6.18 | Fact |
| 3 | `PCI_ENDPOINT{,_CONFIGFS}`, `PCIE_ROCKCHIP_DW_EP`, `PCI_EPF_TEST`, SNPS PCIe3 PHY | Fact |
| 4 | Disable RC node, enable `pcie3x4_ep`, SRNS PHY, no PERST GPIO, disable SMMU | Fact |
| 5 | Yes; use `1d87:3588`, not `4586:b6f2` | Fact |
| 6 | 64 KiB align; BAR4 forbidden; start at 128 KiB / 1 MiB; stay inside 256 MiB RC window | Fact |
| 7 | eDMA wired and used by `pci_epf_test`; Blade 3 not measured | Fact + Unknown |
| 8 | MSI and MSI-X advertised by EPC; RC MSI unproven | Fact + Unknown |
| 9 | Likely yes (no ACS, vendor 19 Gbit/s claim); not demonstrated here | Assumption |
| 10 | Windows, 256 MiB cap, iATU setup, SMMU, ATS, SRIS extras, U-Boot | mixed |
| 11 | Not as the blade datapath API | Fact |
| 12 | Enumerate, assign BARs, optional control plane; never payload | Fact |
| 13 | Per-blade EPF netdev + peer BAR map published by RC | Design |
| 14 | EPF+netdev, tiny RC helper, U-Boot fixup; maybe 10-line SRIS | Design |
| 15 | EPC, PHY SRNS, `pci_epf_test`, DW iATU/eDMA | Fact |

---

## Phase 1 stop line

Stop and do not write `net_device` code until all of these are true
on **one** Ubuntu Blade in **one** slot:

1. Cluster Box `lspci` shows `NN:00.0 [1d87:3588]` behind 02:00/04/08/0c.
2. `LnkSta` on that downstream port is **8 GT/s, x4** (or we document
   why not).
3. BAR0 is assigned inside `0x20000000–0x2fffffff`.
4. RC `devmem`/`pcimem` write of `0x12345678` is read back on the EP
   (Phase 2, but the mapping is a Phase 1 exit criterion if easy).
5. Unplugging / powering the blade makes the Type-0 function
   disappear without hanging the MT7620A.
