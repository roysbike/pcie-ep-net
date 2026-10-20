// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * blade3-nvme-recover: bring the M.2 NVMe of a Mixtile Blade 3 back after
 * the Cluster Box pulsed PERST#.
 *
 * In a Cluster Box the M.2 PERST# is the blade's one U.2 PERST#, shared
 * with the endpoint link to the Cluster Box (see the pcie3x2 node of the
 * DTB). When the Cluster Box reboots it holds PERST#: the NVMe resets
 * under a live driver and its link drops. The Talos 6.18 dw-rockchip
 * host has no link-down handling (no "sys" IRQ in RC mode), so the link
 * stays down, the NVMe I/O times out after 30 s and nvme marks the
 * controller dead. Mainline has a fix in the host driver (reset_root_port,
 * Linux 7.3); this module does the same from outside so the official
 * kernel and modules stay as they are.
 *
 * It polls the link of the Root Port above every NVMe behind a
 * rockchip-dw-pcie host. When the link drops it restarts the LTSSM (once
 * a second until the link is back); once the link is up it resets the
 * NVMe with pci_reset_function(), which runs nvme's reset_prepare and
 * reset_done: the controller is brought up again with its queues and
 * namespaces, and the I/O that waited is retried. That has to happen
 * before the nvme I/O timeout (30 s) gives the controller up.
 * The link down also resets the non-sticky registers of the Root Port
 * (command, bus numbers), and after PERST# the BARs of the NVMe are gone,
 * while pci_reset_function() saves and restores the config space it
 * finds. So the config space of both, saved at load time, goes back
 * first.
 *
 * It does not bind to the device; nvme stays its driver.
 */
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "pcie-designware.h"

#define HOST_DRIVER	"rockchip-dw-pcie"
#define MAX_PORTS	4

static unsigned int poll_ms = 20;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Link poll interval in ms (default 20)");

static unsigned int retrain_ms = 1000;
module_param(retrain_ms, uint, 0644);
MODULE_PARM_DESC(retrain_ms, "Restart the LTSSM this often while the link is down, in ms (default 1000)");

static unsigned int stable_ms = 1000;
module_param(stable_ms, uint, 0644);
MODULE_PARM_DESC(stable_ms, "The link must stay up this long before the NVMe reset, in ms (default 1000)");

static unsigned int recoveries;
module_param(recoveries, uint, 0444);
MODULE_PARM_DESC(recoveries, "NVMe resets done after a link down (read only)");

struct port {
	struct pci_dev *nvme;
	struct pci_dev *rp;		/* Root Port above it */
	struct dw_pcie *pci;
	/* Config space from load time */
	struct pci_saved_state *nvme_saved, *rp_saved;
	bool up;
	unsigned long down_since;
	unsigned long up_since;		/* link back since, 0: not yet */
	unsigned long retrained;
};

static struct port ports[MAX_PORTS];
static unsigned int nports;
static struct task_struct *task;

/* Ask for the top speed again: the link down resets the port logic bit
 * that dw_pcie_setup_rc() set, and the link trains to 2.5 GT/s only.
 * In L0 the write starts the speed change, otherwise the next training
 * does it.
 */
static void speed_change(struct dw_pcie *pci)
{
	u32 val = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);

	dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL,
			   val | PORT_LOGIC_SPEED_CHANGE);
}

static void retrain(struct port *p)
{
	struct dw_pcie *pci = p->pci;

	/* rockchip: LTSSM off, then on. No reset GPIO on this port (DTB),
	 * so start_link does not touch PERST#.
	 */
	if (pci->ops && pci->ops->stop_link)
		pci->ops->stop_link(pci);
	speed_change(pci);
	if (pci->ops && pci->ops->start_link)
		pci->ops->start_link(pci);
	p->retrained = jiffies;
}

