#!/bin/sh
# Connectivity matrix over omi0. Run from the workstation.
#
#   scripts/matrix-test.sh
#
# Nodes with a shell (SSH aliases) send; every member is a target.
# Talos nodes have no shell: they are tested as targets (ICMP, and a TCP
# connection to apid on port 50000 over the fabric address).
#
#   SOURCES="clusterbox:10.20.0.1 blade160:10.20.0.160 blade173:10.20.0.173"
#   TALOS="10.20.0.201 10.20.0.204"
set -u

SOURCES=${SOURCES-"clusterbox:10.20.0.1 blade160:10.20.0.160 blade173:10.20.0.173"}
TALOS=${TALOS-"10.20.0.201 10.20.0.204"}
TARGETS=""
for s in $SOURCES; do TARGETS="$TARGETS ${s#*:}"; done
TARGETS="$TARGETS $TALOS"

fail=0
printf '%-12s %-14s %-10s %-10s %s\n' from to ping jumbo rtt
for s in $SOURCES; do
	host=${s%%:*}
	src=${s#*:}
	for t in $TARGETS; do
		[ "$t" = "$src" ] && continue
		# The BMC runs busybox ping: no -M, -s limited by its MTU.
		if [ "$host" = clusterbox ]; then
			out=$(ssh "$host" "sudo -n ping -c 5 -W 1 -q $t 2>&1 | tail -2")
		else
			out=$(ssh "$host" "sudo -n ping -c 5 -i 0.2 -W 1 -q $t 2>&1 | tail -2")
		fi
		loss=$(echo "$out" | grep -o '[0-9.]*% packet loss' | cut -d% -f1)
		rtt=$(echo "$out" | sed -n 's/.*= [0-9.]*\/\([0-9.]*\)\/.*/\1/p')
		if [ "$host" = clusterbox ]; then
			jout=$(ssh "$host" "sudo -n ping -c 3 -W 1 -s 8000 -q $t 2>&1 | tail -2")
		else
			jout=$(ssh "$host" "sudo -n ping -c 3 -i 0.2 -W 1 -M do -s 8972 -q $t 2>&1 | tail -2")
		fi
		jloss=$(echo "$jout" | grep -o '[0-9.]*% packet loss' | cut -d% -f1)
		[ "${loss:-100}" = 0 ] || fail=1
		[ "${jloss:-100}" = 0 ] || fail=1
		printf '%-12s %-14s %-10s %-10s %s\n' "$host" "$t" \
			"${loss:-?}%" "${jloss:-?}%" "${rtt:-?} ms"
	done
done

# TCP to apid on the Talos nodes, from every blade with a shell (the
# workstation is not on the fabric).
for s in $SOURCES; do
	host=${s%%:*}
	[ "$host" = clusterbox ] && continue
	for t in $TALOS; do
		printf 'tcp %-12s -> %s:50000 ' "$host" "$t"
		if ssh "$host" "timeout 3 bash -c '</dev/tcp/$t/50000'" 2>/dev/null; then
			echo ok
		else
			echo FAIL
			fail=1
		fi
	done
done
exit $fail
