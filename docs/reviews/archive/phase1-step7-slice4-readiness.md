# Round six, question 5: is osoe ready to be public?

Review of `main` at `375973a`, run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, question 5 only. This is step E's
merge point, the release candidate: the team redesign's last step
(`docs/decisions/2026-10-04-team-lanes.md`, `2026-10-04-team-scans.md`, design
§5.7 and §5.8).

The previous round's answer to this question is
`phase1-step7-slice3-readiness.md` (tree `d8a651d`). Its fixes merged as
`docs/decisions/2026-10-04-step-d-review-fixes.md` records, and they are
checked below as claims.

`main` has since moved by one commit of text, `b2804cd` (step E's merge point,
`docs/decisions/2026-10-04-step-e-merge-point.md`). Its files were read where
they bear on this question, and where they change an answer it is said.
Labels: *documented*, *observed in source*, *measured*, *recalled*.

## How it was checked

Every run was made in the `osoe` VM (Debian 13, Linux 6.12, aarch64, four
cores, Slurm on one node), under its lock, on a release build of a copy of the
tree with the stranger's defaults (no `-DOSOE_TEST_PINS`). The tree was built
by the README's quick start as printed, then installed to a prefix.

58 jobs were run, most made to fail on purpose:

- 50 under `osoe-launch -k`, so that what ends the job is the library's own
  reaction, not the launcher's;
- 3 more under `osoe-launch` for `SHMEM_INFO` and `SHMEM_DEBUG`;
- 2 under `srun`;
- the quick start's two runs, and the stranger's smoke test on logical nodes.

The failure program, `efail.c`, has a mode for each failure step E added or
changed:

- large and small collectives that one PE or one node never enters;
- mismatched operations, counts, classes and scan kinds;
- a death between large reductions, and a node stopped during them;
- the team limit at the new default;
- hangs read with `osoe-stat`.

No check was delegated. The VM's copy is removed.

## Verdict

**Ready to be public after one README sentence (Q5-1) and a set of minors.
Nothing in this question blocks release.** In this question's area the tree can
be tagged 0.1.0 once that sentence, and the `--help` line beside it, say what
`osoe-stat --summary` follows. Every other change below is a message, a
sentence or a line of the checklist.

Step E made every new failure loud and named.

- **Mismatches.** A large reduction called with another operation or count,
  or a large scan of another kind or count, on another node ends the job in 65
  to 74 ms, naming both PEs and both collectives. A reduction or broadcast whose
  nodes disagree on its class is named within 73 ms (by a poor message for
  reductions, Q5-2) or proved a deadlock, as the Limits say.
- **Deadlocks.** A large reduction or scan that one PE or one node never enters
  is proved a deadlock in 3.4 s (reductions) and 5.6 s (scans) at a 2 s
  setting, naming each PE's routine.
- **No false ends.** A node stopped 10 s in the middle of large collectives is
  waited for, and the job runs on to a right result.
- **The new setting.** `OSOE_SMALL_COLLECTIVE_BYTES` is refused by name for
  every bad value, and compared among a node's PEs and between nodes, as the
  README says.
- **The teams' new reach.** At the new default a program holds 253 splits, and
  the 254th fails on every PE with round five's corrected message.

The previous round's fixes in this question's area hold under runs made to
break them.

What is new:

1. **`osoe-stat --summary` follows barriers and syncs, not reductions,
   broadcasts, collects or scans** (Q5-1).
   - It walked a hung team sync to the culprit's node.
   - On the same team hung in a reduction, large or small, every node read "no
     epoch open, no child missing". Its last column named a node-mate of the
     culprit, not the culprit.
   - This predates step E. Every earlier round's `--summary` check hung a
     barrier or a sync.
   - The README sends a user to this view "to walk a hang down". It must say
     what it walks.
2. **Messages and records.**
   - The class-split refusal is the one mismatch message with no cause in it
     (Q5-2).
   - The `/dev/shm` refusal blames the heap for what step E's region costs
     (Q5-3).
   - `SHMEM_INFO`'s new row omits the scans (Q5-4).
   - The checklist's after-release list starts at round four, losing round
     three's (Q5-5). The checklist also lacks the tag commit's status
     paragraph and the quick start on the Pi.
   - "How osoe was made" generalizes the later method to the first week
     (Q5-6), and the README's "one naming that PE" is not what other nodes say
     (Q5-7).

