#!/bin/sh
# Bind pci_epf_test to the RK3588 PCIe3 x4 Endpoint controller.
# Run as root on the Ubuntu Blade. Does not implement a netdev.
#
# IDs are the upstream pci_endpoint_test pair for RK3588, *not*
# Mixtile 4586:b6f2 (that would attach proprietary miop.ko on the RC).

set -eu

CFG=/sys/kernel/config
EPC_CLASS=/sys/class/pci_epc
FUNC=functions/pci_epf_test/func1

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

if [ ! -d "$EPC_CLASS" ] || [ -z "$(ls -A "$EPC_CLASS" 2>/dev/null)" ]; then
	echo "no PCI EPC in $EPC_CLASS" >&2
	echo "pcie3x4_ep did not probe; check DTB and U-Boot compatible" >&2
	dmesg | grep -iE 'pcie|pci_ep' | tail -30 || true
	exit 1
fi

EPC_NAME=$(ls "$EPC_CLASS" | head -n 1)
echo "using EPC $EPC_NAME"

if ! grep -qw pci_epf_test /proc/modules; then
	modprobe pci_epf_test
fi

if ! mountpoint -q "$CFG"; then
	mount -t configfs none "$CFG"
fi

cd "$CFG/pci_ep"

if [ ! -d "$FUNC" ]; then
	mkdir "$FUNC"
fi

# 0x1d87 / 0x3588: drivers/misc/pci_endpoint_test.c RK3588 entry.
printf '%s\n' 0x1d87 > "$FUNC/vendorid"
printf '%s\n' 0x3588 > "$FUNC/deviceid"
printf '%s\n' 0xff   > "$FUNC/baseclass_code"
printf '%s\n' 8      > "$FUNC/msi_interrupts"

# Keep BARs small so the MT7620A 256 MiB window can assign them.
# pci_epf_test defaults are already 128 KiB / 1 MiB; only override
# if the function created the per-BAR files.
if [ -d "$FUNC/pci_epf_test.0" ]; then
	for bar in 0 1 2 3 5; do
		f="$FUNC/pci_epf_test.0/bar${bar}_size"
		[ -w "$f" ] && printf '%s\n' 131072 > "$f"
	done
	# BAR4 is reserved on RK3588; leave whatever the EPF did.
fi

CTRL="controllers/$EPC_NAME"
if [ ! -e "$CTRL/func1" ] && [ ! -L "$CTRL/func1" ]; then
	ln -s "../../$FUNC" "$CTRL/func1"
fi

# Start LTSSM. Safe to write 1 again if already started.
printf '1\n' > "$CTRL/start"

echo "pci_epf_test started on $EPC_NAME (1d87:3588)"
echo "on the Cluster Box: echo 1 > /sys/bus/pci/rescan && lspci -tvnn"
dmesg | grep -iE 'pcie|pci_epf|ltssm|link' | tail -20 || true
