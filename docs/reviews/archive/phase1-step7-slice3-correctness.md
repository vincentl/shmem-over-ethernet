# Round five, question 2: is osoe correct, at step D's merge point?

Review of `main` at `d8a651d`, run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, question 2 only, with the lead's
changes: the tree the team redesign's step D left
(`docs/decisions/2026-10-03-team-storage.md`, `2026-10-03-team-allocation.md`;
`docs/design/team-resources-proposal.md` §3; design §5.7), with the previous
round's fixes (`docs/decisions/2026-10-04-review-fixes.md`) and its findings in
this question (`phase1-release-fixes-correctness.md`, F1 to F5 and its note on
the team redesign) checked as claims. Step E is not built; what this review
has to say about it is kept apart at the end. Labels: *documented*, *observed in
source*, *measured*, *recalled*. Every run was made in this session, in the
`osoe-load` VM (Debian 13, kernel 6.12, aarch64, 4 cores) from a copy of the
tree in its `/tmp`, under the VM's lock; the models ran on the host (Python
3.14.7). The two probes written for this review (`test-rvhold.c`,
`test-rvsplit.c`) and the VM logs are outside the repository, in the reviewer's
scratch directory, which the lead has; the VM copies are removed.

## Verdict

**Correct as built: nothing in this question needs fixing before release, and
step D does not stand in the way of it.** The protocols step D added hold as
far as I could push them, on the source, in the models and under runs made to
break them:

- a number agreed by an AND over the parent, every branch of the split decided
  from the reductions' results alone, so that every parent PE runs the same
  rounds and returns the same status;
- what a number owns reset when a PE takes it again, and the generic phase's
  per-block state emptied when its block's generation moves, consistent in
  every combination of block and number I could construct - and the reset is
  tested: removed, 11 team tests fail;
- a block's life in the model's order, with the code's own guard (an even
  generation at naming is fatal) against the one ordering a test cannot force;
- the engine following the generation, its forwards self-contained, and no
  write of the engine's to a block after the release that lets its members go;
- the posted bitmap right by C11's argument as well as in its SC model;
- the redesigned D7: waits only to strictly higher parent numbers, every exit
  clearing its pending words, and a stress of four threads a PE splitting at
  once - 1,204 splits a PE a run, up to two lost rounds a split - passing on
  three maps under eight busy loops and under ThreadSanitizer.

The models reproduce the record's state counts exactly, and every listed
breakage fails by the property it names. The whole suite passes under eight
busy loops (690 of 690), the newest tests 1,900 runs under the same load,
`osoe-pacer teams` and `mix` with `failures=0`. The previous round's F1 fix
holds, and its note on the team redesign - a forward queued across a block's
reset - is closed in the code as it recommended.

The one finding above minor is about evidence, not code: the release hold the
adversarial read added - a team's members held until its RELEASEs leave the
control queues - is right, engages in practice, and is the only thing standing
between a valid program and a fatal, yet no test sees it and no model has it;
the suite passes with it removed (G1). Then three minors: step D names
`relaxed` on 37 protocol accesses, which §3.16 forbids (each one ordered by
another word, so the rule, not the code, is what is now false; G2); a model
check no breakage reaches (G3); a stale count in the README (G4). For this
question: release once step E is built and has had the same runs; take G1's
test when convenient.

## Findings, ranked

### G1. The release hold has no test and no model, and the suite passes without it (after release; high confidence; *measured*)

- **Location**: `shmem/src/progress.c:1211-1240` (`release_local`; the hold is
  the `releases_queued` test at `:1227`), `:240-266` (`forward`,
  `release_left_queue`), `:349-375` (`known_team`'s fatal on relearning a block
  with RELEASEs still queued); `docs/decisions/2026-10-03-team-allocation.md`,
  "Choices made without the author", the fourth item, and the adversarial
  read's item 4.
- **What it guards** (*observed in source*): a forwarding node's RELEASE waits
  in its control queue while the connection toward the child has no room. Before
  the hold, the node applied its own release anyway; its members could then
  leave the team, give its block back, and a new team claim it - and the
  engine, relearning the block at the new generation, finds the old team's
  RELEASE still counted, and ends the job, as `known_team` was written to.
