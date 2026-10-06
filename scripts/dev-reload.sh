#!/bin/sh
# Reload a development build of openmiop-ep on the blades and, if
# given, a new Cluster Box helper. Run from the workstation; uses SSH
# aliases only.
#
#   scripts/dev-reload.sh path/to/openmiop-ep.ko [path/to/openmiop-rc]
#
# The helper is stopped while the blades reload: an RC MMIO read in
# flight while an EP drops its link can wedge the MT7620A. Probe
# resets the EP config space; the helper restores BAR0, the command
# register and MPS when it starts again.
set -eu

RC=${RC:-clusterbox}
BLADES=${BLADES:-"blade160 blade173"}
KO=${1:?usage: $0 openmiop-ep.ko [openmiop-rc]}
HELPER=${2:-}

ssh "$RC" 'sudo -n /etc/init.d/openmiop stop >/dev/null 2>&1 || true'

for b in $BLADES; do
	scp -q "$KO" "$b:/tmp/openmiop-ep.ko"
	ssh "$b" "set -e
		A=\$(cat /etc/openmiop.addr)
		if lsmod | grep -q '^openmiop_ep'; then
			sudo -n ip link set omi0 down || true
			sudo -n rmmod openmiop_ep
		fi
		sudo -n insmod /tmp/openmiop-ep.ko
		sudo -n ip link set omi0 mtu 9000
		sudo -n ip link set omi0 up
		sudo -n ip addr replace \"\$A\" dev omi0
		echo \"\$(hostname -I | cut -d' ' -f1): omi0 \$A\""
done

if [ -n "$HELPER" ]; then
	ssh "$RC" "cat > /tmp/openmiop-rc" <"$HELPER"
	ssh "$RC" 'sudo -n cp /tmp/openmiop-rc /usr/bin/openmiop-rc && sudo -n chmod 755 /usr/bin/openmiop-rc'
fi
ssh "$RC" 'sudo -n /etc/init.d/openmiop start </dev/null >/dev/null 2>&1 &
	sleep 4
	ip -br addr show omi0'
