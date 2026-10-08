// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * omi-rc: the Cluster Box (BMC) side of openmiop, in the kernel.
 *
 * The MT7620A is the root complex above the ASM2824 switch that the
 * Blade 3 endpoints sit behind. Data between blades never passes
 * through here; this module
 *
 *   - watches the link of every switch downstream port (Data Link Layer
 *     Link Active, which the switch answers itself) and sends nothing to
 *     a blade unless its link has been up for link_stable_ms;
 *   - restores BAR0, Memory/Bus Master and a common MPS after a blade
 *     reloaded (config space, once per attach, link stable);
 *   - makes each blade a push-mode offer (OMI_RC_PUSH) and activates it
 *     from the state the blade then pushes into our memory;
 *   - publishes the peer table, detaches blades that leave, releases a
 *     blade on request before nodectl resets it or cuts its power;
 *   - is the BMC's omi0: gateway v2 frames from the blades arrive in a
 *     ring in our memory (their eDMA), ours go out as posted writes into
 *     their gateway ring, then their doorbell.
 *
 * It never reads a blade's BAR. A read in flight while a blade's link
 * drops without warning (reset, power cut, crash) stops the MT7620A
 * root complex until the Cluster Box reboots, which resets the whole
 * fabric. Blades need an openmiop-ep that takes push-mode offers
 * (OMI_SSVID_PUSH in their config space); others are left alone.
 *
 * Replaces the userspace helper openmiop-rc; do not run both.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/random.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>

#include "openmiop.h"

#define DRV_NAME	"omi-rc"

static uint slots = 32;
module_param(slots, uint, 0444);
MODULE_PARM_DESC(slots, "Slots per blade->BMC gateway ring (power of two)");

static uint poll_us = 100;
module_param(poll_us, uint, 0644);
MODULE_PARM_DESC(poll_us, "Gateway poll period while traffic is recent, us");

static uint idle_us = 1000;
module_param(idle_us, uint, 0644);
MODULE_PARM_DESC(idle_us, "Gateway poll period when idle, us");

static uint link_stable_ms = 1500;
module_param(link_stable_ms, uint, 0644);
MODULE_PARM_DESC(link_stable_ms, "A blade's link must be up this long before we touch it");

#define LINK_POLL_MS	5
#define LEAVE_DOWN_MS	30000	/* after a leave or release: wait for the drop */
#define STATUS_MS	20	/* look at pushed state */
#define OFFER_RETRY_MS	1000	/* no state after an offer: offer again */
#define PROBE_MS	200
#define LEGACY_MS	10000	/* blade without push mode: look again */
#define RESCAN_MS	10000	/* empty port with a link: rescan at most this often */
#define SILENT_MS	(5 * OMI_PUSH_REFRESH_MS)
#define LEAVE_MAX_MS	1000
#define GW2_SLOT	OMI_GW_SLOT
#define BUSY_NS		(20 * NSEC_PER_MSEC)

enum rc_state {
	RS_NONE,	/* no endpoint enumerated behind the port */
	RS_QUIET,	/* hands off until quiet_until and a stable link */
	RS_PROBE,	/* config space: restore, check push support */
	RS_OFFER,	/* offer made, waiting for its state */
	RS_ACTIVE,	/* in the peer tables */
	RS_LEAVING,	/* left the tables, waiting for the peers */
};

struct rc_ep {
	/* ctl thread, under rc->ctl_lock */
	enum rc_state state;
	u8 node;
	struct pci_dev *port;		/* switch downstream port */
	struct pci_dev *pdev;		/* the blade, NULL until enumerated */
	bool link;
	unsigned long link_since, link_next;
	u32 link_downs;
	unsigned long quiet_until, next, offer_at, leave_start, seen_at;
	unsigned long need_down;	/* 0, or probe only after a drop until then */
	u32 down_seen;
	unsigned long rescan_at;
	u32 mps_cap;			/* DEVCAP MPS + 1, 0: unknown */
	bool warned;
	u64 bar_pci;			/* bus address of its BAR0 */
	u32 gen;			/* table_gen last written */
	u32 pub_epoch[OMI_MAX_NODES];
	u32 last_seq;

	/* Set by the ctl thread before gw_on (release), read by NAPI/xmit. */
	void __iomem *bar;		/* written, never read */
	void *area;			/* our memory it pushes into */
	dma_addr_t area_dma;
	size_t area_size;
	u32 token;
	u32 epoch;
	u8 mac[ETH_ALEN];
	u32 features, db_off, db_data;
	bool gw_on;			/* gateway usable */

	u32 tail;			/* blade -> BMC ring, NAPI */
	spinlock_t txlock;		/* BMC -> blade ring */
	u32 rc_head, rc_tail;
};

