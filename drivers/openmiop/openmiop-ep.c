// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * openmiop endpoint: RK3588 PCIe3 as an endpoint, netdev omi0.
 *
 * Every blade exposes a 16 MiB BAR backed by its own DRAM (layout in
 * openmiop.h). The Cluster Box (root complex) publishes a peer table
 * into each BAR. Blades then send frames to each other with the PCIe
 * eDMA engine, straight into a receive ring in the peer's BAR; the
 * payload crosses the PCIe switch and never enters the root complex.
 * Frames for the root complex use a small gateway ring that the
 * Cluster Box copies with its CPU.
 *
 * Register programming follows the public Synopsys DWC layout used by
 * drivers/pci/controller/dwc/pcie-designware.c and the Rockchip client
 * block used by pcie-dw-rockchip.c. This is not the Mixtile MIOP
 * driver.
 *
 * Contexts and what they own:
 *
 *   xmit      routes a frame; copies gateway frames into the gateway
 *             ring; queues P2P frames for the TX thread.
 *   tx thread maps queued frames, builds one eDMA linked list for a
 *             batch (payloads first, then one head write per peer)
 *             and waits for it.
 *   NAPI      drains the P2P rings and the gateway ring, returns
 *             credit to the senders.
 *   ctl       (kthread, probe..remove) follows the peer table,
 *             connects to peers, stands in for the missing RX
 *             interrupt by scheduling NAPI, wakes the TX queue.
 *
 * Memory ordering (see also openmiop.h):
 *
 *   - A sender's eDMA writes payloads, then the ring head, on one
 *     channel. PCIe posted writes from one requester are not
 *     reordered (relaxed ordering is not set on these TLPs), so a
 *     receiver that sees the new head also sees the payload in DRAM.
 *   - The BAR is cacheable on the receiver. The receiver invalidates
 *     the head line, reads head, then invalidates each slot it is
 *     about to read. Invalidating after the head read discards any
 *     line the CPU fetched speculatively before the payload landed.
 *   - Credit (our consumer tail) is written into the sender's BAR
 *     with writel(). writel() only orders earlier stores, so mb()
 *     first makes sure the slot reads completed: otherwise the sender
 *     could overwrite a slot we are still copying.
 */

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_reserved_mem.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/rtnetlink.h>
#include <linux/sched.h>
#include <linux/u64_stats_sync.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "openmiop.h"

#define DRV_NAME			"openmiop-ep"

#define PCIE_CLIENT_GENERAL_CON		0x000
#define PCIE_CLIENT_MODE_MASK		0xf0u
#define PCIE_CLIENT_LTSSM_BIT		BIT(2)
#define PCIE_CLIENT_INTR_STATUS_MISC	0x010
#define PCIE_LINK_REQ_RST_NOT_INT	BIT(2)
#define PCIE_CLIENT_HOT_RESET_CTRL	0x180
#define PCIE_LTSSM_APP_DLY2_EN		BIT(1)
#define PCIE_LTSSM_APP_DLY2_DONE	BIT(3)
#define PCIE_LTSSM_ENABLE_ENHANCE	BIT(4)
#define PCIE_CLIENT_LTSSM_STATUS	0x300
#define PCIE_LINKUP_MASK		0x30000u

#define PCIE_MISC_CONTROL_1_OFF		0x8bc
#define PCIE_DBI_RO_WR_EN		BIT(0)
#define PCIE_PORT_LINK_CONTROL		0x710
#define PORT_LINK_DLL_LINK_EN		BIT(5)
#define PORT_LINK_MODE_MASK		GENMASK(21, 16)
#define PORT_LINK_MODE_2_LANES		(0x3u << 16)
#define PORT_LINK_MODE_4_LANES		(0x7u << 16)
#define PCIE_LINK_WIDTH_SPEED_CONTROL	0x80c
#define PORT_LOGIC_LINK_WIDTH_MASK	GENMASK(12, 8)
#define PORT_LOGIC_LINK_WIDTH_2_LANES	(0x2u << 8)
#define PORT_LOGIC_LINK_WIDTH_4_LANES	(0x4u << 8)
/* pcie30-phy-grf. Mode is bits 2:0; the high half is the write enable. */
#define RK3588_PCIE3PHY_GRF_BASE	0xfd5b8000UL
#define RK3588_PCIE3PHY_GRF_SIZE	0x10000
#define RK3588_PCIE3PHY_GRF_CMN_CON0	0x0
#define PHY_MODE_NANBNB			0	/* two PCIe3 x2 */
#define PHY_MODE_AGGREGATION		4	/* one PCIe3 x4 */

#define PCIE_ATU_TYPE_MEM		0x0
#define PCIE_ATU_ENABLE			BIT(31)
#define PCIE_ATU_BAR_MODE_ENABLE	BIT(30)
#define PCIE_ATU_FUNC_NUM_MATCH_EN	BIT(19)
#define PCIE_ATU_INCREASE_REGION_SIZE	BIT(13)
#define PCIE_ATU_REGION_CTRL1		0x000
#define PCIE_ATU_REGION_CTRL2		0x004
#define PCIE_ATU_LOWER_BASE		0x008
#define PCIE_ATU_UPPER_BASE		0x00c
#define PCIE_ATU_LIMIT			0x010
#define PCIE_ATU_LOWER_TARGET		0x014
#define PCIE_ATU_UPPER_TARGET		0x018
#define PCIE_ATU_UPPER_LIMIT		0x020
#define PCIE_EXP_CAP_ID			0x10
#define PORT_LOGIC_SPEED_CHANGE		BIT(17)
/* Fallbacks when the DT node only has a 4 MiB "dbi" region. */
#define DEFAULT_DBI_ATU_OFFSET		0x300000
#define DEFAULT_DBI2_OFFSET		0x100000
/* Unrolled eDMA follows the iATU block (PCIE_DMA_UNROLL_BASE). */
#define ATU_EDMA_OFFSET			0x80000

/* Upstream rk3588 pcie3x4 "addr_space". CPU stores here become MemWr
 * TLPs once an outbound iATU region maps the window. Used when the DT
 * node has no addr_space resource.
 */
static unsigned long long ob_base = 0x900000000ULL;
module_param(ob_base, ullong, 0444);
MODULE_PARM_DESC(ob_base, "CPU physical base of the outbound window if DT has no addr_space");

static bool use_edma = true;
module_param(use_edma, bool, 0444);
MODULE_PARM_DESC(use_edma, "Send blade-to-blade frames with the PCIe eDMA engine (off: gateway only)");

static uint lanes = 2;
module_param(lanes, uint, 0444);
MODULE_PARM_DESC(lanes, "Link width. 2 matches Blade 3 (the other two PHY lanes are the M.2 NVMe). 4 aggregates the PHY onto this controller and drops the NVMe");

/* Synopsys DWC eDMA (unrolled) write engine, channel 0. A write
 * channel reads local DRAM (SAR) and emits MemWr TLPs to the PCIe
 * address in DAR. LIE makes the done bit latch; the mask register
 * keeps the IRQ pin quiet so we can poll.
 */
#define EDMA_CTRL		0x008
#define EDMA_WR_ENB		0x00c
#define EDMA_WR_DOORBELL	0x010
#define EDMA_WR_INT_STATUS	0x04c
#define EDMA_WR_INT_MASK	0x054
#define EDMA_WR_INT_CLEAR	0x058
#define EDMA_WR_LL_ERR		0x090
#define EDMA_WR_CTRL_LO		0x200
#define EDMA_WR_CTRL_HI		0x204
#define EDMA_WR_LLP_LO		0x21c
#define EDMA_WR_LLP_HI		0x220
#define EDMA_CTRL_CB		BIT(0)
#define EDMA_CTRL_TCB		BIT(1)
#define EDMA_CTRL_LLP		BIT(2)
#define EDMA_CTRL_LIE		BIT(3)
#define EDMA_CTRL_CCS		BIT(8)
#define EDMA_CTRL_LLE		BIT(9)
#define EDMA_CTRL_TD		BIT(26)
#define EDMA_INT_DONE		BIT(0)
#define EDMA_INT_ABORT		BIT(16)

/* One eDMA linked-list element. A link element stores the next
 * pointer in sar and leaves dar unused.
 */
struct edma_lli {
	u32 control;
	u32 transfer_size;
	u32 sar_lo;
	u32 sar_hi;
	u32 dar_lo;
	u32 dar_hi;
};

#define OMI_TX_BATCH		32
#define OMI_TXQ_SIZE		256
#define OMI_LL_MAX		256	/* data + head elements per run */
#define OMI_MAX_SEGS		(MAX_SKB_FRAGS + 1)

/* Local scratch at OMI_SCRATCH_OFF: head values the eDMA copies into
 * peers, and the linked list. Peers and the RC never write here.
 */
struct omi_scratch {
	u32 head[2][OMI_MAX_NODES];		/* per list */
	struct edma_lli ll[2][OMI_LL_MAX + 1];	/* double-buffered */
	struct omi_slot_hdr txh[OMI_TXQ_SIZE];	/* one per txq entry */
};

/* One eDMA run: a contiguous range of txq entries and the slots it
 * takes in each peer's ring. Two of them alternate, so the next run is
 * built while the previous one is on the wire.
 */
struct omi_batch {
	u32 first;			/* txq index of the first entry */
	u32 n;				/* entries */
	u32 base[OMI_MAX_NODES];	/* first slot per peer */
	u32 cnt[OMI_MAX_NODES];		/* slots per peer */
	u32 gen;			/* ep->build_gen when built */
	u32 bytes;
	int nel;			/* list elements, 0: nothing to send */
	int list;			/* which scratch list */
	bool built;
};

/* Without an RX interrupt the ctl thread polls. It polls every
 * 20-50 us while traffic is recent, then every 200-400 us, so an idle
 * link costs little CPU. Busy loops * ~40 us is about 80 ms.
 */
#define OMI_POLL_BUSY_LOOPS	2000
#define OMI_POLL_IDLE_US	200
#define OMI_CONNECT_RETRY	(HZ / 2)
#define OMI_STALL_TIMEOUT	(HZ / 10)
#define OMI_DOWN_ACK_TIMEOUT_MS	2000

/* Source MAC learning for frames from bridges behind a peer. */
#define OMI_FDB_SIZE		256
#define OMI_FDB_GW		0xffu	/* learned behind the gateway */

enum omi_peer_state {
	OMI_PEER_DOWN,
	OMI_PEER_CONNECTING,
	OMI_PEER_UP,
};

struct omi_peer {
	/* ctl thread */
	u32 epoch;			/* table epoch we act on, 0: none */
	u64 pci;
	u8 mac[ETH_ALEN];
	void __iomem *win;		/* window into the peer's BAR */
	bool win_ok;			/* release/acquire with NAPI */
	u32 token;			/* our connect token for this peer */
	unsigned long conn_sent;

	/* tx_lock */
	enum omi_peer_state state;
	bool stalled;
	u32 head;			/* next slot we fill */
	u32 tail;			/* consumed by the peer (credit) */
	u32 queued;			/* frames queued, not yet sent */
	unsigned long full_since;

	/* NAPI */
	u32 rx_epoch;			/* sender token we accepted */
	u32 rx_tail;
	u32 rx_credit;			/* last tail written to the sender */
	bool rx_ack;			/* ack not yet written */
};

struct omi_txd {
	struct sk_buff *skb;
	u8 mask;			/* P2P targets still to send to */
	u8 nseg;
	bool mapped;
	dma_addr_t addr[OMI_MAX_SEGS];
	u32 len[OMI_MAX_SEGS];
};

struct omi_xmit_stats {
	u64_stats_t packets, bytes, dropped, gw_packets, gw_full, queue_stops;
	struct u64_stats_sync syncp;
};

struct omi_tx_stats {
	u64_stats_t p2p_frames, dma_runs, dma_elems, dma_errors, dropped,
		    peer_full_drops, queue_wakes, dma_wait_ns, dma_bytes;
	struct u64_stats_sync syncp;
};

struct omi_rx_stats {
	u64_stats_t packets, bytes, p2p_packets, gw_packets, errors,
		    dropped, resyncs, connects, polls;
	struct u64_stats_sync syncp;
};

struct omi_ctl_stats {
	u64_stats_t poll_cycles, napi_kicks, table_updates, peer_up,
		    peer_down, stalls, queue_wakes, link_resets;
	struct u64_stats_sync syncp;
};

