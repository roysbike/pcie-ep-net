/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared BAR0 layout for openmiop, protocol version 4.
 *
 * Every endpoint (EP) exposes one 16 MiB BAR backed by its own DRAM.
 * The root complex (RC, the Cluster Box) maps all EP BARs. EPs reach
 * each other through outbound iATU windows and the eDMA engine; that
 * traffic crosses the PCIe switch and never enters the RC.
 *
 * Rules:
 *
 * - Each 64-byte line has exactly one writer. The BAR is cacheable on
 *   the EP, so the EP flushes lines it writes and invalidates lines
 *   written by others before reading them.
 * - EP to EP communication is write-only. An EP never issues a PCIe
 *   read to another EP: a read to a peer that just went away completes
 *   with UR and can raise an SError. Ring heads, connect requests,
 *   acknowledgements and credits are all posted writes into the other
 *   EP's BAR.
 * - The RC reads and writes EP BARs with CPU MMIO. It owns the control
 *   line and the peer table, and moves the gateway rings.
 *
 * Node index: the RC gives every EP an index (0 .. OMI_MAX_NODES - 1)
 * that only depends on the switch port it sits behind, so it survives
 * reloads. Ring i in an EP's BAR carries frames from the node with
 * index i.
 *
 * Both CPUs are little-endian (RK3588, MT7620A).
 */
#ifndef OPENMIOP_H
#define OPENMIOP_H

#include <linux/types.h>

#define OPENMIOP_MAGIC		0x31494d4fu	/* bytes "OMI1" on LE */
#define OPENMIOP_VERSION	4u

/* Programmed into the endpoint config space. Not Mixtile 4586:b6f2,
 * so the proprietary miop.ko on the Cluster Box will not bind.
 * 1d87 is the Rockchip vendor id already used by the silicon default;
 * 4f4d is a development device id for this driver.
 */
#define OPENMIOP_PCI_VENDOR	0x1d87u
#define OPENMIOP_PCI_DEVICE	0x4f4du

#define OPENMIOP_BAR_SIZE	(16u * 1024u * 1024u)

#define OMI_MAX_NODES		8u
/* Queues per sender/receiver pair (OMI_FEAT_MQ). */
#define OMI_MAX_QUEUES		4u

/* omi_hdr.flags */
#define OMI_F_UP		0x1u	/* header valid, rings initialised */
#define OMI_F_DOWN		0x2u	/* EP is leaving; RC must detach it */

/* omi_ctl.flags */
#define OMI_RC_UP		0x1u

/* Gateway slot flags (RC <-> EP). */
#define OMI_GW_RELAYED		0x1u	/* RC copied this from another EP */

/* ---- 0x0000: header, written by the owning EP ---- */
struct omi_hdr {
	__u32 magic;
	__u32 version;
	__u32 epoch;		/* random, non-zero, new on every probe */
	__u32 flags;		/* OMI_F_* */
	__u8 mac[6];
	__u16 n_rings;		/* P2P receive rings, indexed by sender */
	__u32 ring_off;		/* offset of ring 0 */
	__u32 ring_slots;	/* slots per ring, power of two */
	__u32 slot_size;	/* bytes per slot, including omi_slot_hdr */
	__u32 gw_off;		/* offset of struct omi_gw */
	__u32 gw_slots;		/* per direction, power of two */
	__u32 gw_slot_size;	/* bytes per gateway slot incl. header */
	__u32 table_seen;	/* last omi_ctl.table_gen applied */
	__u8 pad[12];
};

/* ---- 0x0040: control, written by the RC ---- */
struct omi_ctl {
	__u32 flags;		/* OMI_RC_UP */
	__u8 mac[6];		/* the RC's own omi0 MAC */
	__u8 self_idx;		/* this EP's node index */
	__u8 pad0;
	__u32 table_gen;	/* bumped after every peer table change */
	__u32 down_ack;		/* = hdr.epoch once the RC has detached us */
	__u32 ep_epoch;		/* hdr.epoch the RC activated; the control
				 * line and the table are only valid for
				 * that epoch
				 */
	__u8 pad[40];
};

/* ---- 0x0080: peer table, written by the RC ----
 * Entry i describes node i. epoch == 0: absent. The RC clears epoch
 * before it changes an entry and writes it last, then bumps table_gen.
 */
struct omi_peer_entry {
	__u32 epoch;
	__u32 bar_lo;		/* PCI address of the peer's BAR0 */
	__u32 bar_hi;
	__u8 mac[6];
	__u8 pad[14];
};

/* ---- 0x0400: one line per remote node, written by that node ----
 * rx_prod[i]: producer state of the ring that carries frames from
 * node i to us. head is written by i's eDMA after the payloads;
 * token is written by i when it (re)connects: a fresh random value
 * per connect, so a stale acknowledgement can never match.
 */
struct omi_prod {
	__u32 head;		/* queue 0 */
	__u32 token;		/* sender's connect token */
	__u32 features;		/* OMI_FEAT_* the sender supports */
	__u32 feat_token;	/* = token when features and nq belong to it */
	__u32 nq;		/* OMI_FEAT_MQ: the sender's queue count */
	__u32 head_q[OMI_MAX_QUEUES - 1];	/* OMI_FEAT_MQ: queues 1.. */
	__u8 pad[32];
};

/* ---- 0x0600: one line per remote node, written by that node ----
 * tx_cons[i]: credit for the ring we fill in node i's BAR. Node i
 * writes how far it has consumed, and acknowledges our connect by
 * echoing our token.
 */
