// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * omi-gw: the Cluster Box BMC side of the openmiop gateway, in the
 * kernel (gateway v2, OMI_HDR_GW2).
 *
 * The BMC (MT7620A, the PCIe root complex) is on the openmiop segment as
 * omi0. With the v1 gateway the userspace helper copied every frame out
 * of a blade's BAR with CPU reads over PCIe, about 15 Mbit/s. Here:
 *
 *  - blade -> BMC: each blade sends its gateway frames with its eDMA
 *    into a ring in BMC memory (as it does to another blade), so the
 *    BMC never reads PCIe; it invalidates the slot and copies the frame
 *    into an skb;
 *  - BMC -> blade: posted writes into the v1 gateway ring of the
 *    blade's BAR, then the blade's doorbell (an MSI on the blade). The
 *    blade reports how far it consumed into BMC memory through an
 *    outbound window, so again nothing is read over PCIe;
 *  - omi0 is a kernel netdev with NAPI, polled from an hrtimer (no
 *    interrupt from the blades): every poll_us while traffic is recent,
 *    every idle_us otherwise.
 *
 * openmiop-rc stays in charge of the fabric (enumeration, peer table,
 * leave handshake, re-enumeration). Run as "openmiop-rc -k", it does not
 * create a TAP or move gateway frames; it tells this module when a node
 * becomes active or goes away by writing "up NODE BDF EPOCH" or
 * "down NODE" to /sys/module/omi_gw/parameters/cmd. Blades without
 * OMI_HDR_GW2 get no gateway in this mode.
 *
 * Frames that arrive for another blade (peers still connecting, floods)
 * are relayed, as the helper did.
 */
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/random.h>
#include <linux/skbuff.h>

#include "openmiop.h"

#define DRV_NAME	"omi-gw"

static uint slots = 32;
module_param(slots, uint, 0444);
MODULE_PARM_DESC(slots, "Slots per blade->BMC ring (power of two)");

static uint poll_us = 100;
module_param(poll_us, uint, 0644);
MODULE_PARM_DESC(poll_us, "Poll period while traffic is recent, us");

static uint idle_us = 1000;
module_param(idle_us, uint, 0644);
MODULE_PARM_DESC(idle_us, "Poll period when idle, us");

#define GW2_SLOT	OMI_GW_SLOT	/* same frames as the v1 gateway */
#define BUSY_NS		(20 * NSEC_PER_MSEC)

struct gw_ep {
	/* gw->lock (changes); readers in NAPI and xmit check active */
	bool active;
	u8 node;
	struct pci_dev *pdev;
	void __iomem *bar;
	u32 epoch;
	u8 mac[ETH_ALEN];
	u32 features, db_off, db_data;

	/* blade -> BMC ring in our memory (gateway v2) */
	void *area;
	dma_addr_t area_dma;
	size_t area_size;
	u32 token;
	u32 tail;			/* NAPI */

	/* BMC -> blade: v1 gateway ring in its BAR (ep->txlock) */
	spinlock_t txlock;
	u32 rc_head;
	u32 rc_tail;			/* last credit seen */
};

struct gw {
	struct net_device *ndev;
	struct napi_struct napi;
	struct hrtimer timer;
	spinlock_t lock;
	u64 last_rx;			/* ktime ns of the last frame */
	struct gw_ep ep[OMI_MAX_NODES];
};

static struct gw *gw;

#define BAR_OFF(f)	offsetof(struct omi_bar_head, f)
#define GW_OFF(f)	(OMI_GW_OFF + offsetof(struct omi_gw, f))

static void __iomem *rc_slot(struct gw_ep *e, u32 i)
{
	return e->bar + OMI_GW_OFF + sizeof(struct omi_gw) +
	       (OMI_GW_SLOTS + (i & (OMI_GW_SLOTS - 1))) * OMI_GW_SLOT;
}

static size_t area_bytes(void)
{
	return OMI_GW2_SLOT0 + (size_t)slots * GW2_SLOT;
}