struct omi_ep {
	struct device *dev;
	struct net_device *ndev;
	void __iomem *apb;
	void __iomem *dbi;
	void __iomem *dbi2;
	void __iomem *atu;
	void __iomem *edma;
	u64 ob_phys;
	void __iomem *ob;		/* OMI_MAX_NODES BAR-sized windows */
	struct clk_bulk_data *clks;
	int nclks;
	struct reset_control *rst;
	struct phy *phy;
	struct regulator *vpcie3v3;
	bool rmem;
	bool link;
	u32 rebar_pos;			/* resizable-BAR capability in DBI */

	void *raw;
	dma_addr_t raw_dma;
	size_t raw_size;
	void *cpu;
	dma_addr_t dma;
	struct omi_bar_head *bar;
	struct omi_gw *gw;
	struct omi_scratch *scratch;
	dma_addr_t scratch_dma;

	u32 epoch;
	u8 self;			/* node index, OMI_MAX_NODES: unknown */
	u32 table_gen;
	u8 rc_mac[ETH_ALEN];
	bool rc_up;
	struct omi_peer peer[OMI_MAX_NODES];

	/* TX */
	spinlock_t tx_lock;
	u8 up_mask;			/* peers in OMI_PEER_UP, not stalled */
	u32 build_gen;			/* tx_lock: batches built */
	u32 done_gen;			/* tx_lock: build_gen of the last batch retired */
	struct omi_txd *txq;
	u32 txq_prod, txq_cons;
	wait_queue_head_t tx_wait;
	struct task_struct *tx_task;
	u32 gw_head;			/* xmit only */
	struct omi_batch batch[2];	/* tx thread */

	/* RX */
	struct napi_struct napi;
	u32 rx_rr;
	u32 gw_rx_tail;
	u64 fdb[OMI_FDB_SIZE];

	struct task_struct *ctl;

	struct omi_xmit_stats xs;
	struct omi_tx_stats ts;
	struct omi_rx_stats rs;
	struct omi_ctl_stats cs;
};

#define omi_inc(st, f) do {					\
	u64_stats_update_begin(&(st)->syncp);			\
	u64_stats_inc(&(st)->f);				\
	u64_stats_update_end(&(st)->syncp);			\
} while (0)

#define omi_add(st, f, v) do {					\
	u64_stats_update_begin(&(st)->syncp);			\
	u64_stats_add(&(st)->f, (v));				\
	u64_stats_update_end(&(st)->syncp);			\
} while (0)

/* ---------------------------------------------------------------- */
/* BAR memory helpers                                                */

static void bar_clean(struct omi_ep *ep, unsigned int off, unsigned int len)
{
	dma_sync_single_for_device(ep->dev, ep->dma + off, len, DMA_TO_DEVICE);
}

static void bar_inval(struct omi_ep *ep, unsigned int off, unsigned int len)
{
	dma_sync_single_for_cpu(ep->dev, ep->dma + off, len, DMA_FROM_DEVICE);
}

#define BAR_OFF(f)	offsetof(struct omi_bar_head, f)

static unsigned int ring_off(u8 sender)
{
	return OMI_RING_OFF + sender * OMI_RING_SLOTS * OMI_SLOT;
}

static unsigned int gw_off(void)
{
	return OMI_GW_OFF;
}

static unsigned int gw_slot_off(bool rc_dir, u32 i)
{
	return OMI_GW_OFF + sizeof(struct omi_gw) +
	       ((rc_dir ? OMI_GW_SLOTS : 0) + (i & (OMI_GW_SLOTS - 1))) *
	       OMI_GW_SLOT;
}

/* ---------------------------------------------------------------- */
/* Controller                                                        */

static void rk_hiword(void __iomem *base, u32 reg, u32 mask, u32 val)
{
	writel((mask << 16) | (val & mask), base + reg);
}

static bool link_is_up(struct omi_ep *ep)
{
	return (readl(ep->apb + PCIE_CLIENT_LTSSM_STATUS) & PCIE_LINKUP_MASK) ==
	       PCIE_LINKUP_MASK;
}

/* Unrolled iATU: region << 9, inbound adds bit 8. Same as
 * PCIE_ATU_UNROLL_BASE() in pcie-designware.h.
 */
static void __iomem *atu_region(struct omi_ep *ep, u32 index, bool inbound)
{
	return ep->atu + ((index << 9) | (inbound ? BIT(8) : 0));
}

static int atu_wait_enable(void __iomem *base)
{
	int i;

	for (i = 0; i < 5; i++) {
		if (readl(base + PCIE_ATU_REGION_CTRL2) & PCIE_ATU_ENABLE)
			return 0;
		mdelay(1);
	}
	return -ETIMEDOUT;
}

static int program_inbound_bar0(struct omi_ep *ep)
{
	void __iomem *base = atu_region(ep, 0, true);

	writel(lower_32_bits(ep->dma), base + PCIE_ATU_LOWER_TARGET);
	writel(upper_32_bits(ep->dma), base + PCIE_ATU_UPPER_TARGET);
	writel(PCIE_ATU_TYPE_MEM, base + PCIE_ATU_REGION_CTRL1);
	writel(PCIE_ATU_ENABLE | PCIE_ATU_FUNC_NUM_MATCH_EN |
	       PCIE_ATU_BAR_MODE_ENABLE | (0u << 8),
	       base + PCIE_ATU_REGION_CTRL2);
	if (atu_wait_enable(base)) {
		dev_err(ep->dev, "inbound iATU did not enable\n");
		return -ETIMEDOUT;
	}
	return 0;
}

/* Outbound region n maps window n (ob_phys + n * BAR size) onto the
 * BAR of node n. Once enabled a region stays enabled until remove:
 * NAPI may store credit through it at any time, and a store to a
 * disabled region is an AXI decode error. A store to a peer that has
 * gone is a posted write the switch drops.
 */
static int program_outbound(struct omi_ep *ep, u8 n, u64 pci_addr)
{
	void __iomem *base = atu_region(ep, n, false);
	u64 cpu = ep->ob_phys + (u64)n * OPENMIOP_BAR_SIZE;
	u64 end = cpu + OPENMIOP_BAR_SIZE - 1;

	writel(lower_32_bits(cpu), base + PCIE_ATU_LOWER_BASE);
	writel(upper_32_bits(cpu), base + PCIE_ATU_UPPER_BASE);
	writel(lower_32_bits(end), base + PCIE_ATU_LIMIT);
	writel(upper_32_bits(end), base + PCIE_ATU_UPPER_LIMIT);
	writel(lower_32_bits(pci_addr), base + PCIE_ATU_LOWER_TARGET);
	writel(upper_32_bits(pci_addr), base + PCIE_ATU_UPPER_TARGET);
	writel(PCIE_ATU_TYPE_MEM | PCIE_ATU_INCREASE_REGION_SIZE,
	       base + PCIE_ATU_REGION_CTRL1);
	writel(PCIE_ATU_ENABLE, base + PCIE_ATU_REGION_CTRL2);
	if (atu_wait_enable(base)) {
		dev_err(ep->dev, "outbound iATU %u did not enable\n", n);
		return -ETIMEDOUT;
	}
	return 0;
}

static void disable_outbound(struct omi_ep *ep)
{
	u8 n;

	for (n = 0; n < OMI_MAX_NODES; n++)
		if (ep->peer[n].win_ok)
			writel(0, atu_region(ep, n, false) + PCIE_ATU_REGION_CTRL2);
}

static void dbi_ro_wr(struct omi_ep *ep, bool en)
{
	u32 v = readl(ep->dbi + PCIE_MISC_CONTROL_1_OFF);

	if (en)
		v |= PCIE_DBI_RO_WR_EN;
	else
		v &= ~PCIE_DBI_RO_WR_EN;
	writel(v, ep->dbi + PCIE_MISC_CONTROL_1_OFF);
}

static void hide_ext_cap(struct omi_ep *ep, u16 cap_id)
{
	u32 prev = 0;
	u32 pos = 0x100;
	int ttl = 48;

	while (ttl-- > 0 && pos >= 0x100) {
		u32 hdr = readl(ep->dbi + pos);
		u16 id = hdr & 0xffff;
		u32 next = (hdr >> 20) & 0xffc;

		if (id == cap_id) {
			if (!prev)
				return;
			hdr = readl(ep->dbi + prev);
			hdr = (hdr & 0x000fffff) | (next << 20);
			writel(hdr, ep->dbi + prev);
			return;
		}
		if (!next || next == pos)
			return;
		prev = pos;
		pos = next;
	}
}

static u32 find_ext_cap(struct omi_ep *ep, u16 cap_id)
{
	u32 pos = 0x100;
	int ttl = 48;

	while (ttl-- > 0 && pos >= 0x100) {
		u32 hdr = readl(ep->dbi + pos);
		u32 next = (hdr >> 20) & 0xffc;

		if ((hdr & 0xffff) == cap_id)
			return pos;
		if (!next || next == pos)
			return 0;
		pos = next;
	}
	return 0;
}

/*
 * BAR0 size comes from the resizable-BAR control register. That
 * register is reset by a link-down or hot reset, but the capability
 * list edit that hides it from the host is not, so the position found
 * at probe is remembered.
 */
static void restrict_rebar(struct omi_ep *ep)
{
	u32 pos;

	if (!ep->rebar_pos)
		ep->rebar_pos = find_ext_cap(ep, PCI_EXT_CAP_ID_REBAR);
	pos = ep->rebar_pos;
	if (pos) {
		unsigned int nbars, i;

		nbars = (readl(ep->dbi + pos + 8) & 0xe0) >> 5;
		if (!nbars)
			nbars = 1;
		if (nbars > 6)
			nbars = 6;
		for (i = 0; i < nbars; i++) {
			u32 base = pos + i * 8;
			u32 cap = readl(ep->dbi + base + 4);
			u32 ctrl = readl(ep->dbi + base + 8);

			cap &= ~0x00fffff0u;
			cap |= (1u << 8);		/* only 16 MiB */
			ctrl &= ~0x00001f00u;
			ctrl |= (4u << 8);		/* 16 MiB */
			writel(cap, ep->dbi + base + 4);
			writel(ctrl, ep->dbi + base + 8);
		}
	}
}

/*
 * Endpoint identity, BARs and link settings. Many of these registers
 * are not sticky: the controller resets them on a link-down or hot
 * reset, so this runs at probe and again after every such reset
 * (mainline: dw_pcie_ep_init_non_sticky_registers()). It must not
 * clobber what the host programmed if the registers survived.
 */
static void program_config_space(struct omi_ep *ep)
{
	u32 classrev, next;
	u32 width = lanes >= 4 ? 4 : 2;
	u32 bar_type = PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH;
	u32 val;
	int i;

	dbi_ro_wr(ep, true);

	writel((OPENMIOP_PCI_DEVICE << 16) | OPENMIOP_PCI_VENDOR, ep->dbi + PCI_VENDOR_ID);

	classrev = readl(ep->dbi + PCI_CLASS_REVISION);
	/* PCI_CLASS_NETWORK_ETHERNET is the 16-bit class (0x0200). */
	classrev = (classrev & 0xff) | ((u32)PCI_CLASS_NETWORK_ETHERNET << 16);
	writel(classrev, ep->dbi + PCI_CLASS_REVISION);

	writeb(1, ep->dbi + PCI_INTERRUPT_PIN);

	/* The resizable-BAR control register is what the host's sizing
	 * read actually uses for BAR0 on this controller.
	 */
	restrict_rebar(ep);

	val = readl(ep->dbi + PCI_BASE_ADDRESS_0);
	if ((val & 0xf) != bar_type)
		writel((val & ~0xfu) | bar_type, ep->dbi + PCI_BASE_ADDRESS_0);

	/* Disable BAR2-5 and the expansion ROM, as dw_pcie_ep_reset_bar()
	 * does: zero the DBI2 shadow (enable/mask) first, then the BAR.
	 * Left enabled they cost the host 4 x 16 MiB + 64 KiB of MMIO each,
	 * and BAR4 exposes the iATU registers to the host.
	 */
	for (i = 2; i < 6; i++) {
		writel(0, ep->dbi2 + PCI_BASE_ADDRESS_0 + i * 4);
		writel(0, ep->dbi + PCI_BASE_ADDRESS_0 + i * 4);
	}
	writel(0, ep->dbi2 + PCI_ROM_ADDRESS);
	writel(0, ep->dbi + PCI_ROM_ADDRESS);

	/* Width is the lanes parameter. x4 only trains if the PHY was
	 * latched in aggregation; a bifurcated PHY still has two lanes.
	 */
	val = readl(ep->dbi + PCIE_PORT_LINK_CONTROL);
	val |= PORT_LINK_DLL_LINK_EN;
	val &= ~PORT_LINK_MODE_MASK;
	val |= width == 4 ? PORT_LINK_MODE_4_LANES : PORT_LINK_MODE_2_LANES;
	writel(val, ep->dbi + PCIE_PORT_LINK_CONTROL);

	val = readl(ep->dbi + PCIE_LINK_WIDTH_SPEED_CONTROL);
	val &= ~PORT_LOGIC_LINK_WIDTH_MASK;
	val |= width == 4 ? PORT_LOGIC_LINK_WIDTH_4_LANES
			  : PORT_LOGIC_LINK_WIDTH_2_LANES;
	val |= PORT_LOGIC_SPEED_CHANGE;
	writel(val, ep->dbi + PCIE_LINK_WIDTH_SPEED_CONTROL);

	/* Target link speed Gen3; LNKCTL2 is 0x30 into the express cap. */
	next = readb(ep->dbi + PCI_CAPABILITY_LIST);
	while (next) {
		if (readb(ep->dbi + next) == PCIE_EXP_CAP_ID) {
			u32 lnkcap = readl(ep->dbi + next + PCI_EXP_LNKCAP);
			u16 ctl2 = readw(ep->dbi + next + PCI_EXP_LNKCTL2);

			lnkcap &= ~PCI_EXP_LNKCAP_MLW;
			lnkcap |= width << 4;
			writel(lnkcap, ep->dbi + next + PCI_EXP_LNKCAP);
			ctl2 = (ctl2 & ~0xfu) | 3u;
			writew(ctl2, ep->dbi + next + PCI_EXP_LNKCTL2);
			break;
		}
		next = readb(ep->dbi + next + 1);
	}

	/* ATS does not work on RK3588 in EP mode (see
	 * rockchip_pcie_ep_hide_broken_ats_cap_rk3588()); PRI depends on
	 * it; the host must not resize BAR0.
	 */
	hide_ext_cap(ep, PCI_EXT_CAP_ID_ATS);
	hide_ext_cap(ep, PCI_EXT_CAP_ID_PRI);
	hide_ext_cap(ep, PCI_EXT_CAP_ID_REBAR);

	dbi_ro_wr(ep, false);

	dev_dbg(ep->dev, "config id %04x:%04x class %06x, x%u Gen3\n",
		 readw(ep->dbi + PCI_VENDOR_ID), readw(ep->dbi + PCI_DEVICE_ID),
		 readl(ep->dbi + PCI_CLASS_REVISION) >> 8, width);
}

