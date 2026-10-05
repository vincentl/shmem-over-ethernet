# 2026-10-05: the fixes of the round-six review (step E's merge point)

The round-six review ran on `main` at `375973a`, the tree step E left, as the
last of the three merge points of `docs/prompts/task-first-release.md`: five
investigators, the refutation of the one finding rated *fix before release*,
and a synthesis, `docs/reviews/archive/phase1-step7-slice4-review.md`, whose
verdict is *release, after the named changes*. This record is those changes,
made on the branch `review-six-fixes` from `main` at `b2804cd` (step E's merge
point), by Claude under the task's mandate - decide what a brief does not list,
and record the decision with its undo - but for the two the author took on the
night of 2026-10-04 (`2026-10-04.md` items 10 and 11).

## The findings the author decided

1. **A large reduction's or broadcast's piece carries 16 KiB** (scale U1; the
   author: "Fix U1 now"). The 16-byte header had sat inside the lane's 16 KiB
   slot, so a piece held 16,368 bytes and every power-of-two size from 16 KiB
   went in one piece more. `internal-region.h` gains `OSOE_INTERNAL_PIECE_BYTES`,
   16 KiB, and a slot, `OSOE_INTERNAL_SLOT_BYTES`, is now the header and the
   piece beside it, 16,400 bytes - a multiple of sixteen, so that every
   element stays aligned; `piece_bytes`, the element-width check and the scans'
   largest piece use the piece. A lane grows by 160 bytes and a PE's region by
   2,720, which leaves every figure the README gives - 6.03 to 6.53 MiB a
   region, a 160 KiB lane - true at its precision. Tests: `test-pipeline.c`'s
   arithmetic (1 MiB is 64 pieces, not 64 and one) and `test-lanes.c`'s one
   `PIECE_BYTES` (`shmem.lanes.boundary`'s three-pieces-and-a-byte size, which
   the old piece in its arithmetic had made three). Measured before and after
   (`docs/design/evidence/pi-cluster/2026-10-04-step-e/review/u1/`): in the
   development VM, four logical nodes, a 32 KiB broadcast 348 against 187 µs, a
   reduction 381 against 255, 64 KiB 523 against 393 and 590 against 442 -
   where 32,768 bytes had taken 1.7 times 32,736, they now take the same; on the
   Pi at 4 × 4, the power-of-two sizes 0.95 to 0.98 of before, the others
   unchanged; at 4 × 1 the broadcasts 0.91 to 0.97 and the reductions of 16 and
   32 KiB 0.98 and 0.97, those of 64 KiB 3% slower on both sides of the edge,
   within the runs' spread. *Undo*: the two constants.
2. **Design §10's "thorough 1.6 spec read" is closed** (conformance M2; the
   author: "close M2"): §10's blocking list is empty, its leftover - the session
   routines' semantics, should they be more than no-ops - on the checklist's
   after-release list, and §4's note to verify each collective's need of
   symmetric memory "during the spec read" says it was.

## The finding rated fix before release

3. **`osoe-stat --summary` walks barriers and syncs, not the data collectives**
   (readiness Q5-1). The refutation was the lead's, in the source: `stat.c`'s
   `read_tree_state` reads the tree state's epoch fields, which only
   `progress.c`'s publish writes, and the generic phase writes none. As the
   investigator proposed: the README's command and its paragraph on a hang, the
   `--help` text and design §3.14 say what the view walks, and that a hung
   reduction, broadcast, gather or scan shows in the PE lines. The view's own
   walk of a data collective is on the checklist's after-release list.

## The minors taken

- **Correctness**: `shmem.lanes.threads` and `.two-a-node`
  (`test-lanes-threads.c`), the investigator's stress as a test, failing 3 of 3
  under `scan_one_lane` and `one_lane` (H1); `tools/modelcheck/lane_scan_model.py`,
  the second part's adversarial model brought into the tree, `model.lane_scan`,
  36 s, its breakages failing and `scan_one_lane` among them (H2);
  `lane_model.py`'s `node_small_down`, holding with one thread a node and failing
  with two at 63 states by DOWN (H3); the grant's two guards in design §5.7 and
  §5.8, `number_taken`'s comment and `test-lanes.c` (H4); the lanes tests under
  ThreadSanitizer but `class-split-hang`, whose deadlock detector judges nothing
  under tsan, and the two-a-node map, whose node-mates meet across processes
  (H5); the scans' comment on memory per team (H6). The pacer's exercise of
  collectives on per-thread teams goes on the after-release list.
- **Scale**: the Pi results and the records say what hides the grant after a
  barrier - the barrier's order, the root's node released a hop before its
  children - with the lead's `reduce-skew` (U2); the README's job object under
  plane and arbitrary distributions (U3); the pipeline row's piece (U1, before
  the author's answer, then rewritten for the fix).
- **Conformance**: a large scan's members compared only when its team spans
  nodes (M1); M3 waits for the checklist's SOS item; M4 is H1's test.
- **Readiness**: the class-split refusal and the `/dev/shm` refusal say what is
  wrong, the latter in a 768-byte buffer that its longest form fits (Q5-2,
  Q5-3); `SHMEM_INFO`'s row (Q5-4); the checklist's three missing items (Q5-5);
  the history section's qualifiers, the evidence count, `2026-10-04.md`'s commit
  count and its piece rule as built, the scans' rule a choice made without the
  author in their record, with its undo (Q5-6); what the other nodes print when
  a job ends (Q5-7).
- **Portability**: the README's x86-64 passages of this tree and its
  x86-ordered aarch64 run, and the checklist's x86-64 item (Q1); the containers'
  counts (Q2); the quick start's `/dev/shm` (Q3); a put's wake best effort
  (Q4).

## Choices made without the author

- **The slot 16,400 bytes, not a line's multiple.** A slot of 16 KiB and a
  128-byte line would have kept every slot on a line of its own, at 1,280 bytes
  a lane and every region figure of the README moved; the header and the piece
  alone keep sixteen-byte alignment, which every element needs, and the figures.
  *Undo*: `OSOE_INTERNAL_SLOT_BYTES` rounded up to `OSOE_INTERNAL_LINE_BYTES`.
- **Two tests kept from tsan** (H5's fix), each with its reason in the
  CMake comment. *Undo*: the two guards.
- **The dated records left as written**: the team-lanes record's "the one
  guard" and "tsan left out", and the step-D record's "nothing skipped" for
  `collect_active_set`, are history; the living documents are corrected.

## Verification

On the branch's tree as verified, `81ba75a` - from which the merge differs only
in this record, the synthesis and the five reports, the decisions index and the
checklist - in the `osoe` VM under the lock (`build/scratch/review-e/verify/run1/`):

- **The four presets**, the floor pins on: debug 718 of 718, release 718 of
  718, asan 715 of 715, tsan 490 of 490 - sixteen more under tsan than at step
  E's merge point, the lanes tests and `shmem.lanes.threads` among them.
- **Tier 3**: 113 of 113.
- **The models**: 30 of 30, `model.lane_scan` in 40 s.
- **`osoe-pacer`**: its collectives at the three settings, its teams and
  threads, and ten minutes of `mix`, `failures=0` on every one.
- **SOS**: the team, collective, active-set and barrier programs on `0,1` and
  `0,0,1,1`, clean but `shmem_team_b2b_collectives`'s two, the runbook's.

And U1's measurement before and after (above); the debug suite after each step
of the work, 718 of 718 at its end.
