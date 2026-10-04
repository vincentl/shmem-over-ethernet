# 2026-10-03: a team number is each member PE's, and is given back (step D, part 2)

The second part of the team redesign's step D, built on
`docs/prompts/phase1-step7-slice3-prompt.md` (the brief), Part 2, from
`docs/design/team-resources-proposal.md` (the proposal) §3, D1, D2 and D4 to D8.
The first part (`2026-10-03-team-storage.md`) moved a node's team blocks into a
pool found by name and kept numbering as it was; this part changes numbering. A
team's number is each member PE's own, agreed by the split among the new team's
members and given back at its destroy; a node's block for a team is given back
when the last of its members there leaves it; and everything that kept state
for a number or a block now knows when it holds another team's.

## The models

Both written before the code, as the brief asks, in `tools/modelcheck/`,
registered under the `model` label with the others (`osoe_models`):

| Model | Case | States | Result |
|---|---|---|---|
| `team_block_model.py` (a block's life) | design | 122,306 | ok, with liveness |
| | `no_holders` (the lead gives the block back as it leaves) | 298 | VIOLATION: block written for team B while a member of A holds it |
| | `no_relearn` (the engine keeps the team it learnt first) | 98 | VIOLATION: a message of generation 3 applied to state learnt at 1 |
| | `map_early` (every member clears the map entry as it leaves) | 111,224 | VIOLATION: a message for team B found no block |
| | `last_clears` (the first design: the last holder clears the entry) | 293 | VIOLATION: the lead left its team and found its name still mapped |
| | `free_first` (the block back in the bitmap before the generation is raised) | 671 | VIOLATION: a message for team B resolved to the block at the wrong generation |
| `posted_bitmap_model.py` (D5) | design | 4,627,960 | ok, with liveness (`liveness_max_states` 6,000,000) |
| | `summary_first` (the summary's bit before the block's) | 157 | STUCK: a post never taken |
| | `wake_first` (the wake before the bits) | 72 | STUCK |
| | `bits_first` (the bits before the handoff word) | 159 | STUCK |
| | `clear_after` (the agent clears the summary after reading the bitmap) | 243 | STUCK |
| `split_numbers_model.py` (D7, as redesigned) | `shared`, design | 194,051 | ok, with liveness |
| | `crossed` (the read's program), design | 12,798 | ok, with liveness |
| | `two_low`, `two_high` (split_2d's two numbers), design | 306,010; 241,197 | ok, with liveness |
| | `second_low`, `second_high` (the second number contested), design | 69,570; 77,742 | ok, with liveness |
| | `crossed`, `token` (the first design, the mask token) | 15,096 | TRAPPED: the read's deadlock |
| | `shared`, `no_pending` (the agreed number taken at once) | 2,004 | VIOLATION (UNIQUE) |
| | `shared`, `release_first` (the pending word cleared before the number is held) | 6,350 | VIOLATION (UNIQUE) |
| | `shared` and `two_low`, `higher_waits` (a take waits for a lower split too) | 14,593; 17,638 | STUCK |
| | `shared`, `lower_fails` (a take fails on a higher split's word too) | 12,811 | VIOLATION (LOWEST): the lowest split loses round after round |
| `pool_dry_model.py` (a dry pool) | `member`, `parent`, `remote`, `split_first`, `two`, design | 2,075; 90; 62; 52; 13 | ok, with liveness |
| | `two`, `first` (the first design's test, the members never zeroed) | 8 | TRAPPED: busy rounds for ever |
| | `member`, `odd_only` (the read's suggested test, an odd generation) | 53 | VIOLATION (LEGIT): failed with the block between its raise and its bit |
| | `member`, `zero_at_leave` (the last holder zeroes the members) | 54 | VIOLATION (LEGIT) |
| | `parent`, `first_round_final` (a dry first round fails the split) | 152 | VIOLATION (LEGIT) |
| | `split_first`, `departing` (the second read's suggested flag) | 73 | TRAPPED: the split waits for a destroy that waits for it |

`team_block_model.py` runs in about 6 s, `split_numbers_model.py` in 21 s,
`pool_dry_model.py` in under one and `posted_bitmap_model.py` in 108 s, on the
Mac. The bitmap's model was written during this part, before its code, once
the arrival scan's cost was measured (below); the brief's Part 1 had asked for
it, and Part 1's record deferred it here.
`last_clears` and the property it breaks (FREE) were added after the first
version of the model, with the change they record (below). The other two
models replace and add to what the first version of this part had - a
`mask_token_model.py` of D7's token, whose design the adversarial read showed
deadlocks a program the model had no way to express, and no model of the dry
pool's test - and were written, with their breakages, before the code that
replaced the first build's (below, "The adversarial read").

## What was built

1. **A number is each PE's own** (D1, D8). `OSOE_MAX_TEAMS` counts the numbers one
   PE holds at once, the three predefined included - still 4 to 1,024, default
   64, agreed at the rendezvous - and a destroyed team gives its number back. A
   PE keeps the numbers it holds in a bitmap (`team.c`, `held`).
2. **The split** (D2, `team.c`): every member builds its descriptor but for the
   number - its node's members, its local lead, and on that lead the node set;
   the would-be local lead of each new team on each node claims the team's block
   from the pool; then one AND reduction over the parent, in place, of every
   member's free mask - a bit for each number it holds no team under and under
   which no block is mapped in its own name - and all ones from every other
   parent PE, beside a word of marks (not busy, not dry) written inverted so that
   the one AND carries them (`collectives.c`, `osoe_team_split_and`). Every
   parent PE reads the same result: `split_strided` takes the lowest bit,
   `split_2d` the lowest for its rows and the next for its columns. A pool found
   empty makes the round busy, run again by every parent PE, in the split's
   first round and while a block is on its way back (`osoe_node_team_returning`:
   fewer holders than the members it was named with, a count a claim zeroes);
   otherwise a dry pool fails the split alike, said by the PE that found it dry,
   naming
   `OSOE_TEAM_BLOCKS` and its node; no bit left fails it alike, said once by the
   parent's rank 0, naming `OSOE_MAX_TEAMS`; a block claimed for a split that
   fails goes back. Then each member takes the number: its descriptor's slot, and
   its counters for the number in its internal region zeroed
   (`osoe_internode_number_taken`, a new call of the seam) - only for a number it
   has held before in the session; and each local lead names its block, writes
   the record and maps the block under the team's name. The counter on world PE
   0, `claimed_seen`, the first-number broadcast and its spare word are gone.
3. **The internal region** (`internal-region.h`): each team number's words gain
   two lines for the splits over it as parent - the mask's words and the marks,
   17 words at the largest `OSOE_MAX_TEAMS` - and the line of the old counter
   goes. Every node computes the same offsets, as before.
4. **A block's life** (D4; `node.c`, `node.h`): `osoe_node_team_name` raises the
   generation (odd while a team holds the block), zeroes the counters and the
   tree state, and writes the name, the holders and - layout 30 - `members`, the
   holders it was named with, kept after the team has left and zeroed by the
   next claim; `osoe_node_team_leave`: the team's local lead
   first clears its name's map entry, then every member brings the holders down
   by one, and the last retracts the record, raises the generation and returns
   the block to the bitmap.
5. **The engine follows the generation** (D4, D5): the arrival handoff is one
   64-bit word, the generation beside the epoch (layout 30), compared whole with
   the one the agent took last (`internode-am.c`); the engine relearns a block
   when a handoff or a resolved message shows another generation, and ends the
   job, naming the broken argument, if the team it is replacing has an epoch
   open or a release unapplied (`progress.c`, `known_team`). The generic data
   half keeps its per-block state at a generation too and empties it when the
   block holds another team (`internode-generic.c`, `team_state`), the bases
   with the counters they count from.
6. **Handles** (D6): a created team's handle is its descriptor's address plus
   the descriptor's generation - how many times the PE has taken the number -
   times the size of the process's pool of descriptors; `osoe_team_of` recovers
   both by arithmetic, never by a read, and names a stale handle "destroyed; the
   number names another team since". The predefined handles stay 0, 1 and 2.
7. **The published epoch row** (item 10): a 64-bit word, the block index plus one
   beside the epoch (layout 30, `OSOE_EPOCH_WORD`), cleared at destroy;
   `osoe-stat` pairs a PE's epoch with the block its word names, and its JSON
   gives `null` for a PE in another team under the number.
8. **`SHMEM_THREAD_MULTIPLE`** (D7, as redesigned; `split_numbers_model.py`):
   nothing is held across the first reduction. Once it has agreed the
   numbers, each member takes them *pending* on its PE (`team.c`,
   `take_pending`): a word per number, from 0 to the parent number plus one by
   compare-and-swap, and then the number found not held; a word another split
   holds is waited for - a futex wait that sweeps - when that split's parent
   number is higher, and lost when it is lower. A second AND reduction over the
   parent says whether every member took its numbers: then each marks them held
   and clears its words, or clears them and every parent PE runs the round
   again. Below `SHMEM_THREAD_MULTIPLE` a split takes its numbers after its one
   reduction, as before. Three counters, `shmem.team_split.calls`,
   `shmem.team_split.busy_rounds` and `shmem.team_split.lost_rounds`.
9. **The posted bitmap** (D5; the brief's Part 1 item 6): a bit a block of the
   pool, behind a summary word of a bit for each 64 blocks, in place of Part 1's
   low-water mark of published blocks (layout 30; `node.h`,
   `team_posted_summary`). A member that posts sets its block's bit, then its
   word's bit in the summary, between its store of the handoff and its bump of
   the lead's wake word; the agent loads the summary, exchanges it if it is not
   zero, exchanges each bitmap word it names, and reads those blocks' handoffs
   (`internode-am.c`). A pass with nothing posted is one load at the default
   pool. The walk it replaces, every block at and above the low-water mark,
   cost the most blocks the node had held at once: with 500 teams held, a
   barrier of the world was about 8 to 11 µs slower in the VM, some 10%
   (`build/scratch/teams-d/scan/`). With the bitmap, alternated with the walk's
   build in a quiet VM, five rounds of 3,000 barriers each way: 500 teams held
   cost the barrier about 1 µs, within the noise - 75.4 to 76.5 µs on `0,1`
   and 87.5 to 88.6 on `0,0,1,1`, the medians of the rounds' medians - where
   the walk cost 4 to 8, 78.5 to 82.9 and 90.0 to 97.9 (`results2.txt`).
10. **The reply pool's term** (D5; the author's answer to Part 1): degree ×
   `OSOE_TEAM_BLOCKS`, 5,120 descriptors at the defaults, about 240 KiB;
   `shmem.reply-pool.overflow` re-sized to thirty-one origins against the
   smallest pool (`1 + 1 + 3 × 8`).
11. **The records**: design §5.7 (the second part's account, the old "ranges
    leak" paragraph made history), §5.2 (the lead's name), §3.8 (the bound),
    §3.12 (the scan and the handoff), §10 (the question on `split_2d`'s numbers
    closed); the README's team limits, its cost table and settings rows; the
    `SHMEM_INFO` text; the comments a sweep of every per-number state found
    saying the opposite.

## What a number owns, and what is reset when a PE takes it again (item 5)

A Sonnet sweep listed every piece of state keyed by a team number, in a PE's
process, its share of the node segment and its internal region. Each, and why
taking the number again is safe for it:

| State | At take |
|---|---|
| `numbered[]` (the live descriptor by number) | set at `take_number`, cleared by `forget` |
| `created[]` (the descriptor's slot) | overwritten whole by `finish`; its generation raised, so an old handle is told from the new team |
| `held`, `pending` | the number marked held by the split - under `SHMEM_THREAD_MULTIPLE` while it is pending, and its pending word cleared after - and cleared last at destroy, after every use of the descriptor |
| the descriptor's `published` pointer, `label`, `block` | rebuilt by `finish` and `take_number` |
| the published epoch word (node segment) | written `(block, 0)` at `take_number`, and 0 at destroy - the sweep found it left stale, and `osoe-stat` would have paired a destroyed team's last epoch with the block's next team |
| the internal region's counters: arrivals and release, the gather's READY and DATA, the scans' rows | zeroed by `osoe_internode_number_taken` before the split's sync, if the PE has held the number before |
| the internal region's slots, spare words, the gather's shape, the split lines | none: each is read only once a counter, or the collective itself, says it was written |
| the generic phase's bases, gathers and scan pieces (by block) | emptied when the block's generation moves (`team_state`) |
| the engine's tree state (by block) | relearnt when the generation moves (`known_team`) |
| the context pool | nothing: destroy ends every shareable context of the team and is fatal on a live private one |

Nothing of the old team is still to arrive when a number is taken again: every
write into a PE's slots and counters is one the PE waits for before it returns
from the collective, and the destroy's sync follows every member's return from
the team's last one (the proposal's "Why giving a number back is safe"); the
sweep checked the reduction and broadcast's pieces, the gather's READY and DATA
and the scans', and found no write a PE does not wait for.

## Choices made without the author

- **The lead clears its own name's map entry as it leaves; the proposal had the
  last holder clear it.** Found by the tests: `shmem.team-split.too-many.L4` (one
  number a PE) failed a split made right after a destroy, because the lead had
  left and a member on its node had not, and the lead's name - so its number -
  was still mapped. Nothing resolves the entry once any member there has left
  the team (no message for the team reaches the node after that), so the lead,
  whose name it is, clears it as it leaves; the model was changed first, gained
  the property FREE (the lead finds its name free once it has left), and the
  first design fails it as `last_clears`. *Undo*: the last holder's give-back
  clears the entry again and the lead does not; the free mask's map read then
  leaves the number out until the last holder has left.
- **A pool found empty while a block may be about to come back makes the round
  busy, not dry: in the split's first round, and while a block is on its way
  back.** Found by `shmem.team-split.dry.L2`: a would-be lead claims before the
  reduction, and the block a destroyed team is giving back is not in the bitmap
  until its last member there has left. The first build's test - a block with
  fewer holders than it was named with - held for ever of a block given back
  and claimed again, the count never zeroed (the adversarial read, below); as
  rebuilt, a claim zeroes the count, so that fewer holders than a nonzero count
  says "on its way back" - a team leaving the block, or its last holder's
  give-back not yet in the bitmap - and a zero count "claimed", and the first
  round is busy too, its claim having come before any parent PE's arrival
  (`pool_dry_model.py`). *Undo*: the round is dry whenever the claim fails, and
  a split made right after a destroy can fail for the destroyed team's block.
- **`SHMEM_THREAD_MULTIPLE` splits take their numbers pending between two
  reductions** (D7 as redesigned), in place of the proposal's mask token, which
  the adversarial read showed deadlocks a valid program (below). The cost is a
  second reduction per round under `SHMEM_THREAD_MULTIPLE` only.
  *Undo*: none that keeps such programs working; the token is the design
  replaced. A cheaper variant, a split that takes its numbers pending only when
  another split of its PE is active, was not taken: it needs the active marks
  the token had, and its saving is one reduction in a rare call.
- **A team's local release waits until its RELEASEs have left the control
  queues** (§3.8; `progress.c`, `release_local`), so that the reply pool's
  bound - a degree's worth of forwards a block - holds when a block is given
  back and claimed again; the adversarial read found the first build only
  drained them best-effort. It changes nothing while a connection has room;
  `transport.am.releases_held` counts the times it is held back. *Undo*: apply the
  release whatever the drain left, and size the pool's term for a block's old
  forwards beside its new ones, which no fixed multiple bounds.
- **A number's counters are zeroed only when the PE has held it before in the
  session**: the heap object is new each session and only teams a PE holds a
  number for write its counters, so a first take touches none of the pages a
  barrier-only team never uses. *Undo*: zero at every take.
- **The engine reads the map, and the block's generation, on every barrier
  message** rather than once per name (D3 says once): two loads beside the
  message's work, and no cache of names to keep in step with the map.
  *Undo*: a per-name cache in the engine, invalidated by the generation.
- **Three counters**, `shmem.team_split.calls`, `shmem.team_split.busy_rounds`
  and `shmem.team_split.lost_rounds`, so that a test can show a round ran again
  (`shmem.team-threads`); and `transport.am.releases_held`. *Undo*: drop them.
- **A wire revision in the handler checksum** (`OSOE_AM_WIRE_REVISION`, Part 1's
  adversarial read): a binary from before step D, whose barrier message's fourth
  word was reserved, refuses to form a job with one after it.

## The adversarial read (at `4178975`)

A fresh agent read the part's diff adversarially before its merge, with a
probe program it built in the VM. Each finding, and what became of it:

1. **High: a split near a dry pool never returned.** The test of a block on
   its way back - fewer holders than it was named with - held for ever of a
   block given back and claimed again, its count never zeroed; a claim is kept
   across busy rounds and named only after the reduction, so a `split_2d` whose
   lead held one claimed block and found no second ran busy rounds for ever,
   and so did two splits on one node and two threads near a dry pool. *Fixed*:
   a claim zeroes the count, and the test reads a nonzero count with fewer
   holders as on its way back and a zero count as claimed
   (`osoe_node_team_returning`), modelled first (`pool_dry_model.py`, whose
   `first` breakage is the build the read found and `odd_only` the fix it
   suggested, which misses a block between its generation's raise and its
   bit); and the first round's dry pool made busy too. Tested by
   `shmem.team-split.dry.L2`'s `split_2d` near the dry pool and its late
   destroy, which the first build's test hangs (below, mutations).
2. **High: D7's mask token deadlocked a valid `SHMEM_THREAD_MULTIPLE`
   program**: held across the split's reduction, which waits for every parent
   PE, by a split whose parent PE waits in another team's collective for a PE
   that runs the lower split's busy rounds for ever (the read's program: parents
   {0, 1} and {0, 2}, and a sync of {1, 2}). The read left the fix to the author
   as a design decision; it is taken here, as the brief's "decide, and record the
   decision with its undo" asks of what it does not list: the token is gone,
   and nothing is held across the first reduction (choices, above;
   `split_numbers_model.py`, whose `token` case reproduces the deadlock).
   Tested by `shmem.team-threads`, the read's program on three PEs and more,
   which the first build hangs.
3. **The README's thread bullet was wrong twice** - priority went by parent
   number, not by which parent was made first, and the lowest split did wait.
   Rewritten for the new design.
4. **Design §3.8's "a team's forwards are drained before its members are
   released"** was best-effort: `drain_into` stops when a connection has no
   room, and `release_local` released anyway. *Fixed* in the code, so that the
   sentence holds (choices, above).
5. **A team of one PE claims a pool block**, though it uses none of the node's
   state: a `split_2d` with `xrange` 1 makes a team of one per PE, so on a node
   of 256 PEs four of them empty the default pool. *Documented* in the
   README's limits; not changed in this part, since a team without a block
   changes what its published epoch word names, which `osoe-stat`, the
   invariants and the tests read. The follow-up: a team of one claims no block,
   maps no name and publishes its epoch word without a block.
6. **Cosmetic**: a block's tree state reset at naming said parent 0 and lowest
   missing child 0; now `OSOE_COLLECTIVE_TREE_NONE`, which `osoe-stat` reads as
   none.

The read found the rest sound, among it: no number or name shared by two live
teams on a PE; every parent PE taking the same branch of the split; the memory
orders of the map entry, the held bits and the counters; "why giving a number
back is safe" for every collective's writes; the engine's relearning; and the
handles' arithmetic.

## The adversarial read of the rebuilt part (at `33dac08`)

A second fresh agent read the range `4178975..33dac08` - the posted bitmap,
which the first read had not, and both rebuilds - with probes in the VM and
cases of its own added to the models. What it found, and what became of each:

1. **High: a split's rounds followed each PE's own thread level.** The levels
   of a job's PEs may differ (spec §9.2); with PE 0 under
   `SHMEM_THREAD_MULTIPLE` and the rest below it, PE 0 ran a second reduction
   the others' closing sync met, and the split ended with "no block is mapped"
   or as a deadlock. *Fixed*: the first reduction's marks carry a bit for it,
   and every parent PE takes the pending path if any parent PE has the level
   (`team.c`, `MARK_NOT_CONCURRENT`). Tested by `shmem.team-levels.*`, PE 0 at
   `MULTIPLE` and the rest at `SERIALIZED`.
2. **Medium: the README promised a destroyed team's place to a split it does
   not always reach.** A team whose member on a node is outside the split's
   parent, past the destroy's sync and not yet gone - preempted, say - holds its
   block with every holder, and a split ordered after the destroy through
   another member can fail dry for it. The read suggested a flag the members set
   before arriving in the destroy's sync; modelled first, it makes a valid
   program run its rounds for ever - a split waiting for a team whose destroy
   waits for the split (`pool_dry_model.py`'s `departing` breakage, `TRAPPED`
   in `split_first`) - and the node has no test that tells the two apart.
   *Narrowed*: the README and design §5.7 say what holds, and the model checks
   it (`remote`).
3. **Low**: `transport.am.releases_held` counts the times a release is held
   back, twice a pass, which its description now says; `release_local`'s note
   on the abort corrected.
4. **Low**: the committed record was a commit behind its tree - superseded here.
5. **Low**: the wait for a pending word now says where it is blocked, for
   `osoe-stat`; its unfairness under a schedule that keeps passing it over is
   stated beside it; and "nothing is held across a reduction" is narrowed to
   the first.

The read found sound: the pending protocol's liveness (waits only to strictly
higher parent numbers, the same on every PE; the second reduction needing
nothing outside the split; every exit clearing its words), the dry rule (no
busy-for-ever path), the release hold (every pop decrements; the relearn's
fatal; nothing in the drain waits on local members), the posted bitmap's
orders, and the memory orders against §3.16 - with three-split cycles of its
own added to the model, unviolated to 18 million states.

## A third read, of the second read's fixes

Before the merge a third fresh agent read what the second read's fixes changed
(`33dac08` to the tree as merged), with the round-four review's own fixes beside
it, and found nothing high or medium. Its lows, each fixed here: the README
said a member outside the split's parent could still make a split fail, which a
would-be lead that was in the destroyed team rules out - now the team's members
there all outside the parent, none of them yet letting go, as §5.7 has it; the
README's cost of a team named one reduction, and now the second under
`SHMEM_THREAD_MULTIPLE`; five comments in `team.c` still said "under
`SHMEM_THREAD_MULTIPLE`" where the rule is any parent PE's level; the mixed-level
test's registration had split another test's comment from its own; the test
read `OSOE_PROCID` alone, where `launcher.h` reads srun's name too; and the
wait for a pending word published its blocked state before its first look and
was not counted by the deadlock detector - now it publishes once it is about
to sleep and counts as every library wait does (`base/sweep.h`), which its
comment had claimed. It found sound every branch of the split, which reads
only the reductions' results; a parent PE outside the new team, and one below
`SHMEM_THREAD_MULTIPLE`, taking the pending path; the pending waits' liveness
with mixed levels; `shmem.team-levels` a valid program that catches the
per-PE rule by design - and, run itself, the pool model, whose `remote` case
fails at 47 states without its allowance in LEGIT.

## Acceptance (the brief's list)

On a release build in the `osoe-load` VM, 70 PEs on one node and on ten logical
nodes of seven, `SHMEM_SYMMETRIC_SIZE=4m` (`build/scratch/teams-d/acceptance/`),
at the default `OSOE_MAX_TEAMS` of 64 and at 128; "skipped" counts the splits
that failed, each an iteration whose collectives the program then leaves out:

| Program | At 64 | At 128 |
|---|---|---|
| `shmem_team_reuse_teams` | exit 0, nothing skipped (before: 3 ranks failed) | the same |
| `shmem_team_split_2d` | exit 0, nothing skipped | the same |
| `shmem_team_max` | 64 teams made on every PE, as it asks: three splits fail when PE 0 holds 61, and after each the program destroys its oldest and the retry succeeds | 64, none failing |
| `reduce_active_set`, `broadcast_active_set`, `repeated_barriers` | **9 of 70 skipped**, as before step D | nothing skipped |
| `collect_active_set` | nothing skipped | the same |

And the per-node split of `SHMEM_TEAM_SHARED` (the plan's P1 case,
`build/scratch/review-r3/scaling/split_shared.c`), one PE a logical node: every
node's team valid at 61, 62 and 64 nodes - before step D, 61 at each, the rest
`SHMEM_TEAM_INVALID`. A whole-job `split_2d` takes two numbers
(`shmem.team-split.exhaust.*`). `osoe-pacer teams --teams-exhaust`, 16 PEs on
four logical nodes: 61 splits of the whole team, then the split returning
nonzero on every PE; the teams destroyed, one more split succeeding on every PE
(`build/scratch/teams-d/part2/verify/pacer2-exhaust.txt`).

**The three that still skip at the default** split a team every iteration and
never destroy it, so at 70 PEs world PE 69 is in 70 live teams at the end -
more than the 61 a PE may hold at 64. The brief lists "a corpus or SOS program
that holds more than 64 teams on one PE through D" as a stop-and-ask; asked, the
author kept the default at 64 through D (2026-10-03): they skip 9 of 70
iterations at 70 PEs until step E makes the default 256, and run whole at 128.

## The split's cost (the brief's stop-and-ask: no more than a hop dearer)

`split-timing.c` (`build/scratch/teams-d/timing/`): fifty splits of the world
back to back, each after a barrier, on release builds of the first part
(before: the claim on world PE 0 and the broadcast) and of this one (after: the
reduction), five alternating series each, one PE a logical node, in the `osoe`
VM. PE 0's median per split, the median of the five series' medians:

| Nodes | Before | After |
|---|---|---|
| 5 | 211 µs (152 to 247) | 196 µs (175 to 237) |
| 8 | 204 µs (179 to 290) | 220 µs (204 to 238) |

The difference is within the series' spread and below one hop of the VM's
loopback (about 27 µs a round trip, the review's Q3): the reduction costs what
the claim and the broadcast did. A split followed by its destroy, two hundred
times: 162 µs at 5 nodes and 192 µs at 8.

**And under `SHMEM_THREAD_MULTIPLE`, after the adversarial read's redesign**
(`split-timing2.c`, `results2.txt`): the same fifty splits, five alternating
series each way, the first build (`4178975`: the token, one reduction) against
the rebuilt (two reductions under `MULTIPLE`), at both levels, the VM busier
that hour than the first measurement's:

| Level, nodes | First build | Rebuilt |
|---|---|---|
| default, 5 | 306 µs | 284 µs |
| default, 8 | 447 µs | 452 µs |
| `MULTIPLE`, 5 | 282 µs | 439 µs |
| `MULTIPLE`, 8 | 446 µs | 663 µs |

At the default level nothing moves. Under `MULTIPLE` the second reduction
costs what a reduction does, half again a split's time. The brief's limit - the
mask reduction no more than a hop dearer than the claim and the broadcast -
holds; this is D7's fix, and the choice is recorded above. The cost could be
won back by folding the second reduction into the split's closing sync, which
synchronizes the same members: each lead would name its block and publish the
record before knowing whether every member took the number, and a lost round
would have to unmap and give back a block no member has yet found. Not taken:
a split is rare, under `MULTIPLE` rarer, and the undo path is new protocol to
model. *Undo*: that fold, modelled first.

## Tests

New or changed in this part (`shmem/tests/`, `tools/pacer/`):

- **`shmem.team-split.*`'s modes** (`test-team-split.c`), each judged by what
  PE 0 says last: `exhaust`, rewritten for numbers a PE's own - two teams with no
  PE in common under one number, a grid taking two, a `split_2d` needing two
  where every PE has one failing alike, PE 0's last number taken and none free
  until both teams under one number are destroyed; `too-many`, its messages;
  `dry`, which replaces `shmem.team-blocks.dry` - five blocks taken, a sixth and
  a team of one failing dry, then a `split_2d` needing three blocks where one is
  free failing too while its lead holds the one it claimed (the adversarial
  read's first defect, which hung there), the destroyed team's block serving the
  next, and, with the pool full, a team PE 1 destroys late serving a split PE 0
  leads and calls at once (a dry first round is busy); `stale-handle`, new, a
  destroyed team's handle used once its number names another team ending the
  job by name; and `reuse`, new - at L4, `0-0-1-1`, `0-1-2-0-1-0` and in tier 3 -
  a team taking a destroyed team's block under another name and generation,
  which the progress engine and the collectives follow.
- **`shmem.team-threads.*`** (`test-team-threads.c`, new), at L1, L2, L4,
  `0-1`, `0-1-2`, `0-0-1-1` and in tier 3 on `0-1`: two threads of each PE
  split two parents together twelve times, both splits succeeding and each team
  working; and on three PEs or more the adversarial read's program, which the
  first design deadlocked - its split of `{0, 2}` losing its first round to
  `{0, 1}`'s number, which PE 0's `shmem.team_split.lost_rounds` shows.
- **`shmem.team-levels.*`** (`test-team-levels.c`, new, after the second
  adversarial read), at L1, L2, L4, `0-1` and, outside tsan, `0-0-1-1`: PE 0
  at `SHMEM_THREAD_MULTIPLE` and the rest at `SERIALIZED`, eight rounds of a
  split of the world, a `split_2d` of it and a split PE 0 is not in, each team
  checked by a sum over it. With each PE choosing its rounds by its own level,
  as the rebuild did, it fails at L2 and L4 with "no block is mapped" and on
  two nodes as a deadlock, three runs in three (`own_level`,
  `build/scratch/teams-d/part2/verify/v3.txt`); at L1, one PE, no level differs.
- **`shmem.team-storage.*`**: team 4 checked alive, and its record gone after
  its destroy; `invariants.h`'s `osoe_test_team_block` reads a PE's epoch word
  for the block it names.
- **`shmem.reply-pool.overflow`**, re-sized for the pool's term of a degree a
  block: 32 PEs, the overflowing node's `OSOE_TEAM_BLOCKS` 8.
- **`osoe-pacer teams`**: a strided team renewed every third round, now that a
  number is given back, and `--teams-exhaust` splitting again after its
  destroys; two planted defects, `teams-renew` and `teams-back`, each caught.

## Mutations

Each of the models' breakages as the line of code it stands for
(`build/scratch/teams-d/part2/mutations/mutate.py`), each run three times
against the 45 team tests (`team-split`, `team-threads`, `team-storage`,
`allgather`, `team.*`, `team-blocks`, `job.disagree.team-blocks`) in the debug
preset, on the tree as rebuilt after the adversarial read (`33dac08`):

| Mutation | Caught |
|---|---|
| `no_pending`: each member takes the agreed numbers at once, nothing pending | 3 of 3 runs, `shmem.team-threads.*` |
| `higher_waits`: a take waits for any split's pending word, a lower one's too | 3 of 3, `shmem.team-threads.*` (each times out) |
| `first`: the first build's test of a block on its way back | 3 of 3, `shmem.team-split.dry.L2` (times out) |
| `first_round_final`: a pool found empty in the first round fails the split | 3 of 3, `shmem.team-split.dry.L2` |
| `no_retry`: a busy round ends the split as failed | 3 of 3, `shmem.team-split.dry.L2` |
| `no_relearn`: the engine keeps the team it first learnt under a block | 3 of 3, the team tests across nodes |
| `no_relearn_generic`: the generic phase keeps its bases across a block's teams | 3 of 3, the same |
| `no_holders`: the lead gives the block back as it leaves, whoever holds it | 2 of 3, `shmem.team-split.force-remote.0-0-1-1` |
| `last_clears`: the first design, the last holder clearing the map entry | 2 of 3, `too-many`, `stale-handle` |
| `release_first`: a pending word cleared before the number is held | 0 of 3 |
| `lower_fails`: a take fails on a higher split's pending word too | 0 of 3 |
| `zero_at_leave`: the last holder zeroes the members, the claim not | 0 of 3 |
| `map_early`: every member clears the map entry as it leaves | 0 of 3 |
| `free_first`: the block back in the bitmap before its generation is raised | 0 of 3 |

The five not caught each need an interleaving no test forces: `release_first`
another split's take between a keeper's clear of its pending word and its held
bit; `lower_fails` the same two splits colliding on two PEs in opposite orders,
round after round - a livelock only an adversarial schedule keeps up;
`zero_at_leave` a lead's claim failing just before the last holder's bit and
its look just after the generation's raise; `map_early` a member of an old team
leaving after its lead has made a new team under its name - and the lead's next
split, if the member is in its parent, waits for that member in the reduction,
after it has left; `free_first` another lead claiming and naming the block
between the last holder's bit and its raise of the generation. The models are
what cover them: `split_numbers_model.py`'s `release_first` and `lower_fails`,
`pool_dry_model.py`'s `zero_at_leave`, and `team_block_model.py`'s `map_early`
and `free_first`. Two were caught three times in three on the first build of
this part and twice now, by the same tests: `no_holders` and `last_clears` hang
on how slowly a member leaves, which the rebuilt split's rounds change. The
first build's three token mutations (`no_token`, `lowest_yields`, `no_retry` of
the token's busy round) went with the token; the token design itself, run whole
(`c8f52e6`) against the rebuilt `shmem.team-threads`, deadlocks the read's
program at four PEs and on `0-0-1-1` (the test's 60 s timeout), and the first
build's dry-pool test, put back, hangs `shmem.team-split.dry.L2`
(`build/scratch/teams-d/part2/mutations2/`).

## Verification

On the tree as rebuilt after the adversarial read (`33dac08`), in the VMs, each
run under the lock (`build/scratch/teams-d/part2/verify/`):

- **The four presets**, the floor pins on: debug 682 of 682, release 682 of
  682, asan 681 of 681, tsan 460 of 460.
- **Tier 3**: 113 of 113.
- **The models**: `team_block`, `split_numbers`, `pool_dry` and
  `posted_bitmap`, every case as expected (`models-d.txt`; the table above is
  the run after the second read, below).
- **SOS**: the team programs on `0,1` and `0,0,1,1`, 26 clean, the two failing
  runs `shmem_team_b2b_collectives`'s, which the runbook names; the whole suite
  at 4 × 2, 161 clean, the two that do not build and the two failing runs the
  runbook's. At 70 PEs on ten logical nodes the first build gave 149 clean and
  14 failing, none of them a team's, and on this tree each is accounted for:
  six ran out of the 8 MiB heap the sweep gave them and pass with 40 MiB
  (`bigget`, `bigput`, `shmalloc`, `shmem_calloc`, `shrealloc`, `mandelbrot`);
  five are SOS's own "requires less than 32 PEs"; `shmemalign` and
  `shmem_team_b2b_collectives` are the runbook's; and `spam`, which ran out of
  the sweep's 400 s, passes given 1,500.
- **Acceptance** (above): the same on this tree, line for line, as on the first
  build (`build/scratch/teams-d/acceptance/results.txt`).
- **`osoe-pacer`**, 16 PEs on four logical nodes: `teams` for 300 s,
  `failures=0` with 50,382 renewals; `--teams-exhaust` (above); and an hour of
  `mix`: 310 periods - the six team exercises on four teams, and the sort and
  the alltoall on the world every fourth - and 1,009 exercise summaries, every
  one `failures=0`, ending `osoe-pacer failures=0 seconds=3609.6`; at the
  pacer's `SHMEM_THREAD_SERIALIZED`, on `33dac08`'s code, before the second
  read's fixes, which change nothing for a split no PE of whose parent has
  `SHMEM_THREAD_MULTIPLE`. The teams run's rate was 42% below the first
  build's - in another hour, the host's three VMs all busy - so the first
  build, this tree and this tree without the release hold of §3.8 ran
  alternately in one VM, a minute each, three rounds
  (`build/scratch/teams-d/part2/ab/`): 14,377, 13,948 and 16,635 collectives a
  second on the first build, 18,192, 17,753 and 25,581 on this tree, 17,681,
  19,232 and 21,565 without the hold - the hold within the rounds' noise, and
  this tree about a quarter faster than the first build, as the posted
  bitmap's scan would make it.
- **Mutations** and **the arrival scan's cost**: above.

After the second read, on the tree as merged - its fixes in, rebased onto the
round-four review's fixes and the first part as squashed - in the `osoe` VM,
the host otherwise quiet (`build/scratch/merge-d/part2-final/`):

- **The four presets**, the floor pins on: debug 690 of 690, release 690 of
  690, asan 687 of 687, tsan 465 of 465.
- **Tier 3**: 113 of 113.
- **The models**, on the Mac: every case as expected, the pool's three new
  among them (the table above;
  `build/scratch/teams-d/part2/verify/models-final.txt`).
- **`shmem.team-levels.*`** three times, and its mutation (Tests, above).
- **The part's first commit alone**, without the pacer's that follows it: it
  builds without a warning, and its 158 team and pacer tests pass, the pacer
  as it was before its renewal (`build/scratch/merge-d/part2/s-check.txt`).
