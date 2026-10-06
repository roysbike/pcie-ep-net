#!/bin/sh
# Standard omi0 test pass between two blades. Run from the workstation.
#
#   scripts/net-test.sh [SECONDS]
#
# Needs iperf3 on both blades and cpu-sample.sh in /tmp (copied here).
set -u

A=${A:-blade160}
B=${B:-blade173}
A_IP=${A_IP:-10.20.0.160}
B_IP=${B_IP:-10.20.0.173}
GW_IP=${GW_IP:-10.20.0.1}
T=${1:-10}
DIR=$(cd "$(dirname "$0")" && pwd)

for h in "$A" "$B"; do
	scp -q "$DIR/cpu-sample.sh" "$h:/tmp/cpu-sample.sh"
	ssh "$h" "pgrep -x iperf3 >/dev/null || iperf3 -s -D -B \$(ip -4 -br addr show omi0 | awk '{print \$3}' | cut -d/ -f1)"
done

echo "### ping"
ssh "$A" "sudo -n ping -c 200 -i 0.005 -q $B_IP | tail -2"
ssh "$A" "sudo -n ping -c 20 -i 0.05 -q $GW_IP | tail -2"
ssh "$A" "sudo -n ping -c 20 -i 0.05 -q -M do -s 8972 $B_IP | tail -2"

iperf() { # label, args
	echo "### $1"
	ssh "$A" "nstat -n"
	ssh "$B" "nstat -n"
	ssh "$A" "sh /tmp/cpu-sample.sh $((T - 2))" >/tmp/net-test.$$.a &
	ssh "$B" "sh /tmp/cpu-sample.sh $((T - 2))" >/tmp/net-test.$$.b &
	ssh "$A" "iperf3 -c $B_IP -B $A_IP -t $T $2" | grep -E "(sender|receiver)$" | grep -E "SUM|^\[ *[0-9]+\]" | tail -4
	wait
	printf '%s: ' "$A"; tr '\n' ' ' </tmp/net-test.$$.a; echo
	printf '%s: ' "$B"; tr '\n' ' ' </tmp/net-test.$$.b; echo
	for h in "$A" "$B"; do
		printf '%s nstat: ' "$h"
		ssh "$h" "nstat TcpRetransSegs TcpExtTCPSACKReorder TcpExtTCPDSACKRecv TcpExtTCPOFOQueue UdpRcvbufErrors 2>/dev/null | tail -n +2 | awk '{printf \"%s=%s \", \$1, \$2}'"
		echo
	done
}

iperf "TCP $A->$B" ""
iperf "TCP $B->$A" "-R"
iperf "TCP bidir" "--bidir"
iperf "TCP $A->$B P4" "-P 4"
iperf "TCP $A->$B P8" "-P 8"
iperf "TCP bidir P4" "--bidir -P 4"
iperf "UDP $A->$B 2G 8900B" "-u -b 2G -l 8900"
iperf "UDP $A->$B 1G 1400B" "-u -b 1G -l 1400"

echo "### counters"
for h in "$A" "$B"; do
	echo "$h:"; ssh "$h" "ip -s link show omi0 | tail -4"
	ssh "$h" "sudo -n dmesg | grep -i openmiop | grep -iE 'err|fail|timeout|not ' | tail -3"
done
rm -f /tmp/net-test.$$.*
