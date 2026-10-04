# Round five, question 5: is osoe ready to be public?

Review of `main` at `d8a651d`, run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, question 5 only, at the merge
point of the team redesign's step D (`docs/decisions/2026-10-03-team-storage.md`,
`docs/decisions/2026-10-03-team-allocation.md`, design §5.7). The previous
round's answer to this question is `phase1-release-fixes-readiness.md` (tree
`08b3da0`); its fixes merged as `docs/decisions/2026-10-04-review-fixes.md`
records, and they are checked below as claims. Step E is not built: the limits
the README marks *temporary* are known, and what bears on E is kept apart.
Labels: *documented*, *observed in source*, *measured*, *recalled*.

Every run was made in the `osoe` VM (Debian 13, Linux 6.12, aarch64, four
cores, Slurm on one node), under its lock. Each ran on a release build of a
copy of the tree with the stranger's defaults (no `-DOSOE_TEST_PINS`),
installed to a prefix. The lead's own verification of this tree passed whole
before these runs: debug 690, release 690, asan 687, tsan 465, tier 3 113 and
models 28.

73 jobs were run, most made to fail on purpose:

- 49 under `osoe-launch -k`, so that what ends the job is the library's own
  reaction, not the launcher's;
- 3 under `srun`;
- 20 starts of the README's launcher-free recipe, and one more by hand.

A failure program, `teamfail.c`, has a mode for each failure step D added or
changed. Round four's program supplied a regression subset of that round's
matrix.

Three mechanical checks were delegated, and are used here only where they were
re-verified: the README's settings against the code, the docs' indexes and the
release checklist, and the repository's hygiene over `08b3da0..d8a651d`. Each
check had controls.

## Verdict

**Ready to be public once step E is built, after two small fixes and a set of
minors; nothing in this question blocks release.** The two fixes are a sentence
and a message for the first, and a few lines of `osoe-stat` for the second.

Step D made every new failure a valid program can meet loud and named:

- **No number left.** A split returns nonzero on every PE, with one line
  naming `OSOE_MAX_TEAMS`.
- **A dry pool.** The split returns nonzero, with one line naming
  `OSOE_TEAM_BLOCKS`, the node and its host.
- **A stale handle.** The job ends, naming the team and saying its number names
  another team since.
- **A split or destroy one PE never enters.** It is proved a deadlock in 3.3 s
  at a 2 s setting, naming the routine.
- **Teams renewed in a loop.** 2,000 splits and destroys in a row all
  succeeded, where before D the 62nd failed.

The previous round's fixes in this question's area hold under runs made to
break them. The launcher-free recipe re-ran after a failed start 10 times of 10
under dash. The huge-pages row is true, and the scrub's residues are gone. The
author has decided which history to publish. What is new:

1. **The README's team limit promises more than a split can give** (Q5-1). A
   new team needs a number that none of its members holds. So the numbers its
   members hold *between them* bound it, not each one's count. Making every pair
   of 33 PEs a team, in a double loop, fails when the two PEs hold 31 and 30
   teams, where the README says 61. The message then names a per-PE limit the
   program does not exceed.
2. **`osoe-stat` cannot tell apart the teams step D lets share a number**
   (Q5-2). It labels a created team by its number, which D made each PE's own.
   On a `split_2d` grid every column prints under one name on every node, so
   the walk down a hung tree that the README advertises cannot follow one
   column.

The rest is minor: messages that interleave, sentences and counts D left stale,
two display caps of 64 in `osoe-stat`, and one new residue of the scrub.

## Findings, ranked

### Q5-1. A split's number must be free on every member, so in some patterns a program holds far fewer than 61 teams a PE, and is told otherwise (fix before release; high)

- **Location**:
  - README "Limits ... Teams", first bullet (:484-497): "A PE is in at most
    `OSOE_MAX_TEAMS` teams at once ... so a program can hold 61 teams of its own
    on every PE".
  - The message, `team.c:1245-1253` (`numbers_used_up`).
  - Design §5.7; proposal D2 and D8.
  - Step E's README row (E10), which keeps the same framing (below, "Step E").
