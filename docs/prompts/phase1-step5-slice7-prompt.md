# Phase 1, step 5, slice 7: the deprecated collectives on any active set

**Status: written 2026-09-26 (night), at the author's request to close
OpenSHMEM 1.6's two remaining gaps** (the other, the deprecated routines'
profiling names, is its own small change). Decided here, without a stop:
the algorithms below. They add no protocol to the node segment and no word
to the design's data structures; each uses the program's own `pSync` and
the library's public RMA, which is what the spec gives these routines.

The deprecated active-set routines of Annex F - `shmem_barrier`,
`shmem_sync`, `shmem_broadcast32`/`64`, the `_to_all` reductions of Table 11,
`shmem_collect32`/`64`, `shmem_fcollect32`/`64`, `shmem_alltoall32`/`64` and
`shmem_alltoalls32`/`64` - run today over the whole job and nothing else, and
any other active set ends the job by name (design §7.4, the known limit). A
team cannot stand in: creating one needs every PE of its parent, and only the
active set's members call. So a partial set gets algorithms of its own,
among its members only.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks.**

## Read first

1. `CLAUDE.md`; the spec's Annex F and §9.10 on active sets, `pSync`, `pWrk`
   and their reuse rules.
2. `collectives.c` (`osoe_active_set_whole_job`), `barrier.c`,
   `broadcast.c`, `collect.c`, `alltoall.c`, `reductions.c`, `combine.h`.
3. Design §7.4 (the known limit) and the ledger's line on active sets.

## Decisions

1. **The whole job keeps today's path**, the world team's collectives. Only
   a partial set takes the new one (`active-set.c`).
2. **Members only, over the program's `pSync`.** A member's rank in the set
   is `(pe - PE_start) >> logPE_stride`. Every signal is a count: an atomic
   add of 1 to a `pSync` word at the receiver, waited for as at least its
   start value plus the count, and taken back by the receiver with an atomic
   add of the negative - so a word is back at `SHMEM_SYNC_VALUE` when every
   member is done, and a fast member's next call, adding early, is counted
   in the next call and not this one. `pSync` is 256 longs here; the words:
   0-63 the barrier's rounds, 64 up-arrivals, 65 down-releases, 66 and 67
   collect's subtree total and offset, 68 and 69 collect's second arrival and
   release. pWrk is not used: the library may take private memory.
3. **A binomial tree over the set's ranks**, rooted at the collective's root
   (broadcast) or rank 0 (the rest), relative rank `v = (rank - root) mod
   size`: `v`'s parent is `v` less its lowest set bit, its children `v +
   2^k` for each `2^k` below that bit, and its subtree the contiguous ranks
   `[v, v + lowbit(v))`. log2 of the set's size in depth. **Up**, a member
   waits for its children, then signals its parent; **down**, a parent puts
   to each child with `shmem_putmem_signal` on the child's release word.
4. **A buffer is written only after its owner entered** (the ledger's rule
   for fcollect): every down phase follows an up phase, so a child has
   arrived before its parent writes its `dest`.
5. The routines:
   - **barrier and sync**: a dissemination barrier, ceil(log2 size) rounds
     of one signal each; barrier quiets first.
   - **broadcast**: up (arrivals), then down from the root, `source` at the
     root and `dest` elsewhere; the root's `dest` untouched, as the spec says.
   - **fcollect**: each member's block into its own `dest` at its rank's
     place; up, a parent gets each child's subtree range out of the child's
     `dest` into its own; down, the root's whole array to every child.
   - **collect**: an up pass of subtree byte totals (a child's total in its
     own `pSync` word, read by its parent), a down pass of offsets, then
     fcollect's up and down over the offsets.
   - **reductions**: `source` into `dest`; up, a parent gets each child's
     partial out of the child's `dest` in chunks into private memory and
     combines it into its own (`combine.h`); down, the root's result to
     every child.
   - **alltoall and alltoalls**: a barrier (every `dest` ready), each member
     puts its blocks straight to each member, a quiet, a barrier.
6. **Validation**: a set whose last member is past the job, a negative
   stride or size, a caller outside the set, a root outside it - each ends
   the job naming the routine and the values, as the whole-job check does.

## Tests

`test-active-set.c`: every routine on sets of stride 0 and 1, starting at 0
and at 1, of 1 member, 2 and all but one, on one node and across nodes;
two disjoint sets at once with their own `pSync`; the same `pSync` reused in
back-to-back calls with no barrier between; each result checked by value,
and `pSync` checked equal to `SHMEM_SYNC_VALUE` after the last call. The
existing whole-job tests stay.

## Stop and ask

- An algorithm here needs a word in the node segment, or anything that grows
  with the job rather than the set.
- The spec's reuse rules for `pSync` cannot be met by counting.
