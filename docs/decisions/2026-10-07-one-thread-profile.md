# 2026-10-07: the one thread, profiled (v0.2 step 4, Part 0)

The first item of v0.2's fourth step, on `docs/prompts/v0.2-step4-progress-helpers-prompt.md`
(the brief) as amended on 2026-10-07: the profile of the lead's agent that every
later claim of the step refers to, and the first measurements of the ladder.

**Where it stands.** Measured on the branch `one-thread-ladder`, from `main` at
`3ee3357`. The author answered question 1 the same evening, and the first rung -
a put fills the room - is built, read adversarially and verified on the branch
at `16dd527` ("What was built", "The adversarial read"). Questions 2 and 3 are
new: the kernel's unsent queue, and the default buffer. The profile finds that over loopback
the one thread is not what holds a stream back: the connection buffer's turnover
is. That is the brief's stop-and-ask - "the ladder's profile showing the
library's own work, not the kernel's copies, as the larger share: put it to the
author before Part 1" - in a form it did not foresee: not the library's work but
its protocol's waits. One question for the author, below; the rungs that do not
depend on it carry on.

## What was measured

The VM `osoe` (Debian 13, Linux 6.12.95, 4 vCPUs, nothing else running in it, the
host at a load of about 2 of 8 CPUs); the release build of `3ee3357`; two logical
nodes over loopback, one PE each (`osoe-launch -n 2 -m 0,1`). `perf` 6.12 samples
the CPU clock at 499 Hz - the virtualization exposes no hardware counters - with
DWARF stacks for the two leads' agents and frame pointers for the whole machine.
Three workloads: `putstream.c`, PE 0 putting 64 KiB or 1 MiB to PE 1 for 30 s
with a quiet each 64 MiB; `osoe-pacer mailbox` on four PEs over the two nodes;
and `echo-uring`'s io_uring stream, the bare stream, in the same session. The
scripts, the patch and every output are in
`docs/design/evidence/v0.2-step4/2026-10-07-profile/`.

## What it found

