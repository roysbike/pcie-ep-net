// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * openmiop endpoint: RK3588 PCIe3 as an endpoint, netdev omi0.
 *
 * Binds the vendor DT node (compatible mixtile,miop-ep-rk3588).
 * Programs a 16 MiB BAR0 onto coherent memory. Frames for the router
 * use the small shared rings. Frames for the other blade are written
 * through an outbound iATU window aimed at that blade's BAR, so they
 * cross the switch and never enter the MT7620A.
 *
 * Register programming follows the public Synopsys DWC layout used by
 * drivers/pci/controller/dwc/pcie-designware.c (Linux 6.1) and the
 * Rockchip client block used by pcie-dw-rockchip.c. This is not the
 * Mixtile MIOP driver.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#include "openmiop.h"

#define PCIE_CLIENT_GENERAL_CON		0x000
#define PCIE_CLIENT_MODE_MASK		0xf0u
#define PCIE_CLIENT_LTSSM_BIT		BIT(2)
#define PCIE_CLIENT_HOT_RESET_CTRL	0x180
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
#define RK3588_PCIE3PHY_GRF_CMN_CON0	0x0
#define PHY_MODE_NANBNB			0	/* two PCIe3 x2 */
#define PHY_MODE_AGGREGATION		4	/* one PCIe3 x4 */

#define PCIE_ATU_REGION_DIR_IB		BIT(31)
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
#define DEFAULT_DBI_ATU_OFFSET		0x300000
#define DEFAULT_DBI2_OFFSET		0x100000

/* Upstream rk3588 pcie3x4 "addr_space". CPU stores here become MemWr
 * TLPs once an outbound iATU region maps the window.
 */
static unsigned long long ob_base = 0x900000000ULL;
module_param(ob_base, ullong, 0444);
MODULE_PARM_DESC(ob_base, "CPU physical base of the outbound iATU window");

static bool use_edma = true;
module_param(use_edma, bool, 0444);
MODULE_PARM_DESC(use_edma, "Send blade-to-blade frames with the PCIe eDMA engine");

static uint lanes = 2;
module_param(lanes, uint, 0444);
MODULE_PARM_DESC(lanes, "Link width. 2 matches this board (the other two PHY lanes are the M.2 NVMe). 4 aggregates the PHY onto this controller and drops the NVMe");

/* RK3588 pcie3x4 embedded DMA at dbi + 0x380000. This is the Synopsys
 * DWC eDMA map Rockchip programs (write engine at +0x00c, channel 0
 * context at +0x200), not the HDMA layout.
 *
 * A write channel reads local DRAM (SAR) and emits MemWr TLPs to the
 * PCIe address in DAR. LIE makes the done bit latch; the mask register
 * keeps the IRQ pin quiet so we can poll.
 */
#define EDMA_OFFSET		0x380000
#define EDMA_CTRL		0x008
#define EDMA_WR_ENB		0x00c
#define EDMA_WR_DOORBELL	0x010
#define EDMA_WR_INT_STATUS	0x04c
#define EDMA_WR_INT_MASK	0x054
#define EDMA_WR_INT_CLEAR	0x058
#define EDMA_WR_CTRL_LO		0x200
#define EDMA_WR_CTRL_HI		0x204
#define EDMA_WR_XFERSIZE	0x208
#define EDMA_WR_SAR_LO		0x20c
#define EDMA_WR_SAR_HI		0x210
#define EDMA_WR_DAR_LO		0x214
#define EDMA_WR_DAR_HI		0x218
#define EDMA_WR_LLP_LO		0x21c
#define EDMA_WR_LLP_HI		0x220
#define EDMA_WR_LL_ERR		0x090
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

/* CPU writes len/head, then the eDMA reads them. The list sits on
 * its own cache line so flushing it does not touch the BAR header.
 */
#define TX_BATCH 16
#define TXQ_SIZE 128

struct openmiop_scratch {
	u32 len[TX_BATCH];
	u32 head;
	u8 pad[64 - (((TX_BATCH + 1) * 4) % 64)];
	struct edma_lli ll[TX_BATCH * 2 + 2];
	struct edma_lli ll1[TX_BATCH + 2] __aligned(64);
};
/* Past the P2P ring (ends at 9 MiB). Peer writes never target this. */
#define EDMA_SCRATCH_OFF	0x00f00000u

struct openmiop_ep {
	struct device *dev;
	struct net_device *ndev;
	void __iomem *apb;
	void __iomem *dbi;
	void __iomem *dbi2;
	void __iomem *atu;
	struct clk_bulk_data *clks;
	int nclks;
	struct reset_control *rst;
	struct phy *phy;
	struct regulator *vpcie3v3;
	bool rmem;
	bool cacheable;
	void *raw;
	dma_addr_t raw_dma;
	size_t raw_size;
	void *cpu;
	dma_addr_t dma;
	struct openmiop_bar *bar;
	struct task_struct *poll;
	struct task_struct *tx_task;
	spinlock_t tx_lock;
	wait_queue_head_t tx_wait;
	struct sk_buff *txq[TXQ_SIZE];
	u32 txq_prod;
	u32 txq_cons;
	u32 tx_inflight;
	bool link;
	bool unroll;
	bool p2p_on;
	void __iomem *ob;
	void __iomem *edma;
	struct openmiop_scratch *scratch;
	dma_addr_t scratch_dma;
	u64 peer_pci;
	bool edma_dead;
	bool edma_used;
	bool ll_dead;
	bool ll_ready;
	bool ll_cb;
	bool ch1_ready;
	bool ch1_cb;
	bool ch1_dead;
	bool tx_busy;
	struct sk_buff *tx_skb;
	dma_addr_t tx_src;
	u32 tx_len;
	u32 tx_pre;
	struct sk_buff *batch_skb[TX_BATCH];
	dma_addr_t batch_src[TX_BATCH];
	int batch_n;
	int batch_max;
	struct napi_struct napi;
	u32 peer_gen;
	u32 p2p_fail_gen;
	unsigned long p2p_next;
	u32 p2p_head;
	u32 p2p_tail_cache;
};

static void rk_hiword(void __iomem *base, u32 reg, u32 mask, u32 val)
{
	writel((mask << 16) | (val & mask), base + reg);
}

static bool link_is_up(struct openmiop_ep *ep)
{
	return (readl(ep->apb + PCIE_CLIENT_LTSSM_STATUS) & PCIE_LINKUP_MASK) ==
	       PCIE_LINKUP_MASK;
}

static void __iomem *atu_ib(struct openmiop_ep *ep, u32 index)
{
	/* Unroll map: region << 9, inbound adds bit 8. Same as
	 * PCIE_ATU_UNROLL_BASE() in pcie-designware.h.
	 */
	return ep->atu + ((index << 9) | BIT(8));
}

static int program_inbound_unroll(struct openmiop_ep *ep)
{
	void __iomem *base = atu_ib(ep, 0);
	int i;

	writel(lower_32_bits(ep->dma), base + PCIE_ATU_LOWER_TARGET);
	writel(upper_32_bits(ep->dma), base + PCIE_ATU_UPPER_TARGET);
	writel(PCIE_ATU_TYPE_MEM, base + PCIE_ATU_REGION_CTRL1);
	writel(PCIE_ATU_ENABLE | PCIE_ATU_FUNC_NUM_MATCH_EN |
	       PCIE_ATU_BAR_MODE_ENABLE | (0u << 8),
	       base + PCIE_ATU_REGION_CTRL2);

	for (i = 0; i < 5; i++) {
		if (readl(base + PCIE_ATU_REGION_CTRL2) & PCIE_ATU_ENABLE)
			return 0;
		mdelay(1);
	}
	return -ETIMEDOUT;
}

