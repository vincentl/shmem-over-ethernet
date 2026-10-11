# SPDX-License-Identifier: LGPL-3.0-or-later
import time
from model import run
# The second read's table (seeds 1, 2, 3; 400 blocks a PE at 8 PEs a node, 200 at 16, 100 at 32; jitter 0.1)
expected = {
    (8, 8):  {"multistream": [2.02, 2.84, 2.14], "c": [4.19, 3.92, 4.33], "h4": [3.17, 3.05, 3.23]},
    (8, 16): {"multistream": [1.82, 1.91, 1.84], "c": [5.25, 5.37, 5.37], "h4": [3.61, 3.61, 3.62]},
    (8, 32): {"multistream": [1.46, 1.47, 1.32], "c": [6.43, 6.23, 6.41], "h4": [3.91, 3.85, 3.90]},
    (5, 8):  {"multistream": [1.75, 1.75, 2.11], "c": [2.72, 3.00, 2.67]},
    (5, 32): {"multistream": [1.22, 1.29, 1.24], "c": [3.76, 3.86, 3.63]},
}
for (N, P), exp in expected.items():
    blocks = 3200 // P
    for order, label in (("multistream", "multistream"), ("c", "c"), ("interleaved", "interleaved")):
        t0 = time.time()
        got = [run(N, P, order, 0.1, blocks, s) for s in (1, 2, 3)]
        peers = [round(g[0], 2) for g in got]
        h4 = [round(g[2], 2) for g in got]
        e = exp.get(label)
        print(f"N={N} P={P} blocks={blocks} order={label:12s} peers={peers} h4={h4} expected={e} {'OK' if e == peers else ''} ({time.time()-t0:.1f}s)", flush=True)
    if "h4" in exp:
        print("   expected h4:", exp["h4"])