1. **The rates.** The library's stream moved 2,955 MiB/s with 64 KiB puts in a
   run with nothing attached and 2,823 with 1 MiB puts; the bare stream 17,003:
   0.17 of it (`shmem.floor`'s own floor, slower, gives about 0.21 - the
   README's "a fifth").
2. **No thread is saturated.** During the stream, by `perf stat --per-thread`
   over 5 s: the writer 25% of a CPU, the sending lead's agent 55%, the
   receiving lead's agent 36% (with 1 MiB puts 24, 56 and 40%), and the whole
   VM 68 to 73% idle. The bare stream's sender is 100% busy and its receiver
   86%: it is bound by one CPU in the kernel, and the library is not.
3. **The writer waits for room on 91% of its puts.** Over the same 5 s,
   `am.reserve_stalls` grew 181,518 against 199,200 put calls, the writer woke
   the lead 154,023 times, and the sending agent woke 64,000 times a second. A
   128 KiB buffer holds one 64 KiB message - two would need 128 KiB and a
   segment header and two message headers - so every 64 KiB of a stream is a
   turnover: the writer asks for a seal and sleeps, the agent seals, sends,
   reopens and wakes it. A 1 MiB put is sixteen such pieces, and 81% of them
   waited.
4. **The agents' time on the CPU** (`classify.py`): the sender's in the TCP
   stack 49% - 15 points of it waking the receiver, since on loopback the
   receive side's softirq runs on the sender - waking threads 14%
   (`try_to_wake_up`), the send's copy 14%, the library's own code 5%, the
   ring 3%. The receiver's in the receive's copy 33%, libc's `memcpy` 25% (the
   library's two copies: into the receive scratch, and the apply's into the
   heap), `do_signal` 11% (the path by which io_uring's task work interrupts
   the thread, which the ladder's `COOP_TASKRUN` rung addresses), the stack
   13%, the library 5%, the ring 4%.
5. **The mailbox** moved 134,500 messages a second, each lead's agent 34% busy,
   each PE 11 to 12%, the VM 70% idle: bound by its round trips, as a
   request-and-acknowledge pattern is.

## What a deeper turnover does

Each line ten seconds of 64 KiB puts, the configurations alternated, two or three
rounds (`sweep.txt`, `ab-*.txt`):

| Configuration | MiB/s | Of the bare stream |
|---|---|---|
| Default: 128 KiB buffers, 64 KiB chunks | 2,617-2,963 | 0.17 |
| 128 KiB buffers, 32 KiB chunks | 3,895-3,902 | 0.23 |
| 128 KiB buffers, 16 KiB chunks | 4,204-4,243 | 0.25 |
| A chunk of half the buffer (patched) | 2,871-3,059 | 0.17 |
| **Fill the room (patched, not merged)** | **6,299-6,488** | **0.37** |
| 256 KiB buffers, 64 KiB chunks | 8,406-8,473 | 0.50 |
| 512 KiB buffers | 8,876-8,926 | 0.52 |
| 1 MiB buffers | 10,236-10,418 | 0.61 |

A chunk of half the buffer changes nothing, because a 64 KiB put is a message of
a chunk and a 32-byte remainder, and two of them still do not fit. *Filling the
room* (`fill-the-room.patch`) lets a put's message take as much of the open
buffer's room as there is, down to 4 KiB, the length decided at the
reservation's CAS from the head it compares; every other message reserves whole,
as today. It is 2.2 times the default at the default memory, and the 71 tests of
puts, RMA, signals, the non-blocking forms and the collectives pass on it in the
release build. It is an experiment: §3.5's reservation is model-checked, and this
changes it.

## What it says

- Over loopback, "a fifth" is the connection buffer's turnover - two wakes
  between threads for every 64 KiB - and not the one thread's CPU. Helpers do
  not move it: one peer's stream stays on one owner. On a fast link the same
  turnover would hold one peer's stream near 3 GiB/s, about 25 Gb/s, whatever
  the number of threads.
- The helpers' case stands where the bytes are: the bare stream's sender is one
  CPU saturated by the kernel's copies and stack. Once the library's turnover
  is deep, its agents meet the same wall, and that is where a second owner
  counts.

## Questions for the author

### 1. The ladder's first rung: fill the room, deepen the buffer, or both

*Answered: (c) - fill the room first, modelled for the order of delivery and
verified, then a deeper default buffer measured on top of it; the author's
words and the caveat in `2026-10-07.md`, items 9 and 10.*

- (a) **Fill the room**: a put's message takes the room the open buffer has,
  down to a floor of 4 KiB, decided at the reservation's CAS (§3.5). 2.2 times
  at no memory; a change to a model-checked protocol, so the reservation model
  first, with a writer whose length depends on the head it compares, then the
  full verification.
- (b) **Deepen the default buffer** to 256 KiB, three 64 KiB puts a turnover:
  2.9 times, no protocol change. The connection area doubles - at 500 nodes
  250 MiB a node in place of 125 - and `OSOE_AM_BUFFER_BUDGET`'s 256 MiB then
  binds from about 500 nodes rather than 1,000.
- (c) Both, the default buffer chosen afterwards by measurement and memory.

**Recommendation: (a) now, then measure (b)'s buffer on top of it before
choosing a default.** (a) costs no memory, and is the design's own intent: a
buffer of twice the chunk was meant to carry two. The buffer's default is a
choice of memory at scale against speed on one link, better taken with (a)'s
numbers in hand. *Undo:* reservations whole again, the patch's one call.

### 2. Cap the kernel's unsent queue - a rung the brief does not list

*Answered: `2026-10-07.md`, item 11, (b).*

Today a fetch-add, a get or a small put behind a put stream waits for some 2.6 MB
the kernel has queued unsent, 14 to 22 ms at a gigabit, whatever the library's
buffer: design §3.2's "two segments" bound is not what is built. The control
wire (Part 2) takes the barrier off that queue; the rest of the data plane stays
on it. `TCP_NOTSENT_LOWAT` on every wire takes the wait to 3.3 to 3.9 ms on the
Pi at 16 KiB, the stream unchanged there and over loopback.

- (a) **16 KiB, fixed**, a setting to change it: measured on the Pi and over
  loopback; on a link above a gigabit, untimed.
- (b) **Scaled to the link**: the cap as about 128 µs of the link's measured
  bandwidth, at least 16 KiB - 16 KiB on the Pi, more where a link is faster, so
  that the kernel always holds enough to keep it busy between the agent's
  passes. A socket option may change at any time, so it is set when step 2 has
  measured the link, before `shmem_init` returns, and 16 KiB until then.
- (c) Leave the queue uncapped and rely on the control wire.

**Recommendation: (b)** - the measured 16 KiB where it was measured, and a rule
that cannot starve a faster link of bytes; (a) is the fallback if the author
would rather not tie it to the link before a fast link has timed it. Design §3.2
rewritten either way. *Undo:* no cap, today's.

### 3. The default buffer, now that filling is in

*Answered: `2026-10-07.md`, item 12, (a).*

On top of filling, 256 KiB buffers carry a loopback stream 1.75 times further
(about 10 GiB/s, 0.6 of the bare stream) and change nothing at a gigabit, where
the link is the limit. (*Corrected:* measured beside the spare, which cost the
128 KiB stream 11%; against filling alone, 1.4 to 1.5 times -
`tip-against-first-rung.txt`.) Their cost: twice the connection area (the author: fine
at 500 nodes) and, with the kernel's queue capped, 1.7 ms at the 99th percentile
of a fetch-add behind a stream on the Pi, not at its median.

- (a) **256 KiB**, `OSOE_AM_BUFFER_BUDGET` raised from 256 MiB to 1 GiB so that
  it still binds near 2,000 nodes.
- (b) 256 KiB allocated, but the depth a writer may fill chosen from the link
  when step 2 has measured it - 128 KiB on a gigabit, the whole above: the Pi's
  tail kept, a word the writers read and a model to say it is safe.
- (c) 128 KiB, as today.

**Recommendation: (a).** The cost is a tail at a gigabit only, and smaller than
what question 2 takes away; (b) is the refinement if the Pi's tail matters more
than a mechanism. *Undo:* the default back to twice the chunk.

## What was built: the first rung, filling the room

On the author's answer (`2026-10-07.md` items 9 and 10), commit `14cdbaa` and the
record's own:

- **The model first.** `reservation_model.py` gains messages of several units, a
  writer whose part takes as much of its message as the room after the head it
  loaded holds - at least `least` units, or the whole remainder when that is
  less - with the CAS comparing that same head, and a put followed by its
  signal's fenced rmw as the writer's next message. Its properties are the
  model's own: ORDER (one writer's units in the order it issued them, which is
  what fence and the signal rest on), ONCE, TORN, SEQ, QUIET, SEGSEQ and
  liveness, and OVERFLOW (a reservation never ends past its buffer). Thirteen
  cases hold - one to three writers, messages of one to three units, buffers of
  two and three, the floor at one and two units, beside the pre-open drain's
  reply and under x86-TSO, up to 277,430 states - and three breakages fail: the
  length taken from one load of the head and the CAS from another (OVERFLOW),
  a part that does not advance the writer's position (ONCE), and a floor not
  lowered for a remainder shorter than it (STUCK). The model's 15 earlier cases
  and 14 earlier breakages are unchanged.