static void port_poll(struct port *p)
{
	bool up = dw_pcie_link_up(p->pci);
	int ret;

	if (p->up) {
		if (up)
			return;
		p->up = false;
		p->down_since = jiffies;
		p->up_since = 0;
		dev_warn(&p->nvme->dev, "link down, restarting it\n");
		retrain(p);
		return;
	}
	if (!up) {
		p->up_since = 0;
		if (time_after(jiffies, p->retrained + msecs_to_jiffies(retrain_ms)))
			retrain(p);
		return;
	}
	/* The link can come up for a moment while the device is still held
	 * in reset (PERST#, a hot reset): a reset then finds no device and
	 * nvme gives the controller up for good. Wait until the link stayed
	 * up and the device answers config reads.
	 */
	if (!p->up_since)
		p->up_since = jiffies | 1;
	if (time_before(jiffies, p->up_since + msecs_to_jiffies(stable_ms)) ||
	    !pci_device_is_present(p->nvme))
		return;
	dev_info(&p->nvme->dev, "link up after %u ms, resetting the NVMe\n",
		 jiffies_to_msecs(jiffies - p->down_since));
	pci_load_saved_state(p->rp, p->rp_saved);
	pci_restore_state(p->rp);
	/* Target speed (LNKCTL2) is back: change up before the reset. */
	speed_change(p->pci);
	msleep(100);
	pci_load_saved_state(p->nvme, p->nvme_saved);
	pci_restore_state(p->nvme);
	ret = pci_reset_function(p->nvme);
	recoveries++;
	if (ret)
		dev_err(&p->nvme->dev, "reset failed: %d\n", ret);
	else
		dev_info(&p->nvme->dev, "reset done\n");
	p->up = true;
}

static int recover_thread(void *data)
{
	unsigned int i;

	while (!kthread_should_stop()) {
		for (i = 0; i < nports; i++)
			port_poll(&ports[i]);
		msleep_interruptible(poll_ms);
	}
	return 0;
}

/* The NVMe sits right below a Root Port of a rockchip-dw-pcie host. */
static struct dw_pcie *nvme_host(struct pci_dev *nvme)
{
	struct pci_dev *rp = pci_upstream_bridge(nvme);
	struct pci_host_bridge *bridge;
	struct device *host;

	if (!rp || pci_pcie_type(rp) != PCI_EXP_TYPE_ROOT_PORT)
		return NULL;
	bridge = pci_find_host_bridge(nvme->bus);
	host = bridge ? bridge->dev.parent : NULL;
	if (!host || !host->driver || strcmp(host->driver->name, HOST_DRIVER))
		return NULL;
	return to_dw_pcie_from_pp((struct dw_pcie_rp *)rp->bus->sysdata);
}

static void put_ports(void)
{
	while (nports) {
		nports--;
		kfree(ports[nports].rp_saved);
		kfree(ports[nports].nvme_saved);
		pci_dev_put(ports[nports].nvme);
	}
}

static int __init recover_init(void)
{
	struct pci_dev *nvme = NULL;

	while (nports < MAX_PORTS &&
	       (nvme = pci_get_class(PCI_CLASS_STORAGE_EXPRESS, nvme))) {
		struct dw_pcie *pci = nvme_host(nvme);
		struct port *p;

		if (!pci)
			continue;
		p = &ports[nports];
		p->rp = pci_upstream_bridge(nvme);
		pci_save_state(p->rp);
		p->rp_saved = pci_store_saved_state(p->rp);
		pci_save_state(nvme);
		p->nvme_saved = pci_store_saved_state(nvme);
		if (!p->rp_saved || !p->nvme_saved) {
			kfree(p->rp_saved);
			kfree(p->nvme_saved);
			continue;
		}
		/* Holding the NVMe holds its Root Port too. */
		p->nvme = pci_dev_get(nvme);
		p->pci = pci;
		p->up = dw_pcie_link_up(pci);
		dev_info(&nvme->dev, "watching the link (%s)\n",
			 p->up ? "up" : "down");
		nports++;
	}
	pci_dev_put(nvme);
	if (!nports)
		return -ENODEV;

	task = kthread_run(recover_thread, NULL, "nvme-recover");
	if (IS_ERR(task)) {
		put_ports();
		return PTR_ERR(task);
	}
	return 0;
}

static void __exit recover_exit(void)
{
	kthread_stop(task);
	put_ports();
}

/* Loaded with nvme, by class. */
static const struct pci_device_id recover_ids[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) },
	{ }
};
MODULE_DEVICE_TABLE(pci, recover_ids);

module_init(recover_init);
module_exit(recover_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mixtile Blade 3 NVMe recovery after a Cluster Box PERST#");