struct rc {
	struct net_device *ndev;
	struct napi_struct napi;
	struct hrtimer timer;
	u64 last_rx;
	struct task_struct *ctl;
	struct mutex ctl_lock;
	bool tables_dirty;
	int nports;
	struct rc_ep ep[OMI_MAX_NODES];
};

static struct rc *rc;

#define BAR_OFF(f)	offsetof(struct omi_bar_head, f)
#define CTL_OFF(f)	(BAR_OFF(ctl) + offsetof(struct omi_ctl, f))
#define GW_OFF(f)	(OMI_GW_OFF + offsetof(struct omi_gw, f))

static void log_ep(struct rc_ep *e, const char *msg)
{
	netdev_info(rc->ndev, "node %u %s: %s\n", e->node, pci_name(e->port), msg);
}

static size_t area_bytes(void)
{
	return OMI_GW2_SLOT0 + (size_t)slots * GW2_SLOT;
}

static struct device *area_dev(struct rc_ep *e)
{
	return &e->port->dev;
}

/* The state the blade pushed, fresh from memory. */
static struct omi_gw2_ep *ep_state(struct rc_ep *e)
{
	dma_sync_single_range_for_cpu(area_dev(e), e->area_dma, OMI_GW2_EP_OFF,
				      sizeof(struct omi_gw2_ep), DMA_BIDIRECTIONAL);
	return e->area + OMI_GW2_EP_OFF;
}

static void ep_state_done(struct rc_ep *e)
{
	dma_sync_single_range_for_device(area_dev(e), e->area_dma, OMI_GW2_EP_OFF,
					 sizeof(struct omi_gw2_ep), DMA_BIDIRECTIONAL);
}

/* ---------------------------------------------------------------- */
/* Link of the switch port                                           */

/*
 * Sample the port's link (at most every LINK_POLL_MS unless force).
 * True when it is up and has been for link_stable_ms.
 */
static bool link_ok(struct rc_ep *e, bool force)
{
	u16 sta = 0;
	bool up;

	if (force || time_after_eq(jiffies, e->link_next)) {
		e->link_next = jiffies + msecs_to_jiffies(LINK_POLL_MS);
		if (pcie_capability_read_word(e->port, PCI_EXP_LNKSTA, &sta))
			sta = 0;
		up = sta != 0xffff && (sta & PCI_EXP_LNKSTA_DLLLA);
		if (up && !e->link)
			e->link_since = jiffies;
		if (!up && e->link) {
			e->link_downs++;
			log_ep(e, "link down at the switch port");
		}
		e->link = up;
	}
	return e->link && time_after_eq(jiffies, e->link_since + msecs_to_jiffies(link_stable_ms));
}

/* ---------------------------------------------------------------- */
/* Gateway: BMC -> blade                                             */

static u32 rc_credit(struct rc_ep *e)
{
	struct omi_gw2_ep *g = ep_state(e);
	u32 t = READ_ONCE(g->rc_tail);

	if (READ_ONCE(g->token) == e->token && e->rc_head - t <= OMI_GW_SLOTS)
		e->rc_tail = t;
	ep_state_done(e);
	return e->rc_tail;
}

static void __iomem *rc_slot(struct rc_ep *e, u32 i)
{
	return e->bar + OMI_GW_OFF + sizeof(struct omi_gw) +
	       (OMI_GW_SLOTS + (i & (OMI_GW_SLOTS - 1))) * OMI_GW_SLOT;
}

/* One frame into blade e's gateway ring, then its doorbell. Posted
 * writes only. False if its ring is full or it is not up.
 */
static bool tx_ep(struct rc_ep *e, const void *data, u32 len, u32 flags)
{
	void __iomem *slot;
	bool ok = false;

	spin_lock(&e->txlock);
	if (!e->gw_on || len > OMI_GW_DATA)
		goto out;
	if (e->rc_head - rc_credit(e) >= OMI_GW_SLOTS)
		goto out;
	slot = rc_slot(e, e->rc_head);
	memcpy_toio(slot + sizeof(struct omi_slot_hdr), data, len);
	writel(flags, slot + offsetof(struct omi_slot_hdr, flags));
	writel(0, slot + offsetof(struct omi_slot_hdr, mask));
	writel(len, slot + offsetof(struct omi_slot_hdr, len));
	e->rc_head++;
	/* writel() keeps the slot stores ahead of the head store. */
	writel(e->rc_head, e->bar + GW_OFF(rc_head));
	if (e->features & OMI_HDR_DOORBELL)
		writel(e->db_data, e->bar + e->db_off);
	ok = true;
out:
	spin_unlock(&e->txlock);
	return ok;
}

static int ep_by_mac(const u8 *mac)
{
	int k;

	for (k = 0; k < OMI_MAX_NODES; k++)
		if (smp_load_acquire(&rc->ep[k].gw_on) && ether_addr_equal(rc->ep[k].mac, mac))
			return k;
	return -1;
}

