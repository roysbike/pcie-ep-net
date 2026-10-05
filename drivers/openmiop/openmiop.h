/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared BAR0 layout for openmiop.
 *
 * Both CPUs are little-endian (RK3588, MT7620A). The endpoint keeps
 * this structure in DRAM and publishes it with an inbound iATU BAR
 * match. The Cluster Box maps that BAR. The buffer is cacheable on
 * the endpoint, so each writer owns its own cache lines: a write-back
 * must not cover a dword the router or the peer stores.
 *
 * Blade-to-blade frames do not enter the router. The router writes
 * each endpoint's PCI BAR address into the other endpoint's header.
 * The endpoint then programs an outbound iATU window at the peer BAR.
 * Blade-to-blade frames are moved by the endpoint eDMA engine.
 *
 * The small ep_tx/rc_tx rings are only the gateway path (blade to the
 * router). They stay at a 1500-byte MTU. The P2P ring lives at
 * OPENMIOP_P2P_OFF and is sized for jumbo frames.
 */
#ifndef OPENMIOP_H
#define OPENMIOP_H

#include <linux/types.h>

#define OPENMIOP_MAGIC		0x31494d4fu	/* bytes "OMI1" on LE */
#define OPENMIOP_VERSION	3u

/* Programmed into the endpoint config space. Not Mixtile 4586:b6f2,
 * so the proprietary miop.ko on the Cluster Box will not bind.
 * 1d87 is the Rockchip vendor id already used by the silicon default;
 * 4f4d is a development device id for this driver.
 */
#define OPENMIOP_PCI_VENDOR	0x1d87u
#define OPENMIOP_PCI_DEVICE	0x4f4du

#define OPENMIOP_FLAG_UP	0x1u

#define OPENMIOP_SLOTS		128u
#define OPENMIOP_SLOT_DATA	1600u
#define OPENMIOP_MAX_FRAME	1514u

/* 16 MiB fits twice in the Cluster Box 256 MiB MMIO window. */
#define OPENMIOP_BAR_SIZE	(16u * 1024u * 1024u)

/* P2P receive ring. The peer writes it through its outbound window.
 * Slot data starts 16 bytes in so stores can be 16-byte aligned.
 * 512 * 16 KiB = 8 MiB, placed at 1 MiB so it stays clear of the
 * gateway rings above.
 */
#define OPENMIOP_P2P_OFF	0x00100000u
#define OPENMIOP_P2P_SLOTS	512u
#define OPENMIOP_P2P_SLOT	16384u
#define OPENMIOP_P2P_HDR	16u
#define OPENMIOP_P2P_DATA	(OPENMIOP_P2P_SLOT - OPENMIOP_P2P_HDR)

struct openmiop_slot {
	__u32 len;
	__u32 flags;
	__u8 data[OPENMIOP_SLOT_DATA];
};

/*
 * Three 64-byte lines.
 *   xmit:  endpoint transmit context (ndo_start_xmit, once at publish)
 *   poll:  endpoint receive context (the poll thread)
 *   dev:   router and the peer. The endpoint only reads this line.
 * Remotes reach DRAM through PCIe, so they do not share the cache.
 */
struct openmiop_bar {
	__u32 magic;
	__u32 version;
	__u32 ep_flags;
	__u32 ep_tx_head;	/* EP produces, RC consumes */
	__u8 ep_mac[6];
	__u8 ep_pad[2];
	__u32 ep_kick;
	__u8 xmit_pad[36];

	__u32 rc_tx_tail;
	__u32 p2p_rx_tail;	/* EP consumes the peer ring */
	__u8 poll_pad[56];

	__u32 rc_flags;
	__u32 ep_tx_tail;
	__u32 rc_tx_head;	/* RC produces, EP consumes */
	__u8 rc_mac[6];
	__u8 rc_pad[2];
	__u32 rc_kick;

	/* Filled by the router once both endpoints have BARs.
	 * peer_gen is written last. The endpoint reprograms its
	 * outbound iATU when peer_gen changes.
	 */
	__u32 peer_bar_lo;
	__u32 peer_bar_hi;
	__u32 peer_gen;
	__u8 peer_mac[6];
	__u8 peer_pad[2];

	/* Peer produces head by DMA into our BAR. */
	__u32 p2p_rx_head;
	__u8 dev_pad[16];

	struct openmiop_slot ep_tx[OPENMIOP_SLOTS];
	struct openmiop_slot rc_tx[OPENMIOP_SLOTS];
};

#endif
