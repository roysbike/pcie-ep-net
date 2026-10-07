// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * omi-dbtest: hardware experiments next to a running openmiop-ep, for the
 * v0.2 interrupt design. Not part of the driver; never load it in
 * production.
 *
 * - eDMA: report the channel counts; run one 4-byte transfer on write
 *   channel `ch` into node `peer`'s BAR at `peer_off`, with the done
 *   interrupt unmasked for that channel, and report which dmaN line
 *   fired.
 * - Doorbell: one GIC MBI vector. Inbound iATU region `region` (address
 *   match) maps BAR0 + win_off (64 KiB) onto the MBI alias frame, so a
 *   PCIe write of the vector's data at win_off + (msg address & 0xffff)
 *   raises the vector. Counts them.
 *
 * Commands (echo into /sys/module/omi_dbtest/parameters/cmd):
 *   edma PEER [OFF [CH]]   run the eDMA test
 *   db REGION [WIN_OFF]    (re)program the doorbell window
 *   dboff                  disable the doorbell window
 *   db0 [SPARE [WIN_OFF]]  doorbell in region 0: copy region 0 (the BAR
 *                          match) to SPARE (default 2), disable 0, then
 *                          program 0 as the window. If the lowest region
 *                          wins on overlap, this is the order that works.
 *                          TARGET (3rd arg): 0 MBI alias (default),
 *                          1 GICD, 2 ITS1 translater page
 *   db0off                 undo db0
 *   cpu 0|1                CPU write of the MBI vector data to the MBI
 *                          alias (0) or GICD_SETSPI_NSR (1)
 *   its DEVID              allocate an ITS1 vector for DeviceID DEVID
 *                          (the requester ID of the PCIe writer); its
 *                          event id is what to write at TRANSLATER
 *   peek OFF               read 16 bytes of BAR0 memory at OFF
 *   dump                   iATU regions, counters, vector
 *
 * Writes only registers of write channel `ch` (never 0), inbound region
 * `region` (never 0) and the eDMA interrupt mask bits of `ch`.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pci_regs.h>
#include <linux/platform_device.h>

#include "openmiop.h"

static int region = 1;
module_param(region, int, 0444);
MODULE_PARM_DESC(region, "Inbound iATU region for the doorbell at load (0: none)");
static uint win_off = 0xff0000;
module_param(win_off, uint, 0444);
static int peer = -1;
module_param(peer, int, 0444);
MODULE_PARM_DESC(peer, "Node index for the eDMA test at load (-1: none)");
static uint peer_off = 0xff0100;
module_param(peer_off, uint, 0444);
static int ch = 1;
module_param(ch, int, 0444);

#define DBI_PHYS	0xa40000000ULL
#define ATU_PHYS	0xa40300000ULL
#define EDMA_OFF	0x80000
#define MBI_ALIAS	0xfe610000ULL
#define GICD_PHYS	0xfe600000ULL
#define ITS1_PHYS	0xfe660000ULL
#define ITS1_XLATE	(ITS1_PHYS + 0x10000)	/* GITS_TRANSLATER at +0x40 */

#define EDMA_CTRL		0x008
#define EDMA_WR_ENB		0x00c
#define EDMA_WR_DOORBELL	0x010
#define EDMA_WR_INT_STATUS	0x04c
#define EDMA_WR_INT_MASK	0x054
#define EDMA_WR_INT_CLEAR	0x058
#define EDMA_CH(c)		(0x200 + (c) * 0x200)
#define CH_CTRL1		0x00
#define CH_SIZE			0x08
#define CH_SAR_LO		0x0c
#define CH_SAR_HI		0x10
#define CH_DAR_LO		0x14
#define CH_DAR_HI		0x18
#define CH_LIE			BIT(3)

#define ATU_CTRL1	0x000
#define ATU_CTRL2	0x004
#define ATU_LBASE	0x008
#define ATU_UBASE	0x00c
#define ATU_LIMIT	0x010
#define ATU_LTARGET	0x014
#define ATU_UTARGET	0x018
#define ATU_ULIMIT	0x020
#define ATU_ENABLE	BIT(31)
#define ATU_INCREASE	BIT(13)