- **The C.** `osoe_connection_reserve_up_to(connection, most, least, r)`, with
  `osoe_connection_reserve` its case `least == most`, one CAS loop; the put's
  part cut to what it took, down to a sixteenth of its largest part - 4,094
  bytes at the default chunk, 62 at the tests' 1 KiB chunk, so that their tiny
  buffers cut puts too. The kernel's side does not change: one send in flight a
  wire, a short send offered again from its byte, the peer's parse across
  provided buffers; `shmem.internode.*` and `shmem.multinode.*` run every put
  through 2 KiB buffers with `OSOE_WIRE_SPLIT` cutting a fifth of the sends and
  receives at random bytes.
- **The tests.** `shmem.connection.fill` runs the C under four writer threads
  sending 1,200 messages of 1 byte to 2 KiB through 1 KiB buffers, beside a
  progress thread that seals, sends, opens and drains replies, against WHOLE,
  ORDER, STALESEQ, FLOOR, FILL (a cut part ends its buffer), BYTES and ACCT;
  `test-internode.c` signals a put of 5,000 bytes, cut in parts, and checks
  every byte has landed when the signal has. Five mutants, each caught by the
  suite's own tests (`mutants.txt`): the header not cut with its part (17
  failing), the payload not cut (17), a stale head (11, `shmem.connection.fill`
  among them), the floor not lowered (17), the part's payload eight bytes short
  (17).
