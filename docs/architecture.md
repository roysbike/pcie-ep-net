# openmiop architecture (protocol v4)

State of `claude/pcie-talos-integration`, 2026-10-06. Everything marked
*measured* was observed on the Cluster Box described below.

## 1. Hardware (measured)

```
 MT7620A (MIPS 24KEc, 580 MHz, OpenWrt 5.15)   Cluster Box BMC, root complex
   00:00.0 root port ── Gen1 x1 ──┐
                                  01:00.0 ASM2824 upstream (cap 8 GT/s x8)
                                  ├─ 02:00.0 ── Gen3 x2 ── 03:00.0 slot 3 → node 0 (blade160)
                                  ├─ 02:04.0 ── Gen3 x2 ── 04:00.0 slot 4 → node 1 (blade173)
                                  ├─ 02:08.0 ──          ── bus 05  slot 2 → node 2
                                  └─ 02:0c.0 ──          ── bus 06  slot 1 → node 3
```

* Each blade is an RK3588. Its PCIe3 PHY is split in two x2 halves:
  lanes 0-1 to the Cluster Box (`fe150000`, pcie3x4 controller, run as
  endpoint), lanes 2-3 to the M.2 NVMe (`fe160000`, pcie3x2 host). The
  links therefore train **Gen3 x2** (8 GT/s, 128b/130b, ≈15.75 Gbit/s
  raw per direction), not x4.
* The ASM2824 has **no ACS capability**; peer-to-peer TLPs between
  downstream ports are routed by address inside the switch.
* The MT7620A decodes one 256 MiB window (`0x20000000-0x2fffffff`).
  Each endpoint exposes only BAR0, 16 MiB, 64-bit prefetchable
  (BAR2-5 and the ROM are disabled by the driver), so four blades need
  64 MiB.
* MSI and MSI-X are advertised by the endpoints but the MT7620A cannot
  use them; the vendor stack falls back to INTx. Nothing in this design
  needs the RC to take interrupts.
* Hot plug: the ASM2824 ports report `HotPlug- Surprise-`. Endpoints
  that appear after the BMC booted are found by a rescan. The bridge
  windows are sized when they are first assigned (see §9).
* MPS: endpoints and downstream ports support 256 B; the root port and
  the switch upstream port 128 B. All endpoints must use the same MPS.
  After a rescan Linux once left one EP at 256 and the other at 128:
  every TLP over 128 bytes from the first was dropped by the second as
  malformed (pings up to 100 bytes worked). The RC helper enforces one
  MPS for all endpoints.

## 2. How a Debian blade becomes an endpoint

1. U-Boot (vendor) sees the Cluster Box sense pins and rewrites
   `/pcie@fe150000` to `compatible = "mixtile,miop-ep-rk3588"`, so the
   in-kernel `pcie-dw-rockchip` host driver does not bind it.
2. The vendor kernel (6.1.99) has no PCI endpoint framework
   (`CONFIG_PCI_ENDPOINT` unset) and no dw-edma. openmiop is a platform
   driver for that node and programs the controller directly:
   resets, PHY (`devm_phy_get("pcie-phy")`, `phy_init`, `phy_power_on`),
   clocks, then DBI (config space), iATU and eDMA registers.
3. `program_phy_mode()` puts the PCIe3 PHY in bifurcated mode
   (two x2) through the PHY GRF, as the vendor DT asks for aggregation.
   The vendor kernel leaves RX common-refclk mode off on all lanes
   (lane CON1 = 0x60): the blade and the Cluster Box do not share a
   reference clock.
4. Config space: Rockchip vendor id `1d87`, device `4f4d`, class
   Ethernet; BAR0 64-bit prefetchable, sized 16 MiB through the
   resizable-BAR control register; BAR2-5/ROM disabled through DBI2;
   ATS, PRI and ReBAR hidden; target x2 Gen3.