/*
 * A frame from node src (src < 0: from our stack). mask: nodes that
 * already have it. Returns true if our stack should get it too.
 */
static bool relay(int src, const u8 *frame, u32 len, u32 mask)
{
	struct net_device *ndev = rc->ndev;
	u32 flags = src >= 0 ? OMI_GW_RELAYED : 0;
	int k;

	if (src >= 0)
		mask |= BIT(src);
	if (!is_multicast_ether_addr(frame)) {
		if (ether_addr_equal(frame, ndev->dev_addr))
			return src >= 0;
		k = ep_by_mac(frame);
		if (k >= 0) {
			if (!(mask & BIT(k)) && !tx_ep(&rc->ep[k], frame, len, flags))
				ndev->stats.tx_dropped++;
			return false;
		}
	}
	/* Broadcast, multicast, unknown unicast: everyone else. */
	for (k = 0; k < OMI_MAX_NODES; k++)
		if (smp_load_acquire(&rc->ep[k].gw_on) && !(mask & BIT(k)) &&
		    !tx_ep(&rc->ep[k], frame, len, flags))
			ndev->stats.tx_dropped++;
	return src >= 0;
}

static netdev_tx_t rc_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	unsigned int len = skb->len;

	if (len < ETH_HLEN || len > OMI_GW_DATA || skb_linearize(skb)) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	local_bh_disable();
	relay(-1, skb->data, len, 0);
	local_bh_enable();
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---------------------------------------------------------------- */
/* Gateway: blade -> BMC                                             */

static int rx_ep(struct rc_ep *e, int budget)
{
	struct net_device *ndev = rc->ndev;
	struct device *dev = area_dev(e);
	struct omi_prod *prod = e->area;
	u32 head, avail, n = slots;
	int done = 0;

	dma_sync_single_range_for_cpu(dev, e->area_dma, 0, sizeof(*prod), DMA_BIDIRECTIONAL);
	head = READ_ONCE(prod->head);
	dma_sync_single_range_for_device(dev, e->area_dma, 0, sizeof(*prod), DMA_BIDIRECTIONAL);
	avail = head - e->tail;
	if (avail > n) {
		e->tail = head;		/* garbage, or a new ring: resync */
		return 0;
	}
	while (avail && done < budget) {
		size_t off = OMI_GW2_SLOT0 + (size_t)(e->tail & (n - 1)) * GW2_SLOT;
		struct omi_slot_hdr *h = e->area + off;
		struct sk_buff *skb;
		u32 len, mask;

		dma_sync_single_range_for_cpu(dev, e->area_dma, off, sizeof(*h), DMA_BIDIRECTIONAL);
		len = READ_ONCE(h->len);
		mask = READ_ONCE(h->mask);
		if (len >= ETH_HLEN && len <= GW2_SLOT - sizeof(*h)) {
			dma_sync_single_range_for_cpu(dev, e->area_dma, off + sizeof(*h), len,
						      DMA_BIDIRECTIONAL);
			if (relay(e->node, (const u8 *)(h + 1), len, mask)) {
				skb = napi_alloc_skb(&rc->napi, len);
				if (skb) {
					skb_put_data(skb, h + 1, len);
					skb->protocol = eth_type_trans(skb, ndev);
					ndev->stats.rx_packets++;
					ndev->stats.rx_bytes += len;
					napi_gro_receive(&rc->napi, skb);
				} else {
					ndev->stats.rx_dropped++;
				}
			}
		} else {
			ndev->stats.rx_errors++;
		}
		/* Lines we read must not linger over the next frame. */
		dma_sync_single_range_for_device(dev, e->area_dma, off, GW2_SLOT, DMA_BIDIRECTIONAL);
		e->tail++;
		avail--;
		done++;
	}
	if (done) {
		/* Credit: a posted write into the blade's BAR. */
		writel(e->tail, e->bar + BAR_OFF(tx_cons[OMI_RC_NODE].tail));
		rc->last_rx = ktime_get_ns();
	}
	return done;
}

static int rc_poll(struct napi_struct *napi, int budget)
{
	int work = 0, k;

	for (k = 0; k < OMI_MAX_NODES && work < budget; k++) {
		struct rc_ep *e = &rc->ep[k];

		if (smp_load_acquire(&e->gw_on))
			work += rx_ep(e, budget - work);
	}
	if (work < budget && napi_complete_done(napi, work)) {
		u64 t = ktime_get_ns() - rc->last_rx < BUSY_NS ? poll_us : idle_us;

		hrtimer_start(&rc->timer, ns_to_ktime(t * NSEC_PER_USEC), HRTIMER_MODE_REL);
	}
	return work;
}

static enum hrtimer_restart rc_timer(struct hrtimer *t)
{
	napi_schedule(&rc->napi);
	return HRTIMER_NORESTART;
}