/* ---------------------------------------------------------------- */
/* BMC -> blade                                                      */

/* Credit for the BMC->blade ring, from our memory (written by the
 * blade's CPU through its window), tagged with the token.
 */
static u32 rc_credit(struct gw_ep *e)
{
	struct omi_gw2_ep *g;

	if (!e->area)
		return e->rc_tail;
	dma_sync_single_range_for_cpu(&e->pdev->dev, e->area_dma, OMI_GW2_EP_OFF,
				      sizeof(*g), DMA_FROM_DEVICE);
	g = e->area + OMI_GW2_EP_OFF;
	if (READ_ONCE(g->token) == e->token && e->rc_head - READ_ONCE(g->rc_tail) <= OMI_GW_SLOTS)
		e->rc_tail = READ_ONCE(g->rc_tail);
	return e->rc_tail;
}

/* Copy one frame into blade e's gateway ring and ring its doorbell.
 * Posted writes only. False if its ring is full.
 */
static bool tx_ep(struct gw_ep *e, const void *data, u32 len, u32 flags)
{
	void __iomem *slot;
	bool ok = false;

	spin_lock(&e->txlock);
	if (!READ_ONCE(e->active) || len > OMI_GW_DATA)
		goto out;
	if (e->rc_head - rc_credit(e) >= OMI_GW_SLOTS)
		goto out;
	slot = rc_slot(e, e->rc_head);
	memcpy_toio(slot + sizeof(struct omi_slot_hdr), data, len);
	writel(flags, slot + offsetof(struct omi_slot_hdr, flags));
	writel(0, slot + offsetof(struct omi_slot_hdr, mask));
	writel(len, slot + offsetof(struct omi_slot_hdr, len));
	e->rc_head++;
	/* writel() orders the slot stores before the head store. */
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
		if (READ_ONCE(gw->ep[k].active) && ether_addr_equal(gw->ep[k].mac, mac))
			return k;
	return -1;
}

/*
 * A frame from node src (src < 0: from our stack). mask: nodes that
 * already have it. Returns true if our stack should get it too.
 */
static bool relay(int src, const u8 *frame, u32 len, u32 mask)
{
	struct net_device *ndev = gw->ndev;
	u32 flags = src >= 0 ? OMI_GW_RELAYED : 0;
	int k;

	if (src >= 0)
		mask |= BIT(src);
	if (!is_multicast_ether_addr(frame)) {
		if (ether_addr_equal(frame, ndev->dev_addr))
			return src >= 0;
		k = ep_by_mac(frame);
		if (k >= 0) {
			if (!(mask & BIT(k)) && !tx_ep(&gw->ep[k], frame, len, flags))
				ndev->stats.tx_dropped++;
			return false;
		}
	}
	/* Broadcast, multicast, unknown unicast: everyone else. */
	for (k = 0; k < OMI_MAX_NODES; k++)
		if (READ_ONCE(gw->ep[k].active) && !(mask & BIT(k)) &&
		    !tx_ep(&gw->ep[k], frame, len, flags))
			ndev->stats.tx_dropped++;
	return src >= 0;
}

static netdev_tx_t gw_xmit(struct sk_buff *skb, struct net_device *ndev)
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
/* blade -> BMC                                                      */