static int program_inbound_viewport(struct openmiop_ep *ep)
{
	int i;

	writel(PCIE_ATU_REGION_DIR_IB, ep->dbi + 0x900);
	writel(lower_32_bits(ep->dma), ep->dbi + 0x918);
	writel(upper_32_bits(ep->dma), ep->dbi + 0x91c);
	writel(PCIE_ATU_TYPE_MEM, ep->dbi + 0x904);
	writel(PCIE_ATU_ENABLE | PCIE_ATU_FUNC_NUM_MATCH_EN |
	       PCIE_ATU_BAR_MODE_ENABLE | (0u << 8),
	       ep->dbi + 0x908);

	for (i = 0; i < 5; i++) {
		if (readl(ep->dbi + 0x908) & PCIE_ATU_ENABLE)
			return 0;
		mdelay(1);
	}
	return -ETIMEDOUT;
}

static int program_inbound_bar0(struct openmiop_ep *ep)
{
	u32 viewport = readl(ep->dbi + 0x900);
	int ret;

	/* 0xffffffff means the controller has unrolled iATU, same test
	 * as dw_pcie_iatu_unroll_enabled().
	 */
	if (viewport == 0xffffffff) {
		ep->unroll = true;
		dev_info(ep->dev, "iATU unroll, target %#llx\n",
			 (unsigned long long)ep->dma);
		ret = program_inbound_unroll(ep);
	} else {
		dev_info(ep->dev, "iATU viewport %#x, target %#llx\n",
			 viewport, (unsigned long long)ep->dma);
		ret = program_inbound_viewport(ep);
	}
	if (ret)
		dev_err(ep->dev, "inbound iATU did not enable\n");
	return ret;
}

/* 16-byte streaming stores. Plain memcpy may use DC ZVA, which faults
 * on the Normal-NC outbound window.
 */
static void wc_copy(void __iomem *dst, const void *src, unsigned int len)
{
	unsigned char *d = (unsigned char __force *)dst;
	const unsigned char *s = src;
	unsigned int i = 0;

	for (; i + 16 <= len; i += 16) {
		unsigned long long a, b;

		memcpy(&a, s + i, 8);
		memcpy(&b, s + i + 8, 8);
		asm volatile("stnp %0, %1, [%2]"
			     :
			     : "r"(a), "r"(b), "r"(d + i)
			     : "memory");
	}
	for (; i + 8 <= len; i += 8) {
		unsigned long long a;

		memcpy(&a, s + i, 8);
		asm volatile("str %0, [%1]"
			     :
			     : "r"(a), "r"(d + i)
			     : "memory");
	}
	for (; i < len; i++)
		asm volatile("strb %w0, [%1]"
			     :
			     : "r"(s[i]), "r"(d + i)
			     : "memory");
	wmb();
}

/* The xmit line and the poll line are written by different contexts.
 * Flushing one must not write the other back over a newer store.
 * The device line is read-only here: invalidating it drops a stale
 * cached copy so a PCIe store from the router or the peer is visible.
 */
static void bar_flush_xmit(struct openmiop_ep *ep)
{
	dma_sync_single_for_device(ep->dev, ep->dma,
				   offsetof(struct openmiop_bar, rc_tx_tail),
				   DMA_TO_DEVICE);
}

static void bar_flush_poll(struct openmiop_ep *ep)
{
	unsigned int off = offsetof(struct openmiop_bar, rc_tx_tail);

	dma_sync_single_for_device(ep->dev, ep->dma + off,
				   offsetof(struct openmiop_bar, rc_flags) - off,
				   DMA_TO_DEVICE);
}

static void bar_inval_dev(struct openmiop_ep *ep)
{
	unsigned int off = offsetof(struct openmiop_bar, rc_flags);

	dma_sync_single_for_cpu(ep->dev, ep->dma + off,
				offsetof(struct openmiop_bar, ep_tx) - off,
				DMA_FROM_DEVICE);
}

static void bar_flush_off(struct openmiop_ep *ep, unsigned int off, unsigned int len)
{
	dma_sync_single_for_device(ep->dev, ep->dma + off, len, DMA_TO_DEVICE);
}

static void bar_inval_off(struct openmiop_ep *ep, unsigned int off, unsigned int len)
{
	dma_sync_single_for_cpu(ep->dev, ep->dma + off, len, DMA_FROM_DEVICE);
}

static void __iomem *atu_ob(struct openmiop_ep *ep, u32 index)
{
	return ep->atu + (index << 9);
}

static int program_outbound(struct openmiop_ep *ep, u64 pci_addr)
{
	void __iomem *base = atu_ob(ep, 0);
	u64 cpu = ob_base;
	u64 end = cpu + OPENMIOP_BAR_SIZE - 1;
	u32 magic;
	int i;

	if (!ep->unroll) {
		dev_err(ep->dev, "P2P needs unrolled iATU\n");
		return -EOPNOTSUPP;
	}
	if (!ep->ob) {
		ep->ob = ioremap_wc(cpu, OPENMIOP_BAR_SIZE);
		if (!ep->ob) {
			dev_err(ep->dev, "ioremap outbound %#llx failed\n", cpu);
			return -ENOMEM;
		}
	}

	writel(lower_32_bits(cpu), base + PCIE_ATU_LOWER_BASE);
	writel(upper_32_bits(cpu), base + PCIE_ATU_UPPER_BASE);
	writel(lower_32_bits(end), base + PCIE_ATU_LIMIT);
	writel(upper_32_bits(end), base + PCIE_ATU_UPPER_LIMIT);
	writel(lower_32_bits(pci_addr), base + PCIE_ATU_LOWER_TARGET);
	writel(upper_32_bits(pci_addr), base + PCIE_ATU_UPPER_TARGET);
	writel(PCIE_ATU_TYPE_MEM | PCIE_ATU_INCREASE_REGION_SIZE,
	       base + PCIE_ATU_REGION_CTRL1);
	writel(PCIE_ATU_ENABLE, base + PCIE_ATU_REGION_CTRL2);

	for (i = 0; i < 5; i++) {
		if (readl(base + PCIE_ATU_REGION_CTRL2) & PCIE_ATU_ENABLE)
			break;
		mdelay(1);
	}
	if (!(readl(base + PCIE_ATU_REGION_CTRL2) & PCIE_ATU_ENABLE)) {
		dev_err(ep->dev, "outbound iATU did not enable\n");
		return -ETIMEDOUT;
	}

	dev_info(ep->dev, "outbound window %#llx -> peer BAR %#llx\n",
		 cpu, pci_addr);
	magic = readl(ep->ob);
	dev_info(ep->dev, "peer BAR magic %#x\n", magic);
	if (magic != OPENMIOP_MAGIC) {
		dev_err(ep->dev, "peer BAR is not reachable through the switch\n");
		writel(0, base + PCIE_ATU_REGION_CTRL2);
		return -EIO;
	}
	return 0;
}

static bool p2p_full(struct openmiop_ep *ep)
{
	u32 fill = ep->p2p_head + ep->tx_inflight - ep->p2p_tail_cache;

	if (fill < OPENMIOP_P2P_SLOTS - 1)
		return false;
	ep->p2p_tail_cache = readl(ep->ob + offsetof(struct openmiop_bar, p2p_rx_tail));
	fill = ep->p2p_head + ep->tx_inflight - ep->p2p_tail_cache;
	return fill >= OPENMIOP_P2P_SLOTS - 1;
}

static void edma_init(struct openmiop_ep *ep)
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
	ep->ll_cb = true;
	ep->ch1_cb = true;
	/* Channel 1 moves bytes, but it shares the write pipe with channel 0
	 * and does not raise throughput. Leave it off so the head pointer
	 * stays on the same channel as the payload.
	 */
	ep->ch1_dead = true;
	dev_info(ep->dev, "eDMA ctrl %#x, write IRQ masked\n", ctrl);
}

static u32 edma_ch_reg(int ch, u32 ch0_reg)
{
	return ch0_reg + ch * 0x200;
}

