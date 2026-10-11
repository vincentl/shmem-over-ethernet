# 2026-10-10: `shmem.helper-wake` waits for the owner's sleeping flag

The release checklist's item before the tag's run of the suite under busy
loops (from the merge point's verification, `docs/decisions/2026-10-10-step4-merge-point.md`),
and half of what the scale review's S1 left open: the agent's idle wait, which
no test guarded against spinning.

## What it was

- **The test counts wakes.** PE 0, alone on its node, puts to each peer node a
  hundred times with a quiet after each; the owner of that peer's wire must be
  woken - its futex completions that saw its wake word move
  (`transport.am.wakes`) - and the node's other helper not. A wake sent to the
  wrong owner shows nowhere else but in latency, so this is the test of the
  mark and the wake naming one owner (`lead_wake_model.py`, `mark_wake_apart`).
- **It asked of its machine that a helper usually sleeps between rounds.** It
  could not tell an owner that spins before it sleeps - which it caught, the
  scale review's mutants woken once to three times a hundred rounds - from one
  preempted between a round's acknowledgement and its sleep. The merge point
  gave it retries and then ran it alone (`RUN_SERIAL`), and beside four busy
  loops on four cores it still failed 9 runs of 20.
- **The agent's idle wait had no such test.** A lead's agent made to spin 200 µs
  before it sleeps passed all but one test, and that one by the load beside it
  (scale S1).

## What was built

- **PE 0 puts only once the owner has raised the flag it raises before it
  sleeps** (`connection-area.h`'s `sleeping`). Every put then finds the owner
  asleep or about to be, and must wake it: the bound is nine wakes in ten
  rounds, where it was one in ten with retries. A preempted owner raises its
  flag when it runs again, and is woken then. PE 0 yields between looks at the
  flag rather than sleeping, to see it within a microsecond or two of its rise
  on an idle machine.
- **What it tells from a sleeper is a spin longer than a round trip.** The
  count is of the futex waits a wake ended, and a ring keeps its wait armed
  across a sleep that a socket's completion ended: an owner that spins after
  raising its flag, but slept through its last acknowledgement's round trip,
  has a wait left armed, which the put's wake completes and counts as a
  sleeper's. So a spin longer than that round trip is caught, and a shorter one
  is caught on the wires where the round trip outlasts it (the adversarial
  read, finding 1; the counts are below). As first written this record, the
  test's comment and the README said a spinner "never sleeps, so its wakes stay
  near none", which is false for a short spin.
- **And the put's own wake is the one counted** (the read, finding 2). Each
  round's quiet asks the same owner for an acknowledgement and wakes it, so the
  owner's count alone passed a put whose wake was lost, or held back for good:
  the read's misroute mutant still had its owner woken 100 times a wire. So PE 0
  reads its own count of the wakes it claimed (`am.wakes_sent`) around each
  put, and must have claimed one at nine puts in ten; and every owner of the
  node but the one the map names is judged, the agent at two helpers among
  them. The PEs of the other nodes wait on a word of their own rather than in a
  barrier, whose arrivals would reach node 0 inside the first peer's rounds.
- **The flag is read where PE 0's own process maps the connection area**,
  found in `/proc/self/maps` - the name is gone from `/dev/shm` by then - at the
  offsets `connection-area.c` now asserts for the test: the layout, the helper
  count and the owners' offset in the header, an owner a cache line.
- **A second run at no helpers**, `shmem.helper-wake.agent.0-1-2-3`, where the
  lead's agent is owner 0 and holds every wire: it raises the same flag before
  its idle wait, so a spin there shows as a helper's does (S1's agent half). The
  PEs' own waits are still unguarded, which the release checklist keeps.
- **Both registered only where `OSOE_INTERNODE` is on**, and not under
  ThreadSanitizer, as before.

## The verification

- **The run at two helpers** (the development VM, at `62f060f`, 2026-10-10 ~15:38;
  `docs/design/evidence/v0.2-step4/2026-10-10-helper-wake/helper-wake-flag-check/`),
  main's test and the branch's, each 20 times alone and 20 times beside four busy
  loops on four cores:

  | | Alone | Beside four busy loops |
  |---|---|---|
  | Main's test | 20 of 20 | 12 of 20 |
  | The branch's | 20 of 20, the owner woken 100 or 101 times in 100 puts | 20 of 20, 100 to 200 times |

  The other helper was woken at no put in any passing run. Three mutants, five
  runs each, every run failing: the scale review's helper spinning 200 µs after
  its flag (the owner woken 1 to 112 times), the same at 30 µs (1 to 198, most
  runs once or twice), and a wake sent to the node's other helper (that helper
  woken 98 or 100 times).
