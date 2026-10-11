#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, the check of S1's fix: for each node of one job, the
# figures the check probe swept on the node's first PE (its job output's node
# lines) beside the same figures computed offline from its PEs' logs by the S1
# refuter's analyze.py, copied here unchanged - exact (peers_in_calls) and
# exact_q (peers_in_calls_with_quiets) from its time_average - and the quiets'
# share of the PEs' time and the window, computed here the way the probe
# defines them; and, from the same logs, the figure the kit printed before
# (peers_at_once, analyze.py's kit_exact) for comparison.
# usage: compare.py LOGDIR JOB_OUTPUT
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze  # noqa: E402

logdir, output = sys.argv[1], sys.argv[2]
printed = {}
for line in open(output, errors="replace"):
    m = re.match(r"multistream node=(\d+) pes=(\d+) mib_per_s=([0-9.]+).*", line)
    if m and "peer_mib=" not in line:
        fields = dict(re.findall(r"(\w+)=([0-9.]+)", line))
        printed[int(m.group(1))] = fields
pes = []
for path in glob.glob(os.path.join(logdir, "pe-*.txt")):
    with open(path) as f:
        head = f.readline().split()
        h = dict(zip(head[0::2], head[1::2]))
        blocks = [tuple(float(x) for x in line.split()[:3]) + (int(line.split()[3]),) for line in f]
    pes.append({"pe": int(h["pe"]), "node": int(h["node"]), "start": float(h["start"]),
                "slice": float(h["slice"]), "nodes": int(h["nodes"]), "blocks": blocks,
                "dropped": int(h["dropped"])})
if not pes:
    sys.exit(f"no logs in {logdir}")
nodes = pes[0]["nodes"]
print(f"{'node':>4} {'pes':>3} {'blocks':>7}  {'in_calls':>8} {'exact':>8} {'diff':>8}  "
      f"{'w_quiets':>8} {'exact_q':>8} {'diff':>8}  {'q_share':>7} {'offline':>7}  "
      f"{'window':>7} {'offline':>7}  {'old_kit':>7}")
worst = 0.0
for n in range(nodes):
    mine = [pe for pe in pes if pe["node"] == n]
    if not mine:
        continue
    calls = analyze.time_average(mine, n, nodes)
    quiets = analyze.time_average(mine, n, nodes, with_quiet=True)
    lo = max(pe["start"] for pe in mine)
    hi = min(max(e for b, e, a, peer in pe["blocks"]) for pe in mine)
    hi_q = min(max(a for b, e, a, peer in pe["blocks"]) for pe in mine)
    quiet = 0.0
    for pe in mine:
        for b, e, a, peer in pe["blocks"]:
            q0, q1 = max(e, lo), min(a, hi_q)
            if q1 > q0:
                quiet += q1 - q0
    share = quiet / (len(mine) * (hi_q - lo))
    old = analyze.kit_exact(mine)
    p = printed.get(n, {})
    got = [float(p.get(k, "nan")) for k in ("peers_in_calls", "peers_in_calls_with_quiets",
                                             "quiet_share", "peers_window_s")]
    d1, d2 = got[0] - calls["exact"], got[1] - quiets["exact"]
    worst = max(worst, abs(d1), abs(d2), abs(got[2] - share))
    print(f"{n:4d} {len(mine):3d} {sum(len(pe['blocks']) for pe in mine):7d}  {got[0]:8.3f} "
          f"{calls['exact']:8.4f} {d1:+8.4f}  {got[1]:8.3f} {quiets['exact']:8.4f} {d2:+8.4f}  "
          f"{got[2]:7.3f} {share:7.4f}  {got[3]:7.3f} {hi - lo:7.4f}  {old:7.2f}"
          + ("  ROOM FULL" if any(pe["dropped"] for pe in mine) else ""))
print(f"largest difference, probe against offline: {worst:.4f} (the probe prints three decimals)")
