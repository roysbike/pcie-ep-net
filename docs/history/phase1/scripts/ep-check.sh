#!/bin/sh
# Read-only health check on the Ubuntu Blade before/after ep-start.

set -eu

echo "===== kernel ====="
uname -a

echo
echo "===== required config ====="
if [ -r /proc/config.gz ]; then
	zcat /proc/config.gz | grep -E 'PCIE_ROCKCHIP_DW_EP|PCI_ENDPOINT|PCI_EPF_TEST|PCI_ENDPOINT_CONFIGFS|PHY_ROCKCHIP_SNPS_PCIE3'
else
	echo "/proc/config.gz not available"
fi

echo
echo "===== EPC ====="
ls -l /sys/class/pci_epc 2>/dev/null || echo "no /sys/class/pci_epc"

echo
echo "===== live DT pcie nodes ====="
for n in pcie@fe150000 pcie-ep@fe150000 phy@fee80000; do
	base=/proc/device-tree/$n
	if [ -d "$base" ]; then
		echo "-- $n --"
		echo -n "compatible: "; tr '\0' ' ' < "$base/compatible"; echo
		if [ -f "$base/status" ]; then
			echo -n "status: "; tr '\0' ' ' < "$base/status"; echo
		fi
	else
		echo "-- $n -- MISSING"
	fi
done

echo
echo "===== dmesg pcie ====="
dmesg | grep -iE 'pcie|pci_ep|ltssm|miop' | tail -40 || true
