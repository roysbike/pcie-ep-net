#!/bin/sh
# Sample CPU load for SECS seconds: total busy %, the openmiop kernel
# threads, and the interrupt count delta. Needs only /proc.
#
#   cpu-sample.sh 10
SECS=${1:-10}
HZ=$(getconf CLK_TCK)

cpu_line() { awk '/^cpu /{print $2+$3+$4+$6+$7+$8, $2+$3+$4+$5+$6+$7+$8}' /proc/stat; }
thr_ticks() {
	for p in $(pgrep openmiop); do
		awk -v n="$(cat /proc/$p/comm)" '{print n, $14+$15}' /proc/$p/stat
	done
}
irq_total() { awk 'NR>1{for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; else break} END{print s}' /proc/interrupts; }

set -- $(cpu_line); b0=$1; t0=$2
th0=$(thr_ticks)
i0=$(irq_total)
sleep "$SECS"
set -- $(cpu_line); b1=$1; t1=$2
th1=$(thr_ticks)
i1=$(irq_total)

awk -v b=$((b1 - b0)) -v t=$((t1 - t0)) -v n="$(nproc)" \
	'BEGIN{printf "cpu busy: %.1f%% of %d cores (%.2f cores)\n", 100*b/t, n, n*b/t}'
echo "$th0" | while read -r name t; do
	[ -n "$name" ] || continue
	e=$(echo "$th1" | awk -v n="$name" '$1==n{print $2}')
	awk -v n="$name" -v d=$((e - t)) -v s="$SECS" -v hz="$HZ" \
		'BEGIN{printf "  %-12s %.1f%% of one core\n", n, 100*d/(s*hz)}'
done
echo "interrupts/s: $(( (i1 - i0) / SECS ))"
