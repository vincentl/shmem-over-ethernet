# Round six, question 2: is osoe correct, at step E's merge point?

Review of `main` at `375973a`, run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, question 2 only, with the lead's
changes: the release candidate the team redesign's step E leaves
(`docs/decisions/2026-10-04-team-lanes.md`, `2026-10-04-team-scans.md`;
`docs/design/team-resources-proposal.md` §4; design §5.7 and §5.8), with the
previous round's findings in this question (`phase1-step7-slice3-correctness.md`,
G1 to G4 and its note on step E) and their fixes
(`docs/decisions/2026-10-04-step-d-review-fixes.md`) checked as claims. `main`
has since moved to `b2804cd`, which changes records, evidence and one model's
docstring path and nothing of the library or its tests, so what follows holds
for it too. Labels: *documented*, *observed in source*, *measured*,
*recalled*. Every run was made in this session in the `osoe-load` VM (Debian
13, kernel 6.12, aarch64, 4 cores) from a copy of the tree in its `/tmp`, under
the VM's lock, the copy removed afterwards; the models ran on the host (Python
3.14.7). The probe, the model variants and the VM scripts written for this
review are in `build/scratch/review-e/q2-evidence/` (git-ignored), which the
lead has.

## Verdict

**Correct as built. In this question's area the tree can be tagged 0.1.0 as
it is: nothing blocks the tag, and nothing must precede it.** Step E's
protocols hold as far as I could push them, on the source, in the models and
in runs made to break them:

- every write step E added - a child's pieces and a parent's releases in a
  lane, a small collective's piece, a lane word, a grant - is one its reader
  waits for before it returns, and none is overwritten before it is read (E5,
  traced for each below);
- the first part's departure from the proposal - a parent reads a child's lane
  word as it takes the child's first piece, not at its first release - is
  right, and the proposal's rule fails in the model as the record says;
- a lane is one live thread's: thread records are claimed by compare-and-swap,
  exhaustion is fatal, a thread keeps its record for its life in a session, a
  PE has as many lanes as records, and a lane's offset follows from settings
  every node agrees on;
- a number taken again cannot meet a grant the team before it left, and is
  guarded twice, not once as the records say (H4);
