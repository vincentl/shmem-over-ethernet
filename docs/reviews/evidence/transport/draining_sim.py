#!/usr/bin/env python3
"""
§3.8 starvation guard, simulated at segment granularity for one connection A->B.
Node B has P PEs each looping on a blocking shmem_getmem of GET bytes from node A.
A's progress thread holds their reply descriptors in the bulk queue. Each freed
buffer enters DRAINING: the PT copies reply chunks (<= CHUNK) round-robin while
they fit, then the buffer turns OPEN and A's local PE writers may reserve the
remainder. A get's next request arrives (and is enqueued) once its reply has been
fully sent (a lower bound on B's think time: zero).
Reports, per policy, the fraction of segment bytes A's own PEs got.
"""
from collections import deque
CAP, CHUNK = 256 * 1024, 64 * 1024
def sim(P, GET, policy, gens=2000):
    q = deque([GET] * P)     # remaining bytes per outstanding get
    pe_bytes = reply_bytes = 0
    for g in range(gens):
        room = CAP
        limit = CAP if policy == "doc" else CAP // 2   # "half" = cap draining at half a buffer
        used = 0
        n = len(q)
        for _ in range(n):   # one round-robin pass, one chunk per descriptor
            if not q: break
            rem = q.popleft()
            c = min(CHUNK, rem)
            if used + c > limit:
                q.appendleft(rem); break
            used += c; rem -= c
            if rem: q.append(rem)
            else: q.append(GET)  # B's PE immediately issues its next get
        reply_bytes += used
        pe_bytes += CAP - used   # A's PEs may fill whatever is left (assume they always want it)
    return pe_bytes / (pe_bytes + reply_bytes)
for P, GET in [(1, 4096), (4, 64 * 1024), (16, 64 * 1024), (256, 4096), (256, 1 << 20)]:
    print("B PEs=%3d get=%7d B: A-PE share of segment bytes: doc policy %.3f, capped-at-half %.3f"
          % (P, GET, sim(P, GET, "doc"), sim(P, GET, "half")))