/* ---------------------------------------------------------------- */
/* Blades                                                            */

/* Stop the gateway towards e; afterwards nothing touches its BAR. */
static void gw_off(struct rc_ep *e)
{
	if (!e->gw_on)
		return;
	spin_lock_bh(&e->txlock);
	WRITE_ONCE(e->gw_on, false);
	spin_unlock_bh(&e->txlock);
	/* NAPI and xmit that saw gw_on finish before we unmap. */
	synchronize_net();
}

/*
 * Leave e alone: no request to it until quiet_ms passed and its link is
 * stable again. The memory it may still push into stays allocated.
 */
static void ep_drop(struct rc_ep *e, const char *why, unsigned int quiet_ms)
{
	log_ep(e, why);
	gw_off(e);
	if (e->bar) {
		iounmap(e->bar);
		e->bar = NULL;
	}
	e->state = RS_QUIET;
	e->quiet_until = jiffies + msecs_to_jiffies(quiet_ms);
	e->warned = false;
	rc->tables_dirty = true;
}

/* Lowest MPS every known blade and every port in front of one supports. */
static u32 common_mps(void)
{
	u32 mps = 5;
	u16 ctl;
	int i;

	for (i = 0; i < rc->nports; i++) {
		struct rc_ep *e = &rc->ep[i];

		if (e->mps_cap && e->mps_cap - 1 < mps)
			mps = e->mps_cap - 1;
		if (e->pdev && !pcie_capability_read_word(e->port, PCI_EXP_DEVCTL, &ctl) &&
		    ((ctl & PCI_EXP_DEVCTL_PAYLOAD) >> 5) < mps)
			mps = (ctl & PCI_EXP_DEVCTL_PAYLOAD) >> 5;
	}
	return mps == 5 ? 0 : mps;
}

/*
 * Config space, the only requests that go to the blade itself, once
 * per attach and only with a stable link: put back what its probe
 * reset. 0 when it takes push-mode offers.
 */
