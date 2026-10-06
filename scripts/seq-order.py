#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Count out-of-order TCP data segments per flow in tcpdump text.

  tcpdump -r cap.pcap -nn -tt -S | seq-order.py

A segment is "late" when it starts below the highest sequence number
already seen for its flow. On the sender this counts retransmits; on
the receiver it also counts reordering on the path.
"""
import re
import sys

LINE = re.compile(r"IP (\S+) > (\S+): .*?seq (\d+):(\d+)")

flows = {}
for line in sys.stdin:
    m = LINE.search(line)
    if not m:
        continue
    key = (m.group(1), m.group(2))
    start, end = int(m.group(3)), int(m.group(4))
    f = flows.setdefault(key, {"pkts": 0, "late": 0, "max": None, "gap": 0})
    f["pkts"] += 1
    if f["max"] is not None:
        if start < f["max"]:
            f["late"] += 1
        elif start > f["max"]:
            f["gap"] += 1
    if f["max"] is None or end > f["max"]:
        f["max"] = end

tot = {"pkts": 0, "late": 0, "gap": 0}
for (src, dst), f in sorted(flows.items()):
    print(f"{src} > {dst}: pkts={f['pkts']} late={f['late']} gaps={f['gap']}")
    for k in tot:
        tot[k] += f[k]
print(f"total: pkts={tot['pkts']} late={tot['late']} gaps={tot['gap']}")
