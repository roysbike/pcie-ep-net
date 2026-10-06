#!/bin/sh
# Reproducible omi0 benchmark between two blades. Run from the
# workstation; uses SSH aliases. Writes a report to stdout:
#
#   scripts/bench.sh [RUNS] [SECONDS] > docs/bench/<name>.txt
#
# Every test runs RUNS times. For each run it records throughput,
# retransmits, per-core CPU on both ends (mpstat), interrupt and
# context-switch rates, softirq time, and the driver counters that
# changed. Needs iperf3, sysstat and ethtool on the blades.
set -u

A=${A:-blade160}
B=${B:-blade173}
A_IP=${A_IP:-10.20.0.160}
B_IP=${B_IP:-10.20.0.173}
GW_IP=${GW_IP:-10.20.0.1}
RC=${RC:-clusterbox}
RUNS=${1:-3}
T=${2:-10}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

on() { ssh "$1" "$2"; }

echo "# omi0 benchmark"
echo "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "commit:      $(git -C "$(dirname "$0")/.." describe --always --dirty)"
for h in "$A" "$B"; do
	echo "$h: kernel $(on "$h" 'uname -r'), module $(on "$h" 'sha256sum /tmp/openmiop-ep.ko 2>/dev/null | cut -c1-16'), mtu $(on "$h" 'cat /sys/class/net/omi0/mtu'), $(on "$h" 'sudo -n ethtool -i omi0 | grep firmware')"
done
on "$RC" 'for d in /sys/bus/pci/devices/*; do
	[ "$(cat $d/device)" = 0x4f4d ] || continue
	echo "link ${d##*/}: $(sudo -n lspci -s ${d##*/} -vv 2>/dev/null | grep -E "LnkSta:" | sed "s/^\s*//"), $(sudo -n lspci -s ${d##*/} -vv 2>/dev/null | grep -E "^\s+MaxPayload" | sed "s/^\s*//")"
done'
echo "runs: $RUNS x ${T}s"
echo

for h in "$A" "$B"; do
	on "$h" "pgrep -x iperf3 >/dev/null || iperf3 -s -D -B \$(ip -4 -br addr show omi0 | awk '{print \$3}' | cut -d/ -f1)"
done

# snapshot HOST FILE: driver counters, interrupts, ctxt, softirq ticks
snap() {
	on "$1" "sudo -n ethtool -S omi0 | tail -n +2;
		awk '/^ctxt/{print \"ctxt:\", \$2} /^intr/{print \"intr:\", \$2}' /proc/stat;
		awk '/^cpu /{print \"softirq_ticks:\", \$8}' /proc/stat;
		nstat -az TcpRetransSegs TcpExtTCPSACKReorder TcpExtTCPOFOQueue UdpRcvbufErrors 2>/dev/null | tail -n +2 | awk '{print \$1\":\", \$2}'" >"$2"
}

delta() { # before after seconds
	awk -v s="$3" 'NR==FNR{gsub(":","",$1); a[$1]=$2; next}
		{gsub(":","",$1); d=$2-a[$1]; if (d==0) next;
		 if ($1=="intr"||$1=="ctxt") printf "%s/s=%d ", $1, d/s;
		 else if ($1=="softirq_ticks") printf "softirq=%.1f%%core ", d/s;
		 else printf "%s=%d ", $1, d}' "$1" "$2"
	echo
}

run() { # label, client host, client args
	label=$1; shift
	n=1
	while [ "$n" -le "$RUNS" ]; do
		snap "$A" "$TMP/a0"; snap "$B" "$TMP/b0"
		on "$A" "mpstat -P ALL $((T - 2)) 1 | awk '/^Average/ && \$2 ~ /^[0-9]+$/ {printf \"c%s:%.0f \", \$2, 100-\$NF}'" >"$TMP/ma" &
		on "$B" "mpstat -P ALL $((T - 2)) 1 | awk '/^Average/ && \$2 ~ /^[0-9]+$/ {printf \"c%s:%.0f \", \$2, 100-\$NF}'" >"$TMP/mb" &
		on "$A" "iperf3 -c $B_IP -B $A_IP -t $T $* 2>&1" |
			grep -E "(sender|receiver)$" >"$TMP/ip"
		grep -q SUM "$TMP/ip" && grep SUM "$TMP/ip" >"$TMP/ip2" || cp "$TMP/ip" "$TMP/ip2"
		res=$(sed -E 's/^\[ *[0-9]+\]//; s/^\[SUM\]//; s/ +0\.00-[0-9.]+ +sec +[0-9.]+ [KMG]?Bytes +/ /; s/ +/ /g' "$TMP/ip2" | tr '\n' ';')
		wait
		snap "$A" "$TMP/a1"; snap "$B" "$TMP/b1"
		echo "## $label run $n: $res"
		echo "   $A cpu: $(cat "$TMP/ma")"
		echo "   $B cpu: $(cat "$TMP/mb")"
		echo "   $A: $(delta "$TMP/a0" "$TMP/a1" "$T")"
		echo "   $B: $(delta "$TMP/b0" "$TMP/b1" "$T")"
		n=$((n + 1))
	done
}

echo "## idle (link up, no traffic)"
on "$A" "mpstat 10 1 | awk '/^Average/{printf \"$A busy %.1f%% of 8 cores\n\", 100-\$NF}'; cat /proc/loadavg" &
on "$B" "mpstat 10 1 | awk '/^Average/{printf \"$B busy %.1f%% of 8 cores\n\", 100-\$NF}'; cat /proc/loadavg" &
snap "$A" "$TMP/a0"; sleep 10; snap "$A" "$TMP/a1"
wait
echo "   $A idle: $(delta "$TMP/a0" "$TMP/a1" 10)"
echo

echo "## latency"
echo "   peer, 1 s interval (after idle): $(on "$A" "sudo -n ping -c 10 -i 1 -q $B_IP | tail -1")"
echo "   peer, 5 ms interval:             $(on "$A" "sudo -n ping -c 400 -i 0.005 -q $B_IP | tail -1")"
echo "   peer, flood 64 B:                $(on "$A" "sudo -n ping -f -c 20000 -q $B_IP | tail -1")"
echo "   gateway, 50 ms interval:         $(on "$A" "sudo -n ping -c 40 -i 0.05 -q $GW_IP | tail -1")"
echo

run "TCP $A->$B P1" ""
run "TCP $B->$A P1 (-R)" "-R"
run "TCP bidir P1" "--bidir"
run "TCP $A->$B P4" "-P 4"
run "TCP $A->$B P8" "-P 8"
run "TCP $B->$A P4 (-R)" "-R -P 4"
run "TCP bidir P4" "--bidir -P 4"
run "TCP $A->$B P1 MSS 1400" "-M 1400"
run "UDP $A->$B 3G 8900B" "-u -b 3G -l 8900"
run "UDP $A->$B 1G 1400B" "-u -b 1G -l 1400"
run "UDP $A->$B 200M 64B" "-u -b 200M -l 64"

echo
echo "## kernel log (openmiop, warnings)"
for h in "$A" "$B"; do
	echo "$h: $(on "$h" "sudo -n dmesg | grep -iE 'openmiop|omi0' | grep -iE 'err|fail|warn|timeout|stall|not ' | tail -3")"
	echo "$h: $(on "$h" "sudo -n dmesg | grep -iE 'WARNING|Oops|BUG|Call trace' | tail -3")"
done
