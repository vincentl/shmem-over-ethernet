# Implement phase 1, step 5, third slice: the deprecated active-set collectives

You are continuing **§7.1 step 5** of **osoe** (OpenSHMEM Over Ethernet),
"Collectives". The first slice built reductions and broadcast; the second put
their internode phase behind `internode.h`. This slice gives programs written
against the deprecated active-set API the routines they call: the `_to_all`
reductions of the spec's Table 11, `shmem_broadcast32` and `shmem_broadcast64`,
`shmem_barrier` and `shmem_sync` - **over the whole job and nothing else**.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

*Written by Claude on 2026-09-24 at the author's direction: "We should implement
deprecated _to_all routines so we can test against some of the corpus
projects." Step 1 had recorded the opposite (§7.4: the constants, not the
routines, so that a call fails at the link). The corpus survey of the same day
found every application call site of these routines passing the whole job as
its active set - ISx, PRK, bale's `xshmem_broadcast64` and hclib's copies - and
only a tutorial example passing anything else, deliberately. The decisions below
are Claude's, for the author's review.*

## Read first

1. `CLAUDE.md`.
2. The spec's active-set rules (§9.10's "Active-set-based collectives"), the
   broadcast and reduction sections' deprecated parts, and Table 11
   (`docs/reference/OpenSHMEM-1.6.pdf`).
3. Design §7.4's row on the deprecated constants, and the header comment of
   `shmem.h` that states the step-1 decision.
4. `collectives.c`, `reductions.c`, `broadcast.c`, `barrier.c`.

## Decisions

1. **The whole job, or a fatal naming the active set.** An active set of every
   PE - `PE_start` 0, `logPE_stride` 0, `PE_size` the job's size, or any stride
   in a job of one - is the world team; any other set ends the job by name.
   Step 1's principle stands: a wrong answer is worse than a missing symbol,
   and a named refusal at run time is neither. Forming a group for an arbitrary
   set of PEs, with the spec's "only the PEs in the set call" rule, is machinery
   osoe does not have, and no corpus program needs it.
2. **`pSync` and `pWrk` are neither read nor written.** The spec requires
   `pSync` "restored to its original contents" on completion, which untouched
   is; its reuse rules then hold trivially, and `pWrk`'s size makes no demand.
   A static `pSync` - symmetric static data, which osoe does not otherwise
   support (§4.3) - works for the same reason.
3. **Each routine is its team form's engine on `SHMEM_TEAM_WORLD`**: the
   `_to_all` reductions call `osoe_reduce` with their operation and element, in
   `reductions.c` beside the team forms, so one C type maps to one element in
   one place; `shmem_barrier` and `shmem_sync` are `shmem_barrier_all` and
   `shmem_sync_all`.
4. **`shmem_broadcast32` and `64` leave the root's dest alone** - "updated on
   all PEs other than the root PE". The engine takes a flag: the root skips its
   copy-out, and when the root is its node's lead, whose dest is the node's
   accumulator, the node's copy is the root's *source* instead - already
   holding the data, read by the internode phase and the members' copy-outs and
   never written. `internode.h`'s broadcast contract says the root node's buffer
   is only read, which is what makes that legal.
5. **`shmem_sync` has two forms under one name**: the spec's C11
   `shmem_sync(team)` and the deprecated `shmem_sync(PE_start, logPE_stride,
   PE_size, pSync)`. C11 counts the arguments, as the context-taking generics
   do; C++ overloads, the deprecated form keeping C linkage; C before C11 has
   the deprecated form only, as the spec does.

## Tests

Every `_to_all` operator on each of Table 11's types, at one element and
several, with negative values where the order is signed, in place, one call
with a static `pSync`; `broadcast32` and `64` from every root, the root's dest
checked unchanged and every other updated, at a size that crosses pieces, and
in place; the active-set barrier and sync beside the C11 `shmem_sync(team)`;
`pSync` checked as it was at the end. One node at 1, 2 and 4 PEs, two
multi-node maps, tier 3's three shapes. A partial active set ending the job by
name. The headers compiled as C11, C99 and C++.

## Not in this slice

Arbitrary active sets. The active-set forms of collect, fcollect, alltoall and
alltoalls, which come with their team forms under the same rule.
