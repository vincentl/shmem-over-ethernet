# The hand-on wake for a reopened connection buffer (P2): modelled, built, timed

P2 of `docs/prompts/v0.2-open-items.md` (round four's scale S1; design §3.11,
"Wake count tracks divisibility"): a buffer reopened wakes one writer waiting
for room, not every one; a writer that reserves and leaves room wakes the next;
and a woken writer that finds too little passes the wake on. The author decided
on 2026-10-28 to build it, its model first, and on 2026-10-29 to build it behind
a setting and time it against waking every writer before it became the default
(`docs/decisions/2026-10-28.md`, `docs/decisions/2026-10-29.md`).

## The model

`turnover_wake_model.py` models a connection's open buffer as `ROOM` units,
writers each needing `least` of them and reserving by a CAS, the turnover's wait
word of `base/wait-word.h` (the word, its count of waiters, the two-sided
order), and the agent, which seals and reopens the buffer whenever it holds a
reservation (`ROOM := CAP`) and then wakes. `wake_all` is the library as it was;
`hand_on` wakes one at the open and after a reservation that leaves room - the
reservation's CAS, then its load of the count, the other side of a waiter's
registration and then its look at the room - and a woken writer that finds too
little passes the wake on once a generation, then loads the word afresh before
it sleeps, since its own bump changed it. `floor` is the C's room worth a wake
(a put's smallest part, below): a hand-on needs that much left, and a pass that
much found. `engine.py` is the copy of `tools/modelcheck/engine.py` it ran with;
`turnover_wake_model.out` the run, 3 minutes 10 seconds.

What it found: `hand_on` is live in every case run - two and three writers
needing one or two units of a buffer of two, the largest 11.6 million states,
and with the floor of two a writer below it, at it and above it in buffers of
two and three - and passes at most once a writer a generation. Without the once
a generation (`pass_always`), two writers too large for the room pass the wake
back and forth: PASSES fails. On the way, a first version's agent, bounded to
one open more than the writers and opening on a writer's request as well,
reported a lost wake that was the bound's; the agent opens on a reservation
alone now, which bounds its opens by the writers' reservations.

What it cannot say: every design here is live, a bare wake of one too, since
the agent opens again after every reservation and wakes again. The hand-on is a
performance change, and the model has no clock; nor can it see starvation.

## As built (branch `hand-on-wake`)

`OSOE_TURNOVER_WAKE=hand-on`; `all`, waking every writer, stays the default.
The open wakes one (`osoe_wait_word_wake_one`); a reservation whose room left is
at least a put's smallest part - a sixteenth of the chunk's payload, 4,094
bytes, its message 4,136 at the defaults - wakes the next (`hand_on` in
`transport-internode.c`); and a woken writer that finds room that large but too
little for its own message passes the wake on (`osoe_wait_word_pass_on`), once a
turnover, then looks again. Room below a put's part wakes no one: a smaller
whole message waits for a later turnover's wake. A put's least is that part, so
puts never pass - the passes are for whole messages larger than a part. Each
writer's own setting decides its hand-on and the node's lead's its agent's wake;
`osoe-stat` counts them as `am.turnover_hand_ons` and `am.turnover_passes`.

## The timing

`turnover-herd.c` is the release fixes' review's herd (S1, its `q3-herd.c`)
again: every PE of node 0 puts BYTES-byte blocks to the one PE of node 1 for a
window, a quiet each 8 MiB, under `osoe-launch -m 0,...,0,1` in the development
VM (4 vCPUs, the two logical nodes over loopback, no helpers - `auto` gives a
pair none). It prints node 0's rate, the agent's CPU a GiB, the writers' sleeps
(their main threads' voluntary context switches) a 64 KiB moved, the waits for
room, hand-ons and passes, the slowest and fastest writer's bytes over the
writers' mean, the longest put call of any writer, and the bytes a segment
carried. `bench.sh` ran it at 4, 8, 16, 32 and 64 writers, 1 MiB and 64 KiB
blocks, both wakes alternated, three series of 5 s (`herd-2026-10-29.txt`);
`summarize.py` makes the tables. The medians:

| 1 MiB blocks | 4 | 8 | 16 | 32 | 64 |
|---|---|---|---|---|---|
| MiB/s, all | 9,150 | 8,401 | 7,740 | 6,753 | 3,958 |
| MiB/s, hand-on | 7,595 | 7,348 | 6,770 | 6,414 | 6,173 |
| hand-on over all | 0.83 | 0.87 | 0.87 | 0.95 | 1.56 |
| sleeps a 64 KiB, all | 0.88 | 1.60 | 3.03 | 6.18 | 14.78 |
| sleeps a 64 KiB, hand-on | 0.76 | 1.25 | 1.31 | 1.31 | 1.31 |
| agent CPU s a GiB, all | 0.100 | 0.104 | 0.110 | 0.104 | 0.162 |
| agent CPU s a GiB, hand-on | 0.104 | 0.106 | 0.110 | 0.113 | 0.115 |
| slowest writer's share, all | 0.84 | 0.79 | 0.89 | 0.82 | 0.60 |
| slowest writer's share, hand-on | 0.99 | 0.99 | 0.99 | 0.98 | 0.97 |
| longest put, ms, all | 4.5 | 11.5 | 22.8 | 41.4 | 510 |
| longest put, ms, hand-on | 2.8 | 4.3 | 6.3 | 13.1 | 26.7 |

| 64 KiB blocks | 4 | 8 | 16 | 32 | 64 |
|---|---|---|---|---|---|
| MiB/s, all | 9,157 | 8,881 | 9,209 | 7,111 | 4,207 |
| MiB/s, hand-on | 7,249 | 6,869 | 6,185 | 5,888 | 5,722 |
| hand-on over all | 0.79 | 0.77 | 0.67 | 0.83 | 1.36 |
| sleeps a 64 KiB, all | 0.93 | 1.79 | 3.18 | 6.09 | 14.31 |
| sleeps a 64 KiB, hand-on | 0.87 | 1.64 | 2.26 | 2.30 | 2.31 |
| agent CPU s a GiB, all | 0.101 | 0.102 | 0.096 | 0.105 | 0.160 |
| agent CPU s a GiB, hand-on | 0.109 | 0.107 | 0.115 | 0.121 | 0.125 |
| slowest writer's share, all | 0.90 | 0.91 | 0.86 | 0.85 | 0.54 |
| slowest writer's share, hand-on | 0.99 | 0.98 | 0.99 | 0.97 | 0.96 |
| longest put, ms, all | 4.1 | 4.6 | 6.4 | 30.5 | 492 |
| longest put, ms, hand-on | 1.6 | 2.7 | 3.4 | 5.5 | 9.7 |

Waking every writer is the herd H12 measured on the cluster: the sleeps a 64 KiB
a quarter of the writers, linear in them, and at 64 writers - 16 a vCPU - the
rate halves, the agent's CPU a GiB rises by half, the slowest writer moves half
the mean (a quarter in its worst series) and a put waits up to 2 s. The hand-on
ends all of that: its sleeps flat from 16 writers, every writer within 4% of the
mean, no put past 27 ms, and at 64 writers 1.36 to 1.56 times the rate. But
below 64 it costs 5 to 35% of the rate. Puts never passed; a hand-on came 0.89
times a 64 KiB at 1 MiB blocks and 1.8 at 64 KiB, whose puts are two parts
each. The internode suite passed whole under hand-on (621 of 621).

**Why the hand-on costs: its chain fills buffers one writer at a time.** The
agent seals the open buffer as soon as it has bytes and nothing is in flight
(§3.9's self-clocking), so a buffer filled by a chain of wakes goes out
emptier: a segment carried 136 to 168 KiB under the hand-on and 179 to 256
under all (`segments-2026-10-29.txt`, two series of 3 s at 4, 16 and 64
writers) - more segments a byte, each paid by both agents - and every link of
the chain is a writer's `futex_wake` on its own path and the next one's
scheduling delay: §3.11's warning of the chain, which the release fixes'
review's bare wake of one measured at 10 to 25%.

## An experiment: wake the room's count at once

Not built in the branch: `experiment.diff` is a diagnostic copy of the branch
(two switches read from the environment, and the prints below), run under
`variants.sh` with four variants alternated, two series of 4 s
(`variants-2026-10-29.txt`). `wake-4` is the open waking four writers - as many
as a reopened buffer holds messages of a chunk at the defaults - and no
hand-on; `wake-4-hand-on` is both. Each one's rate over all's, the medians:

| | 1 MiB: 4 | 8 | 16 | 32 | 64 | 64 KiB: 4 | 8 | 16 | 32 | 64 |
|---|---|---|---|---|---|---|---|---|---|---|
| hand-on | 0.80 | 0.84 | 0.83 | 0.93 | 1.54 | 0.77 | 0.77 | 0.65 | 0.78 | 1.25 |
| wake-4 | 0.99 | 1.00 | 0.98 | 1.04 | 1.79 | 0.98 | 0.98 | 0.91 | 1.11 | 1.79 |
| wake-4-hand-on | 0.94 | 0.95 | 0.91 | 0.99 | 1.61 | 0.90 | 0.91 | 0.76 | 0.84 | 1.35 |

and `wake-4`'s sleeps a 64 KiB 0.86 to 1.62, flat, its slowest writer 0.86 to
0.96 of the mean, its longest put 4 to 33 ms, its segments as full as all's at 4
writers and 156 to 176 KiB at 16 and 64. Waking as many as the room holds keeps
waking every writer's rate where the writers are few and ends the herd where
they are many - the herd's cure is the count, not the chain. What it does not
cover is a room of small messages, which holds more writers than it counts -
a node's PEs' atomics toward one node, say - where the hand-on's chain wakes as
many as fit; that is untested here.

## What is not explained

About one hand-on run in eighteen had every writer's longest put at 45 to 98
ms at once - seen at 4, 8, 16 and 32 writers - where all's and `wake-4`'s
shared waits went to about 20 ms. They are not lost wakes of the turnover. The
diagnostic copy prints the connection's buffers whenever a writer's sleep
reaches its 100 ms sweep and whenever an agent's wait lasts 20 ms: in two such
stalls nothing printed, and the one whose lines were kept
(`stalls-2026-10-29.diag.txt`, a 46 ms stall at 4 writers) shows node 0's two
buffers both sealed, full and wholly committed, its agent asleep 36 ms on the
send in flight with all four writers asleep for room, and node 1's agent asleep
the same 36 ms with nothing to receive - the stream held between the two, not
the turnover's wake. The loopback's TCP is the likelier place, and the kernel's
counters do not pin it (`nstat.sh`, `nstat-2026-10-29.txt`, two runs of twelve
each way): delayed acknowledgements alike, a few retransmissions once under one
wake and once under the other, and the receiver's window closing about 2,000 to
5,000 times under all against 9 to 35 under the hand-on, which sends more
slowly. Sampling the job with `osoe-stat` every few milliseconds, one came in
39 runs, between samples.