- **What happens** (*observed in source*):
  - A split takes the lowest set bit of the AND of every member's free mask
    (`team.c:1309-1318`). A number held by any member of the new team is
    therefore unavailable to it.
  - What bounds a split is the union of the numbers its members hold. That union
    reaches all 61 long before any one PE holds 61, whenever the members'
    numbers do not line up.
  - A PE's numbers line up with its partners' when its teams are nested or
    disjoint: the world split again and again, or rows and then columns.
  - They drift apart when a PE joins teams late whose other members already hold
    the low numbers.
- **Evidence** (*measured*, mode `t_pairs`): every pair {i, j}, i < j, was made
  a team by a split of the world, in the natural double loop, and kept.
  - **32 PEs**: all 496 pairs were made, 31 teams a PE.
  - **33 PEs**, on one node and on three logical nodes of 11:
    - The 527th split, {30, 32}, returned nonzero on every PE, while PE 30 held
      31 teams of its own and PE 32 held 30.
    - PE 0 said: "no team number is free on every PE of the new team:
      OSOE_MAX_TEAMS is 64, the teams one PE may be in at once, 3 of them
      predefined".
    - The mechanism, from a simulation of the same rule that fails at the same
      pair with the same counts: PE 32 had taken numbers from 34 up, because
      each of its partners already held 3 to 33. So its set and PE 30's were
      disjoint, and together they covered all 61.
  - **40 PEs**: {24, 38} failed, holding 37 and 24.
  - **Other patterns**, by a simulation of the same rule (*measured*, Python):

    | Pattern, made in order | Result |
    |---|---|
    | Sliding windows of 8 and 16 PEs | 48 to 60 teams a PE, no failure |
    | The four-neighbour stencil of an 8 x 8 grid | no failure |
    | Random triples among 64 PEs | a split fails with members holding 40, 52 and 59 |
    | The pair double loop, at step E's default of 256 numbers | fails at 129 PEs, holding 127 and 126 |

- **Scenario**: a program builds a team per pair of PEs for pairwise exchanges
  on 40 PEs, and plans by the README's 61. It gets a nonzero split at 37 teams,
  with a message that blames a per-PE limit it is under.
- **Fix**: no change to the algorithm before release.
  - Any choice of number fragments in some order, and step E's 256 numbers move
    the failure further out.
  - The README's bullet states the rule: a new team needs a number that none of
    its members holds, so what they hold between them decides. A team's members
    can run out holding as few as half the 61 each, as the double loop of pairs
    does.
  - The message says what the zero AND means: the new team's members hold,
    between them, every number a program may use.
  - The `OSOE_MAX_TEAMS` row and `SHMEM_INFO`'s "teams one PE may be in at once"
    stay, since they are true as a ceiling.
- **Confidence**: high. D1 and D2 are what the proposal decided. What is wrong
  is the README's "so", and a message that points the user who meets this at
  the wrong cause.

### Q5-2. `osoe-stat` labels a created team by its number, which step D made non-unique, so it cannot follow one team of a grid across nodes (fix before release; high in the facts, medium in the rating)

- **Location**:
  - `tools/stat/stat.c:88-96` (`team_label`: the record's name, or else the
    number).
  - `:516-560` (`summary_line`), and `:543`, its last column.
  - `:726` onward (`--dump`) and `:857` onward (`--json`).
  - `team.c:1093-1095`: a created team's record is named by its number.
  - D's brief, Part 1 item 9: "labels a block by its name and generation".
  - README "Looking inside a job" (:331-332): `--summary` "to walk a hang down".
