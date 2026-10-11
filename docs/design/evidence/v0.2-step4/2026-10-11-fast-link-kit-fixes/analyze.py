#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine, refutation of S1: from multistream-log's per-PE logs (one line a
# block: call start, call return, the time after its quiet, peer node), each
# node's
#   kit       the kit's peers_at_once recomputed (a bit a block, in the slot of
#             its call's return, from each PE's own start; the mean bit count
#             over the slots with any) - checked against the line the job
#             printed, so that the replica is known to be the kit's figure;
#   calls_ms  the investigator's peers_in_calls (a bit in every slot from the
#             call's start to its return);
#   exact     the time-averaged count of distinct peer nodes toward which some
#             PE of the node is inside a call, over the window in which every
#             PE of the node is in its loop - the model's quantity as near as
#             the program sees it (the kit's second read, Appendix);
#   exact_q   the same, a quiet counted toward the block before it;
#   busy      exact over the time at least one peer is busy (comparable to the
#             kit's mean over busy slots);
#   h2, h4    the time-averaged count of distinct helpers among those peers at
#             2 and 4 helpers (peer slot p, leaving the node out, is helper
#             1 + p mod H's: README, OSOE_PROGRESS_HELPERS);
#   per_ms    blocks a node's PEs return from a millisecond;
#   kit@L     the kit's figure with a slot of L ms in place of 1 ms: the same
#             pattern run 1/L times as fast reads, at the kit's 1 ms, what a
#             slot of L ms reads here;
#   even      the least peer's blocks over the most's (the kit's peer_mib).
# usage: analyze.py LOGDIR [JOB_OUTPUT]
import glob
import os
import re
import sys

SLICES = 16384


def popcount(x):
    return bin(x).count("1")


def kit_figure(pes, slot):
    """The kit's figure at a slot of SLOT seconds: a bit a block, in the slot
    of its call's return, each PE's own start the grid's origin; the window
    whole (the kit's SLICES bound does not cut a 4 s window at 1 ms)."""
    slots = {}
    for pe in pes:
        start = pe["start"]
        for b, e, a, peer in pe["blocks"]:
            s = int((e - start) / slot)
            slots[s] = slots.get(s, 0) | (1 << peer)
    busy = [v for v in slots.values() if v]
    return sum(popcount(v) for v in busy) / len(busy) if busy else 0.0


def kit_exact(pes):
    """The kit's figure exactly as the C computes it, SLICES-bounded."""
    slots = [0] * SLICES
    for pe in pes:
        start, slice_ = pe["start"], pe["slice"]
        for b, e, a, peer in pe["blocks"]:
            s = int((e - start) / slice_)
            if s < SLICES:
                slots[s] |= 1 << peer
    busy = [v for v in slots if v]
    return sum(popcount(v) for v in busy) / len(busy) if busy else 0.0


def calls_ms(pes):
    slots = [0] * SLICES
    for pe in pes:
        start, slice_ = pe["start"], pe["slice"]
        for b, e, a, peer in pe["blocks"]:
            s0 = int((b - start) / slice_)
            s1 = int((e - start) / slice_)
            for s in range(s0, min(s1, SLICES - 1) + 1):
                slots[s] |= 1 << peer
    busy = [v for v in slots if v]
    return sum(popcount(v) for v in busy) / len(busy) if busy else 0.0


def calls_figure(pes, slot):
    """The investigator's peers_in_calls at a slot of SLOT seconds: a bit in
    every slot from the call's start to its return; the window whole."""
    slots = {}
    for pe in pes:
        start = pe["start"]
        for b, e, a, peer in pe["blocks"]:
            for s in range(int((b - start) / slot), int((e - start) / slot) + 1):
                slots[s] = slots.get(s, 0) | (1 << peer)
    busy = [v for v in slots.values() if v]
    return sum(popcount(v) for v in busy) / len(busy) if busy else 0.0


def time_average(pes, mine, nodes, with_quiet=False):
    """Distinct peers (and helpers at 2 and 4) with a call in progress,
    time-averaged over [the last PE's start, the first PE's last block's end]."""
    lo = max(pe["start"] for pe in pes)
    hi = min(max((a if with_quiet else e) for b, e, a, peer in pe["blocks"]) for pe in pes)
    events = []
    for pe in pes:
        for b, e, a, peer in pe["blocks"]:
            end = a if with_quiet else e
            if end <= lo or b >= hi:
                continue
            events.append((max(b, lo), 1, peer))
            events.append((min(end, hi), -1, peer))
    events.sort(key=lambda x: (x[0], x[1]))
    count = {}
    area = busy_time = 0.0
    helper_area = {2: 0.0, 4: 0.0}
    last = lo
    for t, delta, peer in events:
        dt = t - last
        if dt > 0:
            busy_peers = [p for p, c in count.items() if c > 0]
            k = len(busy_peers)
            area += k * dt
            if k:
                busy_time += dt
            for h in helper_area:
                helper_area[h] += len({1 + ((p if p < mine else p - 1) % h) for p in busy_peers}) * dt
            last = t
        count[peer] = count.get(peer, 0) + delta
    span = hi - lo
    return {
        "exact": area / span if span > 0 else 0.0,
        "busy": area / busy_time if busy_time > 0 else 0.0,
        "h2": helper_area[2] / span if span > 0 else 0.0,
        "h4": helper_area[4] / span if span > 0 else 0.0,
        "span": span,
    }