## Findings, ranked

### Q5-1. `osoe-stat --summary` follows barriers and syncs only, not the reductions, broadcasts, collects and scans the README sends it after (fix before release: a sentence; the view after release; high in the facts, medium in the rating)

- **Location**:
  - README "Looking inside a job", :393 ("`osoe-stat --summary 1234  # each
    team's collective in progress, to walk a hang down`") and :396-400.
  - README "When a job fails or hangs", :419-421.
  - `osoe-stat --help` (`tools/stat/stat.c:1446-1449`), and design §3.14
    (:1585-1595), whose measured walk is a barrier's.
  - `tools/stat/stat.c:563-636` (`summary_line`):
    - a team other than the world gets a line only when its tree state has
      moved (`:605`);
    - the columns are that tree state's `epoch_open`, `epoch_closed`,
      `own_arrived`, `children_arrived` and `lowest_missing`.
  - `shmem/src/progress.c:226-227`, the only writer of `epoch_open` and
    `epoch_closed`: the progress engine's tree, which barriers and syncs use.
  - `shmem/src/internode-generic.c:1140-1142`: the generic internode phase,
    which carries every reduction, broadcast, collect and scan between nodes,
    writes the tree state only when a collective ends (its class, lane and
    pieces in flight), never while it waits.
- **What happens** (*observed in source*, *measured*):
  - For a hang in a data collective, every node's line shows the team's last
    barrier or sync, closed. "next" is `-`, and nothing points down the tree.
  - A created team that has run only data collectives gets no line at all.
  - The last column shows, for each node, the PE blocked longest, which need
    not be the culprit.
- **Evidence** (*measured*). A created team of all 8 PEs on four logical nodes
  (map `0,0,1,1,2,2,3,3`), synced once; then rank 7 waits on a word nobody
  writes. Detection off; `--summary` at 4 s:
  - **Rank 7 skipping a team sync.**
    - Node 0's line for the team read `3@r0 epoch 2/1 own yes children 2/3
      next 3`, pointing at node 3, the culprit's.
    - The walk works, and so does round five's label.
  - **Rank 7 skipping a 4 KiB sum (large, through the lanes), and a 64-byte one
    (small).**
    - Node 0's line read `3@r0 epoch 0/1 own no children 0/3 next -`; nodes 1
      to 3 read `epoch 0/1 own no children 0/0 next -`. That is the sync's
      closed epoch, with nothing open.
    - Node 3's last column named `rank 6 shmem_long_sum_reduce (epoch 2)`, not
      rank 7 in `shmem_long_wait_until`. No line of the summary named the
      culprit.
  - **Two created teams, {0, 1} and {2, 3} on the map `0,1,0,1`** (round five's
    case, where both took number 3). The second hung in a 4 KiB reduction; the
    summary printed the world team's two lines and nothing else.
  - **The world's large reduction whose root node never enters.** Every node
    read `epoch 0/1 own no ... next -`. The culprit's node showed only because
    both its PEs waited in `shmem_long_wait_until`.
- **What still finds such a hang** (*measured*):
  - `osoe-stat JOB`'s PE lines name every PE's routine and epoch.
  - A deadlock the detector can prove names every PE's wait, in 3.4 s at a
    2 s setting.
  - But `osoe-stat` is what the README offers for the hang the detector cannot
    prove (:401-404): a PE computing for ever, a helper thread, an idle OpenMP
    pool. `--summary` is its walk down a hang, and that walk fails for these
    collectives.
- **Not new in step E.** The data collectives have run in the generic phase
  since step 5's second slice. Every earlier `--summary` check hung a barrier
  or a sync:
  - round three's `shmem.hang-walk`;
  - round four's B6;
  - round five's split, destroy and column syncs.

  No record discusses the summary's scope (*documented*: no dated record
  mentions `--summary` with a data collective).
- **Scenario**: a hybrid program keeps an OpenMP pool idle. It hangs in a row
  team's reduction because one PE computes on.
  - The detector rightly never proves a deadlock.
  - The user runs `--summary`, as the README says, and sees on every node no
    epoch open and no child missing.
  - They conclude the job is not in a collective.
- **Fix**:
  - *Before release*: the README's two passages and the `--help` line say that
    `--summary` walks a barrier or a sync. For a reduction, broadcast, collect
    or scan, the PE lines of `osoe-stat JOB` on each node name each PE's
    routine and epoch, and the PE in another routine is the one to look at.
  - *After release*: the generic phase publishes its epoch open and its
    children arrived into the tree state as it waits. A team runs one
    collective at a time, on the lead's thread. The summary's last column
    then prefers a PE that is not in the team's collective.
- **Confidence**: high in the facts (the source, and the four runs). Medium in
  the rating, since the hang stays diagnosable from the default view. What is
  wrong is where the README points.

### Q5-2. The class-split refusal is the one mismatch message that does not say what is wrong (minor; high)

- **Location**:
  - `internode-generic.c:905-910` (`take_child_lanes`), and the scans' twin at
    `:1901-1906`.
  - README Limits, "Failures", :905-912.
  - The test that pins the text: `shmem.lanes.class-split-named`
    (`shmem/tests/CMakeLists.txt:2005-2010`).
- **Evidence** (*measured*): node 0's PEs pass 200 longs (1,600 bytes: large),
  the other three nodes' 100 (800: small). The job ends with status 1 in 73 ms,
  and its one cause line is:

  > osoe: PE 0: shmem_long_sum_reduce: the lane word of node 1's lead (PE 2),
  > this node's child at position 0, says epoch 0 and lane 0, and this node's
  > lead (PE 0) is in epoch 2's collective

  The other nodes say "node 0 ended its connection without its goodbye: its
  lead has exited or died".

  Every other mismatch measured here ends "PEs called different collectives
  [or scans], or the same one with different arguments": an operation, a count,
  a broadcast's class, a scan's kind, a scan's count. This one says neither
  that nor what differs. "Epoch 0 and lane 0" means the child put no word, as a
  node that took the reduction to be small does.
- **Fix**: say so. For example: "node 1's lead put no lane word, as a node that
  took this reduction to be small (at most `OSOE_SMALL_COLLECTIVE_BYTES`) does:
  PEs called different collectives, or the same one with different counts".
  Change the test's regular expression with it.

### Q5-3. The `/dev/shm` refusal counts step E's internal region as heap and advises lowering the heap (minor; high)

- **Location**:
  - `shmem/src/node.c:983-1000`.
  - README "What does not scale", the `/dev/shm` bullet (:678-683): "P ×
    (`SHMEM_SYMMETRIC_SIZE` + the program's statics + 6.5 MiB)", where 6.5 MiB
    is the region at the defaults.
- **Evidence** (*measured*): 24 PEs on one node, with
  `SHMEM_SYMMETRIC_SIZE=32m`, `OSOE_MAX_TEAMS=1024` and
  `OSOE_SMALL_COLLECTIVE_BYTES=16384`. The lead printed:

  > /dev/shm has 3968 MiB free, but this node's control segment and 24 heaps of
  > 197 MiB and as many copies of the program's 2 MiB of statics need 4779 MiB:
  > lower SHMEM_SYMMETRIC_SIZE, run fewer PEs per node, or enlarge /dev/shm ...

  The program asked for 32 MiB heaps. 165 MiB of each "heap" is the region
  those two settings make (*observed in source*, `internal-region.h`; the
  README's cost table agrees). The same job at those two settings' defaults ran
  with status 0. No value of `SHMEM_SYMMETRIC_SIZE` makes it fit.
- **Why it matters**: the `OSOE_SMALL_COLLECTIVE_BYTES` row invites raising the
  setting "to keep a few kilobytes' reduction or scan in one hop". At 16 KiB
  that is 41 MiB a PE at the default 256 numbers, 165 MiB at 1,024. The rows do
  state the cost.
- **Fix**:
  - The message gives the heap and the region apart, and names the settings
    that size the region: `OSOE_MAX_TEAMS`, `OSOE_SMALL_COLLECTIVE_BYTES`,
    `OSOE_MAX_THREADS`.
  - The bullet says the 6.5 MiB is the defaults' and grows with them.

### Q5-4. `SHMEM_INFO`'s `OSOE_SMALL_COLLECTIVE_BYTES` row leaves out the scans (minor; high)

- **Location**: `shmem/src/init.c:149-153`.
- **Evidence** (*measured*, `SHMEM_INFO=1`): "the largest reduction or
  broadcast that moves through a team's own slots in one piece ... a larger one
  goes through the leading threads' lanes".
- Since step E's second part the scans follow the same line, through every
  member's lane. The README's row says so, after the second part's adversarial
  read fixed it (*documented*, `2026-10-04-team-scans.md`, "The adversarial
  read", item 3). This text was not fixed with it.
- **Fix**: "reduction, broadcast or scan", and "a scan's, every member's".

### Q5-5. The release checklist: right in what it lists as open, short by three items (minor; high)

What is there is right (*documented*, checked against the records):

- the SOS rerun on the tagged tree, now with `collect_active_set` at 32, which
  `b2804cd` corrected;
- the Pi runbook;
- the hour of `mix`;
- the presets, `models`, `tier3` and the suite under load;
- the six MULTIPLE models;
- the floor pins on their machines;
- global exit under Slurm by hand;
- the README's commands on the Pi;
- x86-64 on the tagged tree;
- the release note.

Every after-release item of rounds four and five is carried. Missing:

**(a) Round three's after-release items are on no list** (*documented*,
searched with a control).

- The checklist's "After the release" section says it keeps what the reviews
  rated after release "so that none is lost". It begins at round four.
- Round three's synthesis listed its own after-release items
  (`phase1-release-review.md:142-157`). The checklist, design §10 and the
  README name none of these:
  - the TCP send-buffer question (the second review's F7);
  - hugetlbfs heaps;
  - the mesh's ports bounded for firewalled sites;
  - the rendezvous bound to every address on a multi-homed node (N4, which the
    fixes plan also left for after the tag, `phase1-release-fixes-plan.md:342-347`);
  - a debug compare of collective allocation sizes;
  - the deadlock detector's uncounted mutex sleep, its collection without a
    timeout, and a test that sees the second collection;
  - the `shmem.reply` test spinning;
  - the pacer's three-node relay and `mix`'s node-mate traffic;
  - a test for an x86-only lost wake.
- The search's control: the same search found design §10's "Thorough 1.6 spec
  read".
- Some of the synthesis's items are done or stated: per-node numbers (step D),
  `--gc` and the `.conn`/`.job` names, and the README's limits on `collect` and
  the one progress thread. Round three's readiness report also rated three
  diagnosis items after release (`phase1-release-readiness.md:329-336`), of
  which only "a dead node is named by index" reached round four's list.
- Two of the three are unchanged on this tree (*measured*):
  - "on behalf of PE N" names the PE that noticed, not the culprit (Q5-7);
  - `osoe-stat` shows a stopped node as a waiting one: node 1, stopped 10 s,
    read `live ... rank 2 shmem_long_sum_inscan (PE 1) 10.017 s`.

  The third, an init hang's empty "blocked in", was not rerun.

**(b) The tagged commit's README.** README :16-18 says "osoe has not made a
release yet: there is no version tag". The author's rule is a README true at
every commit, so the commit the tag names needs that paragraph changed. No item
says so.

**(c) The quick start on the Pi.**

- The README's quick start (`5ee2225`) names "Debian 13 or Raspberry Pi OS
  Trixie". It has run in the Debian VM:
  - in the subagent's check (*documented*, `2026-10-04.md`);
  - and here (*measured*): configure 0.4 s, build 19.7 s against the README's
    "20 to 21 seconds", no warning, both rings four lines and status 0, the
    probe passing.
- It has not run on Raspberry Pi OS. The checklist's item for the README's
  commands on the Pi lists the `srun` ring, the batch script, `env.probe` and
  `osoe-stat`, not the quick start.
- **The run, for the lead**:
  1. Use one Pi node, with an ssh session held open (`RemoveIPC`, runbook) and
     a fresh copy of the tagged tree, not the runbook's build.
  2. Run the quick start's commands as printed:
     - `cmake --preset release`;
     - `cmake --build --preset release -j4`;
     - `build/release/tools/wrapper/oshcc -O2 -o ring ring.c`;
     - `build/release/tools/launch/osoe-launch -n 4 -- ./ring`;
     - the same with `-m 0,0,1,1`.
  3. Each run must print the README's four lines and exit 0.
     `build/release/tools/probe/probe` must pass.

  A failure would mean the README's second distribution is unproved.

**Fix**: three lines of the checklist. (a) can be a pointer to round three's
two lists, with what is done struck.

### Q5-6. The record: "How osoe was made" generalizes, and four counts and pointers are stale (minor; high, but medium on "each change")

- **README :1413-1415**: "Every decision is recorded with who took it ... why,
  and how to undo it: the dated records in `docs/decisions/`".
  - The dated records begin on 2026-09-23 (`2026-09-23-autonomous.md` is the
    first).
  - The first week's decisions are in the design notes and the commits. For
    example, `f1d0194` "Record build, dependency, and naming decisions" changed
    only the design notes (*observed in source*).
- **README :1426-1431**:
  - "a fresh Claude agent read each change adversarially before it merged".
    The briefs' table records no review for the first slices: step 1, merged
    2026-09-17, before `CLAUDE.md`. The table's silence is not proof, hence
    medium.
  - "At each merge point five more reviewed the whole tree". Five-question
    reviews ran at five points (rounds two to six), round one on the design
    alone, besides three single-slice reviews.
  - "a refutation pass tried to knock down every finding rated serious".
    `docs/reviews/README.md:8` says rounds four and five; round three's
    synthesis had one too (`phase1-release-review.md:14`).
  - A qualifier ("from the second review on", "since 23 September") makes each
    exact. The section's opening, the author's own, is not touched by this.
- **Exact as written** (*measured*):
  - 539 commits; all but the first and six merges carry a `Co-Authored-By`
    line;
  - the five model names;
  - `CLAUDE.md` from 2026-09-19;
  - Opus 5.5 from 2026-09-22.
- **`docs/README.md:41`**: "908 files sit under ... evidence". The tree holds
  966, and 979 after `b2804cd`. The sentence gives the command, so the number
  can go.
- **`docs/decisions/2026-10-04.md` item 8** (:129-134) records the piece rule
  put to the author ("4 KiB below 64 KiB and 16 KiB from 64 KiB", "to be
  measured on the Pi before step E merges").
  - The code builds a third rule: the count's share of D, at least 8 KiB and at
    most 16 KiB (`internode-generic.c:1812-1814`, `:1995-2010`). It was chosen
    as the best of the three the Pi measured.
  - The index (:66), the part's record and the commit message give it.
  - But the record's "Choices made without the author" does not list it with an
    undo, and item 8 still reads as the rule in force. A line under item 8
    would do.
- **The same record's item 2** (:21, :29) counts "530 commits" and "551
  commits name the Claude model".
  - At `a866dc4` `main` has 532 commits, 525 with the line (*measured*).
  - 551 matches no count I can make; across all refs today there are 593 lines.
  - The README's own count is exact.
- **After this round**: `docs/reviews/README.md`'s "five rounds", and its
  pointer to round five's synthesis.

### Q5-7. "Each of the others, one naming that PE" is not what other nodes say (minor; high)

- **Location**: README :410-411; round three's readiness item
  (`phase1-release-readiness.md:331-332`), on no list since (Q5-5).
- **Evidence** (*measured*, four nodes of two; the large reductions'
  operations differ):
  - The cause is PE 2's grant message. PE 3, its node-mate, names PE 2.
  - PEs 0, 4 and 6, the other nodes' leads, say "node 1 ended its connection
    without its goodbye: its lead has exited or died (§6.4)". That node's lead
    ended by a fatal; it did not die.
  - PEs 1, 5 and 7 say "aborting on behalf of" their own node's lead.
  - The cause's line came first, as the README advises looking for.
- **Fix**: "each of the others a line naming the PE of its node that saw the
  job end, or the node it lost; the first line is the cause". "Exited or died"
  could be "ended".

### Q5-8. The seam: step E kept the boundary (after release; high)

*Observed in source.*

- **The new call.** `osoe_internode_reduce_enter` (`internode.h`) is
  documented as the generic implementation's grant to each child, a put and a
  fenced add, "or nothing, for a reduction it offloads". A backend that needs
  nothing told ignores it.
- **The lanes and grants** live in `internode-generic.c` and
  `internal-region.h`. Both are on the boundary as the implementation every
  backend may keep (`cmake/CheckBoundary.cmake:86-88`), and
  `internode-generic.h` is refused above it (`:76`).
- **Nothing above the seam names a lane.** `collectives.c` calls the entry
  through `internode.h`. The split's zeroing of the grant counters goes through
  the backend's `osoe_internode_number_taken`.
- **An RDMA backend** runs the generic phase as it is, its puts and fenced adds
  as RDMA writes and atomics, as `internode.h` now says of the scans too.
- **Round four's Q5-7** (the io_uring check above the seam, `init.c`) stands,
  and the checklist carries it.

## The previous round's findings, checked as claims

| Finding | Fixed by | Holds? |
|---|---|---|
| Q5-1 the team limit's "61 a PE" | step D's review fixes, item 3 | **Holds**, and at step E's 256 (*measured*). The 254th split of the world fails on every PE with one line from rank 0: "no team number is free on every PE of the new team - between them they hold every one a program may use, though each may hold fewer: OSOE_MAX_TEAMS is 256 ...". After one destroy the next split succeeds. Every pair of 33 PEs at `OSOE_MAX_TEAMS=64` still fails at {30, 32} after 526 splits, now with that message. The README's 253 and its pairs sentence match. |
| Q5-2 `osoe-stat` labels by number | item 4 | **Holds** for what it fixed (*measured*): a hung sync's team reads `3@r0`, and the blocked PE is named by world rank. The view's scope is Q5-1, new. |
| Q5-3 the split's messages interleave | minors | **Holds** (*observed in source*): both go through `osoe_notice`, one buffer and one write (`fatal.c:110-125`, `team.c:1235-1250`). |
| Q5-3 the settings rows: `OSOE_LAUNCH`, `OSOE_TEAM_BLOCKS`, huge pages | minors | **Holds** (README :1221, :1141, :1183). |
| Q5-3 stale text | minors | **Holds** (spot-checked): `node.h:330`, `bootstrap.h`, the test comment, the docs index. The runbook's region size, stale again after E (12.6 MiB at `375973a`), is fixed in `b2804cd`. |
| Q5-3 the scrub's residue, the VM names | minors | **Holds**. `docs/README.md`'s "development machines" paragraph explains `osoe-load` and `osoe-x86`. |
| Q5-3 the six dropped minors | the checklist | **Holds**: all six are on it. Round three's are not (Q5-5). |
| Q5-3 after release: the split's pending wait, a split with other arguments, the counters view's 64 | the checklist | **On it.** |
| Q5-4 the seam | - | **Holds**, and step E kept it (Q5-8). |

## Diagnosability

**Every failure step E added that a program can meet ends the job naming what
failed, or is proved a deadlock naming every PE's wait. No case hung past what
the detector proves, or what a run with detection off left hanging on
purpose.**

Each row was run under `osoe-launch -k` on four logical nodes of two PEs, at
`OSOE_DEADLOCK_SECONDS=2` (*measured*):

| Failure | What the user is told | Who ends, when |
|---|---|---|
| A 4 KiB sum that the last PE never enters | Proved: "every PE of the job waits in the library ...": PE 7 in `shmem_long_wait_until (statics +0x10010)`, the rest `in shmem_long_sum_reduce (epoch 2)` | Every PE, 3.4 s, status 1 |
| The same, with node 0 (the root) never entering, so that the children wait for its grant | Proved alike: the children in the reduction at epoch 2, node 0's PEs in their wait | 3.4 s |
| An 8 KiB scan that a leaf, or the root, never enters | Proved: each PE `in shmem_long_sum_inscan (PE n)`, the PE it waits on | 5.6 s |
| A large sum on three nodes, a large max on one | "the grant from node 0's lead (PE 0) is for epoch 2's sum of 8-byte signed integers, 4096 bytes, and this node's lead (PE 2) is in epoch 2's maximum ...: PEs called different collectives, or the same one with different arguments" | 74 ms |
| The same sum, with another large count on one node | The same, "4096 bytes" against "4160 bytes" | 72 ms |
| A reduction large on node 0 and small on the rest | The lane word's line (Q5-2) | 73 ms |
| A reduction small on node 0 and large on the rest | Proved deadlock, every PE in the reduction at epoch 2: as the Limits say, with no word of a mismatch | 3.4 s |
| A broadcast of 1,000 bytes on node 0 and 1,500 on the rest | "should be piece 0 (1500 bytes) of epoch 2's broadcast ... and is nothing: no piece was put there: PEs called different collectives ..." | 62 ms |
| A large scan, exclusive on PE 1 | "the scan grant from PE 1 is for scan 1 of the team, an exclusive one ... and PE 2 is in ... an inclusive one ...: PEs called different scans, or the same one with different arguments" | 72 ms |
| A large scan, another count on PE 1 | The same, 8800 against 8192 bytes | 65 ms |
| A scan small on PE 1, large on the rest | Proved deadlock | 5.5 s |
| A PE killed by `SIGKILL` between large reductions | "local PE 1 (rank 7, pid …) has ended without finalizing; ending the job (§3.15)"; the rest "on behalf of PE 7" | Every PE, 0.17 s, status 137 |
| Node 1's two PEs stopped 10 s during large sums and scans | Nothing; no false end. On continuing, the job ran on: 19,909 rounds in 20 s, the final sum right. | Status 0 |
| `OSOE_SMALL_COLLECTIVE_BYTES` of 127, 16385, `x`, empty, `1k`, `0x400` or -1 | Every PE: "OSOE_SMALL_COLLECTIVE_BYTES='…' is not between 128 and 16384". 128, 2048 and 16384 run. | Within 4 ms |
| `OSOE_SMALL_COLLECTIVE_BYTES` differing between nodes | "node 1 (or a node below it) has OSOE_SMALL_COLLECTIVE_BYTES=2048; node 0 has 1024: every node must decide alike which collectives are small ..." | Every PE, 0.11 s |
| `OSOE_SMALL_COLLECTIVE_BYTES` differing between a node's PEs | "differs between PEs: the local lead has 1024, this PE has 2048 (§5.8)". The other node says what it waits for until the launcher ends it, as the README documents. | That node at once |
| `OSOE_MAX_TEAMS` of 3, 1025 or `abc` (4 and 1024 run); differing between nodes | Named, with its range; the difference ended the job in 0.12 s | At start |
| `OSOE_MAX_THREADS` of 0 or 1024 (1023 runs); `OSOE_COLLECTIVE_PIPELINE` of 0 or 9 | Named, with its range | At start |
| A 254th split at the default 256; every pair of 33 PEs at 64 | The table above, Q5-1 of round five | Nobody: the program goes on |
| A `/dev/shm` too small for the region | Q5-3 | Refused at start |
| A hung team sync, reduction or broadcast, detection off | `osoe-stat JOB` names every wait. `--summary` walks the sync, and not the rest (Q5-1). | The launcher |

What convinced me:

- Each line names the routine, the PEs and what differed, except Q5-2's.
- The grant carries the collective's epoch, identity and bytes, and the scan
  grant the scan's number among the team's, kind and bytes. So the new protocol
  turns a mismatch between nodes into a named refusal a hop before a piece
  could land wrong, where step D's tree named it a piece later.
- The deadlock detector saw every wait the lanes added that was measured
  here: a child's wait for its parent's grant, and a scan member's for its
  parent or child.
- A stopped node is still not taken for a dead one.

**The stranger's path** (*measured*, the installed prefix):

- `srun -K1 --cpu-bind=none -N1 -n4 osoe-pacer --quick`: "failures=0" in
  0.7 s.
- The README's ring under `srun`: four lines.
- `osoe-pacer --quick` on four logical nodes of two, where the lanes run:
  "failures=0" in 3.0 s.
- The tools answer `--version` with `0.1.0`.

## The README, read as its intended reader meets step E

- **The quick start is true as printed** (Q5-5 c): the build's time, the four
  lines, status 0 both times, the probe.
- **The settings are right** (*measured*, the library itself):
  - `OSOE_MAX_TEAMS` 256, 4 to 1024, *checked*;
  - `OSOE_SMALL_COLLECTIVE_BYTES` 1024, 128 to 16384, *checked* among a
    node's PEs and between nodes;
  - `OSOE_MAX_THREADS` 1 to 1023;
  - `OSOE_COLLECTIVE_PIPELINE` 1 to 8.

  Only those two defaults moved in step E (*observed in source*, the diff of
  `bootstrap.h`).
- **The team limits are right at 256**: 253 a PE, the pairs, and the 70-PE
  programs running every round, which `b2804cd`'s acceptance confirms (with
  `collect_active_set` at 32, as the checklist now says).
- **The class rule's limit is right**, both ways and for broadcasts and scans,
  as the table shows.
- **The scaling section** is the scale review's to check. As a reader meets it,
  it defines its terms (lane, grant, hop) in the rows that use them, and names
  what does not scale. Dense, but a reference, and placed after "How osoe
  works".
- **"How osoe was made"** is exact in every count, and generalizes in three
  phrases (Q5-6).
- **The six directory READMEs** were checked against the tree, path by path
  (*measured*):
  - `base/tests/` holds nine test programs, as said;
  - `tools/stat/` includes nothing from `shmem/src`;
  - `dev/lima/osoe.yaml` has four CPUs, 8 GiB, 30 GiB and the five packages;
  - every file the root README's two tables name exists.

  `docs/README.md`'s three parts read as a newcomer's map.

## The repository

*Measured*, each search with a control that found something known.

- **The licence**:
  - All 49 source-like files the delta adds or changes (`d8a651d..375973a`)
    carry `SPDX-License-Identifier: LGPL-3.0-or-later`.
  - In the whole tree only the four specification examples lack it, as the
    README says.
  - No other identifier appears.
- **Third-party material**: the delta to `b2804cd` adds 113 files, no binary,
  none over 138 KB (the hour of `mix`'s summary). No foreign notice.
- **Private material**: no home path, account, key, token, hardware address or
  private address in the 13,230 added lines. What they do carry, all
  deliberate:
  - the development VM's default address, `192.168.5.15`, named in a record;
  - the author's own address, which item 4 of `2026-10-04.md` records as his
    choice;
  - the runbook's `pi-login` alias, in the tree since September.
- **`docs/` as a record**:
  - Step E's two records are in the established form: the models, mutations,
    the adversarial read and what each changed, choices made without the
    author with their undos, and what was left. Item 8's rule (Q5-6) is the
    one choice not listed.
  - The decisions index carries E's lines, and the briefs' table E's row (moved
    to `done/` in `b2804cd`).
  - The stale counts are Q5-6's.

## What is sound

- **Step E's failures**: every mismatch of operation, count or kind between
  nodes is named within 75 ms, and one of class is named or proved;
  every missing entrant is proved a deadlock within 6 s at a 2 s setting; a
  death ends the job in 0.17 s; and a stopped node is waited for.
- **The new setting**: refused by name for every malformed value, compared
  three ways, its row and `SHMEM_INFO`'s range right.
- **The team limits** at the new default, with round five's corrected message.
- **Round five's fixes** in this area: the labels, the message, the one-write
  notices, the rows, the stale text.
- **The stranger's path**: the quick start as printed, the installed smoke test
  under `srun` and over logical nodes.
- **The history's account of itself**: every count exact.
- **The licence and the repository's content** over the delta.
- **The seam**: step E kept the boundary.

## Sources

- **Runs**: the failure program `efail.c`, the scripts `batch1.sh` to
  `batch5.sh`, their outputs and the per-job logs are in the investigator's
  scratch directory, `<scratchpad>/q5e/`. Its full path names the author's
  account, so it is in the hand-off to the lead rather than here. The VM's
  copy, `/tmp/q5e`, is removed.
- **Read**:
  - `README.md` whole, `docs/README.md`, the six directory READMEs,
    `docs/decisions/README.md`, `docs/prompts/README.md`,
    `docs/release-checklist.md` (and `b2804cd`'s);
  - the step E records, `2026-10-04.md`, `2026-10-04-step-d-review-fixes.md`
    and `b2804cd`'s merge-point record;
  - round five's readiness report and synthesis, round four's readiness
    report, and round three's synthesis and readiness report;
  - `phase1-release-fixes-plan.md` (the after-release lines);
  - `shmem/src/internode-generic.c` (the grants, lane words, scans, the
    published state), `internode.h`'s step E diff, `node.c` (the capacity
    check), `init.c` (`SHMEM_INFO`), `fatal.c`, `progress.c:219-227`;
  - `tools/stat/stat.c` (the summary), `tools/stat/segment.h` (layout 31),
    `cmake/CheckBoundary.cmake`, `CMakePresets.json`,
    `tools/modelcheck/CMakeLists.txt`.
- **Git**: the commit and co-author counts, the first commits, the delta's
  added lines and files, the evidence counts at four commits.
