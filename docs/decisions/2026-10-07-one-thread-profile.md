# 2026-10-07: the one thread, profiled (v0.2 step 4, Part 0)

The first item of v0.2's fourth step, on `docs/prompts/v0.2-step4-progress-helpers-prompt.md`
(the brief) as amended on 2026-10-07: the profile of the lead's agent that every
later claim of the step refers to, and the first measurements of the ladder.

**Where it stands.** Measured on the branch `one-thread-ladder`, from `main` at
`3ee3357`. The author answered the question below the same evening, and the
first rung - a put fills the room - is built on the branch, "What was built". The profile finds that over loopback
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
  part cut to what it took, down to a sixteenth of its largest part - 4 KiB at
  the default chunk, 62 bytes at the tests' 1 KiB chunk, so that their tiny
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

## What goes on meanwhile

The rungs that do not depend on the answer, each measured against the one below
it: `COOP_TASKRUN` and `DEFER_TASKRUN` (the receiver's 11% in the signal path),
registered files, the provided buffers' count and size, and the receive's
scratch copy - half of the receiver's `memcpy`, avoidable for a message that
lies whole in one provided buffer. The zero-copy send last, behind its setting
(`2026-10-07.md` item 7).