static DEFINE_MUTEX(lock);
static void __iomem *dbi, *atu, *edma;
static struct platform_device *mdev;	/* holds the MSI vector */
static struct device *epdev;
static int db_irq = -1, dma_irq[4] = { -1, -1, -1, -1 };
static int db_region;			/* programmed region, 0: none */
static int db0_spare;			/* db0: region holding the BAR match */
static atomic_t db_count, dma_count[4];
static struct msi_msg db_msg;
static phys_addr_t bar_phys;
static struct of_changeset its_ocs;
static struct device_node *its_np;	/* our dynamic node */
static struct platform_device *its_pdev;
static bool its_pdev_ours;		/* created here, not by the OF notifier */
static int its_irq = -1;
static struct msi_msg its_msg;
static atomic_t its_count;
static void *buf;
static dma_addr_t buf_dma;

static void __iomem *ib(int i)
{
	return atu + ((i << 9) | BIT(8));
}

static u64 bar_pci(void)
{
	u32 lo = readl(dbi + PCI_BASE_ADDRESS_0);
	u32 hi = readl(dbi + PCI_BASE_ADDRESS_1);

	return ((u64)hi << 32 | lo) & ~0xfULL;
}

static void write_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	db_msg = *msg;
}

static irqreturn_t db_isr(int irq, void *d)
{
	int n = atomic_inc_return(&db_count);

	if (n <= 5 || !(n & 1023))
		pr_info("omi-dbtest: doorbell irq %d on cpu %d (count %d)\n",
			irq, raw_smp_processor_id(), n);
	return IRQ_HANDLED;
}

static void write_its_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	its_msg = *msg;
}

static irqreturn_t its_isr(int irq, void *d)
{
	int n = atomic_inc_return(&its_count);

	if (n <= 5 || !(n & 1023))
		pr_info("omi-dbtest: ITS irq %d on cpu %d (count %d)\n",
			irq, raw_smp_processor_id(), n);
	return IRQ_HANDLED;
}

static void its_free(void)
{
	if (its_irq >= 0)
		free_irq(its_irq, NULL);
	its_irq = -1;
	if (its_pdev) {
		platform_device_msi_free_irqs_all(&its_pdev->dev);
		if (its_pdev_ours)
			platform_device_unregister(its_pdev);
		else
			put_device(&its_pdev->dev);
		its_pdev = NULL;
	}
	if (its_np) {
		of_changeset_revert(&its_ocs);
		of_changeset_destroy(&its_ocs);
		its_np = NULL;
	}
}

/* An ITS1 vector whose DeviceID is devid, via a dynamic DT node with
 * msi-parent = <&its1 devid>. A PCIe write of the event id to the
 * TRANSLATER raises it if the controller tags the write with devid.
 */