5. A 16 MiB, 16 MiB-aligned buffer (`dma_alloc_noncoherent`) is the BAR
   memory; inbound iATU region 0 in BAR-match mode points BAR0 at it.
6. LTSSM is enabled with `APP_DLY2` set; the link comes up in ~100 ms.
7. The netdev `omi0` is registered; the ctl thread starts.
8. On the Cluster Box, `openmiop-rc` finds the endpoint, restores BAR0,
   Memory Space + Bus Master and MPS, maps the BAR and activates the
   node (§4).

Nothing else is required on Debian: no configfs, no userspace on the
blade, no firmware, no reserved memory (the vendor DT's `miop_dma` pool
is used if present but not needed). `scripts/openmiop.service` loads
the module and sets the address from `/etc/openmiop.addr`.

## 3. BAR layout (`openmiop.h`)

| Offset | Content | Writer |
| --- | --- | --- |
| `0x0000` | `omi_hdr`: magic, version 4, epoch, flags, MAC, geometry, `table_seen` | owning EP |
| `0x0040` | `omi_ctl`: RC up, RC MAC, node index, `table_gen`, `down_ack`, `ep_epoch` | RC |
| `0x0080` | `peers[8]`: per node epoch, BAR address, MAC | RC |
| `0x0400` | `rx_prod[8]`: per sender ring head + connect token | that sender |
| `0x0600` | `tx_cons[8]`: per receiver credit (tail) + ack | that receiver |
| `0x1000` | gateway rings EP↔RC, 2 × 64 slots × 9280 B | EP / RC |
| `0x200000` | P2P rings: 4 senders × 256 slots × 10 KiB | senders (eDMA) |
| `0xf00000` | eDMA scratch: head values, linked list, slot headers | owning EP only |

Every 64-byte line has exactly one writer. The BAR is cacheable on the
owner, which cleans lines it writes and invalidates lines others write
before reading them; a single writer per line means a clean can never
write back over somebody else's store.

## 4. Control plane

**Node index.** The RC numbers an endpoint by the position of its
parent bridge among the switch's downstream ports. On the ASM2824 this
is 02:00.0→0, 02:04.0→1, 02:08.0→2, 02:0c.0→3; it depends only on the
slot, so it survives reloads and reboots. Ring *i* in any BAR carries
frames from node *i*.

**Epoch.** Each probe and each host link reset picks a new random
epoch. The RC activates a node by writing `ctl.ep_epoch`; the EP trusts
the control line and the peer table only while that matches its own
epoch, so a stale control line is never taken for a live session.

**Peer table.** For every active node the RC writes the other nodes'
epoch, BAR address and MAC into `peers[]` (epoch cleared first, written
last, then `table_gen` bumped). The EP re-reads an entry until its
epoch is stable, so a half-written entry is never used.

**Connect (write-only).** EPs never read each other's BAR: a read to a
peer whose link just dropped completes with UR (or not at all) and can
raise an SError on the blade. Instead:

1. sender *s* programs outbound iATU region *r* at node *r*'s BAR;
2. *s* writes `rx_prod[s].head = 0`, then a fresh random `token`, into
   *r*'s BAR (posted writes, ordered by `writel()`);
3. *r* sees a new token on ring *s*, sets its tail to the head and
   writes `tx_cons[r] = {tail, ack = token}` into *s*'s BAR;
4. *s* sees its token echoed and starts sending.

A new token per connection means an ack left from an earlier
connection can never match. Credit returns the same way: after NAPI
consumed frames, *r* writes its tail into *s*'s BAR.

**Leave.** `rmmod` sets `OMI_F_DOWN`. The RC removes the node from all
tables, waits until every other node has applied the new table
(`hdr.table_seen`), writes `ctl.down_ack`, and does not touch the
leaving node for 3 s. Only then does the EP drop its link.