static int rx_ep(struct gw_ep *e, int budget)
{
	struct net_device *ndev = gw->ndev;
	struct device *dev = &e->pdev->dev;
	struct omi_prod *prod = e->area;
	u32 head, avail, n = slots;
	int done = 0;

	dma_sync_single_range_for_cpu(dev, e->area_dma, 0, sizeof(*prod), DMA_FROM_DEVICE);
	head = READ_ONCE(prod->head);
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

		dma_sync_single_range_for_cpu(dev, e->area_dma, off, sizeof(*h), DMA_FROM_DEVICE);
		len = READ_ONCE(h->len);
		mask = READ_ONCE(h->mask);
		if (len >= ETH_HLEN && len <= GW2_SLOT - sizeof(*h)) {
			dma_sync_single_range_for_cpu(dev, e->area_dma, off + sizeof(*h), len,
						      DMA_FROM_DEVICE);
			if (relay(e->node, (const u8 *)(h + 1), len, mask)) {
				skb = napi_alloc_skb(&gw->napi, len);
				if (skb) {
					skb_put_data(skb, h + 1, len);
					skb->protocol = eth_type_trans(skb, ndev);
					ndev->stats.rx_packets++;
					ndev->stats.rx_bytes += len;
					napi_gro_receive(&gw->napi, skb);
				} else {
					ndev->stats.rx_dropped++;
				}
			}
		} else {
			ndev->stats.rx_errors++;
		}
		/* Lines we read must not be written back over the next frame. */
		dma_sync_single_range_for_device(dev, e->area_dma, off, GW2_SLOT, DMA_FROM_DEVICE);
		e->tail++;
		avail--;
		done++;
	}
	if (done) {
		dma_sync_single_range_for_device(dev, e->area_dma, 0, sizeof(*prod), DMA_FROM_DEVICE);
		/* Credit: a posted write into the blade's BAR. */
		writel(e->tail, e->bar + BAR_OFF(tx_cons[OMI_RC_NODE].tail));
		gw->last_rx = ktime_get_ns();
	}
	return done;
}

static int gw_poll(struct napi_struct *napi, int budget)
{
	int work = 0, k;

	for (k = 0; k < OMI_MAX_NODES && work < budget; k++) {
		struct gw_ep *e = &gw->ep[k];

		if (READ_ONCE(e->active) && READ_ONCE(e->token))
			work += rx_ep(e, budget - work);
	}
	if (work < budget && napi_complete_done(napi, work)) {
		u64 t = ktime_get_ns() - gw->last_rx < BUSY_NS ? poll_us : idle_us;

		hrtimer_start(&gw->timer, ns_to_ktime(t * NSEC_PER_USEC), HRTIMER_MODE_REL);
	}
	return work;
}

static enum hrtimer_restart gw_timer(struct hrtimer *t)
{
	napi_schedule(&gw->napi);
	return HRTIMER_NORESTART;
}

/* ---------------------------------------------------------------- */
/* Nodes (commands from openmiop-rc)                                 */

static void ep_down(struct gw_ep *e)
{
	spin_lock_bh(&gw->lock);
	spin_lock(&e->txlock);
	WRITE_ONCE(e->active, false);
	WRITE_ONCE(e->token, 0);
	spin_unlock(&e->txlock);
	spin_unlock_bh(&gw->lock);
	/* No access to this blade from here on: it may be gone. */
	synchronize_net();
	if (e->bar) {
		iounmap(e->bar);
		e->bar = NULL;
	}
	if (e->pdev) {
		if (e->area)
			dma_unmap_single(&e->pdev->dev, e->area_dma, e->area_size, DMA_FROM_DEVICE);
		pci_dev_put(e->pdev);
		e->pdev = NULL;
	}
	if (e->area) {
		free_pages_exact(e->area, e->area_size);
		e->area = NULL;
	}
	netdev_info(gw->ndev, "node %u down\n", e->node);
}

/*
 * openmiop-rc has just activated node n: its BAR answers and holds our
 * control line for epoch. Read the header once and make the gateway v2
 * offer: ack the token in tx_cons[OMI_RC_NODE], then the control fields
 * with the token last.
 */