- **What happens** (*observed in source*):
  - After D1, two live teams with no PE in common may hold one number. They do
    whenever one split creates several teams.
    - `split_2d` gives every row one number and every column the next
      (`team.c:1418-1456`).
    - Successive `split_strided` calls over disjoint rows agree the same lowest
      bit.
  - `osoe-stat` labels a created team by its number.
  - `--dump` adds the block index, which is node-local, and the generation.
  - The team's name in D3's sense, the pair (local lead, number), is printed
    nowhere.
  - Nothing printed is both the same on every node and unique. Before D a number
    was unique job-wide, and the label was enough.
- **Evidence** (*measured*):
  - **Two strided teams under one number.** Teams {0, 1} and {2, 3} on the map
    0,1,0,1 both took number 3. The second was hung: PE 3 waited on a word, with
    detection off.
    - `--summary` printed two lines `3` on node 0, and one on node 1. Node 1's
      line was the *other* team's, since the hung team's block there had opened
      no epoch.
    - `--dump` showed "team 3 (number 3) in block 1022" and "... block 1023" on
      both nodes.
    - `--dump --json` gave two entries with `"number":3`.
  - **A `split_2d` grid.** `xrange` 2 over 4 logical nodes of 2 PEs: rows on
    nodes, and both columns spanning all four under number 4. PE 5 waited on a
    word instead of its column's fourth sync.
    - `--summary` printed two lines `4` on every node.
    - Node 0's line for the hung column read "epoch 4/3 own yes children 2/3
      next 2".
    - On node 2, the two `4` lines read alike: "epoch 0/3 own no children 0/0".
    - Their last column said "PE0 shmem_barrier_all (epoch 4)". That is local
      id 0, which is rank 4, not the culprit. The summary prints a PE as `PE`
      and its *local* id (`stat.c:543`), where every message of the library and
      the README use "PE n" for the rank.
    - With the detector on, the culprit, rank 5, is named in the detector's
      report (proved at 3 s), which names no team either. With it off, the
      counters view's PE lines name each PE's wait, as they named rank 3's in
      the two-team case.
- **Scenario**: a grid code hangs in one column's reduction on a cluster.
  Following the README, the user runs `--summary` on the root's node and is
  told to look at node 2. There they find one line per column, all `4`, with a
  last column that names a PE by a number that is someone else's rank.
- **Fix**: a few lines of `stat.c` and a sentence in "Looking inside a job".
  - Label a created team by its number and its world ranks, which its record
    carries on every node (`world_start`, `world_stride`, `members`). Every team
    that spans nodes is a progression (decision E).
  - The pair (number, world ranks) is unique among live teams, since two teams
    under one number share no PE.
  - Use that label in `--summary`, `--dump` and the JSON.
  - Print the rank, not the local id, in the summary's last column.
  - If the author would rather leave the label until after the release, the
    least is a README sentence that the teams of one split print under one
    number.

### Q5-3. Minor items, each a sentence, a message or a line (minor; high unless stated)

**Messages and diagnosis**

- **The split's two messages interleave** (*measured*).
  - `pool_dry` and `numbers_used_up` (`team.c:1234-1253`) are `fprintf` calls
    to unbuffered `stderr`. A 262-byte line goes out in three writes (128, 128
    and 7 bytes, `strace`).
  - So a `split_2d` that finds a pool dry for several would-be leads at once
    prints their lines cut into each other: one line ends "OSOE_TEAM_osoe:
    PE 6: ...".
  - The library's fatal path formats into one buffer and writes once, for
    exactly this reason (`fatal.c:16-20`). The same would do here.
- **"A PE of that node saying why"** (README :501-502) holds for
  `split_strided`, one line per dry node (*measured*). A `split_2d` whose
  teams are of one PE prints one line per would-be lead that found the pool
  empty: seven lines over two nodes at `OSOE_TEAM_BLOCKS=8` (*measured*).
- **"Skip their last nine rounds without saying so"** (README :815-818). The
  programs are silent, but the library is not: the acceptance runs at 70 PEs
  hold nine lines from world PE 0 naming `OSOE_MAX_TEAMS`.