static int ep_restore(struct rc_ep *e)
{
	struct pci_dev *pdev = e->pdev;
	u32 id, ss, lo, hi, cap;
	u16 cmd;

	if (pci_read_config_dword(pdev, PCI_VENDOR_ID, &id) ||
	    id != (OPENMIOP_PCI_DEVICE << 16 | OPENMIOP_PCI_VENDOR))
		return -ENODEV;
	if (pci_read_config_dword(pdev, PCI_SUBSYSTEM_VENDOR_ID, &ss) ||
	    ss != (OMI_SSID_PUSH << 16 | OMI_SSVID_PUSH)) {
		if (!e->warned) {
			log_ep(e, "blade driver without push mode, left alone (update openmiop-ep)");
			e->warned = true;
		}
		return -EOPNOTSUPP;
	}
	e->bar_pci = pci_bus_address(pdev, 0);
	if (!e->bar_pci || pci_resource_len(pdev, 0) < OPENMIOP_BAR_SIZE) {
		if (!e->warned) {
			log_ep(e, "BAR0 has no address; the bridge windows need re-sizing");
			e->warned = true;
		}
		return -ENOSPC;
	}
	pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &lo);
	pci_read_config_dword(pdev, PCI_BASE_ADDRESS_1, &hi);
	if ((lo & ~0xfu) != lower_32_bits(e->bar_pci) || hi != upper_32_bits(e->bar_pci)) {
		pci_write_config_dword(pdev, PCI_BASE_ADDRESS_0, lower_32_bits(e->bar_pci));
		pci_write_config_dword(pdev, PCI_BASE_ADDRESS_1, upper_32_bits(e->bar_pci));
		log_ep(e, "BAR0 restored");
	}
	if (!pcie_capability_read_dword(pdev, PCI_EXP_DEVCAP, &cap))
		e->mps_cap = (cap & PCI_EXP_DEVCAP_PAYLOAD) + 1;
	pcie_capability_clear_and_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_PAYLOAD,
					   common_mps() << 5);
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	if ((cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) !=
	    (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
		pci_write_config_word(pdev, PCI_COMMAND,
				      cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	return 0;
}

static int ep_area(struct rc_ep *e)
{
	struct device *dev = area_dev(e);

	if (e->area)
		return 0;
	/* Physically contiguous and 64 KiB aligned, as the blade's
	 * outbound window wants (buddy blocks are aligned to their size).
	 * The MT7620A maps PCI 0..256 MiB onto its RAM 1:1.
	 */
	e->area_size = PAGE_SIZE << get_order(max_t(size_t, area_bytes(), OMI_GW2_ALIGN));
	e->area = alloc_pages_exact(e->area_size, GFP_KERNEL | __GFP_ZERO);
	if (!e->area)
		return -ENOMEM;
	e->area_dma = dma_map_single(dev, e->area, e->area_size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, e->area_dma) || (e->area_dma & (OMI_GW2_ALIGN - 1))) {
		if (!dma_mapping_error(dev, e->area_dma))
			dma_unmap_single(dev, e->area_dma, e->area_size, DMA_BIDIRECTIONAL);
		free_pages_exact(e->area, e->area_size);
		e->area = NULL;
		return -ENOMEM;
	}
	return 0;
}

/*
 * Push-mode offer, posted writes into its BAR: the gateway v2 ring and
 * state line in our memory, acked in tx_cons[OMI_RC_NODE] first, token
 * last, OMI_RC_PUSH without OMI_RC_UP (that comes with activation).
 */
static void ep_offer(struct rc_ep *e)
{
	void __iomem *b = e->bar;
	u32 token;
	int i;

	do {
		token = get_random_u32();
	} while (!token || token == e->token);
	e->token = token;
	/* A state line from an earlier offer must not pass for this one. */
	memset(e->area + OMI_GW2_EP_OFF, 0, sizeof(struct omi_gw2_ep));
	dma_sync_single_range_for_device(area_dev(e), e->area_dma, OMI_GW2_EP_OFF,
					 sizeof(struct omi_gw2_ep), DMA_BIDIRECTIONAL);
	writel(0, b + BAR_OFF(tx_cons[OMI_RC_NODE].tail));
	writel(token, b + BAR_OFF(tx_cons[OMI_RC_NODE].ack));
	for (i = 0; i < ETH_ALEN; i++)
		writeb(rc->ndev->dev_addr[i], b + CTL_OFF(mac) + i);
	writeb(e->node, b + CTL_OFF(self_idx));
	writel(0, b + CTL_OFF(ep_epoch));
	writel(lower_32_bits(e->area_dma), b + CTL_OFF(gw2_ring_lo));
	writel(upper_32_bits(e->area_dma), b + CTL_OFF(gw2_ring_hi));
	writel(slots, b + CTL_OFF(gw2_slots));
	writel(GW2_SLOT, b + CTL_OFF(gw2_slot_size));
	writel(token, b + CTL_OFF(gw2_token));
	writel(OMI_RC_PUSH, b + CTL_OFF(flags));
	e->offer_at = jiffies;
}

static void ep_activate(struct rc_ep *e, struct omi_gw2_ep *st)
{
	void __iomem *b = e->bar;
	int i;

	e->epoch = READ_ONCE(st->epoch);
	memcpy(e->mac, st->mac, ETH_ALEN);
	e->features = READ_ONCE(st->features);
	e->db_off = READ_ONCE(st->db_off);
	e->db_data = READ_ONCE(st->db_data);
	if (e->db_off & 3 || e->db_off > OPENMIOP_BAR_SIZE - 4)
		e->features &= ~OMI_HDR_DOORBELL;
	e->rc_head = e->rc_tail = READ_ONCE(st->rc_tail);
	e->last_seq = READ_ONCE(st->seq);
	e->seen_at = jiffies;
	e->tail = 0;
	for (i = 0; i < OMI_MAX_NODES; i++)
		e->pub_epoch[i] = U32_MAX;	/* full table on the next publish */
	writel(e->rc_head, b + GW_OFF(rc_head));
	writel(e->epoch, b + CTL_OFF(ep_epoch));
	writel(OMI_RC_UP | OMI_RC_PUSH, b + CTL_OFF(flags));
	spin_lock_bh(&e->txlock);
	smp_store_release(&e->gw_on, true);
	spin_unlock_bh(&e->txlock);
	e->state = RS_ACTIVE;
	rc->tables_dirty = true;
	netdev_info(rc->ndev, "node %u active: %pM, BAR %#llx, epoch %#x%s\n", e->node, e->mac,
		    (u64)e->bar_pci, e->epoch,
		    e->features & OMI_HDR_DOORBELL ? ", doorbell" : "");
	napi_schedule(&rc->napi);
}

/* Write node k's entry into e's table if it changed. */
static bool table_put(struct rc_ep *e, int k, struct rc_ep *src)
{
	void __iomem *pe = e->bar + BAR_OFF(peers[k]);
	u32 want = src ? src->epoch : 0;
	int i;

	if (e->pub_epoch[k] == want)
		return false;
	writel(0, pe + offsetof(struct omi_peer_entry, epoch));
	if (src) {
		writel(lower_32_bits(src->bar_pci), pe + offsetof(struct omi_peer_entry, bar_lo));
		writel(upper_32_bits(src->bar_pci), pe + offsetof(struct omi_peer_entry, bar_hi));
		for (i = 0; i < ETH_ALEN; i++)
			writeb(src->mac[i], pe + offsetof(struct omi_peer_entry, mac) + i);
		/* writel() orders it after the fields. */
		writel(want, pe + offsetof(struct omi_peer_entry, epoch));
	}
	e->pub_epoch[k] = want;
	return true;
}

static void tables_publish(void)
{
	int i, k;

	rc->tables_dirty = false;
	for (i = 0; i < rc->nports; i++) {
		struct rc_ep *e = &rc->ep[i];
		bool changed = false;

		if (e->state != RS_ACTIVE)
			continue;
		for (k = 0; k < OMI_MAX_NODES; k++) {
			struct rc_ep *src = k < rc->nports ? &rc->ep[k] : NULL;

			if (k == i)
				continue;
			changed |= table_put(e, k, src && src->state == RS_ACTIVE ? src : NULL);
		}
		if (changed) {
			e->gen++;
			writel(e->gen, e->bar + CTL_OFF(table_gen));
		}
	}
}

/* Every active blade applied the table that no longer lists the leaver? */
static bool peers_detached(void)
{
	int i;

	for (i = 0; i < rc->nports; i++) {
		struct rc_ep *e = &rc->ep[i];
		struct omi_gw2_ep *st;
		bool behind;

		if (e->state != RS_ACTIVE)
			continue;
		st = ep_state(e);
		behind = (s32)(READ_ONCE(st->table_seen) - e->gen) < 0;
		ep_state_done(e);
		if (behind)
			return false;
	}
	return true;
}

static void ep_find(struct rc_ep *e)
{
	struct pci_dev *pdev;

	if (e->pdev || !e->port->subordinate)
		return;
	pdev = pci_get_slot(e->port->subordinate, PCI_DEVFN(0, 0));
	if (!pdev)
		return;
	if (pdev->vendor != OPENMIOP_PCI_VENDOR || pdev->device != OPENMIOP_PCI_DEVICE) {
		pci_dev_put(pdev);
		return;
	}
	e->pdev = pdev;
	e->state = RS_QUIET;
	e->quiet_until = jiffies;
	log_ep(e, pci_name(pdev));
}

/* A blade that trained its link after we (or Linux) enumerated. */
static void ep_rescan(struct rc_ep *e)
{
	if (e->pdev || !link_ok(e, false) || time_before(jiffies, e->rescan_at))
		return;
	e->rescan_at = jiffies + msecs_to_jiffies(RESCAN_MS);
	log_ep(e, "link up with nothing enumerated, rescanning");
	pci_lock_rescan_remove();
	pci_rescan_bus(e->port->subordinate ? e->port->subordinate : e->port->bus);
	pci_unlock_rescan_remove();
	ep_find(e);
}

static void ep_step(struct rc_ep *e)
{
	struct omi_gw2_ep *st;
	bool ok;

	if (e->state == RS_NONE) {
		ep_find(e);
		if (e->state == RS_NONE)
			ep_rescan(e);
		return;
	}
	ok = link_ok(e, false);
	if (e->bar && !e->link && e->state != RS_LEAVING) {
		/* Not a single request more. */
		ep_drop(e, "link lost", 0);
		return;
	}

	switch (e->state) {
	case RS_NONE:
		return;
	case RS_QUIET:
		if (time_before(jiffies, e->quiet_until) || !ok)
			return;
		/* After a leave or release its link drops soon: until then
		 * the link being up means nothing.
		 */
		if (e->need_down && e->link_downs == e->down_seen &&
		    time_before(jiffies, e->need_down))
			return;
		e->need_down = 0;
		e->state = RS_PROBE;
		e->next = jiffies;
		return;
	case RS_PROBE:
		if (time_before(jiffies, e->next) || !link_ok(e, true))
			return;
		e->next = jiffies + msecs_to_jiffies(PROBE_MS);
		switch (ep_restore(e)) {
		case 0:
			break;
		case -EOPNOTSUPP:
			e->next = jiffies + msecs_to_jiffies(LEGACY_MS);
			return;
		default:
			return;
		}
		if (ep_area(e))
			return;
		e->bar = ioremap(pci_resource_start(e->pdev, 0), OPENMIOP_BAR_SIZE);
		if (!e->bar)
			return;
		ep_offer(e);
		e->state = RS_OFFER;
		e->next = jiffies;
		return;
	case RS_OFFER:
		if (time_before(jiffies, e->next) || !link_ok(e, true))
			return;
		e->next = jiffies + msecs_to_jiffies(STATUS_MS);
		st = ep_state(e);
		if (READ_ONCE(st->token) == e->token && READ_ONCE(st->magic) == OPENMIOP_MAGIC &&
		    READ_ONCE(st->version) == OPENMIOP_VERSION &&
		    (READ_ONCE(st->flags) & (OMI_F_UP | OMI_F_DOWN)) == OMI_F_UP) {
			ep_activate(e, st);
			ep_state_done(e);
			return;
		}
		ep_state_done(e);
		if (time_after(jiffies, e->offer_at + msecs_to_jiffies(OFFER_RETRY_MS))) {
			if (!e->warned) {
				log_ep(e, "no state pushed yet, offering again");
				e->warned = true;
			}
			ep_offer(e);
		}
		return;
	case RS_LEAVING:
		/* Its link drops right after the ack. */
		if (!e->link || peers_detached() ||
		    time_after(jiffies, e->leave_start + msecs_to_jiffies(LEAVE_MAX_MS))) {
			if (e->link)
				writel(e->epoch, e->bar + CTL_OFF(down_ack));
			ep_drop(e, "detached", 3000);
			e->need_down = jiffies + msecs_to_jiffies(LEAVE_DOWN_MS);
			e->down_seen = e->link_downs - (e->link ? 0 : 1);
		}
		return;
	case RS_ACTIVE:
		break;
	}

	if (time_before(jiffies, e->next))
		return;
	e->next = jiffies + msecs_to_jiffies(STATUS_MS);
	st = ep_state(e);
	if (READ_ONCE(st->token) != e->token || READ_ONCE(st->magic) != OPENMIOP_MAGIC ||
	    READ_ONCE(st->epoch) != e->epoch) {
		ep_state_done(e);
		ep_drop(e, "reset without leaving", 500);
		return;
	}
	if (READ_ONCE(st->flags) & OMI_F_DOWN) {
		ep_state_done(e);
		log_ep(e, "leaving");
		gw_off(e);
		e->state = RS_LEAVING;
		e->leave_start = jiffies;
		rc->tables_dirty = true;
		return;
	}
	if (READ_ONCE(st->seq) != e->last_seq) {
		e->last_seq = READ_ONCE(st->seq);
		e->seen_at = jiffies;
	}
	ep_state_done(e);
	/* Link up but nothing pushed for a while: its kernel is gone. */
	if (time_after(jiffies, e->seen_at + msecs_to_jiffies(SILENT_MS)))
		ep_drop(e, "no state from the blade for 5 s", 0);
}

static int ctl_thread(void *data)
{
	int i;

	while (!kthread_should_stop()) {
		mutex_lock(&rc->ctl_lock);
		for (i = 0; i < rc->nports; i++)
			ep_step(&rc->ep[i]);
		if (rc->tables_dirty)
			tables_publish();
		mutex_unlock(&rc->ctl_lock);
		usleep_range(1000, 2000);
	}
	return 0;
}

/*
 * nodectl is about to reset a blade or cut its power: its link will
 * drop without a leave. Writing the switch port ("02:0c.0" or
 * "0000:02:0c.0") returns once nothing goes to that blade any more,
 * until its link has dropped and come back stable.
 */
static int release_set(const char *val, const struct kernel_param *kp)
{
	char name[16];
	int i, ret = -ENODEV;

	if (!rc || sscanf(val, "%15s", name) != 1)
		return -EINVAL;
	mutex_lock(&rc->ctl_lock);
	for (i = 0; i < rc->nports; i++) {
		struct rc_ep *e = &rc->ep[i];
		const char *pn = pci_name(e->port);
		size_t ln = strlen(name), lp = strlen(pn);

		if (ln > lp || strcmp(pn + lp - ln, name))
			continue;
		if (e->state != RS_NONE && e->state != RS_QUIET)
			ep_drop(e, "released for reset or power off", 0);
		else
			log_ep(e, "released for reset or power off");
		e->need_down = jiffies + msecs_to_jiffies(LEAVE_DOWN_MS);
		e->down_seen = e->link_downs - (e->link ? 0 : 1);
		ret = 0;
	}
	mutex_unlock(&rc->ctl_lock);
	return ret;
}

static const struct kernel_param_ops release_ops = {
	.set = release_set,
};
module_param_cb(release, &release_ops, NULL, 0200);
MODULE_PARM_DESC(release, "Switch port (e.g. 02:0c.0) of a blade about to be reset or powered off");

/* ---------------------------------------------------------------- */
/* netdev and module                                                 */

static int rc_open(struct net_device *ndev)
{
	napi_enable(&rc->napi);
	hrtimer_start(&rc->timer, ns_to_ktime((u64)idle_us * NSEC_PER_USEC), HRTIMER_MODE_REL);
	netif_start_queue(ndev);
	return 0;
}

static int rc_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	hrtimer_cancel(&rc->timer);
	napi_disable(&rc->napi);
	hrtimer_cancel(&rc->timer);
	return 0;
}