struct omi_cons {
	__u32 tail;		/* queue 0 */
	__u32 ack;		/* = our token once node i accepted it */
	__u32 features;		/* OMI_FEAT_* node i offers us */
	__u32 db_off;		/* doorbell word in node i's BAR */
	__u32 db_data;		/* value to write there, + queue index */
	__u32 db_token;		/* = ack when the offer (features .. nq) belongs to it */
	__u32 nq;		/* OMI_FEAT_MQ: queues node i set up for us */
	__u32 tail_q[OMI_MAX_QUEUES - 1];	/* OMI_FEAT_MQ: queues 1.. */
	__u8 pad[24];
};

/*
 * Optional features, negotiated per connection. A peer that does not
 * know them never writes these fields; *_token ties them to the
 * current connect token, so values from an earlier connection never
 * count.
 *
 * OMI_FEAT_DOORBELL, receiver: after a run's head writes, the sender
 * also writes db_data at db_off in the receiver's BAR. The receiver
 * maps that address onto an interrupt (on RK3588: an inbound iATU
 * window onto the GIC ITS translater), so it need not poll its rings.
 * Sender: it rings when offered.
 *
 * OMI_FEAT_MQ: the sender's ring area in the receiver's BAR is split
 * into nq rings of ring_slots / nq slots each, ring q at
 * ring_off(sender) + q * (ring_slots / nq) * slot_size. Queue q has its
 * own head (prod.head or prod.head_q[q - 1]) and credit (cons.tail or
 * cons.tail_q[q - 1]), and its doorbell value is db_data + q. One queue
 * is written by one eDMA channel and read by one NAPI context, so flows
 * that stay on a queue stay in order. Used only when both sides
 * announce the same nq; otherwise the pair uses one ring (queue 0) with
 * all ring_slots.
 */
#define OMI_FEAT_DOORBELL	0x1u
#define OMI_FEAT_MQ		0x2u

/* ---- 0x0180: hello lines, one per remote node, written by that node ----
 * Reconnect without the RC. After a fabric reset (Cluster Box reboot,
 * re-enumeration) every endpoint gets a new epoch and the RC is not
 * there yet to publish a table. An endpoint whose BAR came back at the
 * address the last table gave it writes its new epoch into each peer it
 * knew, at that peer's last known address. The receiver connects only
 * if both views agree: to_mac is its own MAC, and mac and bar match
 * what its own last table said about the sender. Epoch is written last.
 * Ignored while the RC is up: then the table is authoritative.
 */
struct omi_hello {
	__u32 epoch;		/* sender's hdr.epoch, 0: none */
	__u8 mac[6];		/* sender */
	__u8 to_mac[6];		/* receiver, as the sender knows it */
	__u32 bar_lo;		/* sender's BAR0 */
	__u32 bar_hi;
	__u8 pad[8];
};

struct omi_bar_head {
	struct omi_hdr hdr;
	struct omi_ctl ctl;
	struct omi_peer_entry peers[OMI_MAX_NODES];
	struct omi_hello hello[OMI_MAX_NODES];
	__u8 pad[0x400 - 0x80 - OMI_MAX_NODES * (sizeof(struct omi_peer_entry) +
						  sizeof(struct omi_hello))];
	struct omi_prod rx_prod[OMI_MAX_NODES];
	struct omi_cons tx_cons[OMI_MAX_NODES];
};

/* Slot header in front of every frame, P2P and gateway. For P2P the
 * sender DMAs the header and the frame in one transfer.
 */
struct omi_slot_hdr {
	__u32 len;
	__u32 flags;		/* OMI_SLOT_* */
	__u32 mask;		/* gateway: node indices that already have it */
	__u32 hash;		/* OMI_SLOT_HASH: the sender's flow hash */
};

#define OMI_SLOT_HASH		0x1u	/* hash is valid (L4 flow hash) */

/* ---- gateway (EP <-> RC), at hdr.gw_off ----
 * ep_prod/rc_cons: frames from the EP to the RC.
 * rc_prod/ep_cons: frames from the RC to the EP.
 * Each index lives on its own line with its own writer.
 */
struct omi_gw {
	__u32 ep_head;		/* EP */
	__u8 pad0[60];
	__u32 ep_tail;		/* RC */
	__u8 pad1[60];
	__u32 rc_head;		/* RC */
	__u8 pad2[60];
	__u32 rc_tail;		/* EP */
	__u8 pad3[60];
	/* followed by gw_slots ep_tx slots, then gw_slots rc_tx slots */
};

/* Geometry used by this implementation. The receiver publishes its
 * geometry in omi_hdr; senders use the published values.
 */
#define OMI_GW_OFF		0x1000u
#define OMI_GW_SLOTS		64u
#define OMI_GW_SLOT		9280u	/* 16 header + 9216 data, 64-aligned */
#define OMI_RING_OFF		0x200000u
#define OMI_RING_SLOTS		256u
#define OMI_SLOT		10240u
#define OMI_N_RINGS		4u
/* Local eDMA scratch. Peers and the RC never write here. */
#define OMI_SCRATCH_OFF		0xf00000u
/* 64 KiB of BAR0 that this implementation maps onto its doorbell. */
#define OMI_DB_WIN_OFF		0xff0000u
#define OMI_DB_WIN_SIZE		0x10000u

#define OMI_GW_DATA		(OMI_GW_SLOT - sizeof(struct omi_slot_hdr))
#define OMI_SLOT_DATA		(OMI_SLOT - sizeof(struct omi_slot_hdr))

#endif