static int ep_up(unsigned int n, unsigned int bus, unsigned int devfn, u32 epoch)
{
	struct gw_ep *e = &gw->ep[n];
	struct omi_bar_head __iomem *h;
	void __iomem *ctl;
	u32 token;
	int i;

	if (e->active || e->pdev)
		ep_down(e);
	e->pdev = pci_get_domain_bus_and_slot(0, bus, devfn);
	if (!e->pdev)
		return -ENODEV;
	if (e->pdev->vendor != OPENMIOP_PCI_VENDOR || e->pdev->device != OPENMIOP_PCI_DEVICE ||
	    pci_resource_len(e->pdev, 0) < OPENMIOP_BAR_SIZE) {
		ep_down(e);
		return -ENODEV;
	}
	e->bar = ioremap(pci_resource_start(e->pdev, 0), OPENMIOP_BAR_SIZE);
	if (!e->bar) {
		ep_down(e);
		return -ENOMEM;
	}
	h = e->bar;
	if (readl(&h->hdr.magic) != OPENMIOP_MAGIC || readl(&h->hdr.epoch) != epoch) {
		ep_down(e);
		return -ESTALE;
	}
	e->node = n;
	e->epoch = epoch;
	for (i = 0; i < ETH_ALEN; i++)
		e->mac[i] = readb(&h->hdr.mac[i]);
	e->features = readl(&h->hdr.features);
	e->db_off = readl(&h->hdr.db_off);
	e->db_data = readl(&h->hdr.db_data);
	if (e->db_off & 3 || e->db_off > OPENMIOP_BAR_SIZE - 4)
		e->features &= ~OMI_HDR_DOORBELL;
	e->rc_head = readl(e->bar + GW_OFF(rc_head));
	e->rc_tail = readl(e->bar + GW_OFF(rc_tail));
	e->tail = 0;

	if (!(e->features & OMI_HDR_GW2)) {
		netdev_warn(gw->ndev, "node %u has no gateway v2; no gateway for it\n", n);
		ep_down(e);
		return -EOPNOTSUPP;
	}

	/* Physically contiguous, and 64 KiB aligned as the blade's
	 * outbound window wants (buddy blocks are aligned to their size).
	 */
	e->area_size = max_t(size_t, PAGE_ALIGN(area_bytes()), OMI_GW2_ALIGN);
	e->area_size = PAGE_SIZE << get_order(e->area_size);
	e->area = alloc_pages_exact(e->area_size, GFP_KERNEL | __GFP_ZERO);
	if (!e->area) {
		ep_down(e);
		return -ENOMEM;
	}
	e->area_dma = dma_map_single(&e->pdev->dev, e->area, e->area_size, DMA_FROM_DEVICE);
	if (dma_mapping_error(&e->pdev->dev, e->area_dma) ||
	    (e->area_dma & (OMI_GW2_ALIGN - 1))) {
		dma_unmap_single(&e->pdev->dev, e->area_dma, e->area_size, DMA_FROM_DEVICE);
		free_pages_exact(e->area, e->area_size);
		e->area = NULL;
		ep_down(e);
		return -ENOMEM;
	}

	do {
		token = get_random_u32();
	} while (!token);
	writel(0, e->bar + BAR_OFF(tx_cons[OMI_RC_NODE].tail));
	writel(token, e->bar + BAR_OFF(tx_cons[OMI_RC_NODE].ack));
	ctl = e->bar + BAR_OFF(ctl);
	writel(lower_32_bits(e->area_dma), ctl + offsetof(struct omi_ctl, gw2_ring_lo));
	writel(upper_32_bits(e->area_dma), ctl + offsetof(struct omi_ctl, gw2_ring_hi));
	writel(slots, ctl + offsetof(struct omi_ctl, gw2_slots));
	writel(GW2_SLOT, ctl + offsetof(struct omi_ctl, gw2_slot_size));
	writel(token, ctl + offsetof(struct omi_ctl, gw2_token));

	spin_lock_bh(&gw->lock);
	e->token = token;
	WRITE_ONCE(e->active, true);
	spin_unlock_bh(&gw->lock);
	netdev_info(gw->ndev, "node %u up: %pM, ring %#llx (%u slots)%s\n", n, e->mac,
		    (u64)e->area_dma, slots,
		    e->features & OMI_HDR_DOORBELL ? ", doorbell" : "");
	napi_schedule(&gw->napi);
	return 0;
}

static DEFINE_MUTEX(cmd_lock);