**Host link reset.** A link-down or hot reset (`LINK_REQ_RST_NOT` in
the Rockchip client block, polled by the ctl thread) resets the
controller's non-sticky registers. With `APP_DLY2` the LTSSM waits
until software is done. The driver then stops P2P, restores config
space, **reprograms every iATU region** and starts a new epoch, then
releases the link. Two hardware details made this necessary
(measured with `omi-peek` from the BMC): the iATU kept its enable bits
but lost its targets, and the hidden ReBAR capability stayed hidden
while BAR0 fell back to 1 GiB. Without the fix the host's BAR0
accesses landed in unrelated blade memory.

**RC safety rule.** A read that is in flight when an endpoint's link
drops can be discarded by the switch; a completion timeout on the
MT7620A root port once reset the whole fabric (switch config cleared,
root port unable to reach it until the BMC rebooted). The helper never
reads a leaving endpoint, waits after every detach, checks the link
with a config read (which the downstream port answers itself when its
link is down) before memory reads, and only rescans while a port is
empty. With these rules, 8 back-to-back reload cycles of both blades
with the helper running passed (the same sequence failed on the first
cycle before).

## 5. Data path

```
xmit ── route ──┬─ gateway: copy into EP→RC ring (CPU), RC relays/TAP
                └─ P2P: txq ──► tx thread ──► eDMA linked list ──► peer BAR ring
peer: ctl thread notices head ≠ tail ──► napi_schedule ──► NAPI ──► GRO ──► stack
                                                         └─ credit write into sender's BAR
```

**TX.** `ndo_start_xmit` routes the frame (peer table MAC, then learned
source MACs, then flood) and queues it. The TX thread maps up to 32
frames (scatter-gather, no linearize), builds one eDMA linked list
(per target: 16-byte slot header from scratch, then the skb segments;
at the end one 4-byte head write per touched peer), rings the doorbell
and waits for the done bit. The queue is stopped in the same `tx_lock`
section as the enqueue that fills a ring, so the stack never sees
`NETDEV_TX_BUSY` (a requeued skb could be overtaken and reorder TCP).
A peer that does not consume for 100 ms is marked stalled and its
frames are dropped, so one dead peer cannot stop the shared queue.

**RX.** There is no RX interrupt (§7). The ctl thread checks the ring
heads every 20-50 µs for ~80 ms after traffic, then every 200-400 µs,
sleeping in `TASK_IDLE`. When work is pending it calls
`napi_schedule()`; the NAPI poll drains the rings round-robin within
its budget, copies each frame into a `napi_alloc_skb()` buffer, hands
it to `napi_gro_receive()` and returns credit. While NAPI is scheduled
the ctl thread does not kick it again.

**Memory ordering** (also in the driver header comment):

| Where | Barrier | Why |
| --- | --- | --- |
| sender eDMA list | none in software | one channel, payload elements before the head element; posted writes from one requester are not reordered |
| receiver head → slot | invalidate head line, read head, then invalidate the slot | a line fetched speculatively before the payload arrived is discarded |
| receiver credit | `mb()` then `writel()` | `writel()` orders stores only; the slot loads must finish before the sender may reuse the slot |
| gateway RX tail | `mb()` before the store + clean | same, towards the RC |
| gateway TX | `dma_sync_single_for_device()` (ends in `dsb`) before the head store | the RC must not see the head before the slot |
| window publication | `smp_store_release`/`smp_load_acquire` on `win_ok` | NAPI must not store through a window before its iATU region is programmed |
| connect | `writel(head)` then `writel(token)` | the receiver that sees the token sees the head |

## 6. L2 semantics

* Unicast to a peer: P2P. Unicast to the RC MAC or to a MAC learned
  behind the gateway: gateway ring. Unknown unicast: flooded to every
  peer and the gateway, like a switch.
* Broadcast and multicast (ARP, IPv6 ND, `ff02::1`): every peer over
  P2P plus the gateway. The slot header carries the set of nodes that
  already got the frame; the RC relays it only to the rest.
