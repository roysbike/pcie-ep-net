# Push mode: the Cluster Box never reads a blade

Status: released in v0.3.0-rc.1. Run on the lab Cluster Box since
2026-10-11 (four blades, `omi-rc` loaded instead of the helper, blade
reset with a `nodectl` release).

## Why

The Cluster Box BMC (MT7620A) is the PCIe root complex above the ASM2824
switch. If the BMC has a read of a blade's BAR in flight when that
blade's link drops without warning (reset pulse, power cut, kernel
crash), the MT7620A root complex stops answering: config reads of the
switch return 0, then hang. Only a BMC reboot recovers, and that reboot
resets the switch below it, so every blade loses its links for the
length of the BMC boot. Seen on 2026-10-08; the switch itself was not
reset (the other blades kept their P2P traffic). The root port offers
no programmable completion timeout (DevCap2 0x10).

`openmiop-rc` read the blades all the time: header (magic, epoch, leave
flag), `table_seen` of the peers, and the gateway ring. Gating those
reads on the switch port's link state (`claude/fabric-resilience`)
helps for announced events (leave, `nodectl` release), not for a blade
that just dies.

## Protocol

Everything the root complex needs from a blade, the blade writes into
the root complex's memory. The root complex only writes into blades
(posted writes), and reads a blade's config space once per attach, with
a link that has been up for 1.5 s.

1. **Ready marker.** The blade driver sets Subsystem Vendor/ID to
   `OMI_SSVID_PUSH`/`OMI_SSID_PUSH` (0x4f4d/0x0001) in its config space
   before its link comes up, at probe and after every link reset. By
   then its header is initialised and BAR0's inbound translation is in
   place, so writes into the BAR land in it.
2. **Offer.** The root complex allocates an area in its memory (64 KiB
   aligned) and writes into the blade's BAR: `tx_cons[OMI_RC_NODE]`
   (tail 0, ack = token), then the control line: its MAC, the blade's
   node index, `ep_epoch` 0, the gateway v2 ring (`gw2_ring_*`, slots,
   slot size), `gw2_token`, and `flags = OMI_RC_PUSH`.
3. **State.** The blade maps its outbound window 7 (`OMI_RC_NODE`) onto
   the area and writes `struct omi_gw2_ep` at offset 0x40: magic,
   version, epoch, header flags (`OMI_F_UP`, `OMI_F_DOWN`), `table_seen`,
   features, doorbell word and value, MAC, `rc_tail` (its position in
   the root complex's gateway ring), `seq`, then the token last. It
   writes again whenever one of them changes, and every second.
4. **Activation.** When the area holds the token, a known magic and
   version and `OMI_F_UP`, the root complex writes `ep_epoch` = epoch
   and `flags = OMI_RC_UP | OMI_RC_PUSH`. From here the blade is as with
   the v4 helper: peer table, gateway, leave.
5. **Liveness.** Link: Data Link Layer Link Active of the switch port
   (the switch answers this itself). Blade: the epoch and token in the
   area; `seq` must move at least every 5 s, otherwise the blade's
   kernel is considered gone and it leaves the tables.
6. **Leave.** The blade sets `OMI_F_DOWN` and pushes it (from
   `bar_leave()`, its ctl thread being stopped). The root complex takes
   it out of the tables, waits until the others' pushed `table_seen`
   caught up (1 s at most), writes `down_ack`, and leaves the blade
   alone until its link dropped and came back.
7. **Release.** Before `nodectl` pulses a reset line or cuts power it
   writes the switch port to `/sys/module/omi_rc/parameters/release`;
   the write returns once nothing goes to that blade any more.
8. **Link reset.** The blade clears `gw2_token` and `flags` in its
   control line and pushes nothing until a new offer: the area an old
   offer named may be gone (BMC rebooted).

Gateway v2 (blade to BMC) uses the area's ring at 0x80: the blade's
eDMA writes frames and the head (offset 0) as for a peer; the BMC
returns credit in `tx_cons[OMI_RC_NODE].tail` of the blade's BAR.
BMC to blade uses the v1 gateway ring in the blade's BAR (posted writes,
then the blade's doorbell); the blade reports progress in `rc_tail` of
the area.

### Gateway pull (v0.4)

Writing frames into a blade's BAR costs the MT7620A one uncached 32-bit
PCIe write per word: BMC to blade stopped at ~90-130 Mbit/s with the BMC
CPU at 99 %. With `OMI_HDR_GW_PULL` the blade fetches them instead:

1. A blade with an eDMA read channel (RK3588: yes) sets
   `OMI_HDR_GW_PULL` in its header features (module parameter
   `gw_pull`, default on).
2. `omi-rc` (parameter `pull`, default on) allocates per blade a copy of
   the RC->EP slots in its own memory (`gw_slots` x `gw_slot_size`,
   64 x 9280 bytes) and names it in the offer: `gwp_ring_lo/hi` in the
   control line, written before `gw2_token`.
3. For a blade that announced the feature, `omi-rc` copies a frame into
   that ring slot (cached copy, then a cache clean of the frame), and
   writes only the slot header, with `OMI_GW_PULL` in its flags, and
   `rc_head` into the blade's BAR.
4. The blade reads the frames of `OMI_GW_PULL` slots with read channel 0
   into the same slots of its BAR, up to 16 slots per linked-list run,
   polled (read interrupts stay masked), then delivers them as before
   and reports `rc_tail` as before. A failed run drops those frames
   (`rx_gw_pull_errors`) and resets the read engine.

Old blades and an old `omi-rc` never meet an `OMI_GW_PULL` slot. Measured
on the lab box on 2026-10-13: BMC to blade without NAT 130 ->
204 Mbit/s, LAN to blade through the BMC's NAT 92 -> 143 Mbit/s; the BMC
CPU is then the limit.

## Compatibility

- A push-mode blade works with the userspace `openmiop-rc` as before:
  that helper reads the header and never sets `OMI_RC_PUSH`.
- `omi-rc` leaves blades without the marker alone (logged once): every
  blade needs the push-mode driver before the BMC switches to the
  module.
- Wire format stays protocol v4 between blades.

## Not done yet

- Re-enumeration of the bridge windows from the module (a blade whose
  BAR got no address is only logged); the helper's `reenumerate` path.
- A blade crash or power cut without a release, on purpose.
