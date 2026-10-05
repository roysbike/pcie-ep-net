#!/bin/sh
# Run on the Cluster Box (OpenWrt, as root) after the Blade EPF starts.
# Prints topology, the four downstream ports, and any Type-0 function.

set -eu

rescan=${1:-}

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root (sudo $0)" >&2
	exit 1
fi

if [ "$rescan" = "--rescan" ]; then
	echo 1 > /sys/bus/pci/rescan
	sleep 1
fi

echo "===== tree ====="
lspci -tvnn || lspci -tv

echo
echo "===== devices ====="
lspci -nn

echo
echo "===== downstream ports ====="
for dev in 02:00.0 02:04.0 02:08.0 02:0c.0; do
	echo "---- $dev ----"
	lspci -nnvv -s "$dev" | grep -E 'LnkCap:|LnkSta:|Memory behind|Bus:|ACS|Access Control' || true
done

echo
echo "===== endpoints (buses 03-06) ====="
found=0
for dev in 03:00.0 04:00.0 05:00.0 06:00.0; do
	if lspci -s "$dev" >/dev/null 2>&1; then
		found=1
		echo "---- $dev ----"
		lspci -nnvv -s "$dev"
	fi
done

if [ "$found" -eq 0 ]; then
	echo "no Type-0 function on buses 03-06"
	echo "blade EPC not started, U-Boot stole the node, or rescan needed"
	exit 2
fi

echo
echo "===== expected Phase 1 IDs ====="
echo "want vendor 1d87 device 3588 (pci_epf_test / RK3588)"
echo "4586:b6f2 would be proprietary MIOP — wrong for this phase"
lspci -nn | grep -E '1d87|3588|4586|b6f2' || true