static int wait_link(struct omi_ep *ep)
{
	int i;

	for (i = 0; i < 50; i++) {
		if (link_is_up(ep)) {
			dev_info(ep->dev, "link up, LTSSM %#x\n",
				 readl(ep->apb + PCIE_CLIENT_LTSSM_STATUS));
			ep->link = true;
			return 0;
		}
		msleep(100);
	}
	dev_err(ep->dev, "link did not come up, LTSSM %#x\n",
		readl(ep->apb + PCIE_CLIENT_LTSSM_STATUS));
	return -ETIMEDOUT;
}

static int hw_start(struct omi_ep *ep)
{
	int ret;

	/* 0xffffffff in the viewport register means unrolled iATU, the
	 * same test as dw_pcie_iatu_unroll_enabled() in 6.1.
	 */
	if (readl(ep->dbi + 0x900) != 0xffffffff) {
		dev_err(ep->dev, "viewport iATU is not supported\n");
		return -ENODEV;
	}

	rk_hiword(ep->apb, PCIE_CLIENT_GENERAL_CON, PCIE_CLIENT_LTSSM_BIT, 0);
	rk_hiword(ep->apb, PCIE_CLIENT_GENERAL_CON, PCIE_CLIENT_MODE_MASK, 0);
	/* APP_DLY2: after a hot reset or link-down reset the LTSSM waits
	 * for DLY2_DONE, so the non-sticky registers are restored before
	 * the host can enumerate us again (as in pcie-dw-rockchip.c).
	 */
	rk_hiword(ep->apb, PCIE_CLIENT_HOT_RESET_CTRL,
		  PCIE_LTSSM_ENABLE_ENHANCE | PCIE_LTSSM_APP_DLY2_EN,
		  PCIE_LTSSM_ENABLE_ENHANCE | PCIE_LTSSM_APP_DLY2_EN);
	writel(readl(ep->apb + PCIE_CLIENT_INTR_STATUS_MISC),
	       ep->apb + PCIE_CLIENT_INTR_STATUS_MISC);

	writew(0, ep->dbi + PCI_COMMAND);
	program_config_space(ep);
	dev_info(ep->dev, "config id %04x:%04x class %06x\n",
		 readw(ep->dbi + PCI_VENDOR_ID), readw(ep->dbi + PCI_DEVICE_ID),
		 readl(ep->dbi + PCI_CLASS_REVISION) >> 8);
	ret = program_inbound_bar0(ep);
	if (ret)
		return ret;

	rk_hiword(ep->apb, PCIE_CLIENT_GENERAL_CON,
		  PCIE_CLIENT_LTSSM_BIT, PCIE_CLIENT_LTSSM_BIT);
	return wait_link(ep);
}

static void hw_stop(struct omi_ep *ep)
{
	rk_hiword(ep->apb, PCIE_CLIENT_GENERAL_CON, PCIE_CLIENT_LTSSM_BIT, 0);
	ep->link = false;
}

/* ---------------------------------------------------------------- */
/* eDMA                                                              */

static void edma_init(struct omi_ep *ep)
{
	u32 ctrl;

	if (!ep->edma)
		return;
	ctrl = readl(ep->edma + EDMA_CTRL);
	if (!ctrl || ctrl == 0xffffffff) {
		dev_info(ep->dev, "eDMA not present (ctrl %#x)\n", ctrl);
		ep->edma = NULL;
		return;
	}
	/* Status bits still latch. This only holds the IRQ pin down. */
	writel(0xffffffff, ep->edma + EDMA_WR_INT_MASK);
	writel(readl(ep->edma + EDMA_WR_LL_ERR) | BIT(0), ep->edma + EDMA_WR_LL_ERR);
}

/* After an abort or timeout: stop the engine and start it again. */
static void edma_reset(struct omi_ep *ep)
{
	writel(0, ep->edma + EDMA_WR_ENB);
	udelay(10);
	writel(EDMA_INT_DONE | EDMA_INT_ABORT, ep->edma + EDMA_WR_INT_CLEAR);
	writel(BIT(0), ep->edma + EDMA_WR_ENB);
}

static void ll_data(struct edma_lli *e, bool cb, u32 len, dma_addr_t sar, u64 dar)
{
	e->control = EDMA_CTRL_TD | (cb ? EDMA_CTRL_CB : 0);
	e->transfer_size = len;
	e->sar_lo = lower_32_bits(sar);
	e->sar_hi = upper_32_bits(sar);
	e->dar_lo = lower_32_bits(dar);
	e->dar_hi = upper_32_bits(dar);
}

/* Link element back to the start. Its CB is the opposite of the data
 * elements; TCB flips the consumer cycle, which is how the engine
 * recycles the same list on the next doorbell.
 */
static void ll_link(struct edma_lli *e, bool cb, dma_addr_t next)
{
	e->control = EDMA_CTRL_LLP | EDMA_CTRL_TCB | (cb ? EDMA_CTRL_CB : 0);
	e->transfer_size = 0;
	e->sar_lo = lower_32_bits(next);
	e->sar_hi = upper_32_bits(next);
	e->dar_lo = 0;
	e->dar_hi = 0;
}

/*
 * Start list b->list. CCS and LLP are programmed for every run: data
 * elements carry CB=1 and the list ends in a link element with CB=0,
 * where the engine stops. The two lists therefore never share cycle
 * state. LIE on the last element latches the done bit.
 */
static void edma_kick(struct omi_ep *ep, struct omi_batch *b)
{
	struct edma_lli *ll = ep->scratch->ll[b->list];
	dma_addr_t ll_dma = ep->scratch_dma +
			    offsetof(struct omi_scratch, ll[b->list]);
	void __iomem *dma = ep->edma;

	ll[b->nel - 1].control |= EDMA_CTRL_LIE;
	ll_link(&ll[b->nel], false, ll_dma);
	dma_sync_single_for_device(ep->dev, ll_dma, (b->nel + 1) * sizeof(*ll),
				   DMA_TO_DEVICE);
	dma_sync_single_for_device(ep->dev,
				   ep->scratch_dma + offsetof(struct omi_scratch, head[b->list]),
				   sizeof(ep->scratch->head[0]), DMA_TO_DEVICE);
	dma_sync_single_for_device(ep->dev,
				   ep->scratch_dma + offsetof(struct omi_scratch, txh),
				   sizeof(ep->scratch->txh), DMA_TO_DEVICE);

	writel(EDMA_INT_DONE | EDMA_INT_ABORT, dma + EDMA_WR_INT_CLEAR);
	writel(BIT(0), dma + EDMA_WR_ENB);
	writel(EDMA_CTRL_CCS | EDMA_CTRL_LLE, dma + EDMA_WR_CTRL_LO);
	writel(0, dma + EDMA_WR_CTRL_HI);
	writel(lower_32_bits(ll_dma), dma + EDMA_WR_LLP_LO);
	writel(upper_32_bits(ll_dma), dma + EDMA_WR_LLP_HI);
	/* writel() orders the list stores (cleaned above) before the
	 * doorbell. Channel 0, stop bit clear: start.
	 */
	writel(0, dma + EDMA_WR_DOORBELL);
}

/* ---------------------------------------------------------------- */
/* TX                                                                */

/* tx_lock held. Room for one more frame to every peer in mask. */
static bool peers_have_room(struct omi_ep *ep, u8 mask)
{
	unsigned long m = mask;
	unsigned int p;

	for_each_set_bit(p, &m, OMI_MAX_NODES) {
		struct omi_peer *pr = &ep->peer[p];

		if (pr->queued + (pr->head - pr->tail) >= OMI_RING_SLOTS - 1)
			return false;
	}
	return true;
}

/* tx_lock held. Pick up credit the peer wrote into our tx_cons line. */
static void peer_refresh_tail(struct omi_ep *ep, unsigned int p)
{
	struct omi_peer *pr = &ep->peer[p];
	unsigned int off = BAR_OFF(tx_cons[p]);
	struct omi_cons *c = (void *)ep->bar + off;
	u32 tail;

	bar_inval(ep, off, sizeof(*c));
	if (READ_ONCE(c->ack) != pr->token)
		return;
	tail = READ_ONCE(c->tail);
	/* Ignore credit that would claim more than we sent. */
	if (pr->head - tail <= OMI_RING_SLOTS)
		pr->tail = tail;
}

/* tx_lock held. Wake the queue if every live peer has room. */
static bool omi_maybe_wake(struct omi_ep *ep)
{
	unsigned long m = ep->up_mask;
	unsigned int p;

	if (!netif_queue_stopped(ep->ndev))
		return false;
	for_each_set_bit(p, &m, OMI_MAX_NODES)
		peer_refresh_tail(ep, p);
	if (ep->txq_prod - ep->txq_cons >= OMI_TXQ_SIZE ||
	    !peers_have_room(ep, ep->up_mask))
		return false;
	netif_wake_queue(ep->ndev);
	return true;
}

static void txd_unmap(struct omi_ep *ep, struct omi_txd *d)
{
	int i;

	if (!d->mapped)
		return;
	dma_unmap_single(ep->dev, d->addr[0], d->len[0], DMA_TO_DEVICE);
	for (i = 1; i < d->nseg; i++)
		dma_unmap_page(ep->dev, d->addr[i], d->len[i], DMA_TO_DEVICE);
	d->mapped = false;
}

static int txd_map(struct omi_ep *ep, struct omi_txd *d)
{
	struct sk_buff *skb = d->skb;
	int i;

	d->nseg = 0;
	d->addr[0] = dma_map_single(ep->dev, skb->data, skb_headlen(skb),
				    DMA_TO_DEVICE);
	if (dma_mapping_error(ep->dev, d->addr[0]))
		return -ENOMEM;
	d->len[0] = skb_headlen(skb);
	d->nseg = 1;
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *f = &skb_shinfo(skb)->frags[i];
		dma_addr_t a = skb_frag_dma_map(ep->dev, f, 0, skb_frag_size(f),
						DMA_TO_DEVICE);

		if (dma_mapping_error(ep->dev, a)) {
			d->mapped = true;
			txd_unmap(ep, d);
			return -ENOMEM;
		}
		d->addr[d->nseg] = a;
		d->len[d->nseg] = skb_frag_size(f);
		d->nseg++;
	}
	d->mapped = true;
	return 0;
}

/* Drop the per-peer accounting of frame d for the peers in mask. */
static void txd_release(struct omi_ep *ep, struct omi_txd *d, u8 mask)
{
	unsigned long m = mask;
	unsigned int p;

	for_each_set_bit(p, &m, OMI_MAX_NODES)
		ep->peer[p].queued--;
}

