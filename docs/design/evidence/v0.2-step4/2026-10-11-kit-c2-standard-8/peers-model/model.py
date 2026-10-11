# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit's peers-in-use model: the first adversarial read's Appendix
# (docs/reviews/archive/v0.2-fast-link-kit-adversarial-read.md, "the model
# behind finding 1") with the second read's additions (docs/reviews/archive/
# v0.2-fast-link-kit-second-adversarial-read.md, "Appendix: the lists and the
# model"): the order "c" (multistream.c's target lists, rebuilt from the C's
# steps), node 0's blocks to each peer, and the helpers busy at H = 2 and 4.
# Rebuilt from the two Appendices on 2026-10-11 for the kit's 4, 8 and 16 PEs a
# node on c2-standard-8; verify.py reproduces the second read's table, every
# seed (verify.out), and rerun3.py gives the runbook's figures (rerun3.out).
import random, sys

def c_targets(firsts, ranks, me):
    npes = len(firsts)
    node_first = [p for p in range(npes) if firsts[p] == p]
    nodes_count = len(node_first)
    node_of, node_size = [None] * npes, [0] * npes
    for p in range(npes):
        for n in range(nodes_count):
            if node_first[n] == firsts[p]:
                node_of[p] = n
                node_size[n] += 1
    largest, mine, my_rank = max(node_size[:nodes_count]), node_of[me], ranks[me]
    targets = []
    if nodes_count > 1:
        for i in range((nodes_count - 1) * largest):
            n = (mine + 1 + (my_rank + i) % (nodes_count - 1)) % nodes_count
            rank = (my_rank + i // (nodes_count - 1)) % node_size[n]
            targets.append(next(p for p in range(npes) if node_of[p] == n and ranks[p] == rank))
    return targets, node_of, nodes_count

def run(N, P, order, jitter, blocks_each, seed):
    rnd = random.Random(seed); npes = N * P; node = lambda pe: pe // P; lists = []
    firsts = [(p // P) * P for p in range(npes)]
    ranks = [p % P for p in range(npes)]
    for me in range(npes):
        t = [(me + k) % npes for k in range(1, npes) if node((me + k) % npes) != node(me)]
        if order == "interleaved":
            local = me % P; peers = [(node(me) + 1 + j) % N for j in range(N - 1)]
            t = [peers[(j + local) % (N - 1)] * P + (r + local) % P for r in range(P) for j in range(N - 1)]
        elif order == "c":
            t, _, _ = c_targets(firsts, ranks, me)
        lists.append([node(x) for x in t])
    pos = [0] * npes; left = [1 + rnd.uniform(-jitter, jitter) for _ in range(npes)]
    done = [0] * npes; area = span = 0.0
    helpers_area = {2: 0.0, 4: 0.0}
    to_peer = [0.0] * N
    while min(done) < blocks_each:
        wire = {}
        for pe in range(npes):
            wire.setdefault((node(pe), lists[pe][pos[pe] % len(lists[pe])]), []).append(pe)
        rate = {pe: 1.0 / len(pes) for pes in wire.values() for pe in pes}
        dt = min(left[pe] / rate[pe] for pe in range(npes))
        busy = {w[1] for w in wire if w[0] == 0}
        area += len(busy) * dt; span += dt
        for h in (2, 4):
            helpers_area[h] += len({1 + (p - 1) % h for p in busy}) * dt   # node 0: slot p - 1
        for pe in range(npes):
            moved = rate[pe] * dt
            if node(pe) == 0:
                to_peer[lists[pe][pos[pe] % len(lists[pe])]] += moved
            left[pe] -= moved
            if left[pe] <= 1e-12:
                pos[pe] += 1; done[pe] += 1; left[pe] = 1 + rnd.uniform(-jitter, jitter)
    peers_bytes = [x for i, x in enumerate(to_peer) if i != 0]
    least_over_most = min(peers_bytes) / max(peers_bytes) if peers_bytes and max(peers_bytes) else 0
    return area / span, helpers_area[2] / span, helpers_area[4] / span, least_over_most

if __name__ == "__main__":
    pass