- **A split waiting on another thread's pending number** publishes its wait
  with detail kind `NONE` and the number as its value (`team.c:1164`).
  `osoe-stat` shows it as `(+0x5)` (`segment.c:607`), and the detector's report
  shows nothing for it (*observed in source*). Saying "pending team number 5"
  would take a new detail kind.
- **A split called with other arguments on one PE** (*measured*) is undefined,
  and is the class the README's Limits already give ("a collective that the
  PEs of one node call differently").
  - **Within a node**, it is silent. PE 1 believed itself rank 1 of a team of
    3, received sums over 4, and the job exited 0.
  - **Across nodes**, every PE's split returned 0 and the first reduction was a
    proved deadlock at 4.3 s, not named as a mismatch.
  - *After release*: since D made the split a reduction, two more words - a hash
    of the arguments and its complement - would let every parent PE see any
    difference, and fail the split alike.

**Settings rows**

- **`OSOE_LAUNCH` must be the same on every PE of a start.** The row (README
  :969) does not say so. Two PEs given different values each wait for the
  other, with notices, until they are killed (*measured*). PE 1 said it "has
  waited 2 s for the local lead, rank 0, to reclaim /osoe....0.0.0, which
  another launch of this job's ids left". The recipe itself is right, and it
  ran 10 of 10.
- **`OSOE_TEAM_BLOCKS`'s row (:890) gives the segment's cost only.**
  - At 65,536 blocks a node's control segment measured 67,719,168 bytes,
    against 1,642,496 at 1,024. Start-up took 0.27 s, against 0.05.
  - The lead's reply descriptors and team array come to about 36 MiB resident
    at 65,536, and each PE's per-block arrays to about 30 MiB of address space.
    That is *observed in source*, by the settings check's arithmetic, and not
    measured.
  - Docker's default 64 MiB `/dev/shm` cannot hold such a segment. Its refusal
    names `/dev/shm`, not the variable.
- **The huge-pages row** says "each PE warns": true, but only in a job of more
  than one node, which alone has a connection area.

**`osoe-stat`'s two caps of 64**

- **Its counters view keeps 64 PEs** (*measured*).
  - At 70 PEs on a node, the header names 70 columns. Every row has 64 values
    and a total over those 64 (`stat.c:592-611`), so the totals miss PEs 64 to
    69 and fall under the wrong header.
  - This dates from 2026-09-17 (`57bb66c`), not from D. It bites at the
    design's 256 PEs a node.
- **The previous round's cap of 64 segments** (`show_job`, `stat.c:1348-1355`)
  stands, and the checklist carries it.

**Text step D left stale** (each verified)

- **The model checker's count and list.** README :119 and :782 say 24 tests;
  the tree registers 28, the lead's run passing all 28. The list of modelled
  protocols (:786-797) reads as the whole set and omits D's four:
  - a block's life;
  - the split's pending numbers;
  - the dry pool;
  - the posted bitmap.

  The "29 minutes" is for 24.
- **`docs/README.md:4-5`**: "What waits on the author is at the end of the
  newest dated record", which `2026-10-04-review-fixes.md` is not.
- **Code and test comments**:
  - `shmem/tests/CMakeLists.txt:380-382`: "team numbers are job-wide".
  - `node.h:330`: `team_count` is commented `OSOE_MAX_TEAMS`; it is
    `OSOE_TEAM_BLOCKS`.
  - `bootstrap.h:79-81` credits `OSOE_MAX_TEAMS` with the node's team blocks.
- **Design notes**:
  - §5.2 (:4381): a block "for the team's number".
  - §5.7's opening (:5028-5049) is still in the present tense of job-wide
    numbers, with an italic correction after it.
  - §5.7's Part 1 paragraph keeps its low-water mark (:5105), with no word that
    layout 30 replaced it by the posted bitmap, which only §3.10 describes.
  - §6.2's list of the settings a node's PEs compare (:5862-5866) lacks
    `OSOE_TEAM_BLOCKS`.
  - §7.3 (:6524): a handle "is the address of the library's descriptor".