/*
 * Build a run from the queued frames that follow `after` (the run on
 * the wire, or NULL). Takes frames in order while every target peer
 * has a free slot; slots are counted on top of `after`. Per target:
 * the 16-byte slot header from scratch, then the skb segments. At the
 * end, one head write per touched peer.
 */
static void tx_build(struct omi_ep *ep, struct omi_batch *b,
		     const struct omi_batch *after, int list)
{
	struct omi_scratch *sc = ep->scratch;
	struct edma_lli *ll = sc->ll[list];
	u32 avail, i, first;
	dma_addr_t hsrc = 0;
	u8 touched = 0;
	int nel = 0;

	memset(b, 0, sizeof(*b));
	b->list = list;
	b->built = true;

	spin_lock_bh(&ep->tx_lock);
	first = ep->txq_cons + (after ? after->n : 0);
	avail = min_t(u32, ep->txq_prod - first, OMI_TX_BATCH);
	spin_unlock_bh(&ep->tx_lock);
	b->first = first;
	if (!avail)
		return;

	/* Map outside the lock. Only this thread consumes txq entries. */
	for (i = 0; i < avail; i++) {
		struct omi_txd *d = &ep->txq[(first + i) & (OMI_TXQ_SIZE - 1)];

		if (!d->mapped && d->mask && txd_map(ep, d)) {
			spin_lock_bh(&ep->tx_lock);
			txd_release(ep, d, d->mask);
			spin_unlock_bh(&ep->tx_lock);
			d->mask = 0;
			omi_inc(&ep->ts, dropped);
		}
	}

	spin_lock_bh(&ep->tx_lock);
	for (i = 0; i < OMI_MAX_NODES; i++)
		b->base[i] = ep->peer[i].head + (after ? after->cnt[i] : 0);
	for (b->n = 0; b->n < avail; b->n++) {
		u32 q = (first + b->n) & (OMI_TXQ_SIZE - 1);
		struct omi_txd *d = &ep->txq[q];
		u8 gone = d->mask & ~ep->up_mask;
		unsigned long m;
		unsigned int p;
		bool room = true;

		/* Peers that went down or stalled since xmit. */
		if (gone) {
			txd_release(ep, d, gone);
			d->mask &= ep->up_mask;
			omi_inc(&ep->ts, peer_full_drops);
		}
		if (nel + hweight8(d->mask) * (d->nseg + 1) + OMI_MAX_NODES > OMI_LL_MAX)
			break;
		m = d->mask;
		for_each_set_bit(p, &m, OMI_MAX_NODES) {
			struct omi_peer *pr = &ep->peer[p];
			u32 used = b->base[p] + b->cnt[p] - pr->tail;

			if (used >= OMI_RING_SLOTS) {
				peer_refresh_tail(ep, p);
				used = b->base[p] + b->cnt[p] - pr->tail;
			}
			if (used >= OMI_RING_SLOTS)
				room = false;
		}
		if (!room)
			break;
		if (d->mask) {
			sc->txh[q].len = d->skb->len;
			sc->txh[q].flags = 0;
			sc->txh[q].mask = 0;
			sc->txh[q].pad = 0;
			hsrc = ep->scratch_dma + offsetof(struct omi_scratch, txh[q]);
		}
		for_each_set_bit(p, &m, OMI_MAX_NODES) {
			struct omi_peer *pr = &ep->peer[p];
			u32 slot = (b->base[p] + b->cnt[p]) & (OMI_RING_SLOTS - 1);
			u64 dar = pr->pci + ring_off(ep->self) + (u64)slot * OMI_SLOT;
			int seg;

			ll_data(&ll[nel++], true, sizeof(struct omi_slot_hdr), hsrc, dar);
			dar += sizeof(struct omi_slot_hdr);
			for (seg = 0; seg < d->nseg; seg++) {
				ll_data(&ll[nel++], true, d->len[seg], d->addr[seg], dar);
				dar += d->len[seg];
				b->bytes += d->len[seg];
			}
			b->cnt[p]++;
			touched |= BIT(p);
		}
	}
	if (b->n)
		b->gen = ++ep->build_gen;
	spin_unlock_bh(&ep->tx_lock);

	if (touched) {
		unsigned long m = touched;
		unsigned int p;

		for_each_set_bit(p, &m, OMI_MAX_NODES) {
			sc->head[list][p] = b->base[p] + b->cnt[p];
			ll_data(&ll[nel++], true, sizeof(u32),
				ep->scratch_dma + offsetof(struct omi_scratch, head[list][p]),
				ep->peer[p].pci + BAR_OFF(rx_prod[ep->self].head));
		}
	}
	b->nel = nel;
}

/* Retire a run: free its frames and, if it went out, advance the heads. */
static void tx_complete(struct omi_ep *ep, struct omi_batch *b, int ret)
{
	u32 i;

	spin_lock_bh(&ep->tx_lock);
	for (i = 0; i < b->n; i++) {
		struct omi_txd *d = &ep->txq[(b->first + i) & (OMI_TXQ_SIZE - 1)];

		txd_release(ep, d, d->mask);
		txd_unmap(ep, d);
		if (d->mask && !ret) {
			omi_inc(&ep->ts, p2p_frames);
			dev_consume_skb_any(d->skb);
		} else {
			omi_inc(&ep->ts, dropped);
			dev_kfree_skb_any(d->skb);
		}
		d->skb = NULL;
	}
	ep->txq_cons += b->n;
	/* On failure the peers never saw the new heads; the slots are
	 * free again.
	 */
	if (!ret)
		for (i = 0; i < OMI_MAX_NODES; i++)
			ep->peer[i].head = b->base[i] + b->cnt[i];
	if (b->n)
		ep->done_gen = b->gen;
	if (omi_maybe_wake(ep))
		omi_inc(&ep->ts, queue_wakes);
	spin_unlock_bh(&ep->tx_lock);
	b->built = false;
}

/* Map one queued frame that is not in run b yet. Returns false if
 * there is none: everything queued behind b is mapped.
 */
static bool tx_premap(struct omi_ep *ep, const struct omi_batch *b)
{
	/* Pairs with smp_store_release() in txq_add(): an entry below
	 * txq_prod is completely written.
	 */
	u32 first = b->first + b->n, i, prod = smp_load_acquire(&ep->txq_prod);

	for (i = first; i != prod && i - first < OMI_TX_BATCH; i++) {
		struct omi_txd *d = &ep->txq[i & (OMI_TXQ_SIZE - 1)];

		if (d->mapped || !d->mask)
			continue;
		if (txd_map(ep, d)) {
			spin_lock_bh(&ep->tx_lock);
			txd_release(ep, d, d->mask);
			spin_unlock_bh(&ep->tx_lock);
			d->mask = 0;
			omi_inc(&ep->ts, dropped);
		}
		return true;
	}
	return false;
}

/*
 * Wait for run b. Meanwhile map the frames that queue up behind it, so
 * the next run is built from everything that arrived and costs only
 * the list build when b is done. Polling the eDMA block in a tight loop
 * starves the engine of the bus it fetches descriptors on, so stay off
 * it between checks.
 */
static int tx_wait_premap(struct omi_ep *ep, struct omi_batch *b)
{
	void __iomem *dma = ep->edma;
	int i;

	for (i = 0; i < 20050; i++) {
		u32 st = readl(dma + EDMA_WR_INT_STATUS);

		if (st & EDMA_INT_ABORT) {
			writel(EDMA_INT_DONE | EDMA_INT_ABORT, dma + EDMA_WR_INT_CLEAR);
			return -EIO;
		}
		if (st & EDMA_INT_DONE) {
			writel(EDMA_INT_DONE, dma + EDMA_WR_INT_CLEAR);
			return 0;
		}
		if (tx_premap(ep, b))
			continue;
		if (i < 50)
			cpu_relax();
		else
			udelay(1);
	}
	return -ETIMEDOUT;
}

static int tx_wait_run(struct omi_ep *ep, struct omi_batch *b, u64 t0)
{
	int ret = tx_wait_premap(ep, b);

	omi_add(&ep->ts, dma_wait_ns, ktime_get_ns() - t0);
	if (ret) {
		edma_reset(ep);
		dev_err_ratelimited(ep->dev, "eDMA run of %d elements failed (%d)\n",
				    b->nel, ret);
		omi_inc(&ep->ts, dma_errors);
	} else {
		omi_inc(&ep->ts, dma_runs);
		omi_add(&ep->ts, dma_elems, b->nel);
		omi_add(&ep->ts, dma_bytes, b->bytes);
	}
	return ret;
}

/*
 * While a run is on the wire, the frames queuing behind it are mapped
 * (tx_wait_premap). When it is done, the next run is built from all of
 * them and started at once: the engine idles only for the list build,
 * and runs grow with the load, which amortises the fixed cost of a run
 * (~13 us measured).
 */
static int tx_thread(void *data)
{
	struct omi_ep *ep = data;
	int list = 0;

	set_user_nice(current, -20);
	while (!kthread_should_stop()) {
		struct omi_batch *b = &ep->batch[list];
		u64 t0;
		int ret;

		/* Under sustained load there is always work. Talos runs
		 * PREEMPT_NONE and the vendor kernel PREEMPT_VOLUNTARY:
		 * without this the thread never leaves the CPU, which stalled
		 * RCU for 21 s in a 10 minute bidirectional run.
		 */
		cond_resched();
		tx_build(ep, b, NULL, list);
		if (!b->n) {
			if (READ_ONCE(ep->txq_prod) != READ_ONCE(ep->txq_cons)) {
				/* The first frame waits for credit. */
				usleep_range(20, 50);
				continue;
			}
			wait_event_interruptible_timeout(ep->tx_wait,
				kthread_should_stop() ||
				READ_ONCE(ep->txq_prod) != READ_ONCE(ep->txq_cons),
				HZ);
			continue;
		}
		if (!b->nel) {
			/* Only frames whose peers went away. */
			tx_complete(ep, b, 0);
			continue;
		}
		t0 = ktime_get_ns();
		edma_kick(ep, b);
		ret = tx_wait_run(ep, b, t0);
		tx_complete(ep, b, ret);
		list ^= 1;
	}
	return 0;
}

static void txq_purge(struct omi_ep *ep)
{
	spin_lock_bh(&ep->tx_lock);
	while (ep->txq_cons != ep->txq_prod) {
		struct omi_txd *d = &ep->txq[ep->txq_cons & (OMI_TXQ_SIZE - 1)];

		txd_release(ep, d, d->mask);
		txd_unmap(ep, d);
		dev_kfree_skb_any(d->skb);
		d->skb = NULL;
		ep->txq_cons++;
	}
	spin_unlock_bh(&ep->tx_lock);
}

/* Gateway: copy the frame into the EP->RC ring. mask tells the RC
 * which nodes already got the frame over P2P.
 */
static int gw_tx(struct omi_ep *ep, struct sk_buff *skb, u8 mask)
{
	struct omi_gw *gw = ep->gw;
	unsigned int off;
	struct omi_slot_hdr *h;
	u32 tail;

	if (skb->len > OMI_GW_DATA)
		return -EMSGSIZE;
	bar_inval(ep, gw_off() + offsetof(struct omi_gw, ep_tail), sizeof(u32));
	tail = READ_ONCE(gw->ep_tail);
	if (ep->gw_head - tail >= OMI_GW_SLOTS)
		return -ENOSPC;

	off = gw_slot_off(false, ep->gw_head);
	h = ep->cpu + off;
	h->len = skb->len;
	h->flags = 0;
	h->mask = mask | (ep->self < OMI_MAX_NODES ? BIT(ep->self) : 0);
	h->pad = 0;
	if (skb_copy_bits(skb, 0, h + 1, skb->len))
		return -EFAULT;
	/* The clean completes (dsb) before the head store below. */
	bar_clean(ep, off, sizeof(*h) + skb->len);
	ep->gw_head++;
	WRITE_ONCE(gw->ep_head, ep->gw_head);
	bar_clean(ep, gw_off() + offsetof(struct omi_gw, ep_head), sizeof(u32));
	return 0;
}

static int fdb_lookup(struct omi_ep *ep, const u8 *mac)
{
	u32 h = jhash(mac, ETH_ALEN, 0) & (OMI_FDB_SIZE - 1);
	u64 e = READ_ONCE(ep->fdb[h]);

	if (!(e & BIT_ULL(63)) || memcmp(&e, mac, ETH_ALEN))
		return -1;
	return (e >> 48) & 0xff;
}