def main():
    logdir = sys.argv[1]
    printed = {}
    block_bytes = None
    if len(sys.argv) > 2 and os.path.exists(sys.argv[2]):
        for line in open(sys.argv[2], errors="replace"):
            m = re.match(r"multistream node=(\d+) pes=\d+ mib_per_s=([0-9.]+).*peers_at_once=([0-9.]+)", line)
            if m:
                printed[int(m.group(1))] = (float(m.group(2)), float(m.group(3)))
            m = re.match(r"multistream job mode=\w+ bytes=(\d+)", line)
            if m:
                block_bytes = int(m.group(1))
    pes = []
    for path in glob.glob(os.path.join(logdir, "pe-*.txt")):
        with open(path) as f:
            head = f.readline().split()
            h = dict(zip(head[0::2], head[1::2]))
            blocks = []
            for line in f:
                b, e, a, peer = line.split()
                blocks.append((float(b), float(e), float(a), int(peer)))
        pes.append({"pe": int(h["pe"]), "node": int(h["node"]), "start": float(h["start"]),
                    "slice": float(h["slice"]), "nodes": int(h["nodes"]), "blocks": blocks,
                    "dropped": int(h["dropped"])})
    if not pes:
        print("no logs in", logdir)
        return
    nodes = pes[0]["nodes"]
    rows = []
    for n in range(nodes):
        mine = [pe for pe in pes if pe["node"] == n]
        if not mine:
            continue
        kit = kit_exact(mine)
        cm = calls_ms(mine)
        ta = time_average(mine, n, nodes)
        taq = time_average(mine, n, nodes, with_quiet=True)
        window = max(pe["blocks"][-1][1] for pe in mine) - min(pe["start"] for pe in mine)
        blocks = sum(len(pe["blocks"]) for pe in mine)
        per_ms = blocks / (window * 1000)
        per_peer = {}
        for pe in mine:
            for b, e, a, peer in pe["blocks"]:
                per_peer[peer] = per_peer.get(peer, 0) + 1
        even = min(per_peer.values()) / max(per_peer.values())
        scaled = {L: kit_figure(mine, L * 1e-3) for L in (0.25, 0.5, 2, 4, 8)}
        # The kit's 1 ms at a node rate of 23 Gbit/s (c4-standard-16's,
        # 2,742 MiB/s), the same pattern: a slot of 1 ms times that rate over
        # this node's.
        rate = blocks * (block_bytes or 0) / (1 << 20) / window if block_bytes else 0.0
        at_link = kit_figure(mine, 1e-3 * 2742.0 / rate) if rate > 0 else 0.0
        calls_at_link = calls_figure(mine, 1e-3 * 2742.0 / rate) if rate > 0 else 0.0
        p = printed.get(n)
        rows.append((n, len(mine), p, kit, cm, ta, taq, per_ms, even, scaled,
                     any(pe["dropped"] for pe in mine), at_link, calls_at_link))
    print(f"{'node':>4} {'pes':>3} {'MiB/s':>7} {'printed':>7} {'kit':>5} {'calls_ms':>8} "
          f"{'exact':>5} {'exact_q':>7} {'busy':>5} {'h2':>4} {'h4':>4} {'per_ms':>6} {'even':>5}  "
          "kit@0.25 0.5 2 4 8 ms  kit@23G calls@23G")
    for n, k, p, kit, cm, ta, taq, per_ms, even, scaled, dropped, at_link, calls_at_link in rows:
        ps = f"{p[0]:7.1f} {p[1]:7.2f}" if p else f"{'-':>7} {'-':>7}"
        sc = " ".join(f"{scaled[L]:.2f}" for L in sorted(scaled))
        print(f"{n:4d} {k:3d} {ps} {kit:5.2f} {cm:8.2f} {ta['exact']:5.2f} {taq['exact']:7.2f} "
              f"{ta['busy']:5.2f} {ta['h2']:4.2f} {ta['h4']:4.2f} {per_ms:6.2f} {even:5.3f}  {sc}  {at_link:5.2f} {calls_at_link:5.2f}"
              + ("  LOG FULL" if dropped else ""))


if __name__ == "__main__":
    main()