static const struct net_device_ops rc_ops = {
	.ndo_open = rc_open,
	.ndo_stop = rc_stop,
	.ndo_start_xmit = rc_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

/* The address openmiop-rc used: eth0's, locally administered, last
 * byte changed. Blades and neighbours keep their ARP entries when we
 * replace the helper.
 */
static void rc_mac(struct net_device *ndev)
{
	struct net_device *eth;
	u8 mac[ETH_ALEN];

	eth = dev_get_by_name(&init_net, "eth0");
	if (!eth) {
		eth_hw_addr_random(ndev);
		return;
	}
	ether_addr_copy(mac, eth->dev_addr);
	dev_put(eth);
	mac[0] = (mac[0] & 0xfc) | 0x02;
	mac[5] ^= 0x4d;
	eth_hw_addr_set(ndev, mac);
}

/* Switch downstream ports, numbered by their rank on the bus: the same
 * node index for a slot whatever is plugged in, as openmiop-rc did.
 */
static int find_ports(void)
{
	struct pci_dev *dev = NULL;
	struct pci_bus *bus = NULL;
	int n = 0;

	while ((dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev))) {
		if (!pci_is_pcie(dev) || pci_pcie_type(dev) != PCI_EXP_TYPE_DOWNSTREAM)
			continue;
		if (!bus)
			bus = dev->bus;
		if (dev->bus != bus || n >= OMI_RC_NODE)
			continue;
		rc->ep[n].port = pci_dev_get(dev);
		rc->ep[n].node = n;
		n++;
	}
	return n;
}