/* NAPI only. One 64-bit word per entry, so xmit never sees a torn one. */
static void fdb_learn(struct omi_ep *ep, const u8 *mac, u8 node)
{
	u32 h = jhash(mac, ETH_ALEN, 0) & (OMI_FDB_SIZE - 1);
	u64 e = 0;

	if (is_multicast_ether_addr(mac))
		return;
	memcpy(&e, mac, ETH_ALEN);
	e |= ((u64)node << 48) | BIT_ULL(63);
	if (READ_ONCE(ep->fdb[h]) != e)
		WRITE_ONCE(ep->fdb[h], e);
}

/* Which peer owns dest? Table MACs first, then learned addresses. */
static int route_unicast(struct omi_ep *ep, const u8 *dest)
{
	int p;

	for (p = 0; p < OMI_MAX_NODES; p++)
		if (ep->peer[p].epoch && ether_addr_equal(dest, ep->peer[p].mac))
			return p;
	return fdb_lookup(ep, dest);
}

/* tx_lock held. Queue skb for the TX thread; false if txq is full. */
static bool txq_add(struct omi_ep *ep, struct sk_buff *skb, u8 mask)
{
	struct omi_txd *d;
	unsigned long m = mask;
	unsigned int p;

	if (ep->txq_prod - ep->txq_cons >= OMI_TXQ_SIZE)
		return false;
	d = &ep->txq[ep->txq_prod & (OMI_TXQ_SIZE - 1)];
	d->skb = skb;
	d->mask = mask;
	d->mapped = false;
	d->nseg = 0;
	for_each_set_bit(p, &m, OMI_MAX_NODES)
		ep->peer[p].queued++;
	/* Release: tx_premap() reads entries without tx_lock. */
	smp_store_release(&ep->txq_prod, ep->txq_prod + 1);
	return true;
}

static netdev_tx_t omi_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct omi_ep *ep = netdev_priv(ndev);
	unsigned int len = skb->len;
	bool gw = false, gw_sent = false, queued = false;
	u8 p2p = 0;

	if (len < ETH_HLEN || len > OMI_SLOT_DATA)
		goto drop;
	if (skb_shinfo(skb)->nr_frags + 1 > OMI_MAX_SEGS && skb_linearize(skb))
		goto drop;

	spin_lock(&ep->tx_lock);
	if (is_multicast_ether_addr(skb->data)) {
		p2p = ep->up_mask;
		gw = ep->rc_up;
	} else {
		int p = route_unicast(ep, skb->data);

		if (p >= 0 && p < OMI_MAX_NODES) {
			/* A known peer that is still connecting is relayed by
			 * the RC. A stalled peer is not reading anything: its
			 * frames would only fill the slow gateway ring and
			 * starve real gateway traffic, so drop them.
			 */
			if (ep->up_mask & BIT(p))
				p2p = BIT(p);
			else if (!ep->peer[p].stalled)
				gw = ep->rc_up;
		} else if (p == OMI_FDB_GW || ether_addr_equal(skb->data, ep->rc_mac)) {
			gw = ep->rc_up;
		} else {
			/* Unknown unicast floods, as on a switch. */
			p2p = ep->up_mask;
			gw = ep->rc_up;
		}
	}
	spin_unlock(&ep->tx_lock);

	/* The gateway copy is made now; the P2P path keeps the skb. */
	if (gw) {
		int ret = gw_tx(ep, skb, p2p);

		if (!ret) {
			gw_sent = true;
			omi_inc(&ep->xs, gw_packets);
		} else if (ret == -ENOSPC) {
			omi_inc(&ep->xs, gw_full);
		}
	}

	if (p2p) {
		spin_lock(&ep->tx_lock);
		/* A peer may have gone since routing. */
		p2p &= ep->up_mask;
		if (p2p)
			queued = txq_add(ep, skb, p2p);
		/* Stop in the same lock section as the enqueue that fills a
		 * ring, so the stack never needs NETDEV_TX_BUSY: a requeued
		 * skb can be overtaken by later ones from another CPU.
		 */
		if (ep->txq_prod - ep->txq_cons >= OMI_TXQ_SIZE ||
		    !peers_have_room(ep, ep->up_mask)) {
			netif_stop_queue(ndev);
			omi_inc(&ep->xs, queue_stops);
		}
		spin_unlock(&ep->tx_lock);
		if (queued)
			wake_up(&ep->tx_wait);
	}

	if (!queued && !gw_sent)
		goto drop;
	omi_inc(&ep->xs, packets);
	omi_add(&ep->xs, bytes, len);
	if (!queued)
		dev_consume_skb_any(skb);
	return NETDEV_TX_OK;

drop:
	omi_inc(&ep->xs, dropped);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---------------------------------------------------------------- */
/* RX                                                                */

static void rx_deliver(struct omi_ep *ep, const void *data, u32 len,
		       u8 from, bool p2p)
{
	struct net_device *ndev = ep->ndev;
	struct sk_buff *skb;

	skb = napi_alloc_skb(&ep->napi, len);
	if (!skb) {
		omi_inc(&ep->rs, dropped);
		return;
	}
	skb_put_data(skb, data, len);
	/* P2P payloads only crossed PCIe links (LCRC protected) and DRAM. */
	if (p2p)
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->protocol = eth_type_trans(skb, ndev);
	fdb_learn(ep, eth_hdr(skb)->h_source, from);
	u64_stats_update_begin(&ep->rs.syncp);
	u64_stats_inc(&ep->rs.packets);
	u64_stats_add(&ep->rs.bytes, len);
	u64_stats_inc(p2p ? &ep->rs.p2p_packets : &ep->rs.gw_packets);
	u64_stats_update_end(&ep->rs.syncp);
	napi_gro_receive(&ep->napi, skb);
}

/* Write credit (and, after a connect, the ack) into the sender's BAR. */
static void rx_credit(struct omi_ep *ep, unsigned int r)
{
	struct omi_peer *pr = &ep->peer[r];
	void __iomem *c;

	/* Pairs with smp_store_release() in peer_set(): the iATU region
	 * is programmed before we store through the window.
	 */
	if (!smp_load_acquire(&pr->win_ok))
		return;
	c = pr->win + BAR_OFF(tx_cons[ep->self]);
	if (pr->rx_ack) {
		writel(pr->rx_tail, c + offsetof(struct omi_cons, tail));
		writel(pr->rx_epoch, c + offsetof(struct omi_cons, ack));
		pr->rx_ack = false;
		pr->rx_credit = pr->rx_tail;
		return;
	}
	if (pr->rx_credit == pr->rx_tail)
		return;
	/* The slot reads above must complete before the sender can see
	 * the credit and reuse the slots. writel() only orders earlier
	 * stores, so a full barrier is needed for the loads.
	 */
	mb();
	writel(pr->rx_tail, c + offsetof(struct omi_cons, tail));
	pr->rx_credit = pr->rx_tail;
}

static int rx_ring(struct omi_ep *ep, unsigned int r, int budget)
{
	struct omi_peer *pr = &ep->peer[r];
	unsigned int poff = BAR_OFF(rx_prod[r]);
	struct omi_prod *prod = (void *)ep->bar + poff;
	u32 head, epoch, avail;
	int done = 0;

	bar_inval(ep, poff, sizeof(*prod));
	epoch = READ_ONCE(prod->token);
	if (!epoch)
		return 0;
	dma_rmb();
	head = READ_ONCE(prod->head);
	if (epoch != pr->rx_epoch) {
		/* Sender (re)connected; it wrote head before the token. */
		pr->rx_epoch = epoch;
		pr->rx_tail = head;
		pr->rx_ack = true;
		omi_inc(&ep->rs, connects);
	}
	avail = head - pr->rx_tail;
	if (avail > OMI_RING_SLOTS) {
		pr->rx_tail = head;
		omi_inc(&ep->rs, resyncs);
		avail = 0;
	}
	while (avail && done < budget) {
		unsigned int off = ring_off(r) +
			(pr->rx_tail & (OMI_RING_SLOTS - 1)) * OMI_SLOT;
		struct omi_slot_hdr *h = ep->cpu + off;
		u32 len;

		bar_inval(ep, off, sizeof(*h));
		len = READ_ONCE(h->len);
		if (len < ETH_HLEN || len > OMI_SLOT_DATA) {
			omi_inc(&ep->rs, errors);
		} else {
			bar_inval(ep, off + sizeof(*h), len);
			rx_deliver(ep, h + 1, len, r, true);
		}
		pr->rx_tail++;
		avail--;
		done++;
	}
	rx_credit(ep, r);
	return done;
}

static int gw_rx(struct omi_ep *ep, int budget)
{
	struct omi_gw *gw = ep->gw;
	u32 head;
	int done = 0;

	bar_inval(ep, gw_off() + offsetof(struct omi_gw, rc_head), sizeof(u32));
	head = READ_ONCE(gw->rc_head);
	if (head - ep->gw_rx_tail > OMI_GW_SLOTS) {
		ep->gw_rx_tail = head;
		omi_inc(&ep->rs, resyncs);
	}
	while (ep->gw_rx_tail != head && done < budget) {
		unsigned int off = gw_slot_off(true, ep->gw_rx_tail);
		struct omi_slot_hdr *h = ep->cpu + off;
		u32 len;

		bar_inval(ep, off, sizeof(*h));
		len = READ_ONCE(h->len);
		if (len < ETH_HLEN || len > OMI_GW_DATA) {
			omi_inc(&ep->rs, errors);
		} else {
			bar_inval(ep, off + sizeof(*h), len);
			rx_deliver(ep, h + 1, len, OMI_FDB_GW, false);
		}
		ep->gw_rx_tail++;
		done++;
	}
	if (done) {
		/* Slot reads complete before the RC can see the slots free. */
		mb();
		WRITE_ONCE(gw->rc_tail, ep->gw_rx_tail);
		bar_clean(ep, gw_off() + offsetof(struct omi_gw, rc_tail), sizeof(u32));
	}
	return done;
}

static int omi_napi(struct napi_struct *napi, int budget)
{
	struct omi_ep *ep = container_of(napi, struct omi_ep, napi);
	int work = 0;
	unsigned int k;

	omi_inc(&ep->rs, polls);
	if (ep->self < OMI_MAX_NODES) {
		for (k = 0; k < OMI_N_RINGS && work < budget; k++) {
			unsigned int r = (ep->rx_rr + k) % OMI_N_RINGS;

			if (r != ep->self)
				work += rx_ring(ep, r, budget - work);
		}
		ep->rx_rr++;
	}
	if (work < budget)
		work += gw_rx(ep, budget - work);
	if (work < budget)
		napi_complete_done(napi, work);
	return work;
}

/* ---------------------------------------------------------------- */
/* Control                                                           */

/* Is there anything for NAPI? Lockless hints; NAPI rechecks. */
static bool rx_pending(struct omi_ep *ep)
{
	unsigned int r;

	if (ep->self < OMI_MAX_NODES) {
		for (r = 0; r < OMI_N_RINGS; r++) {
			struct omi_peer *pr = &ep->peer[r];
			unsigned int poff = BAR_OFF(rx_prod[r]);
			struct omi_prod *prod = (void *)ep->bar + poff;
			u32 epoch;

			if (r == ep->self)
				continue;
			bar_inval(ep, poff, sizeof(*prod));
			epoch = READ_ONCE(prod->token);
			if (!epoch)
				continue;
			if (epoch != READ_ONCE(pr->rx_epoch) ||
			    READ_ONCE(prod->head) != READ_ONCE(pr->rx_tail))
				return true;
			/* An ack can only go out once we have a window. */
			if (READ_ONCE(pr->rx_ack) && smp_load_acquire(&pr->win_ok))
				return true;
		}
	}
	bar_inval(ep, gw_off() + offsetof(struct omi_gw, rc_head), sizeof(u32));
	return READ_ONCE(ep->gw->rc_head) != READ_ONCE(ep->gw_rx_tail);
}

static void hdr_publish(struct omi_ep *ep)
{
	bar_clean(ep, BAR_OFF(hdr), sizeof(struct omi_hdr));
}

/* ctl thread. Stop sending to peer p and wait until no eDMA run that
 * may target it is in flight.
 */
static void peer_down(struct omi_ep *ep, unsigned int p)
{
	struct omi_peer *pr = &ep->peer[p];
	u32 target;

	spin_lock_bh(&ep->tx_lock);
	if (pr->state == OMI_PEER_UP)
		omi_inc(&ep->cs, peer_down);
	pr->state = OMI_PEER_DOWN;
	pr->stalled = false;
	pr->epoch = 0;
	ep->up_mask &= ~BIT(p);
	/* Runs built from now on leave p out; wait for the ones before. */
	target = ep->build_gen;
	spin_unlock_bh(&ep->tx_lock);
	while ((s32)(READ_ONCE(ep->done_gen) - target) < 0 && ep->tx_task)
		usleep_range(20, 50);
	dev_info(ep->dev, "peer %u down\n", p);
}