- **The measurement** (release, alternating with the base): the 64 KiB put
  stream 6,183 to 6,365 MiB/s against 2,974 to 3,014; `shmem.floor`'s stream
  ratio, the README's "a fifth", 0.43 to 0.45 by the median of each of six runs
  against 0.18 to 0.20, the library at 6,420 to 6,650 MiB/s and its floor at
  14,172 to 15,157 over 21 pairs; the median round trips of a get, a put with
  quiet and a fetch-add unchanged at 55 µs.

## The adversarial read

A fresh agent read the diff against seven properties - order, every byte once,
no reservation past its buffer, no hang, every other caller as before, the
receiver's parse, the model saying what the C does - and archived its report as
`docs/reviews/archive/v0.2-step4-part0-adversarial-read.md`, its model variants
in `docs/reviews/evidence/v0.2-step4-part0/`. Verdict: properties 1 to 6 kept,
nothing fatal. What was done with each finding:

- **F1, a small message behind a stream** (should-fix, measure first). A stream
  that fills every buffer leaves a fetch-add nowhere to go until a turnover,
  where a 64 KiB put used to leave half of each buffer free. Measured on the Pi
  (the section below): a fetch-add behind the stream waits about 0.4 ms more at
  the median and 0.6 to 1 ms more at the 99th percentile with the kernel's
  queue capped, 5 to 6 ms more without. The read's fix - a part leaves a spare
  at the buffer's end - was modelled (a `tail` breakage failing), built
  (`f643837`) and measured: it changed neither one writer's wait nor three
  writers', and cost the loopback stream 11%. The cost is the bytes ahead of a
  small message in a fuller segment, which a spare cannot change; so it was
  taken out again (`caf0a9f`), and the model keeps a stream beside a writer of
  small messages among its `fill` cases.
- **F2, the floor test's stream pin** (should-fix). The poll the pin guards
  against measured 0.083 to 0.085 against the pin's 0.10 - caught, by little.
  The pin is 0.15 now (`16dd527`): the poll fails it by more than 1.7 times, and
  the library clears it by 2.6 times at its lowest pair beside the suite.
- **F3** (note): the new signal round quieted before it checked, which would
  give the bytes time to land behind the signal; it checks first now.
- **F4, F5** (notes): stale comments - `reserve_waiting`, design §3.11, the test
  headers, the model's docstring - and the figures (4,094 bytes, a sixteenth of
  the largest payload) brought up to date; the mask's comment says why it
  changes nothing.

## What a small message waits behind a stream, on the Pi

Item 10's caveat measured (`docs/design/evidence/v0.2-step4/2026-10-07-pi-head-of-line/`):
two Pi nodes, two PEs each, PE 0 streaming 1 MiB puts to node 1 while PE 1 times
fetch-adds to node 1 over the same connection, the branch's tree at `8a0e2fd`,
alternating, each line two or three runs:

| Connection buffer | Kernel's unsent queue | Fetch-add, median | p99 | Stream |
|---|---|---|---|---|
| (no stream) | - | 171-173 µs | 176-178 µs | - |
| 128 KiB | uncapped (today) | 16.1-22.2 ms | 24.9-27.1 ms | 112 MiB/s |
| 256 KiB | uncapped | 21.1-21.6 ms | 23.3-29.4 ms | 112 MiB/s |
| 512 KiB | uncapped | 21.6-22.2 ms | 26.0-31.6 ms | 112 MiB/s |
| 128 KiB | 16 KiB | 3.87 ms | 4.99 ms | 112 MiB/s |
| 128 KiB | 128 KiB | 5.0-5.5 ms | 6.1 ms | 112 MiB/s |
| 128 KiB | 512 KiB | 8.3 ms | 9.5-10.0 ms | 112 MiB/s |
| 256 KiB | 16 KiB | 3.88 ms | 6.65 ms | 112 MiB/s |
| 512 KiB | 16 KiB | 5.2-5.5 ms | 9.4-10.0 ms | 112 MiB/s |

