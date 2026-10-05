#!/bin/sh
# Reload a development build of openmiop-ep on both blades and bring
# the Cluster Box helper back. Run from the workstation; uses SSH
# aliases only.
#
#   scripts/dev-reload.sh path/to/openmiop-ep.ko [module args...]
#
# The order matters:
#   1. Stop openmiop-rc first. An RC MMIO read in flight while the EP
#      drops LTSSM can wedge the MT7620 config path (see hw_stop()).
#   2. Unload / load on each blade. Probe asserts the controller
#      reset, which clears the EP's BAR0 register and PCI_COMMAND.
#   3. On the RC, write the kernel-assigned BAR0 address back and set
#      Memory Space. Writing sysfs "enable" does not do this: the
#      enable count is already non-zero. CMD_BITS=0x0006 also sets
#      Bus Master; the baseline runs without it.
#   4. Start openmiop-rc again.
set -eu

CMD_BITS=${CMD_BITS:-0x0002}
RC=${RC:-clusterbox}
BLADES=${BLADES:-"blade160 blade173"}
KO=${1:?usage: $0 openmiop-ep.ko [args]}
shift
ARGS="$*"

ssh "$RC" 'sudo -n /etc/init.d/openmiop stop >/dev/null 2>&1 || true'

for b in $BLADES; do
	scp -q "$KO" "$b:/tmp/openmiop-ep.ko"
	ssh "$b" "set -e
		A=\$(cat /etc/openmiop.addr)
		if lsmod | grep -q '^openmiop_ep'; then
			sudo -n ip link set omi0 down || true
			sudo -n rmmod openmiop_ep
		fi
		sudo -n insmod /tmp/openmiop-ep.ko $ARGS
		sudo -n ip link set omi0 mtu 9000
		sudo -n ip link set omi0 up
		sudo -n ip addr replace \"\$A\" dev omi0
		echo \"\$(hostname -I | cut -d' ' -f1): omi0 \$A\""
done

ssh "$RC" 'set -e
	for d in /sys/bus/pci/devices/*; do
		[ "$(cat $d/vendor)" = 0x1d87 ] || continue
		[ "$(cat $d/device)" = 0x4f4d ] || continue
		bdf=${d##*/}
		# "0x0000000021000000": hi = chars 3-10, lo = 11-18.
		start=$(head -n 1 $d/resource | cut -d" " -f1)
		sudo -n setpci -s $bdf BASE_ADDRESS_0=$(echo $start | cut -c11-18)
		sudo -n setpci -s $bdf BASE_ADDRESS_1=$(echo $start | cut -c3-10)
		sudo -n setpci -s $bdf COMMAND='"$CMD_BITS:$CMD_BITS"'
		echo "$bdf BAR0=$(sudo -n setpci -s $bdf BASE_ADDRESS_0) CMD=$(sudo -n setpci -s $bdf COMMAND)"
	done
	sudo -n /etc/init.d/openmiop start </dev/null >/dev/null 2>&1 &
	sleep 6
	ip -br addr show omi0'