- **The records** cite §3.12 for the arrival scan, which is §3.10.
- **The runbook**: `docs/pi-cluster.md:32` gives the control segment as
  1.3 MiB. D added the pool, about 1 MiB at the default; a two-PE node's
  segment measured 1.6 MiB.

**The repository**

- **A new residue of the scrub.** `docs/reviews/archive/phase1-release-fixes-conformance.md:565`,
  added in `c51ba87`, cites a scratch path that names the Unix account. It is the
  one line of the tree that does (the hygiene check, with controls). The
  history carries the account anyway, now that it is published whole, but the
  tree's rule (`docs/README.md`) is that such paths read `~` or `<repo>`. One
  `sed`.
- **The records name VMs the repository does not define.**
  - `osoe-load` and `osoe-x86` appear in 14 and 10 files. `osoe-x86` runs an
    aarch64 kernel with Rosetta registered (*measured*: `uname -m`), yet the
    records report whole suites run "in the `osoe-x86` VM". A stranger could
    take those runs for x86-64 evidence, where the README is careful that no
    x86-64 kernel has run osoe.
  - 102 tracked files cite the git-ignored `build/scratch/`; 31 of those lines
    are new in this delta.
  - A line in `docs/README.md` would cover both.
- **The release checklist dropped six of the last round's minors**, which the
  synthesis sent there:
  - `OSOE_JOB_SECRET` not marked *checked*;
  - `SHMEM_INFO`'s subset;
  - `oshcc -O2 -show`, since the wrapper takes `-show` only as its first
    argument (`oshcc.in:36-37`);
  - `--static`;
  - `osoe-launch` never printing the job id;
  - `transport.h:536-539`'s sentence.

  It carries the 64-segment cap, unknown names, the messages and the seam
  check.

### Q5-4. The seam: step D kept the boundary (after release; high)

*Observed in source.*

- **`internode.h`** now documents every `team` argument as a block index,
  node-local. It also documents a new backend call, `osoe_internode_number_taken`,
  which resets what a backend keeps for a number a PE takes again. Its one
  implementation is the AM backend's, `internode-am.c:79`.
- **The pool, the map and the posted bitmap** sit in `node.h` and `node.c`, on
  the boundary, which every backend keeps (`cmake/CheckBoundary.cmake`).
  - The posted bitmap is set and taken only in `internode-am.c`, below the
    boundary.
  - `team.c` reaches the pool and the map only through `node.h`.
  - The split's reduction goes through the data collectives' seam
    (`collectives.c:291`).
- So a RoCE backend would implement the new call, keep the pool and the map,
  and ignore the bitmap, with nothing above the seam to rewrite.
- **The previous round's Q5-7** stands, and the checklist carries it: the
  io_uring check runs above the seam for every multi-node job (`init.c:292-303`).

## The previous round's findings, checked as claims

| Finding | Fixed by | Holds? |
|---|---|---|
| Q5-1 `OSOE_AM_HUGEPAGES=1` never takes effect | review fixes item 4 | **Holds** (*measured*). The row says it has no effect yet; four PEs on two nodes each printed "could not be mapped with huge pages (Invalid argument); using ordinary pages". Only multi-node jobs warn (Q5-3). |
| Q5-2 the recipe fails when re-run after a failed start | item 2 | **Holds** (*measured*). The recipe as written, under `/bin/sh` (dash), each start exporting `OSOE_LAUNCH=$(date +%s%N)`: after a start in which PE 1 refused `SHMEM_SYMMETRIC_SIZE=abc` and left three names, the next start passed 10 times of 10 and left none. The value is parsed as 64 bits (`bootstrap.c:123-126`). New: the row should say one value for every PE (Q5-3). |
| Q5-3 which history to publish | the author | **Decided** on 2026-10-04 (`a866dc4`, after this tree): the whole history, as the record of the AI-assisted development. One related judgement stays the author's, as before: the README says nothing of the AI assistance, which a stranger meets only in the history, `CLAUDE.md` and the briefs. A line in the README pointing at that record is the author's call. |
| Q5-4 residues of the scrub | review fixes, minors | **Holds** (the hygiene check, with controls). Every hardware address is `xx:xx:xx:xx:xx:xx`. Every `ls -l` owner in the 10 logs and in `stale-segments.out` is `user`. The first name appears nowhere. One new residue (Q5-3). |
| Q5-5 `osoe-stat` shows 64 nodes of a job on one machine | - | **Stands** in source, carried by the checklist. A second cap of 64, PEs in the counters view, is new here (Q5-3). |
| Q5-6 minors | review fixes, minors | **In part.** Taken: the divergent `shmem_malloc` sentence; the settings read after the join (README :1048-1053); the checklist's 1.6 line; the docs index's pointer to the specification. Not carried: six (Q5-3). |
| Q5-7 the seam | - | **Stands**, carried by the checklist (Q5-4). |

