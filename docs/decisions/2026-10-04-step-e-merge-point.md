# 2026-10-04: step E's merge point

The merge point of `docs/prompts/done/phase1-step7-slice4-prompt.md`, the team
redesign's step E: with both parts on `main` - the first,
`2026-10-04-team-lanes.md`, and the second, `2026-10-04-team-scans.md` - the
brief asks for the four presets, `tier3` and `models` on `main` whole; the SOS
sweep at 4 × 2 and past 61 PEs, the release checklist's acceptance; and on the
Pi at 4 × 4 the measurements against step D's merge point, alternating. Then
the full review, whose deliverables are `phase1-step7-slice4-*`, and its fixes:
`main` is then the release candidate the release point is checked on.

## The merge

`main` went from `84a64b8` to `375973a` by two commits, each one part, each
made by `git commit-tree` so that `main`'s tree is the tree that was verified
plus its text: the first part (`e044b08`) and the second (`375973a`). Each part
was read adversarially before it merged - the record of each read is its
part's, and the second part's read found the cost of the grant when a parent
enters late, which both parts' notes now state.

## On `main` whole

The second part was verified on `28d6710`, which held both parts - the second
rebased onto the first's merge - so its run is `main` whole's: `main` differs
from it only in the README, the design notes, the records and the Pi's
evidence. In the `osoe` VM under the lock: the four presets with the floor pins
on, debug 716 of 716, release 716 of 716, asan 713 of 713, tsan 474 of 474;
tier 3 113 of 113; the `models` preset 29 of 29; `osoe-pacer`'s collectives at
three settings of `OSOE_SMALL_COLLECTIVE_BYTES`, its teams and threads and ten
minutes of `mix`, `failures=0` (`build/scratch/team-scans-verify/run1/`).

## The SOS sweep

The whole suite at 4 × 2 on `main`'s archive, with its `oshcc`
(`build/scratch/merge-e/sos/`): 161 programs clean; the two C11 programs that
do not build, `c11_shmem_test_all_any_some` and
`c11_shmem_wait_until_all_any_some` (SOS's `_Generic` over `short *`, an
extension); and the two failing runs, `shmem_team_b2b_collectives`, which
collects more than its destination holds, and `shmemalign`, an alignment the
specification leaves undefined - the runbook's. Line for line step D's merge
point's. The team and collective programs on `0,1` and `0,0,1,1` ran in each
part's verification, clean but `shmem_team_b2b_collectives`.

## Past 61 PEs: the release checklist's acceptance

On a release build of `main` in the `osoe-load` VM, 70 PEs on one node and on
ten logical nodes of seven, `SHMEM_SYMMETRIC_SIZE=4m`, at the default
`OSOE_MAX_TEAMS` of 256 (`build/scratch/merge-e/acceptance/`), as step D's
second part ran it at 64 and 128:

| Program | 70 PEs, one node and ten |
|---|---|
| `shmem_team_reuse_teams` | exit 0, no split failing |
| `shmem_team_split_2d` | exit 0, no split failing |
| `shmem_team_max` | the 64 teams it asks for on every PE, none failing |
| `reduce_active_set`, `broadcast_active_set`, `repeated_barriers` | **nothing skipped** - at 64 they skipped 9 of 70 |
| `collect_active_set` | does not run: "Test requires fewer than 32 PEs", its own `MAX_NPES`; at 32 PEs, on one node and on four logical nodes of eight, exit 0, all 32 of its active sets |

And the per-node split of `SHMEM_TEAM_SHARED`, one PE a logical node: every
node's team valid at 61, 62 and 64 nodes. Step D's record gave
`collect_active_set` "nothing skipped" at 70; it had not run there either - the
program refuses past 32 PEs before it collects anything - and the checklist's
item now reads it at 32.

## The Pi

`main` against step D (`158c13e`), alternating, three runs a side, the second
part's probe and `osoe-bench` as in its three sessions (`docs/pi-cluster-results.md`,
"Step E", the merge point's runs; the raw outputs under
`docs/design/evidence/pi-cluster/2026-10-04-step-e/merge/`). `osoe-pacer --quick`
`failures=0` on both, 6.9 s each.

The session's own medians moved the barrier 12% and the 8-byte reduce 15% -
lines step E does not touch, and a stop-and-ask if they had moved. They had
not. Under `--cpu-bind=none` both lines are bimodal from job to job, on both
trees: a run lands near 182 µs or near 207 for the barrier, near 202 or near
229 for the reduce, and within each mode the trees agree to a microsecond
(181.6 and 181.9; 206.9 and 207.7). This session drew D's faster mode twice and
E's once. Over all twelve runs a side of the four sessions the barrier is
+1% and the 8-byte reduce +2%; and `osoe-bench`, under Slurm's default
binding, shows one mode on both trees in every run of every session - the
barrier 175 to 177 µs, the 8-byte reduce 197 to 198, the broadcast 195 - E
and D within half a percent. The rest, pooled: broadcasts within 1%; the large
reductions of 2 and 4 KiB +11 and +19 µs, the grant; scans of 2 and 4 KiB +138
and +88 µs, the grant on one piece; 8 to 16 KiB equal, 32 KiB 0.94, 64 KiB
0.79 and 1 MiB 0.67 times D.

## What E leaves

- **For the review**, round six, at `375973a`: its five questions, the
  refutation pass and the synthesis, and the fixes it names.
- **For after the release**, on the checklist's list: a team of one PE claims a
  pool block it does not use (step D's first read, item 5).
- **For the author to know**: the grant costs a large collective up to two
  hops, once, when a parent enters after its children - less the parent's own
  intranode time for a reduction - where step D's per-number slots had the
  first piece waiting. The second part's adversarial read found it and the Pi
  measured it on a 2 KiB scan, +228 and +259 µs with the root or a member with
  children late, against +119 when the members enter together; the author had
  accepted "a hop or two" for the scans of 2 to 4 KiB, and the README's cost
  rows and design §5.8 state both cases. Nothing reduces it in this release.
