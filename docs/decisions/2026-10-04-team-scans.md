# 2026-10-04: the scans through the lanes, and 256 numbers (step E, part 2)

The second part of the team redesign's step E, built on
`docs/prompts/phase1-step7-slice4-prompt.md` (the brief), Part 2, from
`docs/design/team-resources-proposal.md` (the proposal) §4, E6 and E10, on the
first part (`2026-10-04-team-lanes.md`). The scans move as the reductions moved:
a small scan through its team number's own small rows, a large one through the
members' lanes, granted as each member enters. With that a team number costs a
PE about 14 KiB, and `OSOE_MAX_TEAMS` defaults to 256.

## What was built

1. **The model first.** `scan_pipeline_model.py`, whose lanes cases the first
   part's branch began - the slots a member's lane, shared by its teams, the
   grant, two teams that share a parent with a child outside the scan its
   parent is in (P and Q) - gains **small scans** (a lowercase letter in a
   case's order: one piece through the team number's own small slots, no grant,
   no lane), interleaved with large ones on one team and on two that share a
   parent, and a breakage, `lanes_node_small` (a member's small slots shared by
   its teams), which fails. `lanes_no_grant` fails where a child is outside its
   parent's scan (PQ) and, as the first model found for the reductions, holds
   where every member is in both teams in one order (AB); `lanes_stale_grant`
   fails. About 15 s.
2. **The layout** (`internal-region.h`): a team number's scans' area is shaped as
   its reductions' rows and words are - a small row per child position and one
   for the down, a lane word and a small slot of `OSOE_SMALL_COLLECTIVE_BYTES`,
   with no piece header since a scan's pieces carry none, rounded to the line;
   then a counter a row and the scans' grant counter, contiguous, the grant's
   words past it. The 4 KiB scan slots per number are gone: a large scan's
   pieces go through a lane's 16 KiB slots, sized to the scan (below).