static void ep_release_all(void)
{
	int i;

	for (i = 0; i < OMI_MAX_NODES; i++) {
		struct rc_ep *e = &rc->ep[i];

		/* Withdraw the offer: the blade stops pushing into memory
		 * we are about to free (it looks every millisecond).
		 */
		if (e->bar && e->link) {
			writel(0, e->bar + CTL_OFF(flags));
			writel(0, e->bar + CTL_OFF(gw2_token));
		}
	}
	msleep(100);
	for (i = 0; i < OMI_MAX_NODES; i++) {
		struct rc_ep *e = &rc->ep[i];

		gw_off(e);
		if (e->bar)
			iounmap(e->bar);
		if (e->area) {
			dma_unmap_single(area_dev(e), e->area_dma, e->area_size, DMA_BIDIRECTIONAL);
			free_pages_exact(e->area, e->area_size);
		}
		if (e->pdev)
			pci_dev_put(e->pdev);
		if (e->port)
			pci_dev_put(e->port);
	}
}

static int __init omi_rc_init(void)
{
	struct net_device *ndev;
	int i, ret;

	BUILD_BUG_ON(sizeof(struct omi_gw2_ep) != 64);
	if (!is_power_of_2(slots) || slots < 2 || slots > 1024)
		return -EINVAL;
	ndev = alloc_etherdev(sizeof(*rc));
	if (!ndev)
		return -ENOMEM;
	rc = netdev_priv(ndev);
	rc->ndev = ndev;
	mutex_init(&rc->ctl_lock);
	for (i = 0; i < OMI_MAX_NODES; i++)
		spin_lock_init(&rc->ep[i].txlock);
	rc->nports = find_ports();
	if (!rc->nports) {
		pr_err(DRV_NAME ": no PCIe switch downstream ports\n");
		free_netdev(ndev);
		rc = NULL;
		return -ENODEV;
	}
	for (i = 0; i < rc->nports; i++)
		rc->ep[i].gen = get_random_u32();	/* never the blade's stale one */
	hrtimer_init(&rc->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rc->timer.function = rc_timer;
	strscpy(ndev->name, "omi0", IFNAMSIZ);
	ndev->netdev_ops = &rc_ops;
	ndev->mtu = 9000;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = OMI_GW_DATA - ETH_HLEN;
	ndev->features |= NETIF_F_GRO;
	rc_mac(ndev);
	netif_napi_add(ndev, &rc->napi, rc_poll, NAPI_POLL_WEIGHT);
	ret = register_netdev(ndev);
	if (ret)
		goto err_netdev;
	rc->ctl = kthread_run(ctl_thread, NULL, "omi-rc");
	if (IS_ERR(rc->ctl)) {
		ret = PTR_ERR(rc->ctl);
		unregister_netdev(ndev);
		goto err_netdev;
	}
	netdev_info(ndev, "openmiop root complex, push mode, %d switch ports, %u gateway slots\n",
		    rc->nports, slots);
	return 0;

err_netdev:
	netif_napi_del(&rc->napi);
	for (i = 0; i < rc->nports; i++)
		pci_dev_put(rc->ep[i].port);
	free_netdev(ndev);
	rc = NULL;
	return ret;
}

static void __exit omi_rc_exit(void)
{
	struct net_device *ndev = rc->ndev;

	kthread_stop(rc->ctl);
	unregister_netdev(ndev);
	ep_release_all();
	netif_napi_del(&rc->napi);
	rc = NULL;
	free_netdev(ndev);
}

module_init(omi_rc_init);
module_exit(omi_rc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("openmiop: Cluster Box root complex side (fabric, peer tables, omi0), push mode");
