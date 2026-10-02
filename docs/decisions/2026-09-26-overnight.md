# Overnight, 2026-09-25 to 26: what merged, and the questions for the author

The author approved three items on the evening of the 25th - "approve 1, 2 (c)
and 3 - go" (`docs/decisions/2026-09-25.md`) - under the standing direction to
merge as each piece is verified and to keep pressing ahead on anything the Pi
cluster does not block. This is the record of the night that followed, and the
handoff for the next session: where things stand, every decision taken without
the author and how to undo it, the five questions waiting for the author, and
where everything is.

## Where things stand

*(Main at `ed6eb6a`, about 07:00 on the 26th.)*

Everything the approvals opened is on main, each piece verified on the tree it
merged, in this order:

1. **Pipelined collectives** (phase 1 step 5 slice 5; merge `687bad1`). The
   generic internode phase keeps D = `OSOE_COLLECTIVE_PIPELINE` pieces in
   flight (default 2, 1 to 8), agreed at the rendezvous and between a node's
   PEs. 1 MiB broadcast on five nodes at a fan-out of 2, in the VM: 12.0 ms at
   D = 1, 2.2 ms at D = 4.
2. **The `pe_quiet` model** (`0b9c818`): the rule that a full quiet never
   piggybacks on a `pe_quiet`, the last open point of `THREAD_MULTIPLE` before
   its slices (`tools/modelcheck/pe_quiet_model.py`).
3. **Step 8's proposal** (`da181b2`): contexts and `THREAD_MULTIPLE` in three
   slices, for the author (question A below).
4. **Team creation** (phase 1 step 7 slice 2; merge `ad22087`):
   `shmem_team_split_strided`, `split_2d`, `destroy`, and every team routine on
   the teams they create. 22 of SOS's 23 team tests pass; the 23rd needs
   contexts.
5. **The allgather** (phase 1 step 5 slice 6; merge `fff1700`): fcollect and
   collect by Bruck's steps among the node leads. 1 MiB per PE on four nodes
   14.1 -> 2.0 ms; 8 bytes per PE 176 -> 280 us (question C).
6. **Block-strided RMA** (phase 1 step 9 slice 1; merge `e61a172`):
   `shmem_ibput`, `shmem_ibget`, every form.
7. **The staged-allgather model** (`1e11ff4`): evidence for question C.
8. **The Pi runbook's next session** (`5c5bcd2`, `ed6eb6a`): pipelined
   broadcasts and reductions at fan-outs of 4 and 2, and the allgather's
   fcollect at one and four PEs a node (`docs/pi-cluster.md` §10).

**Verification, per merge**, in the VM, on the tree that merged (main's tree
equals the verified tree each time):

| Merge | debug / release / asan / tsan | tier 3 | mutations caught | load loop | SOS sweep |
|---|---|---|---|---|---|
| pipelining `687bad1` | 395 / 416 / 394 / 323 | 34 | 9 of 9 | 30 runs each, 60 of the AM stress test | - |
| team creation `ad22087` | 411 / 432 / 410 / 333 | 34 | 6 of 6 | 30 runs each | 151 pass |
| allgather `fff1700` | 422 / 443 / 421 / 333 | 34 | 7 of 7 | 30 runs each | 150 pass |
| block-strided RMA `e61a172` | 424 / 445 / 423 / 334 | - | 5 of 5 | 30 runs each | SOS's c11 `ibput` test, renamed, passes |

Then **the whole tier-2 suite passed 30 times over beside 16 busy loops**, 347
tests a pass, 10,410 runs, with no failure (`build/scratch/flake/v1`, `v2`).

**SOS**: 150 programs pass on both maps. The 21 failing runs are the eight
programs that ask for `THREAD_MULTIPLE` (16), `shmem_team_context` (contexts, 2),
`shmemalign` (an alignment the spec leaves undefined, 2), and
`shmem_team_b2b_collectives` on 0,0,1,1 - which collects 40 longs into a
destination of 10 and is now stopped by name where the old gather corrupted
memory silently. Two C11 tests do not build (`_Generic` over `short *`, as
before).

## Decisions taken without the author

Each is what an adversarial review found, or what the brief left to judgement;
each says how to undo it.

1. **The pipelined loop takes whichever input is ready** (`6dfb1f6`). The
   slice's review found the loop, built exactly as the approved model's, waiting
   for its children while a release sat unconsumed, which held every tree
   deeper than one level to about h / (h - 1) times one piece at a time's speed
   whatever D - the Pi's four nodes at a fan-out of 4 are one level deep and
   would have hidden it. The model now lets a lead take either ready input,
   and passes; the timing simulation and the VM agree the fix gives D times.
   *Undo*: revert `6dfb1f6` - a loss of speed, not of correctness.
2. **A region wait's late wake counts as lost only if nothing moved**
   (`5bcc6b7`, `d6a67d5`). The load loop failed the new AM stress test once in
   seven runs on `invariant.missed_wake`: a waker descheduled between its store
   and its wake looks like a lost wake for an instant, which `base/epoch.c` had
   measured and judged with a margin, and the region waits did not. They now
   do, as `transport-am.c` does. 60 runs under load then passed. *Undo*: revert
   both - the oracle then flags that race again.
