// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * blade3-leds: activity LEDs for the RTL8125 2.5GbE ports of the Mixtile
 * Blade 3.
 *
 * The RTL8125 LED selectors come out of reset as "LEDn: activity at one
 * link speed" (LED0 10M, LED1 100M, LED2 1000M, LED3 2500M), and the
 * Blade 3 jacks are wired to LED0-LED2, so at 2.5G the port stays dark.
 * r8169 can program them (CONFIG_R8169_LEDS), but the Talos kernel is
 * built without LEDS_TRIGGER_NETDEV, which that option needs.
 *
 * This module writes the selectors of every RTL8125 port (default: on
 * at any link speed, blink on activity) and writes them again whenever
 * the port comes up or its link changes, because r8169 resets the chip
 * then. It does not bind to the device; r8169 stays its driver.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>

#define RTL_VENDOR	0x10ec
#define RTL8125_ID	0x8125
#define RTL_MMIO_BAR	2
#define LED_SEL_MASK	0x023f		/* LEDSEL_MASK_8125 in r8169 */

/* LEDSEL0, LEDSEL1, LEDSEL2, LEDSEL3 (16 bit, as in r8169). */
static const u16 led_reg[] = { 0x18, 0x86, 0x84, 0x96 };

/* ACT (bit 9) + link 2500/1000/100/10 (bits 5, 3, 1, 0). */
static ushort mode = 0x022b;
module_param(mode, ushort, 0444);
MODULE_PARM_DESC(mode, "LED selector for all four LEDs (default 0x022b: link at any speed, blink on activity)");

static struct pci_dev *rtl_pci(struct net_device *ndev)
{
	struct device *d = ndev->dev.parent;
	struct pci_dev *pdev;

	if (!d || !dev_is_pci(d))
		return NULL;
	pdev = to_pci_dev(d);
	if (pdev->vendor != RTL_VENDOR || pdev->device != RTL8125_ID)
		return NULL;
	return pdev;
}

static void leds_apply(struct net_device *ndev)
{
	struct pci_dev *pdev = rtl_pci(ndev);
	void __iomem *mmio;
	unsigned int i;

	if (!pdev || pci_resource_len(pdev, RTL_MMIO_BAR) < 0x100 ||
	    !(pci_resource_flags(pdev, RTL_MMIO_BAR) & IORESOURCE_MEM))
		return;
	/* r8169 owns the BAR; a second mapping of these few registers is
	 * fine, the LED selectors are not used by its datapath.
	 */
	mmio = ioremap(pci_resource_start(pdev, RTL_MMIO_BAR), 0x100);
	if (!mmio)
		return;
	for (i = 0; i < ARRAY_SIZE(led_reg); i++) {
		u16 v = readw(mmio + led_reg[i]);

		writew((v & ~LED_SEL_MASK) | (mode & LED_SEL_MASK), mmio + led_reg[i]);
	}
	iounmap(mmio);
}

static int leds_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);

	switch (event) {
	case NETDEV_REGISTER:
	case NETDEV_UP:
	case NETDEV_CHANGE:
		leds_apply(ndev);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block leds_nb = {
	.notifier_call = leds_event,
};

static int __init leds_init(void)
{
	/* Replays NETDEV_REGISTER (and NETDEV_UP for running devices) for
	 * the ports that exist already.
	 */
	return register_netdevice_notifier(&leds_nb);
}

static void __exit leds_exit(void)
{
	unregister_netdevice_notifier(&leds_nb);
}

/* Loaded with r8169 when an RTL8125 shows up; never binds. */
static const struct pci_device_id leds_ids[] = {
	{ PCI_DEVICE(RTL_VENDOR, RTL8125_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, leds_ids);

module_init(leds_init);
module_exit(leds_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mixtile Blade 3 RTL8125 activity LEDs");