## Diagnosability

**Every failure step D added that a valid program can meet is a nonzero return
with one named line. Every misuse of a team ends the job naming the routine.
No case hung beyond what the detector proves, or what a test of a hang left
hanging on purpose.**

Each case below was run under `osoe-launch -k`, on the map 0,0,1,1 unless
stated (*measured*):

| Failure | What the user is told | Who ends |
|---|---|---|
| No number free on every member: 61 splits of the world kept, then one more; and `split_2d` until two are not free | One line from the parent's rank 0: "no team number is free on every PE of the new team: OSOE_MAX_TEAMS is 64 ...", or "no two team numbers are free ... new teams". Nonzero on every PE. After one destroy, the next split succeeds. | Nobody; the program goes on |
| A dry pool, on every node or on node 1 only (`OSOE_TEAM_BLOCKS=8`) | One line per dry node from its would-be lead: "node 1 (lima-osoe) has no team block left ... OSOE_TEAM_BLOCKS is 8". Nonzero everywhere. A destroy frees a block for the next split. | Nobody |
| Teams of one draining a pool (`split_2d`, `xrange` 1) | The same line from each would-be lead that found none, several a node, cut into each other (Q5-3) | Nobody |
| `OSOE_TEAM_BLOCKS` of 7, 65537, `abc`, `16x`, empty, -1 or `0x10` | Every PE: "OSOE_TEAM_BLOCKS='…' is not between 8 and 65536" | Every PE, within 0.01 s |
| `OSOE_TEAM_BLOCKS` differing between a node's PEs | The PE names both values ("the local lead has 1024, this PE has 16"); its node-mate says "another local PE failed during rendezvous". The other node says what it waits for, every 4 s (`OSOE_TEST_NOTICE_SECONDS=2`), as the README documents. | That node at once; the other by the launcher |
| `OSOE_TEAM_BLOCKS` differing between nodes | Nothing; the job runs, as documented | - |
| A stale handle, its number taken again or not | "shmem_team_my_pe: 0x… is team 3, which this PE has destroyed; the number names another team since", or the same without its last clause. The other PEs end "on behalf of PE 1". | Every PE, in 0.06 s |
| A split, or a destroy, that one PE never enters (`OSOE_DEADLOCK_SECONDS=2`) | Proved at 3.3 s: "PE 0, 1 in shmem_team_split_strided (epoch 2)", "PE 3 in shmem_long_wait_until (statics +0x100a0)"; likewise `shmem_team_destroy`. With detection off, `osoe-stat` shows each PE's wait. | Every PE, status 1 |
| A PE computing 12 s before a split | Nothing; no false end | Nobody |
| A split called with other arguments | Within a node, nothing, with wrong sums, exit 0 (the documented class). Across nodes, a proved deadlock at the first collective (Q5-3). | -, or every PE |
| Two teams under one number, one hung; a `split_2d` column hung | Each PE's wait, from `osoe-stat` and the detector; the teams' labels ambiguous (Q5-2) | Every PE once proved |
| A PE killed during a created team's syncs | "local PE 1 (rank 3, pid …) has ended without finalizing; ending the job" | Every PE, within 0.1 s, status 137 |
| A broadcast against a reduction on a created team, between nodes | "a child's partial result, from node 1's lead (PE 2), should be piece 0 … PEs called different collectives" | Every PE, in 0.07 s |
| 2,000 splits and destroys in a row; round four's `teams_past` | Every one succeeded (258 µs each); "created 2000 teams" where round four's run stopped at 61 | - |

