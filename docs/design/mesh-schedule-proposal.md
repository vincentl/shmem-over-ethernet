# A start-up schedule for the mesh

*Built on 2026-10-19 (H23), by the author's decision of 2026-10-11 (A8: built
now, a large system's measurement waived) - as proposed but for W, which counts
connections: eight, four pairs' two wires, since the control wire came after
this proposal and four connections were two pairs (`mesh-schedule.h`); and
the shuffle's draws, MurmurHash3's finalizer over the job, the step and N
(`mesh-schedule.c`, `tools/modelcheck/mesh_model.py`'s `order`). The model,
the timing and tier 3's runs are in `docs/decisions/2026-10-19.md`.*

*A proposal, 2026-10-05, postponed by the author that day until a system large
enough to measure it (`docs/decisions/2026-10-05-before-the-tag.md` item 2). It
answers the author's question of that day:
does anything spread the start-up connections out in time, so that they do not
arrive as a storm, and if not, how would a deterministic schedule that scales
with the node count do it?*

## What happens now

`osoe_wires_open` (`wire.c`, design §6.2): each lead accepts every lower node,
then calls every higher node, **one call at a time, in increasing order**. The
listen backlog is `min(N, 4096)` (`listener.c`, `try_listen`), capped by
`net.core.somaxconn`, which is 4,096 on the kernels osoe needs (5.4 raised it
from 128). Within it the kernel completes each handshake on its own, and the
connection waits until the lead reads its hello; past it a SYN is dropped and
sent again a second later.

**There is no clock in it, but the order spreads the calls.** A lead makes one
call at a time, and lead *k* makes none until every lower lead has called it;
so the callers reaching any one lead arrive staggered, each about a one-way trip
and an answer's CPU behind the one before. `tools/modelcheck/mesh_timing.py`
models it, every lead one thread, at a round trip of 100 µs and 25 + 35 µs of
CPU a call:

- at most **2 SYNs reach a listener within a round trip**, at any node count,
  and 3 with leads entering the mesh up to 10 ms apart;
- at most 3 or 4 connections wait for a lead's answer at once.

So there is no storm for a schedule to prevent. What the order costs is time:
it is a chain, the last lead called after about *N* call times - **20 ms at 64
nodes, 0.32 s at 1,024**, and 2.6 s at 1,024 if the round trip is 1 ms. A lead
that reaches the mesh late collects every lower lead that calls it meanwhile:
lead 64 of 256, 20 ms late, has 64 waiting, and lets them go together: 11
SYNs then reach one listener within a round trip.

## The schedule

1. **Who calls whom: a circulant.** Lead *i* calls lead (*i* + *d*) mod *N* for
   *d* = 1 … ⌊(*N* − 1)/2⌋, and for an even *N* the offset *N*/2 from the lower
   of the two. Every lead makes about *N*/2 calls and answers the rest, where
   today lead 0 makes *N* − 1 and the last lead none.
2. **When: one offset a slot, the same on every lead.** In slot *s* every lead
   calls at offset *d*ₛ, so each slot is a permutation: every listener receives
   exactly one call in it. Slot *s* begins at *t*₀ + *s*·Δ on the lead's
   monotonic clock, *t*₀ being when the rendezvous released it. No clocks are
   synchronized; the rendezvous's release is the common start.
3. **The order of the offsets: one shuffle, seeded by the job id**, so every
   lead computes the same order without exchanging it and every run of a job is
   the same. Taken in order (*d*ₛ = *s* + 1), the callers held up by a late
   lead *j* all have *j* + 1 next and arrive there together; the shuffle sends
   them to different leads (in the model, 9 to 15 SYNs a round trip with 10 ms
   of entry skew, against 5 to 10 shuffled).
4. **At most W calls in flight** (4): a call to a lead not there yet holds one
   place, not the schedule.
5. **One poll loop** answers the listener and drives the lead's own calls -
   dial, hello, answer, confirmation - as one state machine. The circulant's
   waits form cycles, so a lead may never block in a call while not answering;
   the listener's caller places already work this way, and the calling side
   becomes the same.

**The window is ⌈(*N* − 1)/2⌉ slots**, linear in the node count and short: at
Δ = 100 µs, 0.2 ms at 4 nodes, 3.2 ms at 64, 51 ms at 1,024, 205 ms at 4,096.
It is long enough when Δ is at least a call's time over W, so slots do not run
ahead of answers; where calls are slower, W holds the pace and the schedule
runs at the network's speed instead. Δ would be `OSOE_MESH_SLOT`, in µs.

## What the model says

| | now | slotted, shuffled |
|---|---|---|
| 64 nodes, on schedule | 19.8 ms, ≤ 2 SYNs a round trip | 3.4 ms, ≤ 2 |
| 1,024 nodes, on schedule | 322 ms, ≤ 2 | 51.4 ms, ≤ 2 |
| 1,024 nodes, entering within 10 ms | 328-332 ms, ≤ 3 SYNs, ≤ 4 waiting | 61.4 ms, ≤ 10 SYNs, ≤ 29 waiting |
| 1,024 nodes, one lead 20 ms late | 322-342 ms, ≤ 3 waiting | 71.4 ms, 200 waiting at the late lead |
| 1,024 nodes, a 1 ms round trip | 2,624 ms | 263 ms |

The schedule is 6 times faster at a 100 µs round trip and 10 times at 1 ms.
Against it: where leads reach the mesh at different times, it bunches more than
today's order - 10 SYNs a round trip where today has 3, still nothing a
kernel's queues notice - and a late lead finds one waiting call for each slot it
missed, 200 for 20 ms, all in its backlog. So *t*₀ should be the rendezvous's
release with nothing between it and the first slot.

## What it changes

- §6.2's "the lower node connects to the higher" becomes the circulant's rule,
  and the hello's check ("not a lower node", `wire.c`) follows it.
- The calling side of `listener.c` becomes non-blocking, beside its listening
  side; the three-step hello and its retries are unchanged.
- The proof: a model beside `rendezvous_model.py` of the circulant's cycles
  of calls (no lead waits for an answer from one that is not answering), and
  timing on tier 3 at 64 logical nodes, on schedule and with one lead held
  back by a test hook.

**Recommendation: not for 0.1.0.** Today's order already keeps a storm away,
and what the schedule buys is start-up time at hundreds of nodes and on slow
networks. Do it together with on-demand connections (§6.2, "The mesh is still
eager"), which rewrite the same loop.

**Beside the schedule, at a thousand nodes.** A node with more than 1,024
neighbours on one Ethernet segment outgrows Linux's default neighbour table
(`net.ipv4.neigh.default.gc_thresh3`, 1,024 on the Pi cluster) and then resolves
peers' addresses again and again, whatever the schedule. That is a job of more
than 1,025 nodes, where the default connection budget used to end it; since
v0.2 step 4 the budget's ceiling is 4,097 nodes (2,049 before the buffers
halve), so the neighbour table is met first. It is an administrator's setting,
and the README's limits name it.