* Source MACs of received frames are learned per node (bridges behind
  a blade work).
* The RC relays unicast between two endpoints that have no P2P path
  yet (functional fallback, not a data path).
* One MTU everywhere: up to 9246 bytes; 9000 is configured.

Measured: ARP, IPv4, IPv6 link-local and ND, `ff02::1` answered by all
members, jumbo frames to peer and RC.

## 7. Notification: polling, IRQ, MSI

* No openmiop IRQ is used today. The eDMA write-done interrupt is
  masked and polled; `pcie-sys` and the ITS lines in `/proc/interrupts`
  belong to the host controllers, not to openmiop.
* RX notification needs an interrupt on the *receiving* blade for a
  write from another *endpoint*. MSI/MSI-X of the endpoint function go
  to the RC, and the MT7620A cannot take them.
* Candidates for a real RX doorbell (not implemented, need hardware
  experiments):
  1. **GIC ITS doorbell.** An inbound iATU region maps part of BAR0 onto
     the RK3588 GIC-600 ITS `GITS_TRANSLATER`; a peer writes the event
     ID there and the receiver gets an LPI, the mechanism mainline uses
     for endpoint doorbells (`CONFIG_PCI_ENDPOINT_MSI_DOORBELL`,
     pci-epf-vntb). The vendor node has `msi-map = <0 &its 0 0x1000>`,
     so the ITS DeviceID of such a write would be the *sender's*
     requester ID on the BMC's bus numbering (03:00.0 → 0x0300, ...).
     The receiver needs ITS device entries for every peer's DeviceID,
     which Linux's platform-MSI API does not express directly.
  2. **Vendor-defined message.** The sender emits an ID-routed VDM TLP
     through an outbound iATU region of type Msg; the switch routes it
     to the receiver's bus number and the receiving DWC raises its
     `msg` interrupt (SPI 261, present in both DT nodes). No ITS state,
     but the Rockchip client handling of received VDMs is undocumented
     here and must be measured.
* On Talos the upstream `pcie-ep` node also carries the eDMA interrupt
  lines (`dma0`-`dma3`), which would remove the TX spin.

Cost of polling (measured, idle link): 0.6 % of 8 cores, load ≈ 0.2,
~2.7-3.0 k timer interrupts/s per blade; first ping after idle
0.78 ms average (vs 0.32 ms when busy).

## 8. P2P (measured)

Blade-to-blade TCP reaches 6-7.5 Gbit/s per direction and ~14 Gbit/s
bidirectional, while the root port link is Gen1 x1 (≤2 Gbit/s). During
a 6 Gbit/s blade-to-blade stream the BMC CPU is 12 % busy, the same as
idle (14 %; the helper itself shows 0-8 % in `top`, the rest is other
OpenWrt services). Payload therefore never crosses the RC. The switch routes the eDMA writes directly between downstream
ports. Integrity: 1 GiB random data per direction, sha256 identical.

The RC path (gateway) is a control/management path: blade→BMC
12-14 Mbit/s (the MIPS reads the ring with 4-byte MMIO), BMC→blade
~88 Mbit/s.

## 9. Known limitations

* The TX thread spins on the eDMA done bit (one core at ~100 % while
  sending). Throughput is limited by that serial wait and by core
  placement: on runs where the busy thread landed on an A55 core,
  throughput fell to 3-5 Gbit/s (big.LITTLE).
* Single TX queue, single NAPI: multiqueue was not added because the
  measured limit is the serial eDMA wait, not one RX/NAPI core.
* Bridge windows on the BMC are sized when first assigned. A blade
  that appears in an empty slot after the BMC sized the windows may get
  no BAR address; the helper logs it. A BMC reboot (or a quiesced
  re-enumeration, not implemented) fixes it.
* An endpoint that crashes or is reset without the leave handshake can
  still race an RC read; the helper limits but cannot remove that
  window.
* `rmmod` waits up to 2 s for the RC.