- **Today a fetch-add waits about 21 ms behind a stream at a gigabit, whatever the
  buffer.** The library sets no socket buffer and no cap on the kernel's unsent
  queue (`wire.c` sets `TCP_NODELAY` alone), so the kernel queues some 2.6 MB
  ahead of it. §3.2's bound - "roughly two segments", about 2 ms on 1 GbE - is
  not what is built. The control wire (Part 2) takes a barrier off this queue;
  a fetch-add, a get or a small put stays on it.
- **Capping the unsent queue** (`TCP_NOTSENT_LOWAT`, an experiment by the
  environment, `notsent-lowat-experiment.patch`) takes the wait to 3.9 ms at
  16 KiB, with the stream at line rate. The kernel then holds what is in flight
  and a few kilobytes; the library's two buffers hold the rest.
- **With the queue capped the buffer shows**: 256 KiB buffers cost the p99 1.7
  ms and not the median, 512 KiB both. Without the cap the buffer is lost in the
  kernel's queue.

Against the base, alternating three times (`base-fill-1/`, `base-spare-1/`,
`many-1/`), the spare being the read's F1 fix as built and then removed:

| Writers of fetch-adds | Kernel's unsent queue | Base | Filling the room | With the spare |
|---|---|---|---|---|
| one | uncapped | 14.4-16.6 ms, p99 21.1-24.9 | 20.0-21.6 ms, p99 23.8-26.6 | 16.0-20.7 ms, p99 22.7-32.7 |
| one | 16 KiB | 3.3-3.9 ms, p99 3.9-4.4 | 3.3-3.9 ms, p99 4.7-5.0 | 3.9 ms, p99 5.0 |
| three, beside one stream | 16 KiB | 4.0-4.4 ms, p99 5.5-6.1 | 4.43 ms, p99 6.6-7.2 | 4.43 ms, p99 6.65-7.2 |

The stream ran at 112 MiB/s in every line, and every writer was served in every
run: nothing starved.

## On top of filling, over loopback: deeper buffers, and the kernel's queue

In the VM, 64 KiB puts, alternating (`loopback-spare-cap-buffers-floor.txt`,
measured on `f643837` with the spare; `ab-spare-zero.txt` shows the spare's own
11%, so the figures without it are about a ninth higher):

- **Deeper buffers**: 128 KiB 5,555 to 5,743 MiB/s; 256 KiB 9,992 to 10,070;
  512 KiB 8,870 to 9,934; 1 MiB 10,196 to 10,445. On top of filling, 256 KiB is
  1.75 times the default, and the rest a plateau near 0.6 of the bare stream -
  beside the spare; against filling alone, 1.4 to 1.5 times
  (`tip-against-first-rung.txt`: 8.8 to 10.0 GiB/s against 6.4 to 6.5).
- **The kernel's unsent queue capped**: uncapped 5,760 to 5,862; at 16 KiB
  5,762 to 6,357; at 128 KiB 5,752 to 5,934 - the cap costs a loopback stream
  nothing.

## What was built: the second rung, the kernel's unsent queue

On the author's answer (`2026-10-07.md` item 11):