static int cmd_set(const char *val, const struct kernel_param *kp)
{
	char op[8];
	unsigned int n, dom, bus, dev, fn;
	u32 epoch;
	int ret = -EINVAL, cnt;

	if (!gw)
		return -ENODEV;
	cnt = sscanf(val, "%7s %u %x:%x:%x.%x %x", op, &n, &dom, &bus, &dev, &fn, &epoch);
	if (cnt < 2 || n >= OMI_MAX_NODES || n == OMI_RC_NODE)
		return -EINVAL;
	mutex_lock(&cmd_lock);
	if (!strcmp(op, "up") && cnt == 7 && !dom && dev < 32 && fn < 8)
		ret = ep_up(n, bus, PCI_DEVFN(dev, fn), epoch);
	else if (!strcmp(op, "down")) {
		if (gw->ep[n].active || gw->ep[n].pdev)
			ep_down(&gw->ep[n]);
		ret = 0;
	}
	mutex_unlock(&cmd_lock);
	return ret;
}

static const struct kernel_param_ops cmd_ops = {
	.set = cmd_set,
};
module_param_cb(cmd, &cmd_ops, NULL, 0200);
MODULE_PARM_DESC(cmd, "up NODE DDDD:BB:DD.F EPOCH | down NODE (from openmiop-rc -k)");

/* ---------------------------------------------------------------- */
/* netdev                                                            */

static int gw_open(struct net_device *ndev)
{
	napi_enable(&gw->napi);
	hrtimer_start(&gw->timer, ns_to_ktime((u64)idle_us * NSEC_PER_USEC), HRTIMER_MODE_REL);
	netif_start_queue(ndev);
	return 0;
}

static int gw_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	hrtimer_cancel(&gw->timer);
	napi_disable(&gw->napi);
	hrtimer_cancel(&gw->timer);
	return 0;
}

static const struct net_device_ops gw_ops = {
	.ndo_open = gw_open,
	.ndo_stop = gw_stop,
	.ndo_start_xmit = gw_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int __init gw_init(void)
{
	struct net_device *ndev;
	int i, ret;

	BUILD_BUG_ON(sizeof(struct omi_gw2_ep) != 64);
	if (!is_power_of_2(slots) || slots < 2 || slots > 1024)
		return -EINVAL;
	ndev = alloc_etherdev(sizeof(*gw));
	if (!ndev)
		return -ENOMEM;
	gw = netdev_priv(ndev);
	gw->ndev = ndev;
	spin_lock_init(&gw->lock);
	for (i = 0; i < OMI_MAX_NODES; i++) {
		spin_lock_init(&gw->ep[i].txlock);
		gw->ep[i].node = i;
	}
	hrtimer_init(&gw->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	gw->timer.function = gw_timer;
	strscpy(ndev->name, "omi0", IFNAMSIZ);
	ndev->netdev_ops = &gw_ops;
	ndev->mtu = 9000;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = OMI_GW_DATA - ETH_HLEN;
	ndev->features |= NETIF_F_GRO;
	eth_hw_addr_random(ndev);
	netif_napi_add(ndev, &gw->napi, gw_poll, NAPI_POLL_WEIGHT);
	ret = register_netdev(ndev);
	if (ret) {
		netif_napi_del(&gw->napi);
		free_netdev(ndev);
		gw = NULL;
		return ret;
	}
	netdev_info(ndev, "openmiop gateway v2 (%u slots per blade)\n", slots);
	return 0;
}

static void __exit gw_exit(void)
{
	struct net_device *ndev = gw->ndev;
	int i;

	unregister_netdev(ndev);
	mutex_lock(&cmd_lock);
	for (i = 0; i < OMI_MAX_NODES; i++)
		if (gw->ep[i].active || gw->ep[i].pdev)
			ep_down(&gw->ep[i]);
	mutex_unlock(&cmd_lock);
	netif_napi_del(&gw->napi);
	free_netdev(ndev);
}

module_init(gw_init);
module_exit(gw_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("openmiop gateway v2: Cluster Box BMC side (omi0)");
