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
  flag when it runs again, and is woken then; an owner that spins after raising
  it sees the wake word move during its spin and never sleeps, so its wakes stay
  near none however long PE 0 waits. PE 0 yields between looks at the flag
  rather than sleeping, to see it within a microsecond or two of its rise on an
  idle machine.
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

*Undo:* the test back to its retries, its run at no helpers and the asserted
offsets removed, `RUN_SERIAL` back, the README's sentence and the release
checklist's two items back.
