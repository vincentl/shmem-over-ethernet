# SPDX-License-Identifier: LGPL-3.0-or-later
# The runbook's cases (docs/fast-link.md, §5): eight and five nodes at 4, 8 and
# 16 PEs a node, multistream's old order and its present one ("c"), three seeds,
# three decimals; and four helpers' busy count beside each.
from model import run
for N in (8, 5):
    for P in (4, 8, 16):
        blocks = 3200 // P
        for order in ("multistream", "c"):
            got = [run(N, P, order, 0.1, blocks, s) for s in (1, 2, 3)]
            print(f"N={N} P={P:2d} order={order:11s} peers={[round(g[0], 3) for g in got]} "
                  f"mean={sum(g[0] for g in got)/3:.2f} h4={[round(g[2], 3) for g in got]} "
                  f"h4 mean={sum(g[2] for g in got)/3:.2f} share of 4={sum(g[2] for g in got)/12:.2f}", flush=True)