3. **The scans** (E6; `osoe_internode_generic_scan`): a scan is small or large by
   its bytes, as a reduction is. A large one: every member lends its thread's
   lane from its entry and grants each of its tree children as it enters - the
   scan's number among the team's (every member's alike, since every member
   makes every scan of its team), its identity (inclusive or not, its element),
   its bytes and its lane - before anything else; a child sends its first total
   up only once its scans' grant counter shows the grant, takes the parent's
   lane from it, and puts its own lane's word before its first add; a parent
   reads each child's word as it takes the child's first total. A grant or a
   word naming another scan ends the job naming both PEs, in words: "the scan
   grant from PE 0 is for scan 1 of the team, an inclusive one of 8-byte signed
   integers, 4096 bytes, and PE 1 is in scan 1 of the team, an exclusive one".
   The loop's rules are the first build's (the window, copy before count, a
   counter a position, down only after up, D pieces' totals kept), its watch
   over the rows' counters and the scans' grant counter. Number reuse zeroes
   the scans' grant counter and words with the rest, and a reader clears the
   grant's words and a lane word once taken, as the reductions' do - and,
   since a scan's pieces carry no header, a large scan's lane slot where a
   header would be once read, so that a slot a scan left reads as nothing to
   the other collectives' header checks (the adversarial read, below). **A
   large scan's pieces are sized to the scan**, in elements: its count's share
   of D, rounded up, but at least 8 KiB of elements and at most a lane's 16 KiB
   slot of them - one piece to 8 KiB, 8 KiB ones to D × 8 KiB, D to D × 16 KiB
   and 16 KiB ones past that - every member computing it alike from the
   scan's count, its element
   and the agreed D (the measurements, below, and the author's decision of the
   evening, `2026-10-04.md` item 8).
4. **`OSOE_MAX_TEAMS` defaults to 256** (E10; the author, 2026-10-02: "256 max
   teams is more than sufficient"): `bootstrap.h`, `SHMEM_INFO`'s text, the
   README's row and its team limits - 253 teams of a PE's own; every pair of
   129 PEs made a team in a double loop fails at a pair holding 127 and 126,
   where the same pairs a round of disjoint pairs at a time reach 253 (both
   figures checked by a simulation of the splits, which also reproduces the
   README's old ones for 64: 33 PEs, 31 and 30); a grid of the whole job, two
   numbers a PE, reaching 126 where it reached 30; and the 70-PE programs that
   keep a team a round running every round, skipping rounds again only above
   253 PEs. Design §5.7, the decisions index, the node's `/dev/shm` comment.
5. **The records**: design §5.7, §5.8 (the scans' paragraph, "Since the team
   redesign's step E"), the region's layout; the README's settings, limits and
   cost rows - no sentence left marked *temporary* by the team work.
6. **The README's scaling section** (the author's request of the evening,
   `2026-10-04.md` item 9): time by n and P, memory by structure at 4 × 4, 64 ×
   64 and 500 × 256, what one more of each setting costs, and what does not
   scale - computed from the code by an agent's sweep, then checked claim by
   claim against the code and the records by another: of about 205 claims, 19
   were loose and are fixed - among them a cell that counted the control
   segment and the connection area with the heaps, the control segment's fixed
   part (1.0 MiB, not 1.1), the scans' measured cost given a cause the
   adversarial read (below) had left open, the one-node barrier the only
   intranode phase timed past 4 PEs, the split's reduction a word wider than
   said, a strided `alltoalls` a message an element, and the prefix scans and
   the deprecated active-set collectives missing from what does not scale.
   Design §7.1, which the brief names, lists the phase's steps to 6 only; the
   teams' account is §5.7 and §5.8.

## What a number costs, and the lanes (the brief's stop-and-ask)

At the defaults - fan-out 4, D = 2, small collectives of 1 KiB, 17 thread
records - a number costs a PE 13.5 KiB at 4 nodes and 15.5 KiB at a thousand:
small rows 5,760 bytes, words 1,024 (the counters and the split lines), the
scans' area 6,528, and the allgather's words 512 at 4 nodes, 2,560 at a
thousand. Without the gather and split lines the brief excludes, 12.8 KiB,
against the proposal's 15. The lanes are 17 of 160 KiB, 2.66 MiB, against E9's
2.7. A PE's region at the default 256 numbers is 6.03 MiB at 4 nodes and 6.53 at
a thousand, against 12.6 MiB at 64 numbers before step E. At 128 bytes a number
is 6.75 KiB at a thousand nodes (the proposal's "6 KiB at 256 bytes" is 8.0
KiB here, the gather's words included).

## Choices made without the author

- **The scans' piece rule built is not the one put to the author.** Item 8 of
  `2026-10-04.md` records "4 KiB below 64 KiB and 16 KiB from 64 KiB"; the Pi
  measured that rule 1.10 times step D at 8 KiB and 1.05 at 32 KiB, where fewer
  messages won, and a third - the count's share of D, at least 8 KiB and at most
  a lane's 16 KiB slot - the fastest at every size, step D's or better from 8 KiB
  (the measurements, below). Built, within the author's decision to size the
  pieces to the scan. *Undo*: the constants in `osoe_internode_generic_scan`,
  `SCAN_LEAST_PIECE_BYTES` and the clamp, for 4 KiB below 64 KiB (round six's
  readiness review, Q5-6, found it unlisted here).
- **The scan's grant names the scan by its number among the team's**, which
  every member counts alike, since a scan has no epoch at the seam
  (`osoe_internode_scan` takes none).
- **The scans keep their own grant counter and lane words**, beside their own
  row counters, rather than the reductions': a reduction's and a scan's counts
  on one team are separate, as their rows always were.
- **A parent reads a child's lane word with the child's first total**, the
  reductions' rule, though a scan's tree never turns - a position's child is
  the same rank in every scan of the team - so the late read the model forbade
  for the reductions would be safe here. Kept for one rule; the scans'
  mutations leave the late read out for that reason.
- **The tests that quote the old default changed**: `shmem.job.disagree.max-teams`
  and `shmem.max-teams.local-disagree{,.lead}` say 256 where they said 64. They
  restate the default, which E10 changes; nothing else of theirs did. The brief
  makes "a test that must change other than the pipeline tests' move to the
  large class" a stop-and-ask: these are put to the author with this record,
  as E10's own consequence. The scan pipeline tests' move to the large class is
  the brief's own: `test-scan.c`'s pieces of 16 KiB, still 21 of them.

## Tests and verification

`shmem.lanes.scan-shared-parent` (T's and U's scan trees rooted at PE 1, node 3
late to T's, PE 2 in U alone entering U's while PE 1 is in T's; U of three
members, since only a child before the last has a total any result depends on -
the mutations found the first, two-member version blind), `scan-turns` (a team's
large scans made by its PEs' threads in turn), `scan-reuse` and `scan-mismatch`.
`test-scan.c`'s large sizes a lane's slot four times over and its many-pieces
scan 21 pieces of 16 KiB, the leaf's min(D, 21) still pinned. The debug suite,
715 of 715 in `osoe-load`.

**Mutations** (`build/scratch/mut/mutations-scans.txt`), each a scan breakage
that is a line of the code, against the lane and scan tests: `scan_no_grant`
fails 10 tests, `scan_stale_grant` 11, `scan_lane_early` 13,
`scan_release_lane_early` 13, `scan_node_small` 3, `scan_grant_unzeroed` 1
(`shmem.lanes.scan-reuse`). With U of three members, `scan-shared-parent` fails
`scan_no_grant` and `scan_node_small` 5 of 5 runs, and `scan-reuse` fails
`scan_grant_unzeroed` 5 of 5; the unmutated tree passes both 5 of 5.

## The measurements (the brief's Part 2; the rule of 2026-09-26)

Step E against step D (`main` at `158c13e`), both release builds, alternating
on the day. The probe, `e-band.c` (`build/scratch/e-measure/`), times on the
world a barrier and a long sum reduction, a broadcast and an inclusive sum scan
at 8 B, 1 KiB, the band of 2 to 16 KiB, 32 and 64 KiB and 1 MiB - each PE's
median over 200 calls (50 at 64 KiB and up) between barriers, the slowest PE's
printed - the review's `q3d-band`, whose source had not survived, rewritten with
scans and the larger sizes.

**On the Pi cluster at 4 x 4** (`srun -K1 --cpu-bind=none -N4
--ntasks-per-node=4`), three sessions of the evening, three runs a side in each,
alternating: D the median of its nine runs, E of the third session's three,
with the piece rule kept:

| | D | E | |
|---|---|---|---|
| barrier | 204 | 208 | +2% |
| reduce, 8 B / 4 KiB / 16 KiB / 1 MiB | 227 / 394 / 1,050 / 29,342 | 230 / 410 / 1,064 / 29,310 | +1.5% / +4% / +1% / 0% |
| broadcast, 8 B / 4 KiB / 16 KiB / 1 MiB | 209 / 303 / 628 / 27,872 | 209 / 306 / 610 / 27,866 | none |
| scan, 8 B / 1 KiB (small) | 370 / 497 | 374 / 507 | +1% / +2% |
| scan, 2 KiB / 4 KiB | 479 / 614 | 636 / 722 | **+158 / +108 µs** |
| scan, 8 / 16 / 32 KiB | 859 / 1,408 / 2,551 | 849 / 1,405 / 2,388 | 0.99 / 1.00 / 0.94 |
| scan, 64 KiB / 1 MiB | 4,897 / 73,349 | 3,929 / 48,596 | **0.80 / 0.66** |

(µs, the slowest PE's median.) **Nothing moved on the reductions and
broadcasts**: at four PEs a node a leaf's intranode phase takes about a hop, and
`grant_timing.py` says the grant then costs nothing. **The scans of 2 and 4 KiB
pay the grant**, one piece that nothing hides - 158 µs at 2 KiB, two hops by
the network's one-way 80 µs and so over the brief's "at most a hop" on that
line: put to the author with the first session's numbers, which showed the
same, and accepted with his choice to size the pieces, its own cost stated ("2
to 4 KiB still pay the grant's hop or two"). Measured by a collective's hop on
the cluster - the message and its handling, 115 to 135 µs - it is about one,
the case of members entering together, as the adversarial read's probe then
showed (below). **The scans from 8 KiB are step
D's or faster**, and from 64 KiB much faster, a lane's 16 KiB pieces moving a
quarter of the messages D's 4 KiB did. That is the third piece rule measured: E's
first build - a lane's 16 KiB at every size - was 1.17 times D at 16 KiB, one
piece where D had kept two of 4 KiB in flight, and 4 KiB pieces below 64 KiB,
the rule put to the author, were 1.10 times D at 8 KiB and 1.05 at 32 KiB, where
fewer messages won; D pieces of at least 8 KiB were the fastest at every scan
size. `osoe-bench` at 4 x 4, three runs a side each session: no line moved
beyond its run-to-run spread. `osoe-pacer --quick`: `failures=0`, 6.8 to
7.0 s. An hour of `mix` on the third tree: `failures=0`, 3,609 s. The raw
outputs are `docs/design/evidence/pi-cluster/2026-10-04-step-e/`, and
`docs/pi-cluster-results.md` has the session.

**In the VM** (`osoe`, one PE a logical node, 5 and 8 nodes; ten threads on its
four CPUs, so a third of a run's lines are noise): at 5 nodes, five runs a side,
the large reductions of 2 to 32 KiB +21 to +29 µs (1.12 to 1.24 times), the
grant's hop - under the brief's 80 µs - and nothing else moved: the barrier, the
small collectives, the broadcasts and the 1 MiB reduction within their spread (a
first session's three runs showed the 1 MiB lines 600 µs slower on two of E's,
which five more runs did not). Scans of 2 to 32 KiB +20 to +27 µs; of 64 KiB 0.44
and 1 MiB 0.32 times D. At 8 nodes, three runs a side, the same shape. `osoe-pacer
--quick` on 5 nodes of two PEs: `failures=0` on both.

## The adversarial read

A read of the branch by an agent of its own (the brief's pre-merge read,
`build/scratch/team-scans-adversarial/`), against E5 for the scans - every
write into a member's lane, small slots, lane words and grant line consumed
before its owner returns - and the record's claims. It found no valid program
that breaks E5, no stale grant, word or slot taken as current, and the layout's
figures and the team limits' (`layout_check.py`, `pairs_sim.py`) as the record
has them; its own model, `lane_scan_model.py` - the reductions, broadcasts and
small collectives of `lane_model.py` beside large and small scans on the same
lanes - holds in its 21 cases, its breakages failing, and confirms the late
read of a lane word safe for a scan. It found four things, all now fixed:

1. **The piece rule cut D + 1 pieces for half the counts of one band**: sized
   in bytes, 2,049 longs at D = 2 were 1,024 + 1,024 + 1, and the third piece
   waits a round trip of the tree for the first's release - 9.2 hops against
   5.2 at 16 members in its timing. Results were right. The rule is now in
   elements (above): the count's share of D, rounded up, clamped.
2. **"A large scan's child may wait a hop more, once" understated the grant**:
   when the root or a member with children enters after its children, its
   grants leave only then, and their first totals arrive a round trip after it
   enters, where its own slots had them waiting - +2.0 hops, against +1.0 when
   the members enter together and nothing when a leaf is late. Its timing is
   in the tree as `tools/modelcheck/scan_grant_timing.py`, and the reductions
   have the same case, which the first part's `grant_timing.py` now times too
   (the proposal's figures had every lead enter at once): the README's cost
   rows, design §5.8 and both records say so. The read's probe settled what
   the Pi's +158 µs at 2 KiB is (`scan-skew.c`; a 2 KiB scan, one piece on
   both trees, step E against step D, three runs a side alternating, the
   medians): E - D +119 µs with nobody late, +259 with world rank 5 - a member
   with children - a millisecond late, +228 with the root late, -6 with a leaf
   late. A collective's hop there, the message and its handling, is 115 to 135
   µs - D's own 227 µs for the root's two hops down, 407 for rank 5's three -
   so one hop, two, two and none, as the timing has it; and the band's +158 is
   the first, the members entering as a barrier releases them, a member and its
   child on other nodes together. The read's other probe (`scan-odd.c`)
   confirms the sizing in elements on the same cluster: 2,049, 3,001 and 4,095
   longs take what 2,048, 3,000 and 4,094 do (1,409, 1,915 and 2,445 µs against
   1,408, 1,889 and 2,456), where step D pays its own fifth 4 KiB piece at
   2,049 (1,560 against 1,427).
3. **Sentences left stale**: `internode.h`'s scan paragraph ("its slots are
   the team number's"), the `OSOE_SMALL_COLLECTIVE_BYTES` and
   `OSOE_COLLECTIVE_PIPELINE` rows, which spoke of reductions and broadcasts
   only, the limits' mismatched collectives, which now say what a mismatched
   scan does - compared with its tree parent when large, nothing compared when
   small - the pairs sentence (rounds of disjoint pairs reach 253 a PE only at
   254 PEs; 129 PEs' rounds all succeed), and `test-scan.c`'s comment.
4. **A scan's pieces have no header, and were left in the lanes uncleared**,
   so the first part's guarantee - a slot nobody rewrote reads as nothing - no
   longer held after a scan: an invalid program's broadcast whose two leads
   disagree on its class could take a scan's leftover bytes as a piece, if
   their first 16 bytes matched the header it expects. A large scan now clears
   a slot where a header would be once it has read it.

## Verification

On the tree as verified, `28d6710` - both parts of step E on `main` at the
first part's merge, so `main` whole once this part merges; the merge differs
from it only in the README, the design notes, the records, the decisions
index and the Pi's evidence - in the `osoe` VM under the lock
(`build/scratch/team-scans-verify/run1/`):

- **The four presets**, the floor pins on: debug 716 of 716, release 716 of
  716, asan 713 of 713, tsan 474 of 474.
- **Tier 3**: 113 of 113.
- **The models**: 29 of 29.
- **`osoe-pacer`**, whose collectives include both sums' scans: its collectives
  a minute each at `OSOE_SMALL_COLLECTIVE_BYTES` of 128, 1,024 and 16,384 on
  three logical nodes of two PEs; its teams and threads two minutes; ten
  minutes of `mix` on four of two: `failures=0` on every one.
- **SOS**: its team, collective, active-set and barrier programs, 47 of them,
  on `0,1` and `0,0,1,1`: every run clean but `shmem_team_b2b_collectives`'s
  two, as on step D's tree and the first part's.