**The rest of round four's matrix behaved as that round recorded**, on this
tree:

- a PE killed in a barrier;
- a barrier against a wait, proved at 5.5 s on a 2 s setting, where the D cases
  took 3.3 s; the setting is a floor, not a promise;
- a node stopped 8 s with 64 MiB of puts toward it, waited for, the quiet
  completing in 7.76 s;
- a broadcast against a reduction between nodes;
- a team sync one PE skips;
- a bad setting on one PE;
- the huge-pages warning;
- `SHMEM_INFO`, which now lists `OSOE_TEAM_BLOCKS`.

**The stranger's path under Slurm** (`srun -K1 --cpu-bind=none -N1 -n4`, the
installed prefix):

- `osoe-pacer --quick` ended "failures=0" in 1.9 s.
- `osoe-pacer teams --teams-exhaust` made 61 splits, saw the next fail on every
  PE with one line, destroyed them, and split once more.
- `osoe-pacer teams --seconds 20` made 9,986 renewals, with failures=0.

What convinced me: each line above names the variable, the node or the routine
a user would go to next. The two new kinds of number - a team number per PE and
a block per node - are named apart in every message, so `OSOE_MAX_TEAMS` and
`OSOE_TEAM_BLOCKS` are never confused.

## The README, read as its intended reader meets step D

- **The settings are right.** The settings check confirmed every row, each
  default, range and *checked* mark, against the code:
  - `OSOE_TEAM_BLOCKS`: 1,024, 8 to 65,536, compared among a node's PEs and not
    between nodes;
  - `OSOE_MAX_TEAMS`: 64, 4 to 1,024, compared both ways;
  - each number's 200 KiB, which is 201.5 KiB by `internal-region.h`;
  - a block's "about a kilobyte", which is 1,024 bytes on aarch64.
- **The Teams limits are true but for Q5-1.** The second bullet - a team
  destroyed before a split, "by the PE that would lead the new team there, or
  by PEs of the split's parent that are all of the team's members there" - is
  precise and matches §5.7 and `pool_dry_model.py`. It is also the hardest
  sentence in the README for a reader who is not a SHMEM expert. A first
  sentence saying it matters only at a pool's last block would let most readers
  stop there.
- **The scaling table's team row** is true in substance. The reduction is of
  ⌈`OSOE_MAX_TEAMS` / 64⌉ words and a word of marks.

## Step E (the proposal's §4), kept apart

These are not findings against this tree.

- **E10's README row, and the Limits bullet, as E's brief writes them** (item 3
  of `phase1-step7-slice4-prompt.md`, on `main` after this tree), keep "the
  teams one PE may be in at once".
  - At 256 numbers that would read "253 teams of its own on every PE".
  - Q5-1's double loop of pairs fails at 129 PEs, holding 127 and 126.
  - When E writes the row, it should state the rule as Q5-1 does.
- **E adds models**: the README's count and its list of modelled protocols
  (Q5-3) want updating in the same commit.
- **E's item 10 has `osoe-stat` read the new layout of the internal region.**
  Q5-2's label could ride with it.

## The repository

*From the hygiene check, with controls; re-verified where cited.*

- **The licence**:
  - 523 of the 527 source-like files carry `SPDX-License-Identifier:
    LGPL-3.0-or-later`. The four exceptions are the specification's examples,
    which carry its notice.
  - All 57 source-like files the delta adds or changes carry it, and none
    carries another identifier.
  - The delta adds no third-party material, no binary, and no file over
    200 KB.
