# Project history

Notes from the start of the project (October 2026). They describe how
openmiop came about and are kept for reference; they are not maintained
and parts of them no longer match the current driver. The current design
is in [../architecture.md](../architecture.md).

| Document | What it is |
| --- | --- |
| [feasibility.md](feasibility.md) | Can a Blade 3 be a PCIe endpoint behind the Cluster Box switch: hardware, vendor stack, risks |
| [live-miop-2026-10-05.md](live-miop-2026-10-05.md), [clusterbox-live-2026-10-05.md](clusterbox-live-2026-10-05.md) | What Mixtile's MIOP stack and the Cluster Box BMC did on a live box |
| [baseline.md](baseline.md) | First measurements with the vendor stack |
| [phase1-runbook.md](phase1-runbook.md), [phase1/](phase1/) | Phase 1: endpoint bring-up with `pci-epf-test`, device tree overlay, kernel config, enumeration scripts |
| [v0.2-design.md](v0.2-design.md) | Design of v0.2: RX doorbell interrupts and multiqueue |
| [talos-port.md](talos-port.md) | What Talos 1.14.2 needed (since then built in [mixtile-talos](https://github.com/roysbike/mixtile-talos)) |