/* ctl thread. Send (or resend) our connect into peer p's BAR. */
static void peer_connect(struct omi_ep *ep, unsigned int p)
{
	struct omi_peer *pr = &ep->peer[p];
	void __iomem *prod = pr->win + BAR_OFF(rx_prod[ep->self]);

	/* head before token: a receiver that sees the token sees head.
	 * Both are posted writes from this CPU through one window, and
	 * the barrier in writel() keeps them in order.
	 */
	writel(0, prod + offsetof(struct omi_prod, head));
	writel(pr->token, prod + offsetof(struct omi_prod, token));
	pr->conn_sent = jiffies;
}

static void peer_set(struct omi_ep *ep, unsigned int p, u32 epoch, u64 pci,
		     const u8 *mac)
{
	struct omi_peer *pr = &ep->peer[p];
	u32 token;

	if (!ep->edma || !ep->ob)
		return;
	if (!pr->win_ok || pr->pci != pci) {
		if (program_outbound(ep, p, pci))
			return;
		pr->pci = pci;
		/* Pairs with smp_load_acquire() in rx_credit(). */
		smp_store_release(&pr->win_ok, true);
	}
	/* A new token per connect: an ack left over from an earlier
	 * connection to this peer can never match it.
	 */
	do {
		token = get_random_u32();
	} while (!token || token == pr->token);

	spin_lock_bh(&ep->tx_lock);
	memcpy(pr->mac, mac, ETH_ALEN);
	pr->epoch = epoch;
	pr->token = token;
	pr->state = OMI_PEER_CONNECTING;
	pr->head = 0;
	pr->tail = 0;
	pr->full_since = 0;
	spin_unlock_bh(&ep->tx_lock);
	peer_connect(ep, p);
	dev_info(ep->dev, "peer %u %pM at %#llx, connecting\n", p, mac, pci);
}

static void table_apply(struct omi_ep *ep)
{
	struct omi_ctl *ctl = &ep->bar->ctl;
	u32 gen;
	unsigned int p;

	if (!ep->rc_up)
		return;
	bar_inval(ep, BAR_OFF(ctl), sizeof(*ctl) + sizeof(ep->bar->peers));
	gen = READ_ONCE(ctl->table_gen);
	if (gen == ep->table_gen)
		return;
	dma_rmb();
	if (ep->self >= OMI_MAX_NODES) {
		u8 self = READ_ONCE(ctl->self_idx);

		if (self >= OMI_N_RINGS)
			return;
		ep->self = self;
		dev_info(ep->dev, "node index %u\n", self);
	}
	for (p = 0; p < OMI_MAX_NODES; p++) {
		struct omi_peer_entry *e = &ep->bar->peers[p];
		u32 epoch, again;
		u64 pci;
		u8 mac[ETH_ALEN];

		if (p == ep->self)
			continue;
		/* The RC writes epoch last; reread it so the fields match. */
		do {
			epoch = READ_ONCE(e->epoch);
			dma_rmb();
			pci = ((u64)READ_ONCE(e->bar_hi) << 32) | READ_ONCE(e->bar_lo);
			memcpy(mac, e->mac, ETH_ALEN);
			dma_rmb();
			bar_inval(ep, BAR_OFF(peers[p]), sizeof(*e));
			again = READ_ONCE(e->epoch);
		} while (again != epoch);

		if (p >= OMI_N_RINGS)
			continue;
		if (epoch == ep->peer[p].epoch && (!epoch || pci == ep->peer[p].pci))
			continue;
		if (ep->peer[p].epoch)
			peer_down(ep, p);
		if (epoch && pci && is_valid_ether_addr(mac))
			peer_set(ep, p, epoch, pci, mac);
	}
	ep->table_gen = gen;
	omi_inc(&ep->cs, table_updates);
	WRITE_ONCE(ep->bar->hdr.table_seen, gen);
	hdr_publish(ep);
}

static void peers_poll(struct omi_ep *ep)
{
	unsigned int p;

	for (p = 0; p < OMI_N_RINGS; p++) {
		struct omi_peer *pr = &ep->peer[p];
		struct omi_cons *c;

		if (p == ep->self || pr->state != OMI_PEER_CONNECTING)
			continue;
		c = (void *)ep->bar + BAR_OFF(tx_cons[p]);
		bar_inval(ep, BAR_OFF(tx_cons[p]), sizeof(*c));
		if (READ_ONCE(c->ack) == pr->token) {
			spin_lock_bh(&ep->tx_lock);
			pr->state = OMI_PEER_UP;
			pr->head = 0;
			pr->tail = READ_ONCE(c->tail);
			ep->up_mask |= BIT(p);
			spin_unlock_bh(&ep->tx_lock);
			omi_inc(&ep->cs, peer_up);
			dev_info(ep->dev, "peer %u up\n", p);
		} else if (time_after(jiffies, pr->conn_sent + OMI_CONNECT_RETRY)) {
			peer_connect(ep, p);
		}
	}

	/* A peer whose ring stayed full for OMI_STALL_TIMEOUT is not
	 * reading (interface down, stuck). Stop waiting for it so the
	 * shared queue keeps moving; frames to it are dropped until it
	 * returns credit.
	 */
	spin_lock_bh(&ep->tx_lock);
	for (p = 0; p < OMI_N_RINGS; p++) {
		struct omi_peer *pr = &ep->peer[p];
		u32 old = pr->tail;

		if (pr->state != OMI_PEER_UP)
			continue;
		peer_refresh_tail(ep, p);
		if (pr->stalled) {
			if (pr->tail != old) {
				pr->stalled = false;
				pr->full_since = 0;
				ep->up_mask |= BIT(p);
			}
			continue;
		}
		/* Same test as xmit uses to stop the queue: once a peer has
		 * no room, nothing reaches the TX thread any more, so the
		 * stall has to be seen here.
		 */
		if (!peers_have_room(ep, BIT(p))) {
			if (!pr->full_since) {
				pr->full_since = jiffies | 1;
			} else if (time_after(jiffies, pr->full_since + OMI_STALL_TIMEOUT)) {
				pr->stalled = true;
				ep->up_mask &= ~BIT(p);
				omi_inc(&ep->cs, stalls);
				dev_warn_ratelimited(ep->dev,
						     "peer %u not consuming, dropping its frames\n",
						     p);
			}
		} else {
			pr->full_since = 0;
		}
	}
	if (omi_maybe_wake(ep))
		omi_inc(&ep->cs, queue_wakes);
	spin_unlock_bh(&ep->tx_lock);
}

static void rc_poll(struct omi_ep *ep)
{
	struct omi_ctl *ctl = &ep->bar->ctl;
	bool up;

	bar_inval(ep, BAR_OFF(ctl), sizeof(*ctl));
	/* A control line left from before our last epoch is not a session. */
	up = (READ_ONCE(ctl->flags) & OMI_RC_UP) &&
	     READ_ONCE(ctl->ep_epoch) == ep->epoch;
	if (up && !ep->rc_up) {
		spin_lock_bh(&ep->tx_lock);
		memcpy(ep->rc_mac, ctl->mac, ETH_ALEN);
		ep->rc_up = true;
		spin_unlock_bh(&ep->tx_lock);
	} else if (!up && ep->rc_up) {
		spin_lock_bh(&ep->tx_lock);
		ep->rc_up = false;
		spin_unlock_bh(&ep->tx_lock);
	}
	if (up && !ether_addr_equal(ep->rc_mac, ctl->mac)) {
		spin_lock_bh(&ep->tx_lock);
		memcpy(ep->rc_mac, ctl->mac, ETH_ALEN);
		spin_unlock_bh(&ep->tx_lock);
	}
}

/*
 * The host side reset our link (hot reset, link down, Cluster Box
 * reboot). The controller has reset its non-sticky registers and
 * holds the LTSSM until DLY2_DONE. Everything in flight to or from
 * peers may have been lost, so stop P2P, restore the registers and
 * start a new epoch: the RC re-activates us and every peer
 * reconnects with fresh ring state.
 */
static void link_reset(struct omi_ep *ep)
{
	unsigned int p;

	omi_inc(&ep->cs, link_resets);
	for (p = 0; p < OMI_MAX_NODES; p++)
		if (ep->peer[p].epoch)
			peer_down(ep, p);
	spin_lock_bh(&ep->tx_lock);
	ep->rc_up = false;
	spin_unlock_bh(&ep->tx_lock);

	/* The iATU loses its targets but can keep the enable bits: a
	 * region left alone would translate to the wrong memory. Program
	 * every region again while DLY2 still holds the link down, so no
	 * TLP passes through a stale translation.
	 */
	program_config_space(ep);
	program_inbound_bar0(ep);
	for (p = 0; p < OMI_MAX_NODES; p++)
		if (ep->peer[p].win_ok)
			program_outbound(ep, p, ep->peer[p].pci);

	do {
		ep->epoch = get_random_u32();
	} while (!ep->epoch);
	WRITE_ONCE(ep->bar->hdr.epoch, ep->epoch);
	hdr_publish(ep);

	rk_hiword(ep->apb, PCIE_CLIENT_HOT_RESET_CTRL,
		  PCIE_LTSSM_APP_DLY2_DONE, PCIE_LTSSM_APP_DLY2_DONE);
	dev_info(ep->dev, "link reset by host, new epoch %#x\n", ep->epoch);
}