static int its_vector(u32 devid)
{
	struct device_node *its = NULL, *root, *np;
	struct irq_domain *dom;
	struct resource r;
	u32 mp[2];
	int ret, irq;

	its_free();
	for_each_compatible_node(np, NULL, "arm,gic-v3-its") {
		if (!of_address_to_resource(np, 0, &r) && r.start == ITS1_PHYS) {
			its = of_node_get(np);
			break;
		}
	}
	if (!its) {
		pr_err("omi-dbtest: ITS1 node at %#llx not found\n", ITS1_PHYS);
		return -ENODEV;
	}
	dom = irq_find_matching_fwnode(of_fwnode_handle(its), DOMAIN_BUS_NEXUS);
	if (!dom)
		dom = irq_find_matching_fwnode(of_fwnode_handle(its), DOMAIN_BUS_ANY);
	mp[0] = its->phandle;
	mp[1] = devid;
	pr_info("omi-dbtest: ITS1 node %pOF phandle %#x domain %s\n", its, mp[0],
		dom ? dom->name : "(none)");
	of_node_put(its);
	if (!dom)
		return -ENODEV;

	root = of_find_node_by_path("/");
	of_changeset_init(&its_ocs);
	np = of_changeset_create_node(&its_ocs, root, "omi-dbtest-its");
	of_node_put(root);
	if (!np) {
		of_changeset_destroy(&its_ocs);
		return -ENOMEM;
	}
	ret = of_changeset_add_prop_u32_array(&its_ocs, np, "msi-parent", mp, 2);
	if (!ret)
		ret = of_changeset_apply(&its_ocs);
	if (ret) {
		of_changeset_destroy(&its_ocs);
		return ret;
	}
	its_np = np;
	/* A node added under the root gets a platform device from the OF
	 * reconfig notifier; use that one, create one only if it did not.
	 */
	its_pdev = of_find_device_by_node(np);
	its_pdev_ours = !its_pdev;
	if (!its_pdev) {
		its_pdev = platform_device_alloc("omi-dbtest-its", PLATFORM_DEVID_NONE);
		if (!its_pdev) {
			its_free();
			return -ENOMEM;
		}
		device_set_node(&its_pdev->dev, of_fwnode_handle(np));
		ret = platform_device_add(its_pdev);
		if (ret) {
			pr_err("omi-dbtest: platform_device_add: %d\n", ret);
			platform_device_put(its_pdev);
			its_pdev = NULL;
			its_free();
			return ret;
		}
	}
	pr_info("omi-dbtest: ITS device %s (%s)\n", dev_name(&its_pdev->dev),
		its_pdev_ours ? "created" : "from OF notifier");
	dev_set_msi_domain(&its_pdev->dev, dom);
	ret = platform_device_msi_init_and_alloc_irqs(&its_pdev->dev, 1, write_its_msg);
	if (ret) {
		pr_err("omi-dbtest: ITS MSI alloc failed %d\n", ret);
		its_free();
		return ret;
	}
	irq = msi_get_virq(&its_pdev->dev, 0);
	ret = request_irq(irq, its_isr, 0, "omi-its-doorbell", NULL);
	if (ret) {
		its_free();
		return ret;
	}
	its_irq = irq;
	pr_info("omi-dbtest: ITS1 DeviceID %#x: irq %d msg %#x_%08x data %#x\n",
		devid, irq, its_msg.address_hi, its_msg.address_lo, its_msg.data);
	return 0;
}

static int cpu_write(int sel)
{
	phys_addr_t base = sel ? GICD_PHYS : MBI_ALIAS;
	void __iomem *p;
	int before = atomic_read(&db_count);

	if (db_irq < 0)
		return -ENODEV;
	p = ioremap(base, 0x1000);
	if (!p)
		return -ENOMEM;
	writel(db_msg.data, p + 0x40);
	iounmap(p);
	mdelay(5);
	pr_info("omi-dbtest: CPU wrote %#x to %pa+0x40: doorbells %d -> %d\n",
		db_msg.data, &base, before, atomic_read(&db_count));
	return 0;
}

static irqreturn_t dma_isr(int irq, void *d)
{
	int i = (long)d;
	u32 st = readl(edma + EDMA_WR_INT_STATUS);
	u32 mine = st & (BIT(ch) | BIT(16 + ch));

	if (!mine)
		return IRQ_NONE;
	atomic_inc(&dma_count[i]);
	pr_info("omi-dbtest: dma%d (irq %d, cpu %d) status %#x\n", i, irq,
		raw_smp_processor_id(), st);
	writel(mine, edma + EDMA_WR_INT_CLEAR);
	return IRQ_HANDLED;
}

static void dump_region(int i)
{
	void __iomem *b = ib(i);

	pr_info("omi-dbtest: inbound %d ctrl1 %#x ctrl2 %#x base %#x_%08x limit %#x_%08x target %#x_%08x\n",
		i, readl(b + ATU_CTRL1), readl(b + ATU_CTRL2),
		readl(b + ATU_UBASE), readl(b + ATU_LBASE),
		readl(b + ATU_ULIMIT), readl(b + ATU_LIMIT),
		readl(b + ATU_UTARGET), readl(b + ATU_LTARGET));
}