- **Private material**: one new residue (Q5-3). No home path, no address, no
  key, no secret and no captured environment otherwise. The history is
  decided.
- **`docs/` as a record**: step D's two records are long but in order - the
  models with their state counts, the mutations, the adversarial reads and what
  each changed, and every choice made without the author with its undo. The
  decisions index's team lines (:59-64, :73-74) say the same as the records,
  and the old design appears only as reversed. A stranger can trace any line of
  `team.c` to a decision. The dangling pointers are the known ones (Q5-3).

## What is sound

- **Step D's failures**: every one a program can meet is named, alike on every
  parent PE, once, and the program goes on. Every misuse ends the job naming
  the routine. Numbers and blocks come back: 2,000 renewals, a destroy freeing
  the place for the next split, both pools exhausted and recovered.
- **Proving a deadlock with teams**: a split or a destroy one PE never enters
  is proved in 3.3 s at a 2 s setting, naming the routine and the epoch. A PE
  computing 12 s outside a split is never judged deadlocked.
- **Round four's matrix on this tree**: deaths, a stopped node, deadlocks, a
  mismatch between nodes and refusals all behave as that round recorded.
- **The launcher-free recipe**: 10 restarts of 10 after a failed start, under
  dash.
- **The stranger's path**: the installed smoke test and the teams exercise
  under `srun`.
- **The settings**: every row's default, range and *checked* mark, and
  `SHMEM_INFO`'s new lines.
- **The licence and the repository's content**, as measured before, and
  re-measured for what changed.
- **The seam**: step D kept the boundary.

## Sources

- **Runs**: the scripts and raw outputs are in the investigator's scratch
  directory, `<scratchpad>/q5-d/`. Its full path names the author's account,
  so it is in the hand-off to the lead rather than here.
  - `teamfail.c`, the failure program: modes `t_hold_max`, `t_dry`,
    `t_renew`, `t_stale`, `t_split_missing`, `t_split_compute`,
    `t_destroy_missing`, `t_split_args`, `t_same_number`, `t_kill_in_team`,
    `t_mm_team`, `t_linger`, `t_pairs`, `t_grid_hang`.
  - `batchD.sh` and `batchD.out` (cases D1-D17, R-*), with `logs/`.
  - `batchE.sh` and `batchE.out` (the counters view at 70 PEs, the dump's
    JSON, the teams of one).
  - `slurm.sh` and `slurm.out`.
  - `relaunch.sh` (the recipe).
  - `case.sh`, `setup.sh`.
  - `g.out` (the grid), and `l0.out`, `l1.out` (two launch numbers).
  - Round four's `fail.c`, from that round's `q5-runs/diag/`.
  - The VM's copy, `/tmp/q5d`, is removed.
- **The three delegated checks' reports** are in this session. They are used
  only where verified here, by reading the lines cited or by a run.
- **Read**:
  - `README.md`, `docs/README.md`, `docs/decisions/README.md`,
    `docs/release-checklist.md`;
  - the two step D records and `docs/decisions/2026-10-04-review-fixes.md`;
  - `docs/design/team-resources-proposal.md` §3-§4;
    `docs/prompts/phase1-step7-slice3-prompt.md`;
  - design §3.10, §5.7, §6.2, §7.3;
  - `shmem/src/team.c`, `deadlock.c`, `internode.h`, `barrier.c`,
    `collectives.h`, `fatal.c`, `init.c`;
  - `tools/stat/stat.c`, `tools/stat/segment.c`, `tools/wrapper/oshcc.in`,
    `cmake/CheckBoundary.cmake`;
  - the acceptance runs' outputs in `build/scratch/teams-d/acceptance/`, and the
    merge point's SOS sweep in `build/scratch/merge-d/sos/`;
  - on `main` after this tree, only for what they settle: `c306221`'s
    `phase1-step7-slice4-prompt.md` and `a866dc4`'s checklist line.