static int edma_wait_ch(struct openmiop_ep *ep, int ch, u32 pre)
{
	void __iomem *dma = ep->edma;
	u32 done = BIT(ch);
	u32 abort = BIT(16 + ch);
	int i;

	/* A tight readl of the DMA block starves the engine of the same
	 * bus it uses to fetch descriptors. Poll, then stay off it.
	 */
	for (i = 0; i < 50; i++) {
		u32 neu = readl(dma + EDMA_WR_INT_STATUS) & ~pre;

		if (neu & abort) {
			writel(done | abort, dma + EDMA_WR_INT_CLEAR);
			return -EIO;
		}
		if (neu & done) {
			writel(done, dma + EDMA_WR_INT_CLEAR);
			return 0;
		}
		cpu_relax();
	}
	for (i = 0; i < 20000; i++) {
		u32 neu = readl(dma + EDMA_WR_INT_STATUS) & ~pre;

		if (neu & abort) {
			writel(done | abort, dma + EDMA_WR_INT_CLEAR);
			return -EIO;
		}
		if (neu & done) {
			writel(done, dma + EDMA_WR_INT_CLEAR);
			return 0;
		}
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int edma_wait(struct openmiop_ep *ep, u32 pre)
{
	return edma_wait_ch(ep, 0, pre);
}

/* One non-linked-list eDMA write. Used when the linked list aborts,
 * so a bad list program does not drop the link back to CPU copies.
 */
static int edma_xfer(struct openmiop_ep *ep, dma_addr_t sar, u64 dar, u32 len)
{
	void __iomem *dma = ep->edma;
	u32 pre;

	if (!len)
		return -EINVAL;

	writel(EDMA_INT_DONE | EDMA_INT_ABORT, dma + EDMA_WR_INT_CLEAR);
	pre = readl(dma + EDMA_WR_INT_STATUS);

	writel(BIT(0), dma + EDMA_WR_ENB);
	writel(EDMA_CTRL_LIE | EDMA_CTRL_TD, dma + EDMA_WR_CTRL_LO);
	writel(0, dma + EDMA_WR_CTRL_HI);
	writel(len, dma + EDMA_WR_XFERSIZE);
	writel(lower_32_bits(sar), dma + EDMA_WR_SAR_LO);
	writel(upper_32_bits(sar), dma + EDMA_WR_SAR_HI);
	writel(lower_32_bits(dar), dma + EDMA_WR_DAR_LO);
	writel(upper_32_bits(dar), dma + EDMA_WR_DAR_HI);
	wmb();
	/* Channel 0, stop bit clear: start. */
	writel(0, dma + EDMA_WR_DOORBELL);
	return edma_wait(ep, pre);
}

static void ll_data(struct edma_lli *e, bool cb, bool lie, u32 len,
		    dma_addr_t sar, u64 dar)
{
	u32 control = EDMA_CTRL_TD;

	if (cb)
		control |= EDMA_CTRL_CB;
	if (lie)
		control |= EDMA_CTRL_LIE;
	e->control = control;
	e->transfer_size = len;
	e->sar_lo = lower_32_bits(sar);
	e->sar_hi = upper_32_bits(sar);
	e->dar_lo = lower_32_bits(dar);
	e->dar_hi = upper_32_bits(dar);
}

/* Link element. sar holds the next pointer. CB is the opposite of the
 * data elements, which is how the engine recycles the same list.
 */
static void ll_link(struct edma_lli *e, bool cb, dma_addr_t next)
{
	u32 control = EDMA_CTRL_LLP | EDMA_CTRL_TCB;

	if (cb)
		control |= EDMA_CTRL_CB;
	e->control = control;
	e->transfer_size = 0;
	e->sar_lo = lower_32_bits(next);
	e->sar_hi = upper_32_bits(next);
	e->dar_lo = 0;
	e->dar_hi = 0;
}

static int p2p_tx_single(struct openmiop_ep *ep, dma_addr_t src, u32 len, u32 off)
{
	u64 slot = ep->peer_pci + off;
	dma_addr_t head_dma = ep->scratch_dma + offsetof(struct openmiop_scratch, head);
	int ret;

	ret = edma_xfer(ep, src, slot + OPENMIOP_P2P_HDR, len);
	if (ret)
		return ret;

	ep->scratch->len[0] = len;
	dma_sync_single_for_device(ep->dev, ep->scratch_dma, 4, DMA_TO_DEVICE);
	ret = edma_xfer(ep, ep->scratch_dma, slot, 4);
	if (ret)
		return ret;

	ep->p2p_head++;
	ep->scratch->head = ep->p2p_head;
	dma_sync_single_for_device(ep->dev, head_dma, 4, DMA_TO_DEVICE);
	return edma_xfer(ep, head_dma,
			 ep->peer_pci + offsetof(struct openmiop_bar, p2p_rx_head), 4);
}

/* One doorbell for every queued frame. Lengths land before the head
 * pointer, so the peer never sees a slot whose payload is still in flight.
 * The caller has already taken the skbs; this always consumes the batch.
 */
static int tx_flush(struct openmiop_ep *ep)
{
	struct openmiop_scratch *sc = ep->scratch;
	struct edma_lli *ll = sc->ll;
	dma_addr_t ll_dma = ep->scratch_dma + offsetof(struct openmiop_scratch, ll);
	void __iomem *dma = ep->edma;
	int n = ep->batch_n;
	int i, ei, ret;
	u32 pre, total = 0;
	bool cb;

	if (!n)
		return 0;

	if (ep->ll_dead || !dma) {
		ret = 0;
		for (i = 0; i < n; i++) {
			struct sk_buff *skb = ep->batch_skb[i];
			u32 slot_i = ep->p2p_head % OPENMIOP_P2P_SLOTS;
			u32 off = OPENMIOP_P2P_OFF + slot_i * OPENMIOP_P2P_SLOT;

			if (!ret)
				ret = p2p_tx_single(ep, ep->batch_src[i], skb->len, off);
			dma_unmap_single(ep->dev, ep->batch_src[i], skb->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(skb);
		}
		ep->batch_n = 0;
		return ret;
	}

	cb = ep->ll_cb;
	sc->head = ep->p2p_head + n;
	ei = 0;
	/* Lengths are posted before the engine starts. The head pointer
	 * stays on this channel, after the payloads, so the peer cannot
	 * observe a slot until its bytes have been written.
	 */
	for (i = 0; i < n; i++) {
		u32 slot_i = (ep->p2p_head + i) % OPENMIOP_P2P_SLOTS;
		u32 off = OPENMIOP_P2P_OFF + slot_i * OPENMIOP_P2P_SLOT;

		total += ep->batch_skb[i]->len;
		writel(ep->batch_skb[i]->len, ep->ob + off);
	}
	wmb();
	readl(ep->ob + offsetof(struct openmiop_bar, p2p_rx_head));
	for (i = 0; i < n; i++) {
		u32 slot_i = (ep->p2p_head + i) % OPENMIOP_P2P_SLOTS;
		u32 off = OPENMIOP_P2P_OFF + slot_i * OPENMIOP_P2P_SLOT;
		u64 slot = ep->peer_pci + off;

		ll_data(&ll[ei++], cb, false, ep->batch_skb[i]->len,
			ep->batch_src[i], slot + OPENMIOP_P2P_HDR);
	}
	ll_data(&ll[ei++], cb, true, 4,
		ep->scratch_dma + offsetof(struct openmiop_scratch, head),
		ep->peer_pci + offsetof(struct openmiop_bar, p2p_rx_head));
	ll_link(&ll[ei], !cb, ll_dma);
	wmb();
	dma_sync_single_for_device(ep->dev, ep->scratch_dma, sizeof(*sc),
				   DMA_TO_DEVICE);

	writel(EDMA_INT_DONE | EDMA_INT_ABORT, dma + EDMA_WR_INT_CLEAR);
	pre = readl(dma + EDMA_WR_INT_STATUS);
	writel(BIT(0), dma + EDMA_WR_ENB);
	if (!ep->ll_ready) {
		writel(EDMA_CTRL_CCS | EDMA_CTRL_LLE, dma + EDMA_WR_CTRL_LO);
		writel(0, dma + EDMA_WR_CTRL_HI);
		writel(lower_32_bits(ll_dma), dma + EDMA_WR_LLP_LO);
		writel(upper_32_bits(ll_dma), dma + EDMA_WR_LLP_HI);
	}
	wmb();
	writel(0, dma + EDMA_WR_DOORBELL);
	ret = edma_wait(ep, pre);

	for (i = 0; i < n; i++) {
		dma_unmap_single(ep->dev, ep->batch_src[i], ep->batch_skb[i]->len,
				 DMA_TO_DEVICE);
		if (!ret)
			dev_consume_skb_any(ep->batch_skb[i]);
		else
			dev_kfree_skb_any(ep->batch_skb[i]);
	}
	ep->batch_n = 0;

	if (ret) {
		ep->ll_dead = true;
		ep->ll_ready = false;
		dev_err(ep->dev, "eDMA list failed (%d), int %#x ctrl %#x\n",
			ret, readl(dma + EDMA_WR_INT_STATUS),
			readl(dma + EDMA_WR_CTRL_LO));
		return ret;
	}

	ep->p2p_head += n;
	ep->ll_cb = !ep->ll_cb;
	ep->ll_ready = true;
	if (!ep->edma_used) {
		ep->edma_used = true;
		dev_info(ep->dev, "eDMA list tx %u bytes\n", total);
	}
	if (n > ep->batch_max) {
		ep->batch_max = n;
		dev_info(ep->dev, "eDMA batch %d frames, %u bytes\n", n, total);
	}
	return 0;
}

/* xmit only enqueues. The DMA wait runs on tx_task, so TCP can build
 * the next burst while the engine moves this one.
 */
static int p2p_tx(struct openmiop_ep *ep, struct sk_buff *skb, bool *held)
{
	u32 slot_i;
	u32 off;
	void __iomem *slot;

	*held = false;
	if (skb->len > OPENMIOP_P2P_DATA)
		return -EMSGSIZE;

	if (ep->edma && ep->scratch && !ep->edma_dead && ep->tx_task) {
		if (skb_linearize(skb))
			return -EAGAIN;
		spin_lock_bh(&ep->tx_lock);
		if (ep->txq_prod - ep->txq_cons >= TXQ_SIZE || p2p_full(ep)) {
			spin_unlock_bh(&ep->tx_lock);
			return -EBUSY;
		}
		ep->txq[ep->txq_prod & (TXQ_SIZE - 1)] = skb;
		ep->txq_prod++;
		ep->tx_inflight++;
		spin_unlock_bh(&ep->tx_lock);
		wake_up(&ep->tx_wait);
		*held = true;
		return 0;
	}

	if (p2p_full(ep))
		return -EBUSY;
	slot_i = ep->p2p_head % OPENMIOP_P2P_SLOTS;
	off = OPENMIOP_P2P_OFF + slot_i * OPENMIOP_P2P_SLOT;
	slot = ep->ob + off;
	wc_copy(slot + OPENMIOP_P2P_HDR, skb->data, skb->len);
	writel(skb->len, slot);
	wmb();
	ep->p2p_head++;
	writel(ep->p2p_head, ep->ob + offsetof(struct openmiop_bar, p2p_rx_head));
	readl(ep->ob + offsetof(struct openmiop_bar, p2p_rx_head));
	return 0;
}

static bool tail_word_matches(struct openmiop_ep *ep, struct sk_buff *skb,
			      u32 slot_base, int index)
{
	u32 len = skb->len;
	u32 slot_i = (slot_base + index) % OPENMIOP_P2P_SLOTS;
	u32 off = OPENMIOP_P2P_OFF + slot_i * OPENMIOP_P2P_SLOT;
	u32 pos, expect0, expect1, i;

	if (len < 8)
		return true;
	pos = (len - 8) & ~3u;
	memcpy(&expect0, skb->data + pos, 4);
	memcpy(&expect1, skb->data + pos + 4, 4);
	for (i = 0; i < 200; i++) {
		if (readl(ep->ob + off + OPENMIOP_P2P_HDR + pos) == expect0 &&
		    readl(ep->ob + off + OPENMIOP_P2P_HDR + pos + 4) == expect1)
			return true;
		udelay(1);
	}
	return false;
}

static int kick_payloads(struct openmiop_ep *ep, int ch, struct sk_buff **skb,
			 dma_addr_t *src, int n, u32 slot_base, u32 *pre_out)
{
	struct edma_lli *ll = ch ? ep->scratch->ll1 : ep->scratch->ll;
	dma_addr_t ll_dma = ep->scratch_dma + (ch ? offsetof(struct openmiop_scratch, ll1)
						  : offsetof(struct openmiop_scratch, ll));
	bool *cbp = ch ? &ep->ch1_cb : &ep->ll_cb;
	bool *ready = ch ? &ep->ch1_ready : &ep->ll_ready;
	bool cb = *cbp;
	void __iomem *dma = ep->edma;
	int i, ei = 0;
	u32 pre;

	for (i = 0; i < n; i++) {
		u32 slot_i = (slot_base + i) % OPENMIOP_P2P_SLOTS;
		u32 off = OPENMIOP_P2P_OFF + slot_i * OPENMIOP_P2P_SLOT;

		ll_data(&ll[ei++], cb, i == n - 1, skb[i]->len, src[i],
			ep->peer_pci + off + OPENMIOP_P2P_HDR);
	}
	ll_link(&ll[ei], !cb, ll_dma);
	wmb();
	dma_sync_single_for_device(ep->dev, ll_dma, (ei + 1) * sizeof(*ll),
				   DMA_TO_DEVICE);

	writel(BIT(ch) | BIT(16 + ch), dma + EDMA_WR_INT_CLEAR);
	pre = readl(dma + EDMA_WR_INT_STATUS);
	writel(BIT(0), dma + EDMA_WR_ENB);
	if (!*ready) {
		writel(EDMA_CTRL_CCS | EDMA_CTRL_LLE,
		       dma + edma_ch_reg(ch, EDMA_WR_CTRL_LO));
		writel(0, dma + edma_ch_reg(ch, EDMA_WR_CTRL_HI));
		writel(lower_32_bits(ll_dma), dma + edma_ch_reg(ch, EDMA_WR_LLP_LO));
		writel(upper_32_bits(ll_dma), dma + edma_ch_reg(ch, EDMA_WR_LLP_HI));
	}
	wmb();
	writel(ch, dma + EDMA_WR_DOORBELL);
	*pre_out = pre;
	return 0;
}

static void post_lens(struct openmiop_ep *ep, struct sk_buff **skb, int n, u32 slot_base)
{
	int i;

	for (i = 0; i < n; i++) {
		u32 slot_i = (slot_base + i) % OPENMIOP_P2P_SLOTS;
		u32 off = OPENMIOP_P2P_OFF + slot_i * OPENMIOP_P2P_SLOT;

		writel(skb[i]->len, ep->ob + off);
	}
}

/* The two write channels share one link but not one descriptor walker.
 * Payloads go out together; the head pointer is published only after a
 * read of each batch's last bytes has come back, so it cannot pass them.
 */
static int tx_two(struct openmiop_ep *ep, int n0, struct sk_buff **skb1,
		   dma_addr_t *src1, int n1)
{
	u32 base = ep->p2p_head;
	u32 pre0, pre1;
	int ret0, ret1, i;
	bool vis0, vis1;

	post_lens(ep, ep->batch_skb, n0, base);
	post_lens(ep, skb1, n1, base + n0);
	wmb();
	readl(ep->ob + offsetof(struct openmiop_bar, p2p_rx_head));

	kick_payloads(ep, 1, skb1, src1, n1, base + n0, &pre1);
	kick_payloads(ep, 0, ep->batch_skb, ep->batch_src, n0, base, &pre0);
	ret1 = edma_wait_ch(ep, 1, pre1);
	ret0 = edma_wait_ch(ep, 0, pre0);
	if (!ret0) {
		ep->ll_cb = !ep->ll_cb;
		ep->ll_ready = true;
	} else {
		ep->ll_ready = false;
	}
	if (!ret1) {
		ep->ch1_cb = !ep->ch1_cb;
		ep->ch1_ready = true;
	} else {
		ep->ch1_ready = false;
		ep->ch1_dead = true;
		dev_err(ep->dev, "eDMA ch1 failed (%d)\n", ret1);
	}
	vis0 = !ret0 && tail_word_matches(ep, ep->batch_skb[n0 - 1], base, n0 - 1);
	vis1 = !ret1 && tail_word_matches(ep, skb1[n1 - 1], base + n0, n1 - 1);
	if (ret0 || ret1 || !vis0 || !vis1) {
		for (i = 0; i < n0; i++) {
			dma_unmap_single(ep->dev, ep->batch_src[i], ep->batch_skb[i]->len,
					 DMA_TO_DEVICE);
			dev_kfree_skb_any(ep->batch_skb[i]);
		}
		for (i = 0; i < n1; i++) {
			dma_unmap_single(ep->dev, src1[i], skb1[i]->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(skb1[i]);
		}
		ep->batch_n = 0;
		if (!vis0 || !vis1)
			dev_err_ratelimited(ep->dev, "eDMA payload not visible\n");
		return -EIO;
	}

	ep->p2p_head = base + n0 + n1;
	writel(ep->p2p_head, ep->ob + offsetof(struct openmiop_bar, p2p_rx_head));
	readl(ep->ob + offsetof(struct openmiop_bar, p2p_rx_head));
	for (i = 0; i < n0; i++) {
		dma_unmap_single(ep->dev, ep->batch_src[i], ep->batch_skb[i]->len,
				 DMA_TO_DEVICE);
		dev_consume_skb_any(ep->batch_skb[i]);
	}
	for (i = 0; i < n1; i++) {
		dma_unmap_single(ep->dev, src1[i], skb1[i]->len, DMA_TO_DEVICE);
		dev_consume_skb_any(skb1[i]);
	}
	ep->batch_n = 0;
	if (n0 + n1 > ep->batch_max) {
		ep->batch_max = n0 + n1;
		dev_info(ep->dev, "eDMA batch %d frames on 2 channels\n", n0 + n1);
	}
	return 0;
}

static int tx_thread(void *data)
{
	struct openmiop_ep *ep = data;

	set_user_nice(current, -20);
	while (!kthread_should_stop() ||
	       READ_ONCE(ep->txq_prod) != READ_ONCE(ep->txq_cons)) {
		struct sk_buff *skbs[TX_BATCH];
		struct sk_buff *skb1[TX_BATCH];
		dma_addr_t src1[TX_BATCH];
		int n = 0, n1 = 0, mapped = 0, mapped1 = 0;
		int i;

		spin_lock_bh(&ep->tx_lock);
		while (n < TX_BATCH && ep->txq_cons != ep->txq_prod) {
			skbs[n++] = ep->txq[ep->txq_cons & (TXQ_SIZE - 1)];
			ep->txq_cons++;
		}
		if (n == TX_BATCH && !ep->ch1_dead && !ep->ll_dead) {
			while (n1 < TX_BATCH && ep->txq_cons != ep->txq_prod) {
				skb1[n1++] = ep->txq[ep->txq_cons & (TXQ_SIZE - 1)];
				ep->txq_cons++;
			}
		}
		spin_unlock_bh(&ep->tx_lock);

		if (!n) {
			wait_event_interruptible_timeout(ep->tx_wait,
				kthread_should_stop() ||
				READ_ONCE(ep->txq_prod) != READ_ONCE(ep->txq_cons),
				HZ);
			continue;
		}

		for (i = 0; i < n; i++) {
			dma_addr_t src = dma_map_single(ep->dev, skbs[i]->data, skbs[i]->len,
							DMA_TO_DEVICE);

			if (dma_mapping_error(ep->dev, src)) {
				dev_kfree_skb_any(skbs[i]);
				continue;
			}
			ep->batch_skb[mapped] = skbs[i];
			ep->batch_src[mapped] = src;
			mapped++;
		}
		for (i = 0; i < n1; i++) {
			dma_addr_t src = dma_map_single(ep->dev, skb1[i]->data, skb1[i]->len,
							DMA_TO_DEVICE);

			if (dma_mapping_error(ep->dev, src)) {
				dev_kfree_skb_any(skb1[i]);
				continue;
			}
			skb1[mapped1] = skb1[i];
			src1[mapped1] = src;
			mapped1++;
		}
		ep->batch_n = mapped;
		if (mapped && mapped1)
			tx_two(ep, mapped, skb1, src1, mapped1);
		else if (mapped)
			tx_flush(ep);
		if (!mapped && mapped1) {
			for (i = 0; i < mapped1; i++) {
				dma_unmap_single(ep->dev, src1[i], skb1[i]->len, DMA_TO_DEVICE);
				dev_kfree_skb_any(skb1[i]);
			}
		}

		spin_lock_bh(&ep->tx_lock);
		ep->tx_inflight -= n + n1;
		if (netif_queue_stopped(ep->ndev) && !p2p_full(ep) &&
		    ep->txq_prod - ep->txq_cons < TXQ_SIZE)
			netif_wake_queue(ep->ndev);
		spin_unlock_bh(&ep->tx_lock);
	}
	return 0;
}

/* NAPI context only. One NAPI instance drains both rings, so frames
 * reach the stack in ring order whichever CPU the poll runs on.
 */
static void rx_deliver(struct openmiop_ep *ep, struct sk_buff *skb)
{
	napi_gro_receive(&ep->napi, skb);
}

static int p2p_rx(struct openmiop_ep *ep, int budget)
{
	struct openmiop_bar *bar = ep->bar;
	struct net_device *ndev = ep->ndev;
	u32 head, tail;
	int done = 0;

	bar_inval_dev(ep);
	head = READ_ONCE(bar->p2p_rx_head);
	tail = READ_ONCE(bar->p2p_rx_tail);
	while (tail != head && done < budget) {
		u32 off = OPENMIOP_P2P_OFF + (tail % OPENMIOP_P2P_SLOTS) * OPENMIOP_P2P_SLOT;
		u8 *slot = ep->cpu + off;
		u32 len;
		struct sk_buff *skb;

		bar_inval_off(ep, off, 64);
		len = READ_ONCE(*(u32 *)slot);
		if (len < ETH_HLEN || len > OPENMIOP_P2P_DATA) {
			ndev->stats.rx_errors++;
			tail++;
			continue;
		}
		bar_inval_off(ep, off + OPENMIOP_P2P_HDR, len);
		skb = netdev_alloc_skb_ip_align(ndev, len);
		if (!skb) {
			ndev->stats.rx_dropped++;
			break;
		}
		skb_put_data(skb, slot + OPENMIOP_P2P_HDR, len);
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		rx_deliver(ep, skb);
		tail++;
		done++;
		if ((done & 7) == 0) {
			bar_inval_dev(ep);
			head = READ_ONCE(bar->p2p_rx_head);
		}
	}
	if (tail != READ_ONCE(bar->p2p_rx_tail)) {
		WRITE_ONCE(bar->p2p_rx_tail, tail);
		bar_flush_poll(ep);
	}
	return done;
}

static void p2p_watch(struct openmiop_ep *ep)
{
	struct openmiop_bar *bar = ep->bar;
	u32 gen;
	u64 pci;
	int ret;

	bar_inval_dev(ep);
	gen = READ_ONCE(bar->peer_gen);
	if (!gen || gen == ep->peer_gen)
		return;
	if (ep->p2p_fail_gen == gen && time_before(jiffies, ep->p2p_next))
		return;
	pci = ((u64)READ_ONCE(bar->peer_bar_hi) << 32) | READ_ONCE(bar->peer_bar_lo);
	if (!pci)
		return;
	ret = program_outbound(ep, pci);
	if (ret) {
		ep->p2p_fail_gen = gen;
		ep->p2p_next = jiffies + HZ;
		return;
	}
	ep->peer_pci = pci;
	ep->p2p_head = readl(ep->ob + offsetof(struct openmiop_bar, p2p_rx_head));
	ep->p2p_tail_cache = readl(ep->ob + offsetof(struct openmiop_bar, p2p_rx_tail));
	smp_wmb();
	ep->peer_gen = gen;
	ep->p2p_on = true;
	dev_info(ep->dev, "P2P on, peer %pM gen %u\n", bar->peer_mac, gen);
}

static void dbi_ro_wr(struct openmiop_ep *ep, bool en)
{
	u32 v = readl(ep->dbi + PCIE_MISC_CONTROL_1_OFF);

	if (en)
		v |= PCIE_DBI_RO_WR_EN;
	else
		v &= ~PCIE_DBI_RO_WR_EN;
	writel(v, ep->dbi + PCIE_MISC_CONTROL_1_OFF);
}

static void hide_ext_cap(struct openmiop_ep *ep, u16 cap_id)
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
			dev_info(ep->dev, "hid ext cap %#x at %#x\n", cap_id, pos);
			return;
		}
		if (!next || next == pos)
			return;
		prev = pos;
		pos = next;
	}
}

static void restrict_rebar(struct openmiop_ep *ep)
{
	u32 pos = 0x100;
	int ttl = 48;

	while (ttl-- > 0 && pos >= 0x100) {
		u32 hdr = readl(ep->dbi + pos);
		u16 id = hdr & 0xffff;
		u32 next = (hdr >> 20) & 0xffc;
		unsigned int nbars, i;

		if (id != PCI_EXT_CAP_ID_REBAR) {
			if (!next || next == pos)
				return;
			pos = next;
			continue;
		}

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
		dev_info(ep->dev, "resizable BAR limited to 16 MiB (%u)\n", nbars);
		return;
	}
}

static void program_config_space(struct openmiop_ep *ep)
{
	u32 classrev;
	int i;

	writew(0, ep->dbi + PCI_COMMAND);

	dbi_ro_wr(ep, true);

	writel((OPENMIOP_PCI_DEVICE << 16) | OPENMIOP_PCI_VENDOR, ep->dbi + PCI_VENDOR_ID);

	classrev = readl(ep->dbi + PCI_CLASS_REVISION);
	/* PCI_CLASS_NETWORK_ETHERNET is the 16-bit class (0x0200). */
	classrev = (classrev & 0xff) | ((u32)PCI_CLASS_NETWORK_ETHERNET << 16);
	writel(classrev, ep->dbi + PCI_CLASS_REVISION);

	writeb(1, ep->dbi + PCI_INTERRUPT_PIN);

	/*
	 * On this RK3588 integration dbi+1MiB aliases the config space,
	 * so a DBI2 mask write would clobber the BAR. The resizable-BAR
	 * control register is what the host's sizing read actually uses.
	 */
	restrict_rebar(ep);

	/* Keep an address the host already assigned. A reload that clears
	 * it makes the BAR decode nowhere until the host writes it again.
	 */
	{
		u32 bar_lo = readl(ep->dbi + PCI_BASE_ADDRESS_0);

		writel((bar_lo & ~0xfu) |
		       PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH,
		       ep->dbi + PCI_BASE_ADDRESS_0);
	}
	for (i = 2; i < 6; i++)
		writel(0, ep->dbi + PCI_BASE_ADDRESS_0 + i * 4);

	/* Width is the lanes parameter. x4 only trains if the PHY was
	 * latched in aggregation; a bifurcated PHY still has two lanes.
	 */
	{
		u32 val = readl(ep->dbi + PCIE_PORT_LINK_CONTROL);
		u32 width = lanes >= 4 ? 4 : 2;

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
		dev_info(ep->dev, "controller link width x%u\n", width);
	}

	/* Target link speed Gen3. The capability pointer walk is the
	 * standard PCI list; LNKCTL2 is 0x30 into the express cap.
	 */
	{
		u32 next = readb(ep->dbi + PCI_CAPABILITY_LIST);

		while (next) {
			u8 id = readb(ep->dbi + next);

			if (id == PCIE_EXP_CAP_ID) {
				u32 lnkcap = readl(ep->dbi + next + PCI_EXP_LNKCAP);
				u16 ctl2 = readw(ep->dbi + next + PCI_EXP_LNKCTL2);
				u32 width = lanes >= 4 ? 4 : 2;

				lnkcap &= ~PCI_EXP_LNKCAP_MLW;
				lnkcap |= width << 4;
				writel(lnkcap, ep->dbi + next + PCI_EXP_LNKCAP);
				ctl2 = (ctl2 & ~0xfu) | 3u;
				writew(ctl2, ep->dbi + next + PCI_EXP_LNKCTL2);
				dev_info(ep->dev, "target link Gen3 x%u LNKCAP %#x\n",
					 width, readl(ep->dbi + next + PCI_EXP_LNKCAP));
				break;
			}
			next = readb(ep->dbi + next + 1);
		}
	}

	hide_ext_cap(ep, PCI_EXT_CAP_ID_ATS);
	hide_ext_cap(ep, PCI_EXT_CAP_ID_PRI);
	hide_ext_cap(ep, PCI_EXT_CAP_ID_REBAR);

	dbi_ro_wr(ep, false);

	dev_info(ep->dev, "config id %04x:%04x class %06x\n",
		 readw(ep->dbi + PCI_VENDOR_ID),
		 readw(ep->dbi + PCI_DEVICE_ID),
		 readl(ep->dbi + PCI_CLASS_REVISION) >> 8);
}

static int wait_link(struct openmiop_ep *ep)
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

static int hw_start(struct openmiop_ep *ep)
{
	int ret;

	rk_hiword(ep->apb, PCIE_CLIENT_GENERAL_CON, PCIE_CLIENT_LTSSM_BIT, 0);
	rk_hiword(ep->apb, PCIE_CLIENT_GENERAL_CON, PCIE_CLIENT_MODE_MASK, 0);
	rk_hiword(ep->apb, PCIE_CLIENT_HOT_RESET_CTRL,
		  PCIE_LTSSM_ENABLE_ENHANCE, PCIE_LTSSM_ENABLE_ENHANCE);

	program_config_space(ep);
	ret = program_inbound_bar0(ep);
	if (ret)
		return ret;

	rk_hiword(ep->apb, PCIE_CLIENT_GENERAL_CON,
		  PCIE_CLIENT_LTSSM_BIT, PCIE_CLIENT_LTSSM_BIT);
	return wait_link(ep);
}

static void hw_stop(struct openmiop_ep *ep)
{
	/* Drops the link. Stop the BMC daemon first: an MMIO read in
	 * flight while LTSSM goes down wedges the MT7620 config path.
	 */
	rk_hiword(ep->apb, PCIE_CLIENT_GENERAL_CON, PCIE_CLIENT_LTSSM_BIT, 0);
	ep->link = false;
}

static int gateway_rx(struct openmiop_ep *ep, int budget)
{
	struct openmiop_bar *bar = ep->bar;
	struct net_device *ndev = ep->ndev;
	u32 head, tail;
	int done = 0;

	bar_inval_dev(ep);
	head = READ_ONCE(bar->rc_tx_head);
	tail = READ_ONCE(bar->rc_tx_tail);
	while (tail != head && done < budget) {
		unsigned int slot_off = offsetof(struct openmiop_bar,
					      rc_tx[tail % OPENMIOP_SLOTS]);
		struct openmiop_slot *slot = ep->cpu + slot_off;
		u32 len;
		struct sk_buff *skb;

		bar_inval_off(ep, slot_off, sizeof(*slot));
		len = READ_ONCE(slot->len);
		if (len < ETH_HLEN || len > OPENMIOP_MAX_FRAME) {
			ndev->stats.rx_errors++;
			tail++;
			continue;
		}
		skb = netdev_alloc_skb_ip_align(ndev, len);
		if (!skb) {
			ndev->stats.rx_dropped++;
			break;
		}
		skb_put_data(skb, slot->data, len);
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		rx_deliver(ep, skb);
		tail++;
		bar_inval_dev(ep);
		head = READ_ONCE(bar->rc_tx_head);
		done++;
	}
	if (tail != READ_ONCE(bar->rc_tx_tail)) {
		WRITE_ONCE(bar->rc_tx_tail, tail);
		bar_flush_poll(ep);
	}
	return done;
}

static int omi_napi(struct napi_struct *napi, int budget)
{
	struct openmiop_ep *ep = container_of(napi, struct openmiop_ep, napi);
	int work = 0;

	if (ep->p2p_on)
		work = p2p_rx(ep, budget);
	if (work < budget)
		work += gateway_rx(ep, budget - work);
	if (work < budget)
		napi_complete_done(napi, work);
	return work;
}

static int poll_thread(void *data)
{
	struct openmiop_ep *ep = data;
	struct net_device *ndev = ep->ndev;

	set_user_nice(current, -20);

	while (!kthread_should_stop()) {
		struct openmiop_bar *bar = ep->bar;
		bool pending = false;

		p2p_watch(ep);
		bar_inval_dev(ep);
		if ((READ_ONCE(bar->rc_flags) & OPENMIOP_FLAG_UP) || ep->p2p_on) {
			if (!netif_carrier_ok(ndev))
				netif_carrier_on(ndev);
		} else if (netif_carrier_ok(ndev)) {
			netif_carrier_off(ndev);
		}
		if (ep->p2p_on &&
		    READ_ONCE(bar->p2p_rx_head) != READ_ONCE(bar->p2p_rx_tail))
			pending = true;
		if (READ_ONCE(bar->rc_tx_head) != READ_ONCE(bar->rc_tx_tail))
			pending = true;
		if (netif_queue_stopped(ndev)) {
			u32 tx_head = READ_ONCE(bar->ep_tx_head);
			u32 tx_tail = READ_ONCE(bar->ep_tx_tail);

			if (tx_head - tx_tail < OPENMIOP_SLOTS &&
			    (!ep->p2p_on || !p2p_full(ep)))
				netif_wake_queue(ndev);
		}
		if (pending) {
			/* Nothing raises an interrupt for a peer write, so this
			 * thread stands in for the RX IRQ. local_bh_enable()
			 * runs the NAPI poll before returning.
			 */
			local_bh_disable();
			napi_schedule(&ep->napi);
			local_bh_enable();
			cond_resched();
		} else {
			usleep_range(20, 50);
		}
	}
	return 0;
}

static int omi_open(struct net_device *ndev)
{
	struct openmiop_ep *ep = netdev_priv(ndev);

	napi_enable(&ep->napi);
	ep->tx_task = kthread_run(tx_thread, ep, "openmiop-tx");
	if (IS_ERR(ep->tx_task)) {
		int err = PTR_ERR(ep->tx_task);

		ep->tx_task = NULL;
		napi_disable(&ep->napi);
		return err;
	}
	ep->poll = kthread_run(poll_thread, ep, "openmiop-ep");
	if (IS_ERR(ep->poll)) {
		int err = PTR_ERR(ep->poll);

		ep->poll = NULL;
		kthread_stop(ep->tx_task);
		ep->tx_task = NULL;
		napi_disable(&ep->napi);
		return err;
	}
	netif_start_queue(ndev);
	return 0;
}

static int omi_stop(struct net_device *ndev)
{
	struct openmiop_ep *ep = netdev_priv(ndev);

	netif_stop_queue(ndev);
	if (ep->tx_task) {
		kthread_stop(ep->tx_task);
		ep->tx_task = NULL;
	}
	if (ep->poll) {
		kthread_stop(ep->poll);
		ep->poll = NULL;
	}
	napi_disable(&ep->napi);
	tx_flush(ep);
	netif_carrier_off(ndev);
	return 0;
}

static int rc_tx(struct openmiop_ep *ep, struct sk_buff *skb)
{
	struct openmiop_bar *bar = ep->bar;
	u32 head = READ_ONCE(bar->ep_tx_head);
	u32 tail = READ_ONCE(bar->ep_tx_tail);
	struct openmiop_slot *slot;

	if (skb->len > OPENMIOP_MAX_FRAME)
		return -EMSGSIZE;
	if (head - tail >= OPENMIOP_SLOTS)
		return -EBUSY;
	slot = &bar->ep_tx[head % OPENMIOP_SLOTS];
	memcpy(slot->data, skb->data, skb->len);
	WRITE_ONCE(slot->len, skb->len);
	bar_flush_off(ep, (u8 *)slot - (u8 *)bar, sizeof(slot->len) + sizeof(slot->flags) + skb->len);
	WRITE_ONCE(bar->ep_tx_head, head + 1);
	WRITE_ONCE(bar->ep_kick, head + 1);
	bar_flush_xmit(ep);
	return 0;
}

static netdev_tx_t omi_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct openmiop_ep *ep = netdev_priv(ndev);
	struct openmiop_bar *bar = ep->bar;
	const u8 *dest = skb->data;
	bool multi = dest[0] & 1;
	bool to_peer = false;
	bool to_rc = false;
	bool held = false;
	int ret;

	bar_inval_dev(ep);
	if (skb->len < ETH_HLEN) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	/* Unicast for the peer stays on the switch. Broadcast and
	 * anything that is not the peer also goes to the router, so
	 * ARP and the gateway address still work.
	 */
	if (ep->p2p_on && (multi || !ether_addr_equal(dest, bar->rc_mac)))
		to_peer = true;
	if (multi || !ep->p2p_on || !ether_addr_equal(dest, bar->peer_mac))
		to_rc = true;

	if (to_peer && skb->len > OPENMIOP_P2P_DATA)
		to_peer = false;
	if (to_rc && skb->len > OPENMIOP_MAX_FRAME)
		to_rc = false;
	if (!to_peer && !to_rc) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	if (to_peer && p2p_full(ep)) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}
	if (to_rc) {
		u32 head = READ_ONCE(bar->ep_tx_head);
		u32 tail = READ_ONCE(bar->ep_tx_tail);

		if (head - tail >= OPENMIOP_SLOTS) {
			netif_stop_queue(ndev);
			return NETDEV_TX_BUSY;
		}
	}

	/* Copy into the gateway ring before the peer path takes the skb. */
	if (to_rc) {
		ret = rc_tx(ep, skb);
		if (ret == -EBUSY && !to_peer) {
			netif_stop_queue(ndev);
			return NETDEV_TX_BUSY;
		}
	}
	if (to_peer) {
		ret = p2p_tx(ep, skb, &held);
		if (ret == -EBUSY) {
			netif_stop_queue(ndev);
			return NETDEV_TX_BUSY;
		}
		if (ret && !held) {
			ndev->stats.tx_dropped++;
			dev_kfree_skb(skb);
			return NETDEV_TX_OK;
		}
		if (ret) {
			ndev->stats.tx_errors++;
			return NETDEV_TX_OK;
		}
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	if (!held)
		dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops omi_netdev_ops = {
	.ndo_open = omi_open,
	.ndo_stop = omi_stop,
	.ndo_start_xmit = omi_xmit,
};

static void publish_bar(struct openmiop_ep *ep, struct net_device *ndev)
{
	struct openmiop_bar *bar = ep->bar;

	memset(bar, 0, sizeof(*bar));
	bar->version = OPENMIOP_VERSION;
	bar->ep_flags = OPENMIOP_FLAG_UP;
	memcpy(bar->ep_mac, ndev->dev_addr, ETH_ALEN);
	WRITE_ONCE(bar->magic, OPENMIOP_MAGIC);
	dma_sync_single_for_device(ep->dev, ep->dma, sizeof(*bar), DMA_TO_DEVICE);
	bar_inval_dev(ep);
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
	u32 before, after, st0, st1;

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

	grf = ioremap(RK3588_PCIE3PHY_GRF_BASE, 0x1000);
	if (!grf) {
		dev_err(dev, "pcie30 phy grf map failed\n");
		goto out_clk;
	}
	before = readl(grf + RK3588_PCIE3PHY_GRF_CMN_CON0);
	st0 = readl(grf + 0x904);
	st1 = readl(grf + 0xa04);
	writel((0x7u << 16) | mode, grf + RK3588_PCIE3PHY_GRF_CMN_CON0);
	after = readl(grf + RK3588_PCIE3PHY_GRF_CMN_CON0);
	iounmap(grf);
	dev_info(dev, "pcie30 phy mode %u (was %#x, now %#x, status %#x %#x)\n",
		 mode, before, after, st0, st1);
out_clk:
	if (pclk) {
		clk_disable_unprepare(pclk);
		clk_put(pclk);
	}
}

static int openmiop_probe(struct platform_device *pdev)
{
	struct openmiop_ep *ep;
	struct net_device *ndev;
	struct resource *dbi_res;
	int ret;

	ndev = alloc_etherdev(sizeof(*ep));
	if (!ndev)
		return -ENOMEM;

	ep = netdev_priv(ndev);
	ep->dev = &pdev->dev;
	ep->ndev = ndev;
	spin_lock_init(&ep->tx_lock);
	init_waitqueue_head(&ep->tx_wait);
	platform_set_drvdata(pdev, ep);

	ep->apb = devm_platform_ioremap_resource_byname(pdev, "pcie-apb");
	if (IS_ERR(ep->apb)) {
		ret = PTR_ERR(ep->apb);
		goto err_free;
	}
	ep->dbi = devm_platform_ioremap_resource_byname(pdev, "pcie-dbi");
	if (IS_ERR(ep->dbi)) {
		ret = PTR_ERR(ep->dbi);
		goto err_free;
	}
	dbi_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pcie-dbi");
	if (!dbi_res || resource_size(dbi_res) < DEFAULT_DBI_ATU_OFFSET + 0x400) {
		dev_err(&pdev->dev, "pcie-dbi region is too small for iATU\n");
		ret = -EINVAL;
		goto err_free;
	}
	ep->dbi2 = ep->dbi + DEFAULT_DBI2_OFFSET;
	ep->atu = ep->dbi + DEFAULT_DBI_ATU_OFFSET;
	if (use_edma && resource_size(dbi_res) >= EDMA_OFFSET + 0x220)
		ep->edma = ep->dbi + EDMA_OFFSET;

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

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (!ret)
		ep->rmem = true;
	else
		dev_info(&pdev->dev, "no reserved-mem pool (%d), using dma\n", ret);

	/* Two windows so one of them is aligned to the BAR size. Buddy
	 * allocations already are; a reserved-mem pool often is not.
	 */
	ep->raw_size = OPENMIOP_BAR_SIZE * 2;
	ep->raw = dma_alloc_noncoherent(&pdev->dev, ep->raw_size, &ep->raw_dma,
					DMA_BIDIRECTIONAL, GFP_KERNEL);
	ep->cacheable = ep->raw != NULL;
	if (!ep->raw) {
		ep->raw = dma_alloc_coherent(&pdev->dev, ep->raw_size, &ep->raw_dma,
					     GFP_KERNEL);
	}
	if (!ep->raw) {
		ep->raw_size = OPENMIOP_BAR_SIZE;
		ep->raw = dma_alloc_noncoherent(&pdev->dev, ep->raw_size, &ep->raw_dma,
						DMA_BIDIRECTIONAL, GFP_KERNEL);
		ep->cacheable = ep->raw != NULL;
		if (!ep->raw)
			ep->raw = dma_alloc_coherent(&pdev->dev, ep->raw_size,
						     &ep->raw_dma, GFP_KERNEL);
	}
	if (!ep->raw) {
		dev_err(&pdev->dev, "BAR buffer alloc failed\n");
		ret = -ENOMEM;
		goto err_clk;
	}
	ep->dma = ALIGN(ep->raw_dma, OPENMIOP_BAR_SIZE);
	if (ep->dma + OPENMIOP_BAR_SIZE > ep->raw_dma + ep->raw_size) {
		dev_err(&pdev->dev, "BAR buffer %#llx is not %u-aligned\n",
			(unsigned long long)ep->raw_dma, OPENMIOP_BAR_SIZE);
		ret = -EINVAL;
		goto err_dma;
	}
	ep->cpu = ep->raw + (ep->dma - ep->raw_dma);
	ep->bar = ep->cpu;
	if (EDMA_SCRATCH_OFF + sizeof(*ep->scratch) <= OPENMIOP_BAR_SIZE) {
		ep->scratch = ep->cpu + EDMA_SCRATCH_OFF;
		ep->scratch_dma = ep->dma + EDMA_SCRATCH_OFF;
	}
	dev_info(&pdev->dev, "BAR dma %#llx (raw %#llx) size %u %s\n",
		 (unsigned long long)ep->dma, (unsigned long long)ep->raw_dma,
		 OPENMIOP_BAR_SIZE, ep->cacheable ? "cacheable" : "uncached");

	ret = hw_start(ep);
	if (ret)
		goto err_dma;
	edma_init(ep);

	BUILD_BUG_ON(offsetof(struct openmiop_bar, rc_tx_tail) != 64);
	BUILD_BUG_ON(offsetof(struct openmiop_bar, rc_flags) != 128);
	BUILD_BUG_ON(offsetof(struct openmiop_bar, ep_tx) != 192);
	BUILD_BUG_ON(sizeof(struct openmiop_bar) > OPENMIOP_P2P_OFF);
	BUILD_BUG_ON(OPENMIOP_P2P_OFF + OPENMIOP_P2P_SLOTS * OPENMIOP_P2P_SLOT >
		     OPENMIOP_BAR_SIZE);
	BUILD_BUG_ON(offsetof(struct openmiop_scratch, ll) % 64 != 0);
	BUILD_BUG_ON(offsetof(struct openmiop_scratch, ll1) % 64 != 0);
	BUILD_BUG_ON(TX_BATCH * 2 + 2 > ARRAY_SIZE(((struct openmiop_scratch *)0)->ll));

	strscpy(ndev->name, "omi%d", IFNAMSIZ);
	ndev->netdev_ops = &omi_netdev_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->mtu = 9000;
	ndev->max_mtu = OPENMIOP_P2P_DATA - ETH_HLEN;
	ndev->features |= NETIF_F_GRO | NETIF_F_RXCSUM;
	ndev->hw_features |= NETIF_F_GRO | NETIF_F_RXCSUM;
	netif_napi_add(ndev, &ep->napi, omi_napi);
	eth_hw_addr_random(ndev);
	netif_carrier_off(ndev);
	SET_NETDEV_DEV(ndev, &pdev->dev);

	publish_bar(ep, ndev);

	ret = register_netdev(ndev);
	if (ret)
		goto err_hw;

	dev_info(&pdev->dev, "omi netdev %s mac %pM\n", ndev->name, ndev->dev_addr);
	return 0;

err_hw:
	netif_napi_del(&ep->napi);
	hw_stop(ep);
err_dma:
	if (ep->raw) {
		if (ep->cacheable)
			dma_free_noncoherent(&pdev->dev, ep->raw_size, ep->raw,
					     ep->raw_dma, DMA_BIDIRECTIONAL);
		else
			dma_free_coherent(&pdev->dev, ep->raw_size, ep->raw, ep->raw_dma);
	}
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

static int openmiop_remove(struct platform_device *pdev)
{
	struct openmiop_ep *ep = platform_get_drvdata(pdev);

	if (ep->bar) {
		WRITE_ONCE(ep->bar->ep_flags, 0);
		bar_flush_xmit(ep);
	}
	if (ep->ndev) {
		unregister_netdev(ep->ndev);
		netif_napi_del(&ep->napi);
	}
	if (ep->ob)
		iounmap(ep->ob);
	hw_stop(ep);
	if (ep->raw) {
		if (ep->cacheable)
			dma_free_noncoherent(&pdev->dev, ep->raw_size, ep->raw,
					     ep->raw_dma, DMA_BIDIRECTIONAL);
		else
			dma_free_coherent(&pdev->dev, ep->raw_size, ep->raw, ep->raw_dma);
	}
	clk_bulk_disable_unprepare(ep->nclks, ep->clks);
	if (ep->rmem)
		of_reserved_mem_device_release(&pdev->dev);
	phy_power_off(ep->phy);
	phy_exit(ep->phy);
	if (ep->vpcie3v3)
		regulator_disable(ep->vpcie3v3);
	if (ep->ndev)
		free_netdev(ep->ndev);
	return 0;
}

static const struct of_device_id openmiop_of_match[] = {
	{ .compatible = "mixtile,miop-ep-rk3588" },
	{ }
};
MODULE_DEVICE_TABLE(of, openmiop_of_match);

static struct platform_driver openmiop_driver = {
	.probe = openmiop_probe,
	.remove = openmiop_remove,
	.driver = {
		.name = "openmiop-ep",
		.of_match_table = openmiop_of_match,
	},
};
module_platform_driver(openmiop_driver);

MODULE_DESCRIPTION("openmiop RK3588 PCIe endpoint Ethernet");
MODULE_LICENSE("GPL");