static int ctl_thread(void *data)
{
	struct omi_ep *ep = data;
	struct net_device *ndev = ep->ndev;
	unsigned long next_ctl = jiffies;
	unsigned int idle = 0;
	bool running;

	set_user_nice(current, -20);
	while (!kthread_should_stop()) {
		u32 misc;

		omi_inc(&ep->cs, poll_cycles);
		misc = readl(ep->apb + PCIE_CLIENT_INTR_STATUS_MISC);
		if (misc) {
			writel(misc, ep->apb + PCIE_CLIENT_INTR_STATUS_MISC);
			if (misc & PCIE_LINK_REQ_RST_NOT_INT)
				link_reset(ep);
		}
		if (time_after_eq(jiffies, next_ctl)) {
			rc_poll(ep);
			table_apply(ep);
			peers_poll(ep);
			if (ep->rc_up || ep->up_mask) {
				if (!netif_carrier_ok(ndev))
					netif_carrier_on(ndev);
			} else if (netif_carrier_ok(ndev)) {
				netif_carrier_off(ndev);
			}
			next_ctl = jiffies + max_t(unsigned long, 1, msecs_to_jiffies(1));
		}

		running = netif_running(ndev);
		if (running && test_bit(NAPI_STATE_SCHED, &ep->napi.state)) {
			/* NAPI is still draining (or disabled): it reschedules
			 * itself while it has work, so there is nothing to kick.
			 */
			idle = 0;
			usleep_range_state(20, 50, TASK_IDLE);
		} else if (running && rx_pending(ep)) {
			/* Nothing raises an interrupt for a peer write, so this
			 * thread stands in for the RX IRQ. local_bh_enable()
			 * runs the NAPI poll before returning.
			 */
			idle = 0;
			omi_inc(&ep->cs, napi_kicks);
			local_bh_disable();
			napi_schedule(&ep->napi);
			local_bh_enable();
			cond_resched();
		} else if (idle < OMI_POLL_BUSY_LOOPS) {
			/* TASK_IDLE: a sleeping poller is not load. */
			idle++;
			usleep_range_state(20, 50, TASK_IDLE);
		} else {
			usleep_range_state(OMI_POLL_IDLE_US, 2 * OMI_POLL_IDLE_US,
					   TASK_IDLE);
		}
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* netdev                                                            */

static int omi_open(struct net_device *ndev)
{
	struct omi_ep *ep = netdev_priv(ndev);

	napi_enable(&ep->napi);
	ep->tx_task = kthread_run(tx_thread, ep, "omi-tx");
	if (IS_ERR(ep->tx_task)) {
		int err = PTR_ERR(ep->tx_task);

		ep->tx_task = NULL;
		napi_disable(&ep->napi);
		return err;
	}
	netif_start_queue(ndev);
	return 0;
}

static int omi_stop(struct net_device *ndev)
{
	struct omi_ep *ep = netdev_priv(ndev);

	netif_tx_disable(ndev);
	if (ep->tx_task) {
		kthread_stop(ep->tx_task);
		ep->tx_task = NULL;
	}
	txq_purge(ep);
	napi_disable(&ep->napi);
	return 0;
}

static void omi_get_stats64(struct net_device *ndev,
			    struct rtnl_link_stats64 *s)
{
	struct omi_ep *ep = netdev_priv(ndev);
	unsigned int start;

	do {
		start = u64_stats_fetch_begin(&ep->xs.syncp);
		s->tx_packets = u64_stats_read(&ep->xs.packets);
		s->tx_bytes = u64_stats_read(&ep->xs.bytes);
		s->tx_dropped = u64_stats_read(&ep->xs.dropped);
	} while (u64_stats_fetch_retry(&ep->xs.syncp, start));
	do {
		start = u64_stats_fetch_begin(&ep->ts.syncp);
		s->tx_dropped += u64_stats_read(&ep->ts.dropped);
		s->tx_errors = u64_stats_read(&ep->ts.dma_errors);
	} while (u64_stats_fetch_retry(&ep->ts.syncp, start));
	do {
		start = u64_stats_fetch_begin(&ep->rs.syncp);
		s->rx_packets = u64_stats_read(&ep->rs.packets);
		s->rx_bytes = u64_stats_read(&ep->rs.bytes);
		s->rx_errors = u64_stats_read(&ep->rs.errors);
		s->rx_dropped = u64_stats_read(&ep->rs.dropped);
	} while (u64_stats_fetch_retry(&ep->rs.syncp, start));
}

static const struct net_device_ops omi_netdev_ops = {
	.ndo_open = omi_open,
	.ndo_stop = omi_stop,
	.ndo_start_xmit = omi_xmit,
	.ndo_get_stats64 = omi_get_stats64,
	.ndo_validate_addr = eth_validate_addr,
};

/* ethtool -S. Each group has its own writer context. */
#define OMI_STAT(name, grp, f) { name, offsetof(struct omi_ep, grp) + \
	offsetof(typeof(((struct omi_ep *)0)->grp), f), \
	offsetof(struct omi_ep, grp) + offsetof(typeof(((struct omi_ep *)0)->grp), syncp) }

static const struct {
	const char name[ETH_GSTRING_LEN];
	size_t off;
	size_t sync;
} omi_stats[] = {
	OMI_STAT("tx_gw_packets", xs, gw_packets),
	OMI_STAT("tx_gw_ring_full", xs, gw_full),
	OMI_STAT("tx_queue_stops", xs, queue_stops),
	OMI_STAT("tx_p2p_frames", ts, p2p_frames),
	OMI_STAT("tx_dma_runs", ts, dma_runs),
	OMI_STAT("tx_dma_elements", ts, dma_elems),
	OMI_STAT("tx_dma_errors", ts, dma_errors),
	OMI_STAT("tx_dropped_late", ts, dropped),
	OMI_STAT("tx_peer_gone_drops", ts, peer_full_drops),
	OMI_STAT("tx_queue_wakes", ts, queue_wakes),
	OMI_STAT("tx_dma_wait_ns", ts, dma_wait_ns),
	OMI_STAT("tx_dma_bytes", ts, dma_bytes),
	OMI_STAT("rx_p2p_packets", rs, p2p_packets),
	OMI_STAT("rx_gw_packets", rs, gw_packets),
	OMI_STAT("rx_bad_len", rs, errors),
	OMI_STAT("rx_alloc_drops", rs, dropped),
	OMI_STAT("rx_resyncs", rs, resyncs),
	OMI_STAT("rx_connects", rs, connects),
	OMI_STAT("rx_napi_polls", rs, polls),
	OMI_STAT("ctl_poll_cycles", cs, poll_cycles),
	OMI_STAT("ctl_napi_kicks", cs, napi_kicks),
	OMI_STAT("ctl_table_updates", cs, table_updates),
	OMI_STAT("ctl_peer_up", cs, peer_up),
	OMI_STAT("ctl_peer_down", cs, peer_down),
	OMI_STAT("ctl_peer_stalls", cs, stalls),
	OMI_STAT("ctl_queue_wakes", cs, queue_wakes),
	OMI_STAT("ctl_link_resets", cs, link_resets),
};

static void omi_get_drvinfo(struct net_device *ndev, struct ethtool_drvinfo *info)
{
	struct omi_ep *ep = netdev_priv(ndev);

	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	strscpy(info->bus_info, dev_name(ep->dev), sizeof(info->bus_info));
	snprintf(info->fw_version, sizeof(info->fw_version), "proto %u node %d",
		 OPENMIOP_VERSION, ep->self < OMI_MAX_NODES ? ep->self : -1);
}

static int omi_get_sset_count(struct net_device *ndev, int sset)
{
	return sset == ETH_SS_STATS ? ARRAY_SIZE(omi_stats) : -EOPNOTSUPP;
}

static void omi_get_strings(struct net_device *ndev, u32 sset, u8 *buf)
{
	int i;

	if (sset != ETH_SS_STATS)
		return;
	for (i = 0; i < ARRAY_SIZE(omi_stats); i++)
		memcpy(buf + i * ETH_GSTRING_LEN, omi_stats[i].name, ETH_GSTRING_LEN);
}

static void omi_get_ethtool_stats(struct net_device *ndev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct omi_ep *ep = netdev_priv(ndev);
	int i;

	for (i = 0; i < ARRAY_SIZE(omi_stats); i++) {
		const struct u64_stats_sync *sync = (void *)ep + omi_stats[i].sync;
		const u64_stats_t *v = (void *)ep + omi_stats[i].off;
		unsigned int start;

		do {
			start = u64_stats_fetch_begin(sync);
			data[i] = u64_stats_read(v);
		} while (u64_stats_fetch_retry(sync, start));
	}
}

static const struct ethtool_ops omi_ethtool_ops = {
	.get_drvinfo = omi_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_sset_count = omi_get_sset_count,
	.get_strings = omi_get_strings,
	.get_ethtool_stats = omi_get_ethtool_stats,
};

/* ---------------------------------------------------------------- */
/* Probe                                                             */

static void bar_init(struct omi_ep *ep)
{
	struct omi_hdr *h = &ep->bar->hdr;

	/* The whole BAR is visible to the RC and the peers: no stale
	 * kernel data in it.
	 */
	memset(ep->cpu, 0, OPENMIOP_BAR_SIZE);
	do {
		ep->epoch = get_random_u32();
	} while (!ep->epoch);
	h->version = OPENMIOP_VERSION;
	h->epoch = ep->epoch;
	memcpy(h->mac, ep->ndev->dev_addr, ETH_ALEN);
	h->n_rings = OMI_N_RINGS;
	h->ring_off = OMI_RING_OFF;
	h->ring_slots = OMI_RING_SLOTS;
	h->slot_size = OMI_SLOT;
	h->gw_off = OMI_GW_OFF;
	h->gw_slots = OMI_GW_SLOTS;
	h->gw_slot_size = OMI_GW_SLOT;
	h->flags = OMI_F_UP;
	dma_sync_single_for_device(ep->dev, ep->dma, OPENMIOP_BAR_SIZE, DMA_TO_DEVICE);
	/* Magic last: the RC ignores the BAR until it is set. */
	WRITE_ONCE(h->magic, OPENMIOP_MAGIC);
	hdr_publish(ep);
}

/* Tell the RC we are leaving and wait until it has detached us from
 * every peer, so nobody targets this BAR when the link drops. An MMIO
 * read in flight while LTSSM goes down can also wedge the MT7620A.
 */
static void bar_leave(struct omi_ep *ep)
{
	struct omi_ctl *ctl = &ep->bar->ctl;
	int i;

	ep->bar->hdr.flags |= OMI_F_DOWN;
	hdr_publish(ep);
	for (i = 0; i < OMI_DOWN_ACK_TIMEOUT_MS / 10; i++) {
		bar_inval(ep, BAR_OFF(ctl), sizeof(*ctl));
		if (READ_ONCE(ctl->down_ack) == ep->epoch)
			return;
		if (!(READ_ONCE(ctl->flags) & OMI_RC_UP))
			return;
		usleep_range(10000, 11000);
	}
	dev_warn(ep->dev, "RC did not acknowledge leave\n");
}

/* The PHY latches rockchip,pcie30-phymode when its reset is released.
 * phy_init() does that release, and only when this driver is the first
 * user. fe160000 must already be unbound, or the write sits in the GRF
 * and the PHY stays bifurcated. The GRF itself is clocked by the PHY
 * pclk; a write while that clock is off reads back as zero.
 */
static void program_phy_mode(struct device *dev)
{
	struct device_node *np;
	struct clk *pclk = NULL;
	void __iomem *grf;
	u32 mode = lanes >= 4 ? PHY_MODE_AGGREGATION : PHY_MODE_NANBNB;
	u32 before, after;

	np = of_find_compatible_node(NULL, NULL, "rockchip,rk3588-pcie3-phy");
	if (np) {
		pclk = of_clk_get(np, 0);
		of_node_put(np);
	}
	if (IS_ERR_OR_NULL(pclk)) {
		dev_err(dev, "pcie30 phy pclk: %ld\n", pclk ? PTR_ERR(pclk) : -ENODEV);
		pclk = NULL;
	} else if (clk_prepare_enable(pclk)) {
		dev_err(dev, "pcie30 phy pclk enable failed\n");
		clk_put(pclk);
		pclk = NULL;
	}

	grf = ioremap(RK3588_PCIE3PHY_GRF_BASE, RK3588_PCIE3PHY_GRF_SIZE);
	if (!grf) {
		dev_err(dev, "pcie30 phy grf map failed\n");
		goto out_clk;
	}
	before = readl(grf + RK3588_PCIE3PHY_GRF_CMN_CON0);
	if ((before & 0x7) != mode)
		writel((0x7u << 16) | mode, grf + RK3588_PCIE3PHY_GRF_CMN_CON0);
	after = readl(grf + RK3588_PCIE3PHY_GRF_CMN_CON0);
	/* Per-lane RX common-refclk mode (mainline:
	 * rockchip,rx-common-refclk-mode), for comparison between kernels.
	 */
	dev_info(dev, "pcie30 phy mode %u (was %#x, now %#x), lane con1 %#x %#x %#x %#x\n",
		 mode, before, after, readl(grf + 0x1004), readl(grf + 0x1104),
		 readl(grf + 0x2004), readl(grf + 0x2104));
	iounmap(grf);
out_clk:
	if (pclk) {
		clk_disable_unprepare(pclk);
		clk_put(pclk);
	}
}

/* A random address changes on every load and leaves stale ARP entries
 * on the peers and the router. Prefer a DT address, then one derived
 * from the SoC serial number that U-Boot puts in the root node.
 */
static void omi_set_mac(struct device *dev, struct net_device *ndev)
{
	const char *serial;
	u8 mac[ETH_ALEN];
	u32 h0, h1;

	if (!of_get_ethdev_address(dev->of_node, ndev))
		return;

	if (of_property_read_string(of_root, "serial-number", &serial) ||
	    !*serial) {
		dev_warn(dev, "no serial-number, using a random MAC\n");
		eth_hw_addr_random(ndev);
		return;
	}

	h0 = jhash(serial, strlen(serial), 0x6f6d6930);	/* "omi0" */
	h1 = jhash(serial, strlen(serial), h0);
	mac[0] = ((h0 >> 24) & 0xfc) | 0x02;	/* unicast, locally administered */
	mac[1] = h0 >> 16;
	mac[2] = h0 >> 8;
	mac[3] = h0;
	mac[4] = h1 >> 8;
	mac[5] = h1;
	eth_hw_addr_set(ndev, mac);
}

/* The vendor DT (U-Boot rewrites /pcie@fe150000) uses pcie-dbi and
 * pcie-apb; the upstream pcie-ep@fe150000 node uses dbi, dbi2, apb,
 * addr_space and atu.
 */
static void __iomem *map_res(struct platform_device *pdev, const char *a,
			     const char *b, struct resource **res)
{
	struct resource *r = platform_get_resource_byname(pdev, IORESOURCE_MEM, a);

	if (!r && b)
		r = platform_get_resource_byname(pdev, IORESOURCE_MEM, b);
	if (res)
		*res = r;
	if (!r)
		return NULL;
	return devm_ioremap_resource(&pdev->dev, r);
}

static int omi_map_regs(struct platform_device *pdev, struct omi_ep *ep)
{
	struct resource *dbi_res, *atu_res, *as_res;

	ep->apb = map_res(pdev, "apb", "pcie-apb", NULL);
	ep->dbi = map_res(pdev, "dbi", "pcie-dbi", &dbi_res);
	if (IS_ERR_OR_NULL(ep->apb) || IS_ERR_OR_NULL(ep->dbi)) {
		dev_err(&pdev->dev, "missing apb/dbi registers\n");
		return -EINVAL;
	}

	ep->dbi2 = map_res(pdev, "dbi2", NULL, NULL);
	ep->atu = map_res(pdev, "atu", NULL, &atu_res);
	if (IS_ERR(ep->dbi2) || IS_ERR(ep->atu))
		return -EINVAL;
	if (!ep->dbi2 || !ep->atu) {
		if (resource_size(dbi_res) < DEFAULT_DBI_ATU_OFFSET + ATU_EDMA_OFFSET + 0x400) {
			dev_err(&pdev->dev, "dbi region is too small for iATU and eDMA\n");
			return -EINVAL;
		}
		ep->dbi2 = ep->dbi + DEFAULT_DBI2_OFFSET;
		ep->atu = ep->dbi + DEFAULT_DBI_ATU_OFFSET;
		atu_res = NULL;
	}
	if (use_edma && (!atu_res || resource_size(atu_res) >= ATU_EDMA_OFFSET + 0x400))
		ep->edma = ep->atu + ATU_EDMA_OFFSET;

	as_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "addr_space");
	if (as_res) {
		if (resource_size(as_res) < (u64)OMI_MAX_NODES * OPENMIOP_BAR_SIZE) {
			dev_err(&pdev->dev, "addr_space is too small\n");
			return -EINVAL;
		}
		ep->ob_phys = as_res->start;
	} else {
		ep->ob_phys = ob_base;
	}
	return 0;
}