- **The run at no helpers** (at `302ba07`, 2026-10-10 ~19:24;
  `.../helper-wake-agent-check/`): 20 of 20 alone, the agent woken at every one
  of 100 puts to each peer; both runs together beside four busy loops, 20 of 20;
  the scale review's agent mutant - the agent spinning up to 200 µs before its
  idle wait (`mutate-agent.py`) - and the same at 30 µs, five runs each, every
  run failing, the agent woken 0 to 2 times a hundred puts.
- **So the test runs beside others again**: `RUN_SERIAL`, which the grant
  fields gave the counting form, is gone from both runs.

Of those mutants' counts, the 30 µs helper's "1 to 198, most runs once or
twice" is of wires, not runs: nine wires of fifteen were woken once or twice,
two runs of five wholly so, and four wires passed their bound (the read, N1).

## Read adversarially

By a fresh agent at `5a9e34d`
(`docs/reviews/archive/v0.2-helper-wake-adversarial-read.md`): nothing fatal -
the flag an honest sign at the put, every path from a raised flag ending in one
counted wake, the lookup, the asserted offsets and the agent run's registration
right. Its three should-fix findings were taken:

1. **A spin shorter than a round trip counted as a sleep** - the claims are
   corrected (above). Its suggested fix, a second judged wake a round - PE 0
   waiting for the flag again after the put and quieting then, so that the
   quiet's wake lands in a spinner's spin, and asking 180 wakes of 200 - was
   built and measured (`helper-wake-second-check/`): it caught every mutant
   alone, but beside four busy loops a correct library fell short of its bound
   in 5 runs of 20 at two helpers, its count as low as 155 of 200, and in 1 of
   20 at no helpers and in each run under AddressSanitizer. Reasoned, not
   traced: the quiet's wake is lost when the owner, preempted between raising
   its flag and arming its wait, finds the quiet's bit and goes round, with no
   wait armed for the wake to end. A test the suite runs beside others, and
   the tag runs beside eight busy loops, cannot ask that, so the one-wake form
   is kept, with what it catches said.
2. **The quiet's wake standing in for the put's** - now PE 0's claim of the
   put's own wake is checked at every put, and every other owner judged.
3. **`RUN_SERIAL` removed on runs made one test at a time, in the release
   build** - now measured in the merge point's own way, under AddressSanitizer
   beside four busy loops too, and the README and the CMake comment name the
   build (below).

Of its notes: N1, the summaries above and the flag check's parser, which read
the test's lines of `62f060f` and comes out empty against the committed ones,
which its evidence copy now says; N2, the README's ThreadSanitizer sentence,
now covering both runs; N3, the scale review's two spin mutants, now in the
evidence (`helper-wake-mutants/`); N4, the decisions index's atomics line,
which now names this test beside `shmem.memory-orders`; N5, two assumptions of
the lookup that fail loudly, left; N6, `shmem.stat` on the integration branch,
fixed with the blocking gets' read (layout 37).

## After the read

In the development VM at the integration branch's tip with these changes,
2026-10-10 ~20:13 (`helper-wake-final-check/`; the release build but where
named; the bound nine in ten, for the owner's wakes and for PE 0's claims):

| | Alone | Beside four busy loops | AddressSanitizer, beside four busy loops |
|---|---|---|---|
| Two helpers | 20 of 20 | 20 of 20 | 20 of 20 |
| No helpers, the agent | 20 of 20 | 20 of 20 | 20 of 20 |

PE 0 claimed the put's wake at all 100 puts on every wire of every run; no
owner but the one the map names was woken. Seven mutants, five runs alone and
five beside four busy loops, every run failing but one:

| Mutant | Alone | Beside four busy loops |
|---|---|---|
| A helper spinning 200 µs | 5 of 5 failed, the owner woken 1 to 48 times | 5 of 5, 1 to 177 |
| A helper spinning 30 µs | 5 of 5, 2 to 100 | 5 of 5, 1 to 197 |
| The agent spinning 200 µs | 5 of 5, 0 or 1 | 5 of 5, 0 to 179 |
| The agent spinning 30 µs | 5 of 5, 1 to 8 | 4 of 5, 2 to 192 |
| A put's wake sent to the other helper | 5 of 5, that helper woken 100 | 5 of 5, 81 to 100 |
| Every put's wake held back, for the quiet's to cover, two helpers | 5 of 5, no put's wake claimed | 5 of 5, none |
| The same, at no helpers | 5 of 5, none | 5 of 5, none |

A short spin is caught alone, and under load less surely - the one run that
passed was the agent's 30 µs spin beside four busy loops - which is what the
count can tell (finding 1).

*Undo:* the test back to its retries, its run at no helpers, the claim check
and the asserted offsets removed, `RUN_SERIAL` back, the README's sentence and
the release checklist's two items back.