- **Scenario**: three nodes. Node 0 roots team T over nodes 0 and 1, and a PE of
  node 0 streams bulk data to node 1 while node 1's lead process is slow to
  read: descheduled, or stopped. T is destroyed; node 0's lead of T then makes
  a team over nodes 0 and 2, which on node 0 takes T's block (claims come from
  the top of the pool), and synchronizes on it.
- **Evidence** (*measured*): a probe of that scenario (`test-rvhold.c`):
  four PEs on `0,0,1,2`, parents P1 = {0, 2} and Q = {0, 3}; each round T =
  split(P1), three syncs, destroy, then T2 = split(Q), a sync, destroy; while PE
  2 waits in T's destroy with its ARRIVE sent, a helper thread of PE 3 stops PE
  2's process - node 1's lead and agent - for 400 ms, PE 1 puts 32 MiB to PE 2,
  and only then does PE 0 arrive.

  | Library | Runs | Result |
  |---|---|---|
  | as built | 3 | all pass; node 0's `transport.am.releases_held` 107, 104, 118; T's destroy on PE 0 held about 300 ms in most rounds, until PE 2 was continued |
  | the hold removed (`if (0 && t->releases_queued ...)`) | 3 | all end in round 1: "node 0: team 5's block 1021 holds another team (generation 3, learnt at 1) while epoch 0 is open, 0's release is unapplied or 1 of its RELEASEs are queued: a message for the team arrived after its members left it, or they left before its forwards" |
  | the hold removed, the whole release suite | 1 | 689 of 690: the one failure `shmem.strict-c11`, which compiled my probe in the copied tree and stopped on its unused variable - not the mutation |

  No test reads `transport.am.releases_held` (*observed in source*: the
  counter's only uses are `metrics.c:396`, `node.h:483`, `progress.c:1229`).
- **Why it matters**: the project's rule is that a mechanism under an ordering
  or a wake is shown to fail when removed. This one, added by the adversarial
  read, has no test that fails, and no model: `team_block_model.py`'s engine has
  no control queue, and the model "takes as given" that nothing for a team is in
  flight once its destroy's sync completes (its docstring). Its record says only
  that "it changes nothing while a connection has room" (*documented*). The
  previous round recommended `team_block_model.py` gain the case of a forward
  queued across the reset; the code took the recommendation's other half
  instead, a forward's descriptor carrying the message as it will be sent -
  number, receiving lead and block, computed at queueing (`forward`,
  `drain_into:776`) - and added the hold. Both are sound; neither is checked. Rated after release:
  the code is right, a regression would end loudly naming the broken argument,
  its trigger needs bulk traffic to a slow node across a destroy and a reuse,
  and step E does not touch the engine.
- **Fix**: the probe as a tier-2 test - or, simpler than its side channel and
  `SIGSTOP`, a test hook that withholds room on one connection - asserting
  `releases_held` > 0 and the job's success, and failing on the mutation above;
  and in `team_block_model.py`, a RELEASE that may wait in a queue across the
  destroy, with the hold's removal as a breakage.

### G2. Step D names `relaxed` on 37 protocol accesses and adds an implicit-order load; each is ordered by another word, but §3.16 forbids them (minor; high confidence; *observed in source*)

- **Location**: design §3.16 (`shmem-ethernet-design.md:2304-2310`: "Implicit-order
  calls ... and `memory_order_relaxed` are forbidden on protocol words", and
  "`relaxed` nowhere on a protocol word"); `node.h:713-724`, the block name's
  comment, which argues relaxed for its fields. The sites (`git diff
  c51ba87..d8a651d`, every added line): `node.c:949-958` (the pool's start),
  `:1274` (a claim zeroing `members`), `:1297-1307` (a named block's tree state
  reset), `:1326-1340` (naming), `:1387-1388` (the lead's leave);
  `progress-host.c:231, 254, 268`; `internode-generic.c:234, 419, 1576, 1582`;
  `internode-am.c:110`; `team.c:626, 794`; `barrier.c:127`; and `team.c:624`'s
  `atomic_load` with no order, the library's only implicit-order call. Before
  step D the tree had one relaxed access on a protocol word (`reply.h:239`, the
  previous rounds' K4, carried) and no implicit-order call.
- **Each traced** (*observed in source*): the naming stores are the single
  writer's, published by the map entry's release, which every member acquires
  after the split's sync, and by `record.published`'s release, which the engine
  acquires before reading the name (`team_of_record`) or the map's acquire
  before reading the generation (`team_by_name`); a member's reads of its own
  block's generation and number (`internode-am.c:110`, `team_state`,
  `number_of`, `of_block`, `leave`) follow its acquire of the map entry or its
  own naming; the pool's start precedes `ready`'s release; `barrier.c:127` is a
  message. Two need more than a local argument. A claim's zeroing of `members`
  can be read late by another PE's `osoe_node_team_returning`: that costs a busy
  round, or falls in the case the README already allows to fail (below, "The
  dry rule"). And the zeroing of a number's counters at take
  (`internode-generic.c:1576-1582`) has as its next writer, for a team across
  nodes, this node's agent applying a remote node's add: the order runs through
  the agent's acquire of the parent's arrival handoff, which it must take before
  the split's sync can complete - not through the socket - and nothing beside
  the stores says so.
- **Why it matters**: no failure - the code is right. But §3.16 is the
  project's stated discipline, and it now says something false of 37 sites; the
  allocation record says the second read found "the memory orders against §3.16"
  sound (*documented*, `:334`), which by §3.16's own words they are not. A
  reader of §3.16 takes them for defects, or takes its table for every shape in
  use.
- **Fix**: a row in §3.16's table for fields published by another word -
  written before its release, read after its acquire, atomic only because a
  racy reader exists (`osoe-stat`, `returning`), so that relaxed is C11's right
  order - naming these sites and the counters' argument above; or acquire and
  release on them, which costs nothing on these paths. `team.c:624` names an
  order either way.

### G3. `posted_bitmap_model.py`'s ORDER check is reached by none of its breakages (minor; high confidence; *measured*)

- **Location**: `tools/modelcheck/posted_bitmap_model.py`, the check ORDER
  ("each team's arrivals reach the root once each, in epoch order") and its four
  breakages; `internode-am.c:166-177` (`take_arrival`'s comparison with the word
  last consumed, and its comment).
- **Evidence** (*measured*): every breakage fails as STUCK. Broken on purpose: an
  agent that never records what it consumed (`no_consume`) **passes**, 4,627,960
  states - in this design a bit is set once for each new word, so the agent is
  never handed a word it has taken, the comparison is redundant, and the comment
  "a bit taken whose word was taken in an earlier pass ... costs nothing"
  describes a case the design's order cannot produce. With `bits_first` as well,
  ORDER fails at 151 states ("team 0: epoch 1 forwarded after 1: an arrival was
  repeated"): the check works; no listed case shows it.
- **Fix**: that combination, or `arrival_model.py`'s `consume_reread`, as a
  breakage that fails ORDER; the comment corrected. The two other checks no
  listed breakage reaches I broke too, and both fail: `team_block_model.py`'s
  OWN when `no_holders` runs without REWRITE's check (737 states, "a member of
  team A read block 1's counters of generation 3, its own being 1"), and
  `split_numbers_model.py`'s ONE when each member takes the lowest bit of its
  own mask rather than the AND's (`own_mask`: 29 and 67 states in
  `second_low` and `second_high`).

### G4. Minor

- **The README's model count** (*observed in source*): "the 24 tests of the
  model checker" (`README.md:119`) and "the model checker's 24 tests"
  (`:782`) - 28 since step D (26 models, `get_completion.equality` and the
  selftest; `tools/modelcheck/CMakeLists.txt`), which the lead's models preset
  ran, 28 of 28. The sentence listing what is model-checked (`:786-790`) names
  none of step D's four protocols - the block's life, the split's numbers, the
  dry pool, the posted bitmap - though each is checked.
- **Carried, unchanged as planned** (*observed in source*): the previous round's
  F2 (the wait for the lead's `ready`, `node.c:1062-1105`, still has no
  `lead_ended`), F3, F4 (`probe.c:210`'s fixed 50 ms), and F5's items - the
  model tests' pass or fail without the property, the plain `launch`, and K4's
  `transport.h:470`, `reply.h:239` and §3.16's table. All are on the release
  checklist's list of what follows the release (*documented*,
  `2026-10-04-review-fixes.md`).

## Step D: what convinced me

**A number is each member PE's (D1, D2, D8).** Read whole: `team.c`'s split,
`free_mask`, `take_pending`, `wait_pending`, `finish`, `take_number`,
destroy, the handles. Every branch of the split reads only the reductions'
results - the mask, and the marks written inverted so that one AND carries
them - so every parent PE runs the same rounds and returns the same status;
non-members contribute ones; each failure path returns every claimed block
unnamed (`unshape`); a number is held before its pending word is cleared, and
given back last at destroy, after `forget`, so another thread's split that sees
it free sees the descriptor freed. Taken again, a number's counters are zeroed
(`osoe_internode_generic_number_taken`), only when the PE has held it before -
sound, since the heap object is new each session (its name carries the
session, `node.c:1498`) and only a team the PE holds a number for writes them.
*Measured*: with that zeroing removed, 11 of 76 team tests fail, `reuse.0-0-1-1`
ending "the result from the parent, from node 0's lead (PE 1), should be piece
0 (32 bytes) of epoch 3's sum ..., and is piece 0 ... of epoch 7's" - the piece
header turning a stale counter into a named end. The generic phase keeps its
bases per block, emptied when the generation moves; the counters are per
number: I found them consistent for the same block under another number, the
same number in another block, and a member's scans, which every member runs.

**A block's life (D4).** The code's order is the model's: the lead clears its
name's map entry before its own decrement; the last holder retracts the record,
raises the generation, then sets the bit; a claim zeroes `members`; naming
raises the generation to an odd value - and a namer reading an even one ends
the job (`node.c:1326-1329`), which turns `free_first`'s interleaving, which no
test forces, into a named end. Nothing writes a block after its members are
let go: the engine publishes a team's tree state at close, before the release
(`book_arrival`); after `host.release` it touches nothing of the block; a
member's chained wake precedes its own return and leave; and a progress
thread's `osoe_epoch_release_wake` that lands on a counter its block has since
been named again for is a spurious futex wake, which every wait tolerates.

**The engine (D4, D5).** It relearns a block when a handoff or a resolved name
shows another generation, and ends the job rather than overwrite a team with an
epoch open, a release unapplied or a RELEASE queued; a message's name is
resolved through the map on every message, a name with no block named, never
learnt. The posted bitmap is right by C11's argument, beyond its model's SC:
the poster's word store is sequenced before its bit, summary and wake
read-modify-writes; an agent whose acquire load of the wake word read the bump,
or whose exchange of the summary or of the bitmap word read a set bit,
synchronizes with that release and so reads the word stored before it; one
whose load preceded the bump fails the futex compare and passes again.

**D7, as redesigned.** No waits close a cycle: a take waits only for a split of
a strictly higher parent number, and a team's number is one number on all its
members - true also of `SHMEM_TEAM_SHARED`, a different team on each node under
number 1, since no PE is in two nodes' - so "higher" means the same on every
PE. A split holds pending words only between its two reductions, with every
member past the first; every exit clears what it took (a lost take, a lost
round, kept numbers), and the failing paths take none; `holder == mine` can
only be two threads splitting one parent. *Measured* (`test-rvsplit.c`):
four threads a PE, each splitting its own copy of the world 300 rounds -
halves by `split_strided`, or `split_2d` every third - summing over each team,
reducing every split's status over the parent (minimum and maximum equal), and
destroying in varying order; on `0,0,1,1`, `0,1,2,0,1,2` and `0,0,0,0`, at
`OSOE_MAX_TEAMS` 16 and 12, under eight busy loops: 1,204 split calls a PE a
run, 755 to 2,349 rounds lost a run, at 12 between 124 and 165 splits failing
alike for want of a number, and no hang, wrong sum or disagreement; under
ThreadSanitizer, 60 rounds on `0,0,0,0` and `0,0,1,1`, no report. With four
threads a split loses 0.6 to 2 rounds on average, each two reductions; the
README's "the other runs its agreement again" is the two-thread case, and the
unfairness is stated beside `wait_pending`.

**The dry rule.** C11 lets another PE read a claim's zeroing of `members`, or a
namer's stores, late. I found each such read either costs a busy round or falls
in the case the README already lists as able to fail - a team whose members on
the node are all outside the split's parent. The two cases it promises are
ordered: the would-be lead's own leave by program order, and the give-backs of
a team whose members there are all in the parent by the first round's
reduction, before the second round's claim.

**Handles (D6).** Recovered by arithmetic, never read through; a stale handle
is named, the more so when its number names another team
(`shmem.team-split.stale-handle.L2`). A torn read is possible only for a
program that uses a destroyed handle on one thread while another takes its
number - undefined by the specification.

## The models

All four of step D's, on the host: every case as the record's table gives it,
state counts identical (*measured*).

| Model | Design cases | Breakages | Time |
|---|---|---|---|
| `team_block_model.py` | 122,306, ok with liveness | `no_holders` 298 REWRITE; `no_relearn` 98 ENGINE; `map_early` 111,224 NAMED; `last_clears` 293 FREE; `free_first` 671 NAMED | 7 s |
| `split_numbers_model.py` | `shared` 194,051; `crossed` 12,798; `two_low` 306,010; `two_high` 241,197; `second_low` 69,570; `second_high` 77,742; all ok with liveness | `token` 15,096 TRAPPED; `no_pending` 2,004 and `release_first` 6,350 UNIQUE; `higher_waits` 14,593 and 17,638 STUCK; `lower_fails` 12,811 LOWEST | 35 s |
| `pool_dry_model.py` | `member` 2,075, `parent` 90, `remote` 62, `split_first` 52, `two` 13, ok with liveness | `first` 8 TRAPPED; `odd_only` 53, `zero_at_leave` 54, `first_round_final` 152 LEGIT; `departing` 73 TRAPPED | 0.1 s |
| `posted_bitmap_model.py` | 4,627,960, ok with liveness | `summary_first` 157, `wake_first` 72, `bits_first` 159, `clear_after` 243, all STUCK | 146 s |

Every breakage fails by the property its docstring names; the checks no
breakage reaches are G3's. The models are checked under SC; I found no D
protocol whose C orders depend on more than message passing and
read-modify-writes, so TSO or aarch64 adds no outcome the C11 arguments above
do not cover. `abort`, `abort_exit` and `tree_barrier` I did not re-run: the
lead's models preset ran all 28 on this tree.

**Protocols without a model**: the release hold (G1). And, carried from the
previous round, the node's own start-up over `/dev/shm`.

## The previous round's findings, as fixed

| | Finding | What the fixes did | Holds? |
|---|---|---|---|
| F1 | a start by the same ids bound the job tables and connection area a failed start left | a non-lead takes either only if its creator is the creator of the control segment it joined, and otherwise waits for its lead to replace it (`job.c:259-287`, `connection-area.c`'s `made_by` and leftover wait); three `shmem.leftovers.restart` tests | **Yes**: read whole; the waits sweep and end on a dead lead; the restart tests passed in the whole suite under eight busy loops. The record broke both halves, 3 of 3 each; I did not repeat that. |
| Note | the team redesign: a forward queued across its block's reset | the descriptor carries number, receiving lead and block, computed at queueing; the team's members held until its RELEASEs leave the queues | **In part**: the code is as recommended, and sound; the model case was not added, and the hold has no test (G1). |
| F2-F5 | | after release or minor, on the release checklist | Unchanged, as planned (G4). |

## For step E (kept apart: not built)

Read: proposal §4 and `docs/prompts/phase1-step7-slice4-prompt.md` (on `main`
since `c306221`). One thing the brief should make certain, because D made it
load-bearing:

- **A stale grant under a number taken again.** E's grant line and counter are
  per number in the child's internal region, and the brief zeroes them at take
  with the rest (its Part 1 item 6). That zeroing is the only guard: E4's check
  of a grant's "epoch and identity" cannot tell the previous team's last grant
  under the same number from this one's, since every team's epochs restart at
  1, the number is the same, and the block's generation is node-local and
  differs between nodes. If a team's first large reduction meets a grant line
  left by a team that ended at the same epoch with the same operation, the
  child sends its pieces into the lane that grant named - perhaps bound by then
  to another collective of the parent's thread - where only the piece header's
  check, and only if the stray piece is read before it is overwritten, would
  name it; D's stale counters meet that check every time (above). The brief's
  mutations come from model breakages, and `lane_model.py` does not take
  numbers again, so nothing in it would show the grant line's zeroing removed.
  Recommend: that mutation, and a test that reaches it - a number taken again
  across nodes by a team whose first large reduction, and first large scan
  (Part 2), match the previous team's last in epoch and operation - beside
  today's `team-split.reuse` tests, which cover D's counters.

## Runs

| Run | Result |
|---|---|
| the four D models, every case (host) | as the record's table, state counts identical (The models) |
| check breakages of my own (host) | OWN: 737 states, VIOLATION; ONE: 29 and 67, VIOLATION; ORDER: `no_consume` passes (4,627,960), `bits_first` with it 151, VIOLATION |
| release suite, eight busy loops | 690 of 690, 253.5 s |
| the 81 team tests (`team`, `allgather`, `reply-pool`, `job.disagree.team`, `heap-shift.team`, `context.misuse`, `refuse.team`), four runs each, eight busy loops | 324 of 324 |
| the 19 newest - `team-threads`, `team-levels`, `team-split.dry`, `.stale-handle`, `.reuse`, `.too-many`, `.exhaust` - 100 runs each, eight busy loops | 1,900 of 1,900, 53 s |
| `osoe-pacer teams` 120 s and `mix` 240 s, 8 PEs on `0,1,2,3,0,1,2,3`, four busy loops | `failures=0` each; 67,510 team rounds, 22,503 renewals |
| split stress (`test-rvsplit.c`), six runs, eight busy loops | all pass (D7 above) |
| the same under ThreadSanitizer, two maps | no report |
| release-hold probe (`test-rvhold.c`), as built | 3 of 3 pass; `releases_held` 107, 104, 118 |
| the same, the hold removed | 3 of 3 end in round 1 on the relearn fatal |
| the release suite, the hold removed | 689 of 690, the one the probe's own warning |
| the team tests, the zeroing at take removed | 11 of 76 fail |

The lead's verification of this tree, which this report does not repeat: the
four presets with the floor pins on (debug and release 690 of 690, asan 687,
tsan 465), tier 3 113 of 113, the models preset 28 of 28.

## Sources

`shmem/src/team.c`, `team.h`, `node.c`, `node.h`, `progress.c`, `progress.h`,
`progress-host.c`, `internode-am.c`, `internode-generic.c`, `internode.h`,
`collectives.c`, `collectives.h`, `internal-region.h`, `barrier.c`, `am.h`,
`am.c`, `context.c`, `deadlock.c`, `job.c`, `connection-area.c`,
`shm-object.h`, `init.c`; `base/epoch.c`, `base/epoch.h`, `base/sweep.h`;
`tools/stat/segment.c`; `tools/probe/probe.c`; `tools/modelcheck/engine.py`,
`team_block_model.py`, `split_numbers_model.py`, `pool_dry_model.py`,
`posted_bitmap_model.py`, `CMakeLists.txt`; `shmem/tests/test-team-split.c`,
`test-team-threads.c`, `invariants.h`, `CMakeLists.txt`; `README.md`; design
§3.16, §5.7; `docs/design/team-resources-proposal.md`;
`docs/prompts/phase1-step7-slice3-prompt.md` (in `done/` since `c306221`),
`docs/prompts/phase1-step7-slice4-prompt.md`; `docs/decisions/2026-10-03-team-storage.md`,
`2026-10-03-team-allocation.md`, `2026-10-04-review-fixes.md`; the previous
round's `phase1-release-fixes-correctness.md` and `phase1-release-fixes-review.md`.