static void omi_free_bar(struct omi_ep *ep)
{
	if (ep->raw)
		dma_free_noncoherent(ep->dev, ep->raw_size, ep->raw, ep->raw_dma,
				     DMA_BIDIRECTIONAL);
	ep->raw = NULL;
}

static int omi_alloc_bar(struct omi_ep *ep)
{
	/* Twice the size, so one BAR-sized window inside is aligned to
	 * the BAR size as the inbound iATU requires. Buddy allocations
	 * already are; a reserved-mem pool often is not.
	 */
	ep->raw_size = OPENMIOP_BAR_SIZE * 2;
	ep->raw = dma_alloc_noncoherent(ep->dev, ep->raw_size, &ep->raw_dma,
					DMA_BIDIRECTIONAL, GFP_KERNEL);
	if (!ep->raw) {
		ep->raw_size = OPENMIOP_BAR_SIZE;
		ep->raw = dma_alloc_noncoherent(ep->dev, ep->raw_size, &ep->raw_dma,
						DMA_BIDIRECTIONAL, GFP_KERNEL);
	}
	if (!ep->raw) {
		dev_err(ep->dev, "BAR buffer alloc failed\n");
		return -ENOMEM;
	}
	ep->dma = ALIGN(ep->raw_dma, OPENMIOP_BAR_SIZE);
	if (ep->dma + OPENMIOP_BAR_SIZE > ep->raw_dma + ep->raw_size) {
		dev_err(ep->dev, "BAR buffer %pad is not 16 MiB aligned\n", &ep->raw_dma);
		omi_free_bar(ep);
		return -EINVAL;
	}
	ep->cpu = ep->raw + (ep->dma - ep->raw_dma);
	ep->bar = ep->cpu;
	ep->gw = ep->cpu + OMI_GW_OFF;
	ep->scratch = ep->cpu + OMI_SCRATCH_OFF;
	ep->scratch_dma = ep->dma + OMI_SCRATCH_OFF;
	dev_info(ep->dev, "BAR at %pad, cacheable\n", &ep->dma);
	return 0;
}

static int openmiop_probe(struct platform_device *pdev)
{
	struct omi_ep *ep;
	struct net_device *ndev;
	int ret;

	BUILD_BUG_ON(sizeof(struct omi_hdr) != 64);
	BUILD_BUG_ON(sizeof(struct omi_ctl) != 64);
	BUILD_BUG_ON(sizeof(struct omi_peer_entry) != 32);
	BUILD_BUG_ON(BAR_OFF(rx_prod) != 0x400);
	BUILD_BUG_ON(BAR_OFF(tx_cons) != 0x600);
	BUILD_BUG_ON(sizeof(struct omi_bar_head) > OMI_GW_OFF);
	BUILD_BUG_ON(OMI_GW_OFF + sizeof(struct omi_gw) +
		     2 * OMI_GW_SLOTS * OMI_GW_SLOT > OMI_RING_OFF);
	BUILD_BUG_ON(OMI_RING_OFF + OMI_N_RINGS * OMI_RING_SLOTS * OMI_SLOT >
		     OMI_SCRATCH_OFF);
	BUILD_BUG_ON(OMI_SCRATCH_OFF + sizeof(struct omi_scratch) > OPENMIOP_BAR_SIZE);
	BUILD_BUG_ON(offsetof(struct omi_scratch, ll) % 64);
	BUILD_BUG_ON(OMI_N_RINGS > OMI_MAX_NODES || OMI_MAX_NODES > 8);
	BUILD_BUG_ON(!is_power_of_2(OMI_RING_SLOTS) || !is_power_of_2(OMI_GW_SLOTS));

	ndev = alloc_etherdev(sizeof(*ep));
	if (!ndev)
		return -ENOMEM;

	ep = netdev_priv(ndev);
	ep->dev = &pdev->dev;
	ep->ndev = ndev;
	ep->self = OMI_MAX_NODES;
	spin_lock_init(&ep->tx_lock);
	init_waitqueue_head(&ep->tx_wait);
	u64_stats_init(&ep->xs.syncp);
	u64_stats_init(&ep->ts.syncp);
	u64_stats_init(&ep->rs.syncp);
	u64_stats_init(&ep->cs.syncp);
	platform_set_drvdata(pdev, ep);

	ep->txq = devm_kcalloc(&pdev->dev, OMI_TXQ_SIZE, sizeof(*ep->txq), GFP_KERNEL);
	if (!ep->txq) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = omi_map_regs(pdev, ep);
	if (ret)
		goto err_free;

	ep->rst = devm_reset_control_array_get_exclusive(&pdev->dev);
	if (IS_ERR(ep->rst)) {
		ret = PTR_ERR(ep->rst);
		goto err_free;
	}

	ep->vpcie3v3 = devm_regulator_get_optional(&pdev->dev, "vpcie3v3");
	if (IS_ERR(ep->vpcie3v3)) {
		if (PTR_ERR(ep->vpcie3v3) != -ENODEV) {
			ret = PTR_ERR(ep->vpcie3v3);
			goto err_free;
		}
		ep->vpcie3v3 = NULL;
	}

	ret = reset_control_assert(ep->rst);
	if (ret)
		goto err_free;

	if (ep->vpcie3v3) {
		ret = regulator_enable(ep->vpcie3v3);
		if (ret)
			goto err_free;
	}

	ep->phy = devm_phy_get(&pdev->dev, "pcie-phy");
	if (IS_ERR(ep->phy)) {
		ret = PTR_ERR(ep->phy);
		goto err_reg;
	}
	program_phy_mode(&pdev->dev);
	ret = phy_init(ep->phy);
	if (ret)
		goto err_reg;
	ret = phy_power_on(ep->phy);
	if (ret)
		goto err_phy_exit;

	ret = reset_control_deassert(ep->rst);
	if (ret)
		goto err_phy;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		dev_warn(&pdev->dev, "dma mask 64 failed (%d)\n", ret);

	ret = devm_clk_bulk_get_all(&pdev->dev, &ep->clks);
	if (ret < 0)
		goto err_phy;
	ep->nclks = ret;
	ret = clk_bulk_prepare_enable(ep->nclks, ep->clks);
	if (ret)
		goto err_phy;
	msleep(10);

	if (!of_reserved_mem_device_init(&pdev->dev))
		ep->rmem = true;

	ret = omi_alloc_bar(ep);
	if (ret)
		goto err_clk;

	omi_set_mac(&pdev->dev, ndev);
	bar_init(ep);

	ret = hw_start(ep);
	if (ret)
		goto err_dma;
	edma_init(ep);
	if (ep->edma) {
		ep->ob = ioremap(ep->ob_phys, (size_t)OMI_MAX_NODES * OPENMIOP_BAR_SIZE);
		if (!ep->ob)
			dev_warn(ep->dev, "outbound window map failed, gateway only\n");
	}
	{
		unsigned int p;

		for (p = 0; p < OMI_MAX_NODES; p++)
			ep->peer[p].win = ep->ob ? ep->ob + p * OPENMIOP_BAR_SIZE : NULL;
	}

	strscpy(ndev->name, "omi%d", IFNAMSIZ);
	ndev->netdev_ops = &omi_netdev_ops;
	ndev->ethtool_ops = &omi_ethtool_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = OMI_GW_DATA - ETH_HLEN;
	ndev->mtu = 9000;
	ndev->features |= NETIF_F_SG | NETIF_F_GRO | NETIF_F_RXCSUM;
	ndev->hw_features |= NETIF_F_SG | NETIF_F_GRO | NETIF_F_RXCSUM;
	netif_napi_add(ndev, &ep->napi, omi_napi);
	netif_carrier_off(ndev);
	SET_NETDEV_DEV(ndev, &pdev->dev);

	ret = register_netdev(ndev);
	if (ret)
		goto err_hw;

	ep->ctl = kthread_run(ctl_thread, ep, "omi-ctl");
	if (IS_ERR(ep->ctl)) {
		ret = PTR_ERR(ep->ctl);
		ep->ctl = NULL;
		goto err_unreg;
	}

	dev_info(&pdev->dev, "%s mac %pM epoch %#x eDMA %s\n", ndev->name,
		 ndev->dev_addr, ep->epoch, ep->edma ? "on" : "off");
	return 0;

err_unreg:
	unregister_netdev(ndev);
err_hw:
	netif_napi_del(&ep->napi);
	if (ep->ob)
		iounmap(ep->ob);
	hw_stop(ep);
err_dma:
	omi_free_bar(ep);
err_clk:
	clk_bulk_disable_unprepare(ep->nclks, ep->clks);
	if (ep->rmem)
		of_reserved_mem_device_release(&pdev->dev);
err_phy:
	phy_power_off(ep->phy);
err_phy_exit:
	phy_exit(ep->phy);
err_reg:
	if (ep->vpcie3v3)
		regulator_disable(ep->vpcie3v3);
err_free:
	free_netdev(ndev);
	return ret;
}

static void openmiop_teardown(struct platform_device *pdev)
{
	struct omi_ep *ep = platform_get_drvdata(pdev);

	/* ctl first: it touches the netdev and connects peers. Then
	 * unregister stops TX and NAPI: no more writes into peer BARs.
	 */
	kthread_stop(ep->ctl);
	unregister_netdev(ep->ndev);
	bar_leave(ep);
	netif_napi_del(&ep->napi);
	disable_outbound(ep);
	if (ep->ob)
		iounmap(ep->ob);
	hw_stop(ep);
	omi_free_bar(ep);
	clk_bulk_disable_unprepare(ep->nclks, ep->clks);
	if (ep->rmem)
		of_reserved_mem_device_release(&pdev->dev);
	phy_power_off(ep->phy);
	phy_exit(ep->phy);
	if (ep->vpcie3v3)
		regulator_disable(ep->vpcie3v3);
	free_netdev(ep->ndev);
}

/*
 * Reboot and poweroff. Talos never unloads modules and a Debian
 * shutdown need not either, so without this the link would just drop:
 * peers keep writing into a BAR that is gone and the RC may have a read
 * in flight. Leave the fabric as remove() does, then stop the link.
 */
static void openmiop_shutdown(struct platform_device *pdev)
{
	struct omi_ep *ep = platform_get_drvdata(pdev);

	kthread_stop(ep->ctl);
	rtnl_lock();
	dev_close(ep->ndev);
	rtnl_unlock();
	bar_leave(ep);
	disable_outbound(ep);
	hw_stop(ep);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void openmiop_remove(struct platform_device *pdev)
{
	openmiop_teardown(pdev);
}
#else
static int openmiop_remove(struct platform_device *pdev)
{
	openmiop_teardown(pdev);
	return 0;
}
#endif

static const struct of_device_id openmiop_of_match[] = {
	{ .compatible = "mixtile,miop-ep-rk3588" },	/* vendor U-Boot rewrite */
	{ .compatible = "openmiop,rk3588-pcie-ep" },
	{ }
};
MODULE_DEVICE_TABLE(of, openmiop_of_match);

static struct platform_driver openmiop_driver = {
	.probe = openmiop_probe,
	.remove = openmiop_remove,
	.shutdown = openmiop_shutdown,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = openmiop_of_match,
	},
};
module_platform_driver(openmiop_driver);

MODULE_DESCRIPTION("openmiop RK3588 PCIe endpoint Ethernet");
MODULE_LICENSE("GPL");