3. **A node's PEs agree on the fan-out and D** (`14eaee4`), and a heap-size
   backstop refuses node-mates whose heap objects differ (`1ae8319`): a created
   team's local lead may be any PE of a node.
4. **Team creation's review fixes**: a created team's node set is built on its
   local leads alone, O(S) or O(S log S), with no term in the job's node count;
   a failed claim no longer burns numbers (compare-and-swap); a deterministic
   test of the parent sync, which deleting the sync now fails.
5. **The allgather adds a third upcall to `internode.h`**,
   `osoe_internode_team_world_ranks` - arguably the brief's stop condition.
   Kept; question E. Its review fixes: a team's member intervals found once and
   each call O(intervals), READY adds sent first, a 24-byte shape on step 0's
   READY line that names a size mismatch before any byte lands (replacing the
   removed `shmem.collective-divergence.gather`), and a counted, not computed,
   share count.
6. **Step 9 opened without a question** - block-strided RMA - since every form
   follows one the library already has (`docs/prompts/done/phase1-step9-slice1-prompt.md`).
   Sessions (§9.9) turned out to exist already, their hints accepted and
   ignored as the spec allows; the status page said otherwise and is corrected.
7. **Housekeeping, at the author's request on the morning of the 26th**: every
   worktree removed but the main checkout (the disk was at 98%; now 33 GiB
   free). Nothing was lost: uncommitted changes of 15 worktrees are patches and
   tarballs in `build/scratch/worktree-archive/` (`INDEX.txt` says whose), and
   the heads that were on no branch are the branches `archive/wt-cA`,
   `archive/wt-combined`, `archive/wt-muts4`, `archive/wt-muts4a`,
   `archive/wt-muts4f` and `archive/wt-s4d`.

## Questions for the author

**A. Step 8: contexts and `THREAD_MULTIPLE`** (`docs/prompts/done/phase1-step8-slice1-prompt.md`,
eleven numbered decisions; slice 1's build brief is drafted there too). Three
slices: contexts, serial only; then threads, with an interim per-context mutex
on shareable contexts; then §4.11's lock-free shared mode. The choices that are
really choices: that slicing; `OSOE_MAX_CONTEXTS` default 64; handles checked on
every operation; team contexts translating PEs per operation; the landing zone
as extents claimed by CAS for blocking and nonblocking replies; a full thread
table ending the job at every thread level; and item 11, that collectives'
heap-borrowed scratch is not symmetric under `THREAD_MULTIPLE` - a split's word
can move to the internal region, `collect`'s lengths need a design. Eight SOS
programs fail only for want of it.

**B. Team numbers for `split_2d`** (design §10). A `split_2d` claims a number
for every row and column team at once, from the job-wide space numbers are never
reused in: a square grid over P PEs takes 2 sqrt(P), so above about 2.6 x 10^5
PEs none fits even at `OSOE_MAX_TEAMS`' ceiling of 1,024, and at the default of
32 SOS's `split_2d` test loses some splits from six PEs. Proposed: raise the
default to 64 now (each number costs (fan-out + 1) x D x 16 KiB of address space
a PE, 160 KiB at the defaults); design numbers unique per node - a colouring
every PE computes from the grid and the rank map - with the RoCE backend.

**C. Small gathers.** The allgather takes ceil(log2 N) + 1 hops of about 30 us
where the one-level tree took two, so 8 bytes per PE on four nodes costs 280 us
against 176. At scale Bruck wins even for small gathers (N = 4,096: about 13
hops against the tree's 24). The review's proposal: stage step 0's one share in
two per-team slots alternating by gather, which takes READY's hop off the path -
simulated at four nodes, 92 -> 63 us (the tree 73). Model-checked (`1e11ff4`):
safe at two to five nodes; one slot overwrites an uncopied share at four
nodes; a copy before DATA reads a stale slot. A crossover back to the tree would
pay only at five nodes or fewer. Build it, or accept the cost?

**D. Prefix scans (1.6), and when.** Design §5.8's "runs and a parallel prefix
across them" leaves open how a node holding many runs (cyclic layouts, one run a
PE) takes their messages; the internal region is sized for one tree position a
node. Only SOS's `shmemx_` tests use scans; no corpus program does. Proposed:
after step 8's contexts, model first.

**E. The allgather's upcall, or the team's record?** The backend now asks the
SHMEM layer for a team's members as world ranks. Every team with an internode
phase is strided in world ranks - tables come only from splits of
`SHMEM_TEAM_SHARED`, on one node - so the record could carry (world_start,
world_stride) in O(1) and the backend would learn a team, as before, from its
record alone. Kept as built unless the author prefers the record.

## Next session

1. The author's answers to A-E.
2. The Pi cluster, when it is up: `docs/pi-cluster.md` §10, now with the
   pipelined collectives and the allgather's fcollect.
3. After A: step 8's first slice, contexts, from the drafted brief.
4. Lessons for the successor API added tonight: L45-L47 in
   `docs/design/successor-api-stage-setting.md` §2.

**Where things are**: the status page (Claude Docs, "osoe - project status")
lists the questions at its top; the night's review scripts, timing simulations
and model variants are in `build/scratch/evidence-2026-09-26/`; verification
outputs under `build/scratch/{pipeline,teams/split,allgather,ibput,flake}/`.

*Added 2026-10-02: the status page is private to the author and is not in this
repository. The paragraph above stays as it was written.*