static void dump(void)
{
	int i;

	pr_info("omi-dbtest: BAR0 pci %#llx, cmd %#x, inbound 0 target %pa\n",
		bar_pci(), readl(dbi + PCI_COMMAND), &bar_phys);
	for (i = 0; i < 4; i++)
		dump_region(i);
	pr_info("omi-dbtest: vector irq %d msg %#x_%08x data %#x; doorbell window region %d\n",
		db_irq, db_msg.address_hi, db_msg.address_lo, db_msg.data, db_region);
	pr_info("omi-dbtest: ITS vector irq %d msg %#x_%08x data %#x, ITS irqs %d\n",
		its_irq, its_msg.address_hi, its_msg.address_lo, its_msg.data,
		atomic_read(&its_count));
	pr_info("omi-dbtest: doorbells %d, dma irqs %d %d %d %d\n",
		atomic_read(&db_count), atomic_read(&dma_count[0]),
		atomic_read(&dma_count[1]), atomic_read(&dma_count[2]),
		atomic_read(&dma_count[3]));
}

static int alloc_vector(void)
{
	struct device_node *gic;
	struct irq_domain *dom;
	int ret, irq;

	gic = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (!gic)
		return -ENODEV;
	dom = irq_find_matching_fwnode(of_fwnode_handle(gic), DOMAIN_BUS_NEXUS);
	of_node_put(gic);
	if (!dom) {
		pr_err("omi-dbtest: no MBI domain\n");
		return -ENODEV;
	}
	mdev = platform_device_register_simple("omi-dbtest", -1, NULL, 0);
	if (IS_ERR(mdev)) {
		ret = PTR_ERR(mdev);
		mdev = NULL;
		return ret;
	}
	dev_set_msi_domain(&mdev->dev, dom);
	ret = platform_device_msi_init_and_alloc_irqs(&mdev->dev, 1, write_msg);
	if (ret) {
		pr_err("omi-dbtest: MSI alloc failed %d\n", ret);
		return ret;
	}
	irq = msi_get_virq(&mdev->dev, 0);
	ret = request_irq(irq, db_isr, 0, "omi-doorbell", NULL);
	if (ret)
		return ret;
	db_irq = irq;
	pr_info("omi-dbtest: MBI irq %d msg addr %#x_%08x data %#x\n", db_irq,
		db_msg.address_hi, db_msg.address_lo, db_msg.data);
	if ((((u64)db_msg.address_hi << 32 | db_msg.address_lo) & ~0xffffULL) != MBI_ALIAS)
		pr_warn("omi-dbtest: msg address is not in the MBI alias frame\n");
	return 0;
}

static void db_off(void)
{
	if (!db_region)
		return;
	writel(0, ib(db_region) + ATU_CTRL2);
	udelay(10);
	pr_info("omi-dbtest: doorbell window region %d disabled\n", db_region);
	db_region = 0;
}

static int db_on(int r, u32 off)
{
	void __iomem *b;
	u64 base, end, pci = bar_pci();

	if (db_irq < 0)
		return -ENODEV;
	if (r <= 0 || r > 15 || off & 0xffff || off >= OPENMIOP_BAR_SIZE)
		return -EINVAL;
	if (!pci) {
		pr_warn("omi-dbtest: BAR0 not assigned\n");
		return -EAGAIN;
	}
	db_off();
	b = ib(r);
	if (readl(b + ATU_CTRL2) & ATU_ENABLE) {
		pr_err("omi-dbtest: inbound region %d already in use\n", r);
		return -EBUSY;
	}
	base = pci + off;
	end = base + 0xffff;
	writel(lower_32_bits(base), b + ATU_LBASE);
	writel(upper_32_bits(base), b + ATU_UBASE);
	writel(lower_32_bits(end), b + ATU_LIMIT);
	writel(upper_32_bits(end), b + ATU_ULIMIT);
	writel(lower_32_bits(MBI_ALIAS), b + ATU_LTARGET);
	writel(upper_32_bits(MBI_ALIAS), b + ATU_UTARGET);
	writel(upper_32_bits(end) != upper_32_bits(base) ? ATU_INCREASE : 0,
	       b + ATU_CTRL1);
	writel(ATU_ENABLE, b + ATU_CTRL2);
	udelay(10);
	db_region = r;
	dump_region(r);
	pr_info("omi-dbtest: doorbell: write %#x at BAR0 offset %#x (pci %#llx)\n",
		db_msg.data, off + (db_msg.address_lo & 0xffff),
		base + (db_msg.address_lo & 0xffff));
	return 0;
}