- design §3.16 holds in step E's code and in the whole library: no `relaxed`,
  `consume` or `acq_rel`, no implicit-order call (round five's G2 fix holds).

The models' verdicts are as recorded and every listed breakage fails; the whole
debug suite passes under eight busy loops (716 of 716), step E's 165 tests ten
times each under the same load; and a stress of my own - four to eight threads a
PE, each with teams of its own split and destroyed every round, sums, broadcasts
and both scans on either side of the class line - ran more than 9,000 rounds
clean across five maps and seven settings off their defaults (Runs), under load,
under AddressSanitizer and ThreadSanitizer, and with the deadlock detector at
one second.

What I found is about evidence, not code. Ranked:

- **H1 (after release)**: a lane per thread is untested for the scans - a
  one-line regression that sends every scan of a PE through one lane passes
  all 115 of the suite's lane, scan, pipeline and team tests and returns
  wrong prefix sums with status 0 - and `osoe-pacer` cannot see a lane shared
  by threads at all. The stress above is the test; cheap enough to take with
  this review's fixes.
- **H2 (after release)**: the scans' lane protocol - the lane index a grant
  carries, the lane word, threads - is model-checked only by the second part's
  adversarial read's model, outside the tree, which as kept no longer runs.
- **H3 to H6 (minor)**: `lane_model.py`'s UP and DOWN checks are reached only
  with LANE removed, and its small DOWN check by no listed case; "the grant
  counter is the one guard" is stale since the reader's clear; the lanes tests
  are kept from ThreadSanitizer for a reason their maps do not have; a stale
  comment.

## Findings, ranked

### H1. A lane per thread is untested for the scans, and the stress tester cannot see a lane shared by threads at all (after release; high confidence; *measured*)

- **Location**: `shmem/src/internode-generic.c:1978-1982` (a large scan binds
  `own_lane()`, `:755-758`); `shmem/tests/test-lanes.c:270-330` (`two-threads`,
  the suite's only case of a PE's threads leading collectives at once:
  reductions only, its lane check the published `team.tree.lane`, which only
  `in_pieces` publishes, `:1144-1145`); `test-scan.c` (no threads);
  `tools/pacer/collectives.c` and `threads.c` (collectives on each PE's main
  thread; the threads exercise is RMA on contexts);
  `tools/modelcheck/scan_pipeline_model.py` ("each member has one thread and
  one lane"); `docs/decisions/2026-10-04-team-scans.md`, the mutations (no scan
  counterpart of `one_lane`).
- **What it guards** (*observed in source*; proposal E9): two threads of a PE
  may make large collectives on two teams at once, each through its own lane.
  For the scans that is every member, not only a node's leads: any
  `SHMEM_THREAD_MULTIPLE` program whose threads scan teams of their own.
- **Evidence** (*measured*): the mutation `scan_one_lane` - `:1981` made
  `flight.lanes.own = 0`, every large scan of a PE through lane 0 - against the
  115 tests whose names hold `lanes`, `scan`, `pipeline` or `team`: **all 115
  pass**. My stress (`q2-evidence/rq2-lanes-stress.c`: four threads a PE, each
  with its own copy of the world split every round into a team of its own
  shape, then sums, broadcasts and inclusive and exclusive scans of 8 to 5,000
  longs, every result checked) **fails in its first rounds on all three maps**
  (0,1,2,3; 0,0,1,1,2,2; 0,1,2,0,1,2), and silently as far as the library goes:
  "PE 2: thread 1 round 1: inclusive scan of 5000 longs at rank 1, element 4096
  is 3116753, want 4233500" - status 0, nothing named, since a scan's pieces
  carry no header. ThreadSanitizer, on a one-PE-a-node map, reports the race at
  `:2087`, the scan's down copy (H5). The tree as built passes the same stress
  (Runs). For reductions and broadcasts the suite does see a lane shared by
  threads - `one_lane`, `:757` returning 0, fails `shmem.lanes.two-threads`, as
  the record says, and my stress at once ("a child's partial result ...
  should be piece 0 (1032 bytes) of epoch 1's sum ..., and is piece 0 (1032
  bytes) of epoch 3's sum") - but **`osoe-pacer` under that mutation, `mix`,
  `collectives` and `teams` 40 s each on 0,1,2,0,1,2: `failures=0` each**.
  Both step E records cite the pacer's collectives sweep, teams, threads and
  ten minutes of `mix` as verification; none of its exercises has two threads
  of a PE leading collectives, so none could see E9 broken.
- **Why it matters**: the code is right - read below, model-checked outside the
  tree (H2), and clean in every run made here - so nothing a user of 0.1.0
  meets. But the project's standard is that a mechanism is shown to fail when
  it is removed. Here a one-line regression of E9 for the scans would pass the
  whole suite and return wrong prefix sums with status 0, where the same
  regression for the reductions ends the job by name.
- **Fix**: the stress as a tier-2 test - it needs only `shmem.h`; 300 rounds
  take about a second a map unloaded - on 0,1,2,3 and 0,0,1,1,2,2 at least, and
  `scan_one_lane` with `one_lane` in the records' mutation lists; or a scan case
  in `two-threads`, with a scan's lane published as the reductions' is. The
  after-release half: a pacer exercise of collectives on per-thread teams under
  `SHMEM_THREAD_MULTIPLE`.

### H2. The scans' lane protocol is model-checked only outside the tree (after release; high confidence; *measured*)

- **Location**: `tools/modelcheck/scan_pipeline_model.py` (one lane a member:
  no lane index travels with a grant, no lane word, no threads);
  `build/scratch/team-scans-adversarial/lane_scan_model.py` (the second part's
  adversarial read: the reductions, broadcasts and scans of two teams on the
  same lanes, two threads a node, the scans' grants and lane words); the README's
  "Model checking" (`README.md:1032-1046`), which counts "the lanes their large
  pieces go through" and "the scans" among what is checked.
- **Evidence** (*measured*): `lane_scan_model.py` run against this tree's engine:
  21 design cases ok with liveness, its 5 breakages failing, 36 s - as the record
  says. As kept it imports its engine from `build/scratch/wt/team-scans`, which
  no longer exists, so it does not run. With a breakage of mine, a scan through
  lane 0 whatever its thread (`q2-evidence/lane_scan_one_lane.py`), it fails
  LANE in 162 states where node 1 scans P and Q at once on two threads, and holds
  with one thread (375 states). In the tree nothing model-checks a scan's lane
  word, the lane its grant names, or two threads scanning at once.
- **Why it matters**: the README's claim rests, for the scans' lanes, on a file
  a future change cannot re-run; the `models` preset would pass a regression of
  them.
- **Fix**: `lane_scan_model.py` into `tools/modelcheck` with the `model` label
  (36 s), `scan_one_lane` among its breakages; or `scan_pipeline_model.py`
  given threads and lane words.

### H3. `lane_model.py`: every breakage of the large path fails by LANE, and the small path's DOWN check is reached by none (minor; high confidence; *measured*)

- **Location**: `tools/modelcheck/lane_model.py`: the LANE checks at `:291` (a
  release) and `:367` (a put), each made before its store; UP at `:247` (small)
  and `:254` (lane), DOWN at `:380` (small) and `:385` (lane).
- **Evidence** (*measured*): the model's 12 breakage cases all fail by LANE
  but `node_small`, which fails UP on the small slot. With both LANE checks
  made to pass (`q2-evidence/lane_nolane.py`), the lane path's UP and DOWN catch
  every breakage but `no_grant` on teams in one order, which the docstring says
  is safe for the data (8,490 states): `no_grant` UP at 58 states,
  `stale_grant` UP at 989 and 103, `one_lane` UP at 1,709, `lane_early` UP at 98
  and 125, `release_lane_early` DOWN at 21 and 73, `release_lane_late` DOWN at
  2,764 and 13,053. So those checks work, and no listed case reaches them while
  LANE is there. The small DOWN check (`:380`) is reached by no listed case:
  `node_small` shares both small slots and fails UP first. Sharing the small
  release slot alone (`q2-evidence/lane_small_down.py`) holds with one thread a
  node, as it should - a release follows the child's own arrival - and fails
  DOWN at 63 states where node 0's two threads are each a child in a small
  collective of another team at once (`T@1s,U@1s:01`); the design holds there,
  961 states.
- **Fix**: that case and that breakage in the model, and a line in the
  docstring that LANE is the property every lane breakage reaches first. (Round
  five's G3 was this kind of finding, for `posted_bitmap_model.py`.)

### H4. "The grant counter is the one guard": the reader's clear is a second, and turns a stale count into a named end (minor; high confidence; *measured*)

- **Location**: `internode-generic.c:2139-2146` (`number_taken`'s comment);
  `test-lanes.c:38-44`; design §5.7 (`shmem-ethernet-design.md:5156-5158`) and
  §5.8's E5 bullet (`:5425-5433`); `docs/decisions/2026-10-04-team-lanes.md:180`
  ("the grant counter the one guard against a grant the team before left ...
  the `grant_unzeroed` mutation fails it").
- **Evidence** (*measured*): with both grant lines left unzeroed whole -
  counters and words, the reductions' and the scans' (`number_taken`'s two loops
  cut short a line, both `memset`s removed) - `shmem.lanes.reuse` fails "the
  grant from node 0's lead (PE 0) is for nothing: no grant was put there, 0
  bytes, and this node's lead (PE 1) is in epoch 1's sum of 8-byte signed
  integers, 4096 bytes", `shmem.lanes.scan-reuse` the same for the scan grant,
  and the stress the same at once on every map. Every grant is taken within its
  collective, and `take_grant` and `scan_take_grant` clear the words they read
  (`:868`, `:1874`; the first part's adversarial read), so a team ends with its
  grant words zero, and a stale count meets "nothing", never a grant that
  matches. The record's own `grant_unzeroed` left the words zeroed and was
  caught the same way.
- **Why it matters**: the code is safer than its account. Round five's worry - a
  team's last grant matching the next team's first in epoch, identity and bytes
  - cannot arise even with the zeroing gone. The zeroing still matters: it keeps
  the count exact, and without it one silent case is left, a child that sees a
  stale count just as its parent's real grant lands and reads the words torn,
  epoch, identity and bytes new and the lane still zero. A reader of the
  sentence as written takes the zeroing for the only thing between a reused
  number and silent misdirection, and the clear for an invalid program's
  convenience.
- **Fix**: the five places say two guards - the zeroing keeps the count exact;
  the reader's clear makes a stale count end the job by name.

### H5. The lanes tests are kept from ThreadSanitizer for a reason their maps do not have (minor; high confidence; *measured*)

- **Location**: `shmem/tests/CMakeLists.txt:1952-1965` ("tsan is left out of the
  maps for `shmem.internode.*`'s reason"); the reason at `:974-987`: the lead's
  agent writes a local PE's heap, separated from that PE's own touch of it by
  the barrier, whose links - the arrival, "whichever local member happened to be
  last posting the handoff", the root, the release - run through processes tsan
  cannot see.
- **Evidence** (*measured*): every lanes case runs one PE a logical node, so each
  PE is its node's first and only member, the agent that applies its incoming
  puts is a thread of its own process, and what separates the agent's write into
  a lane from the owner's last touch of it is the owner's own send through that
  agent and the counters, all in one process. `test-lanes.c` built by hand
  against the tsan preset's library: the nine cases that should pass (`shared-lead`,
  `two-threads`, `turns`, `rotation`, `boundary`, `reuse`,
  `scan-shared-parent`, `scan-turns`, `scan-reuse`) pass with no report; my
  stress on 0,1,2,3 and 0,1,2, 60 rounds of four threads, none either; and
  ThreadSanitizer does see a lane shared by threads (H1's mutation: a data race
  reported at the scan's down copy). So the per-thread lanes, step E's one new
  piece of thread safety, have never been under ThreadSanitizer in a preset.
- **Fix**: register `shmem.lanes.*`, and H1's test, in the tsan preset.

### H6. Minor

- **A stale comment** (*observed in source*): `internode-generic.c:1691-1693`,
  "Different teams use different memory, so two threads may scan two teams at
  once (spec §9.4), which slots shared by every team would not allow". Since
  step E a large scan's slots are its thread's lane, which every team that
  thread scans shares; what lets two threads scan at once is a lane per thread.
- **Carried, unchanged as planned** (*documented*,
  `docs/release-checklist.md:121-141`): round five's G1 (the release hold of
  §3.8 has no test and no model) and G3 (`posted_bitmap_model.py`'s ORDER
  reached by no breakage), with round four's F2 to F5.

## Step E: what convinced me

**The writes into a lane (E5).** Read whole: `in_pieces`, `take_grant`,
`take_child_lanes`, `combine_children`, `copy_release`, `to_parent`,
`to_children`, `osoe_internode_generic_reduce_enter`, `_reduce`, `_broadcast`,
`osoe_internode_generic_scan` and its helpers, `number_taken`;
`internal-region.h`; `collectives.c`'s `data_collective_at`, `collect_varying`,
`osoe_team_split_and` and `osoe_scan`. A lead's lane receives its children's
pieces, every one of which it combines - a middle lead sends piece k up only
after `children_sent(k)`, and the root releases piece k only after it, so the
loop's end, `down == pieces`, implies every child's every piece taken - and its
parent's releases, every one of which it copies out before it returns. A
broadcast's children put nothing into the lane. A scan's member takes every
child's total for every piece (`down < up` before a down) and every down. So at
return nothing addressed to the lane is still to come (*observed in source*).

**The grant.** Sent at entry - `reduce_enter`, before the lead's wait for its
node's members (`collectives.c:183-186`, and `:466-472` for `collect`'s
lengths), and at a scan's entry before anything else - into the child's grant
line for the number, the counter fenced after the words. A child reads the words
only after an acquire load shows the count at its base plus one, and clears
them. They are never overwritten unread: a parent enters a team's next large
reduction only once it has finished this one, which takes the child's first
piece, which the child sends after taking the grant - through every collective
in between, since each tree spans every node of the team. The scans' the same
(*observed in source*). The one new wait it adds - a child for its parent's
entry - depends only on what the release always did, so it closes no cycle the
old slots did not; the stress at `OSOE_DEADLOCK_SECONDS=1` under twelve busy
loops found no false positive (*measured*).

**The lane word, and the departure.** A child puts its lane and epoch before its
first add; the parent reads and clears it as it takes the child's first piece,
or at the root its first release (`in_pieces:1095-1096`, `:1111-1112`). Nothing
of the team's next collective is written anywhere before this lead's last piece
has gone up, which is after that read, so the next collective's child at the
position - in a tree turned to another root - cannot write first. The
proposal's read at the first release is `release_lane_late`, failing LANE as
recorded (*measured*, below).

**The clears.** Each is sequenced before the reader's next send, and the next
legitimate write into what it cleared is caused by that send: a slot's header
before the piece goes up or the release goes down, which the writer's next piece
into the slot waits for by the window; a grant's or a lane word's before the
first piece. So they race nothing a valid program does, and they are what make a
stale count (H4) and a class split (`class-split-broadcast`) end by name
(*observed in source*).

**Small slots and the class.** A small piece goes into the number's own small
slot, which the next small collective's child reaches only after holding this
one's release; a small release only after the child's own arrival. Small and
large alternate safely, on one team and on two sharing a lead, in the model.
The four class splits a program outside the specification can make behave as
the README's Limits say: a large parent over a small child names the cleared
lane word; a small parent over a large child, broadcast, names the cleared
release slot; a small reduction's parent over a large child leaves the child
waiting for a grant until the deadlock detector ends the job; and of a scan, a
large parent over a small child names the cleared lane word, a small parent over
a large child waits for a scan grant until the detector ends the job (*observed
in source*).

**The scans.** Pieces sized in elements from the count, the width and the agreed
D, alike on every member; at most a lane's 16 KiB slot, which a scan's
headerless piece may fill; one more piece of the team's scans expected in every
row whatever the class, so small scans after large ones and the reverse count
alike; the window carried across scans because a member enters the next only
holding every down of this one (*observed in source*; the model's `aAa`, `pQqP`,
`pqpq` cases).

**A lane's identity.** `osoe_metrics_thread_index` is a counter block claimed by
compare-and-swap, given back only at the thread's exit, and fatal to the job when
none is free (`metrics.c:536-579`); a PE has `OSOE_MAX_THREADS + 1` records and
as many lanes (`node.c:703`, `:1487-1492`). A lane's offset needs only the
fan-out, D, `OSOE_MAX_TEAMS`, `OSOE_SMALL_COLLECTIVE_BYTES` and the node count,
all agreed; `OSOE_MAX_THREADS` may differ between nodes, and the index travels;
the receiver bounds every internal put by its own region (`progress-host.c:69-83`).
The `/dev/shm` check counts each heap with its region, lanes included
(`node.c:981-982`) (*observed in source*).

**Number reuse.** The arrival, release and grant counters, the scans' rows and
grant counter, the gather's words and both grants' words are zeroed by
`number_taken` before the split's sync, after which alone anyone writes them;
nothing of the old team is in flight by then, every write being one its reader
waited for. Lane words and slots are read only after a counter says they were
written. And H4's second guard (*observed in source*, *measured*).

**§3.16.** Step E's new atomic accesses are release stores of single-writer
words (`most_pieces_in_flight`, `collective_class`, `lane`, the zeroed
counters); every grant word, lane word and slot is data published by a fenced
add and read after an acquire load (`osoe_transport_load`) - message passing,
the table's first row. Library-wide, a grep for `relaxed`, `consume`, `acq_rel`
and the implicit calls finds nothing in `shmem/src` or `base`, and finds a planted
copy of each (*measured*).

**The split's own reduction.** At `OSOE_MAX_TEAMS=1024` and
`OSOE_SMALL_COLLECTIVE_BYTES=128` a split's AND is 17 words, 136 bytes: a large
reduction through the parent's leads' lanes, entered by `data_collective_at`
like any other. No test sets both; 800 stress rounds, a split every round, and
the 36 team tests pass there (*measured*).

**Protocols without a model**: number reuse for grants and the clears, which
are simple enough for their tests (`reuse`, `scan-reuse`, `class-split-*`); the
scans' lanes with threads, which need the tree's model (H2). Carried: the
release hold (G1) and the node's start-up over `/dev/shm`.

## The models

On the host, against `375973a`: every case's verdict as its docstring expects.

| Model | Cases | Breakages | Time |
|---|---|---|---|
| `lane_model.py` | 18 ok with liveness (17 design; `release_lane_late` at D = 1, safe as documented) | 12, all failing: `no_grant` 5 and 5, `stale_grant` 199 and 98, `one_lane` 141, `lane_early` 93 and 120, `release_lane_early` 16 and 68, `release_lane_late` 2,759 and 13,048, all LANE; `node_small` 21, UP | 2.3 s |
| `scan_pipeline_model.py` | 18 ok (8 before step E, 9 `lanes`, `lanes_no_grant` on AB, safe as documented) | `window` 14, `count_before_copy` 1,313, `root_early` 29, `ring_short` 44, `summed` 3,316, `lanes_no_grant` PQ 15, `lanes_stale_grant` 52, `lanes_node_small` 9 | 21 s |
| `lane_scan_model.py` (outside the tree; H2) | 21 ok | 5, failing LANE, WORD, DOWN, GRANT | 36 s |
| `grant_timing.py` | +1.00 hop with the leads together, +0.70 at 0.3 of a hop, none at a hop, 4 to 4,096 nodes; a late root or middle lead +1.72 and +1.02 at 21 nodes, none for a late leaf - the record's figures | | 7 s |
| `scan_grant_timing.py` | +1.00 together; +1.96 to +2.02 with a member with children late; none for a late leaf | | 0.2 s |

Breakages of my own (`q2-evidence/`): LANE removed, the lane path's UP and DOWN
reached (H3); the small release slot shared, DOWN at 63 states (H3); a scan's
lane shared by threads, LANE at 162 (H2). The models run under sequential
consistency; step E's protocols are message passing and read-modify-writes, none
of the store-buffering shape, so TSO or aarch64 adds no outcome the C11
arguments above do not cover. `abort`, `abort_exit` and `tree_barrier` I did not
re-run: step E does not touch them, and the record's `models` preset ran 29 of 29
on the tree this one differs from in records only.

## The previous round's findings, as fixed

| | Finding | What was done | Holds? |
|---|---|---|---|
| G1 | the release hold has no test and no model | on the release checklist's list of what follows the release | carried, as planned |
| G2 | 37 relaxed accesses and an implicit-order load against §3.16 | orders named; `shmem.memory-orders` fails the suite on a relaxed, consume or acq_rel order or an implicit call | **yes**: no such access in `shmem/src` or `base`; the grep finds planted ones |
| G3 | `posted_bitmap_model.py`'s ORDER reached by no breakage | on the checklist | carried, as planned |
| G4 | the README's model count and list | 29, step D's four named, and step E's lanes | **yes** |
| Step E | a stale grant under a number taken again; zeroing the only guard; a mutation and a test wanted | `shmem.lanes.reuse` and `scan-reuse`; `grant_unzeroed` and `scan_grant_unzeroed` caught | **yes, and more**: the reader's clear is a second guard (H4) |

## Runs

| Run | Result |
|---|---|
| the step E models and timings, and the read's combined model (host) | as The models |
| my breakages of `lane_model.py` and `lane_scan_model.py` (host) | H3, H2 |
| step E's 165 tests (lanes, scan, pipeline, small-collective, internal-region, collect, reduce, broadcast, team, allgather, the pacer's collective plants), debug | 165 of 165, 9.5 s |
| the same, ten times each, eight busy loops | 1,650 of 1,650, 177 s |
| the whole debug suite, eight busy loops | 716 of 716, 193 s |
| the stress, four threads a PE, eight busy loops: 0,1,2,3 at 1,000 rounds; 0,1,2,3,4 at fan-out 2, 1,000; 0,0,1,1,2,2 at 128 bytes, 1,000; 0,1,2,0,1,2 at D = 1 and fan-out 2, 1,000; 0,1,2,3 with eight threads, 600 | all clean |
| the same under `OSOE_TEST_PERTURB=1` and D = 4, 60 rounds; and with `OSOE_DEADLOCK_SECONDS=1` under twelve busy loops, 800 rounds on three maps and 100 perturbed | all clean; no false deadlock |
| the stress at `OSOE_MAX_TEAMS=1024`, 128 bytes, fan-out 2, six busy loops, 400 rounds on two maps; the 36 team tests at those settings | clean; 36 of 36 |
| the stress under the asan preset (ASan with UBSan), 300 rounds on three maps, four busy loops | clean, no report |
| the stress under the tsan preset, 60 rounds on 0,1,2,3 and 0,1,2; `test-lanes.c`'s nine positive cases under tsan | clean, no report (H5) |
| `one_lane` (`:757`): `shmem.lanes.two-threads`; the stress; `osoe-pacer` `mix`, `collectives`, `teams` | fails; fails, named; `failures=0` each (H1) |
| `scan_one_lane` (`:1981`): the 115 lane, scan, pipeline and team tests; the stress; the same under tsan | 115 of 115 pass; fails, wrong sums, status 0; a data race reported (H1) |
| the reductions' grant line unzeroed whole, then the scans', then both: the lane, scan and team-split tests; the stress | the reductions': `reuse` and three team-split tests fail; the scans': `scan-reuse`; both, with their output: `reuse` and `scan-reuse`, each "for nothing: no grant was put there"; the stress at once on every map, the same words (H4) |
| `osoe-pacer mix` 120 s, 8 PEs on 0,1,2,3,0,1,2,3, and `collectives` 60 s at 128 bytes on 0,1,2,0,1,2, four busy loops | `failures=0` each |

The lead's merge-point runs (the SOS suite at 4 x 2, the acceptance at 70 PEs,
the Pi against step D) and the record's verification of `28d6710` (four presets,
tier 3, `models` 29 of 29) are not repeated here.

## Sources

`shmem/src/internode-generic.c`, `internode-generic.h`, `internode.h`,
`internode-am.c`, `internal-region.h`, `collectives.c`, `metrics.c`, `node.c`,
`node.h`, `progress-host.c`, `transport-local.c`, `perturb.c`, `transport-am.c`
(its diff), `bootstrap.h`, `team.c`, `deadlock.h`; `shmem/tests/test-lanes.c`,
`CMakeLists.txt`; `tools/modelcheck/lane_model.py`, `scan_pipeline_model.py`,
`grant_timing.py`, `scan_grant_timing.py`, `engine.py`, `CMakeLists.txt`;
`tools/pacer/collectives.c`, `threads.c`, `osoe-pacer --help`; `README.md`;
design §3.16, §5.7, §5.8; `docs/design/team-resources-proposal.md` §4;
`docs/prompts/phase1-step7-slice4-prompt.md`; `docs/decisions/2026-10-04-team-lanes.md`,
`2026-10-04-team-scans.md`, `2026-10-04-step-d-review-fixes.md`;
`docs/release-checklist.md`; round five's `phase1-step7-slice3-correctness.md`
and `phase1-step7-slice3-review.md`; `build/scratch/team-scans-adversarial/lane_scan_model.py`
and `build/scratch/mut/mutate.py`, `mutations.txt` (the records' mutations).