- **The limit.** `osoe_wire_unsent_bytes` - the link's bandwidth times 128 µs,
  at least 16 KiB, at most 1 GiB, or `OSOE_WIRE_UNSENT_BYTES` (0 for the
  system's own) - and `osoe_wire_limit_unsent`, `TCP_NOTSENT_LOWAT` on a wire.
  `shmem_init` hands the established link to the transport (`osoe_transport_link`,
  a new call of transport.h's, reaching AM through `osoe_internode_backend_link`),
  and each lead's application thread asks its agent, which alone owns the
  sockets, to set it: a request word taken as a dirty bit is, at a pass and
  again after `lead_sleeping` is raised (`progress-host.c`; as first built the
  application thread set it itself - the second adversarial read's S1). Start-up
  ran without it, so step 2 measured the link as it is; a link set by hand gives
  the bandwidth, under a `fixed` shape too, and otherwise a `fixed` shape the
  floor; a job of one node nothing. An aborting agent lifts it, so that its
  abortive goodbye leaves with the kernel even toward a stalled peer (N6). Under
  `SHMEM_DEBUG` each agent says the limit and the connections it reached.
- **The tests.** `shmem.wire` checks the arithmetic, the option on a real TCP
  socket, a closed wire not counted and the lift; `shmem.unsent.*` (`unsent.sh`)
  each lead's line and that it reached all its connections - nodes less one, or a
  lead that limited nothing would pass (S2) - from a measured link, one set by
  hand above and below the floor, `fixed`, the setting's own value and 0, and
  nothing on one node; `shmem.start-up.wire-unsent-bytes*.invalid` the refusals
  (N4).
- **On the Pi, as first built** (`cap-1/`): the leads measured 116.5 MB/s and
  limited to 16,384; a fetch-add behind a 1 MiB put stream 3.3 to 3.9 ms at the median, 5.0
  at the 99th percentile, three writers of them 4.43 and 6.6 to 7.2, the stream
  at 112 MiB/s - the experiment's figures.

## What was built: the third rung, the default buffer, and the ladder's tip

On the author's answer (`2026-10-07.md` item 12): `OSOE_AM_BUFFER`'s default four
chunks (`OSOE_AM_DEFAULT_BUFFER_CHUNKS`, so that it stays above the floor
whatever the chunk), the floor two; `OSOE_AM_BUFFER_BUDGET` 1 GiB. A sweep found
the tests that assumed the default (`test-am-size`, two start-up refusals) and
some twenty-five statements in the README and the design notes; the README now
says 2,049 nodes at 256 KiB, halved to the floor from 2,050, 4,098 refused.

The tip - filling the room, the kernel's queue capped by the link, 256 KiB
buffers - measured:

- **Over loopback** (`tip-against-first-rung.txt`, `floor-tip-pairs.txt`): the
  64 KiB put stream 8,768 to 10,018 MiB/s against the first rung's 6,404 to
  6,512 - 1.4 to 1.5 times, where the record's "1.75" was measured beside the
  spare; `shmem.floor`'s ratio 0.58 to 0.67 by the median of each of six runs,
  the library at 7.6 to 9.8 GiB/s and the bare stream at 9.1 to 15.0, the host
  under ordinary desktop load; the mailbox 137,418 to 137,756 messages a second
  against 137,087 to 137,112, unchanged. The poll the stream's pin guards
  against measured 0.161 to 0.164, above the pin of 0.15: the pin is 0.3.
- **On the Pi** (`2026-10-07-pi-head-of-line/compare-1/`): `osoe-bench` at 4 x 4
  and 4 x 1, alternating with `3ee3357` three times - every line within the
  noise of the tree before Part 0: puts at 112.1 MiB/s, the barrier 176 and
  185 µs, a fetch-add's, a get's and a put and quiet's round trips, reduce,
  broadcast, fcollect and alltoall. One thing moved: at 4 x 4 the pairs' sixteen
  streams share the links more evenly, the leads' from 64 to 74 MiB/s down to
  28 and the others' from 28 up to 32 to 39, the total 447 MiB/s either way. A
  fetch-add behind a 1 MiB put stream 3.9 ms at the median and 6.65 to 6.92 at
  the 99th percentile, three writers of them 4.43 and 7.2 to 7.8, the stream at
  112 MiB/s.

## What goes on meanwhile

The rungs that do not depend on the answer, each measured against the one below
it: `COOP_TASKRUN` and `DEFER_TASKRUN` (the receiver's 11% in the signal path),
registered files, the provided buffers' count and size, and the receive's
scratch copy - half of the receiver's `memcpy`, avoidable for a message that
lies whole in one provided buffer. The zero-copy send last, behind its setting
(`2026-10-07.md` item 7).