static void copy_region(int from, int to)
{
	static const u32 regs[] = { ATU_CTRL1, ATU_LBASE, ATU_UBASE, ATU_LIMIT,
				    ATU_ULIMIT, ATU_LTARGET, ATU_UTARGET };
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		writel(readl(ib(from) + regs[i]), ib(to) + regs[i]);
	writel(readl(ib(from) + ATU_CTRL2), ib(to) + ATU_CTRL2);
	udelay(10);
}

/* Each step leaves every BAR0 address with a valid translation. */
static int db0_on(int spare, u32 off, int tsel)
{
	static const u64 targets[] = { MBI_ALIAS, GICD_PHYS, ITS1_XLATE };
	u64 target;
	void __iomem *b = ib(0);
	u64 base, end, pci = bar_pci();

	if (db_irq < 0)
		return -ENODEV;
	if (spare < 1 || spare > 15 || off & 0xffff || off >= OPENMIOP_BAR_SIZE ||
	    tsel < 0 || tsel >= ARRAY_SIZE(targets))
		return -EINVAL;
	if (db0_spare)
		return -EBUSY;
	target = targets[tsel];
	if (!pci)
		return -EAGAIN;
	if (!(readl(b + ATU_CTRL2) & ATU_ENABLE) || !(readl(b + ATU_CTRL2) & BIT(30))) {
		pr_err("omi-dbtest: region 0 is not an enabled BAR match\n");
		return -EINVAL;
	}
	if (readl(ib(spare) + ATU_CTRL2) & ATU_ENABLE)
		return -EBUSY;
	db_off();
	copy_region(0, spare);
	dump_region(spare);
	writel(0, b + ATU_CTRL2);
	udelay(10);
	base = pci + off;
	end = base + 0xffff;
	writel(lower_32_bits(base), b + ATU_LBASE);
	writel(upper_32_bits(base), b + ATU_UBASE);
	writel(lower_32_bits(end), b + ATU_LIMIT);
	writel(upper_32_bits(end), b + ATU_ULIMIT);
	writel(lower_32_bits(target), b + ATU_LTARGET);
	writel(upper_32_bits(target), b + ATU_UTARGET);
	writel(upper_32_bits(end) != upper_32_bits(base) ? ATU_INCREASE : 0,
	       b + ATU_CTRL1);
	writel(ATU_ENABLE, b + ATU_CTRL2);
	udelay(10);
	db0_spare = spare;
	dump_region(0);
	pr_info("omi-dbtest: db0: write %#x at BAR0 offset %#x\n", db_msg.data,
		off + (db_msg.address_lo & 0xffff));
	return 0;
}

static void db0_off(void)
{
	if (!db0_spare)
		return;
	writel(0, ib(0) + ATU_CTRL2);
	udelay(10);
	copy_region(db0_spare, 0);
	writel(0, ib(db0_spare) + ATU_CTRL2);
	udelay(10);
	pr_info("omi-dbtest: db0 undone, region 0 restored from %d\n", db0_spare);
	db0_spare = 0;
	dump_region(0);
}

static int peek(u32 off)
{
	u32 *p;

	if (off & 3 || off > OPENMIOP_BAR_SIZE - 16)
		return -EINVAL;
	p = memremap(bar_phys + off, 16, MEMREMAP_WB);
	if (!p)
		return -ENOMEM;
	dma_sync_single_for_cpu(epdev, bar_phys + off, 16, DMA_FROM_DEVICE);
	pr_info("omi-dbtest: BAR0[%#x]: %08x %08x %08x %08x\n", off, p[0], p[1], p[2], p[3]);
	memunmap(p);
	return 0;
}

static int edma_test(int p, u32 off, int c)
{
	void __iomem *cr = edma + EDMA_CH(c);
	struct omi_bar_head *h;
	u64 dar;
	u32 st;
	int i;

	if (p < 0 || p >= OMI_MAX_NODES || c < 1 || c > 3 ||
	    (off & 3) || off < OMI_SCRATCH_OFF + 0x10000 || off >= OPENMIOP_BAR_SIZE)
		return -EINVAL;
	h = memremap(bar_phys, sizeof(*h), MEMREMAP_WB);
	if (!h)
		return -ENOMEM;
	dma_sync_single_for_cpu(epdev, bar_phys, sizeof(*h), DMA_FROM_DEVICE);
	dar = (u64)h->peers[p].bar_hi << 32 | h->peers[p].bar_lo;
	pr_info("omi-dbtest: node %d epoch %#x bar %#llx\n", p, h->peers[p].epoch, dar);
	i = h->peers[p].epoch;
	memunmap(h);
	if (!dar || !i)
		return -ENOENT;
	dar += off;

	*(u32 *)buf = 0x0b5e55ed;
	dma_sync_single_for_device(epdev, buf_dma, 64, DMA_TO_DEVICE);
	writel(readl(edma + EDMA_WR_INT_MASK) & ~(BIT(c) | BIT(16 + c)),
	       edma + EDMA_WR_INT_MASK);
	writel(BIT(0), edma + EDMA_WR_ENB);
	writel(CH_LIE, cr + CH_CTRL1);
	writel(4, cr + CH_SIZE);
	writel(lower_32_bits(buf_dma), cr + CH_SAR_LO);
	writel(upper_32_bits(buf_dma), cr + CH_SAR_HI);
	writel(lower_32_bits(dar), cr + CH_DAR_LO);
	writel(upper_32_bits(dar), cr + CH_DAR_HI);
	writel(c, edma + EDMA_WR_DOORBELL);
	for (i = 0; i < 100; i++) {
		st = readl(edma + EDMA_WR_INT_STATUS);
		if (!(st & (BIT(c) | BIT(16 + c))) && atomic_read(&dma_count[0]) +
		    atomic_read(&dma_count[1]) + atomic_read(&dma_count[2]) +
		    atomic_read(&dma_count[3]))
			break;
		msleep(1);
	}
	pr_info("omi-dbtest: eDMA ch%d to %#llx: status %#x after %d ms; irq counts %d %d %d %d\n",
		c, dar, readl(edma + EDMA_WR_INT_STATUS), i,
		atomic_read(&dma_count[0]), atomic_read(&dma_count[1]),
		atomic_read(&dma_count[2]), atomic_read(&dma_count[3]));
	writel(readl(edma + EDMA_WR_INT_MASK) | BIT(c) | BIT(16 + c),
	       edma + EDMA_WR_INT_MASK);
	writel(BIT(c) | BIT(16 + c), edma + EDMA_WR_INT_CLEAR);
	return 0;
}

static int cmd_set(const char *val, const struct kernel_param *kp)
{
	char op[8] = "";
	u32 a = 0, b = 0, c = 0;
	int n, ret = 0;

	n = sscanf(val, "%7s %i %i %i", op, &a, &b, &c);
	if (n < 1)
		return -EINVAL;
	if (!epdev)
		return -ENODEV;
	mutex_lock(&lock);
	if (!strcmp(op, "edma"))
		ret = edma_test(n > 1 ? a : peer, n > 2 ? b : peer_off, n > 3 ? c : ch);
	else if (!strcmp(op, "db"))
		ret = db_on(n > 1 ? a : region, n > 2 ? b : win_off);
	else if (!strcmp(op, "dboff"))
		db_off();
	else if (!strcmp(op, "db0"))
		ret = db0_on(n > 1 ? a : 2, n > 2 ? b : win_off, n > 3 ? c : 0);
	else if (!strcmp(op, "db0off"))
		db0_off();
	else if (!strcmp(op, "peek"))
		ret = peek(a);
	else if (!strcmp(op, "cpu"))
		ret = cpu_write(a);
	else if (!strcmp(op, "its"))
		ret = n > 1 ? its_vector(a) : -EINVAL;
	else if (!strcmp(op, "dump"))
		dump();
	else
		ret = -EINVAL;
	mutex_unlock(&lock);
	if (ret)
		pr_info("omi-dbtest: '%s' failed: %d\n", op, ret);
	return ret;
}

static const struct kernel_param_ops cmd_ops = {
	.set = cmd_set,
};
module_param_cb(cmd, &cmd_ops, NULL, 0200);
MODULE_PARM_DESC(cmd, "edma PEER [OFF [CH]] | db REGION [WIN_OFF] | dboff | db0 [SPARE [WIN_OFF [TARGET]]] | db0off | peek OFF | cpu 0|1 | its DEVID | dump");

static void cleanup(void)
{
	int i;

	db0_off();
	db_off();
	its_free();
	if (db_irq >= 0)
		free_irq(db_irq, NULL);
	if (mdev) {
		platform_device_msi_free_irqs_all(&mdev->dev);
		platform_device_unregister(mdev);
	}
	for (i = 0; i < 4; i++)
		if (dma_irq[i] >= 0)
			free_irq(dma_irq[i], (void *)(long)i);
	if (buf)
		dma_free_coherent(epdev, 64, buf, buf_dma);
	if (dbi)
		iounmap(dbi);
	if (atu)
		iounmap(atu);
	if (epdev)
		put_device(epdev);
}

static int __init dbtest_init(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	u32 ctrl;
	int i, ret;

	np = of_find_compatible_node(NULL, NULL, "openmiop,rk3588-pcie-ep");
	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -ENODEV;

	dbi = ioremap(DBI_PHYS, 0x1000);
	atu = ioremap(ATU_PHYS, 0x100000);
	buf = dma_alloc_coherent(&pdev->dev, 64, &buf_dma, GFP_KERNEL);
	epdev = &pdev->dev;
	if (!dbi || !atu || !buf) {
		cleanup();
		return -ENOMEM;
	}
	edma = atu + EDMA_OFF;
	ctrl = readl(edma + EDMA_CTRL);
	pr_info("omi-dbtest: eDMA ctrl %#x: %u write, %u read channels\n",
		ctrl, ctrl & 0xf, (ctrl >> 16) & 0xf);
	bar_phys = (u64)readl(ib(0) + ATU_UTARGET) << 32 | readl(ib(0) + ATU_LTARGET);

	for (i = 0; i < 4; i++) {
		char name[8];

		snprintf(name, sizeof(name), "dma%d", i);
		ret = platform_get_irq_byname(pdev, name);
		if (ret < 0)
			continue;
		if (request_irq(ret, dma_isr, IRQF_SHARED, "omi-dbtest-dma",
				(void *)(long)i)) {
			pr_warn("omi-dbtest: request dma%d (irq %d) failed\n", i, ret);
			continue;
		}
		dma_irq[i] = ret;
	}
	ret = alloc_vector();
	if (ret)
		pr_err("omi-dbtest: no doorbell vector: %d\n", ret);
	dump();

	mutex_lock(&lock);
	if (peer >= 0)
		edma_test(peer, peer_off, ch);
	if (region > 0)
		db_on(region, win_off);
	mutex_unlock(&lock);
	return 0;
}

static void __exit dbtest_exit(void)
{
	mutex_lock(&lock);
	dump();
	cleanup();
	mutex_unlock(&lock);
}

module_init(dbtest_init);
module_exit(dbtest_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("openmiop v0.2 doorbell and eDMA interrupt experiments");
