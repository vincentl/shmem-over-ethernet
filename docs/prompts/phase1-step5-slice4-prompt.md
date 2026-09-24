# Implement phase 1, step 5, fourth slice: collect, fcollect, alltoall and alltoalls

You are finishing **§7.1 step 5** of **osoe** (OpenSHMEM Over Ethernet),
"Collectives". The first three slices built reductions and broadcast, put their
internode phase behind `internode.h`, and gave the deprecated active-set forms
over the whole job. This slice adds the rest of §9.10's data movement:
`shmem_collect` and `shmem_fcollect` (§9.10.8), `shmem_alltoall` (§9.10.5) and
`shmem_alltoalls` (§9.10.6) - the typed, sized and `mem` forms, their C11
interfaces, and the deprecated active-set `collect32`/`64`, `fcollect32`/`64`,
`alltoall32`/`64` and `alltoalls32`/`64` under slice 3's whole-job rule.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

*Written by Claude on 2026-09-24. The corpus survey of that day found fcollect
the most used of the four (17 of 29 programs, most for end-of-run timing
gathers; ISx's two), alltoall with one hot user (PRK's transpose, whose
`transpose_a2a` passes buffers from `aligned_alloc` and is refused, rightly),
and alltoalls with none. The decisions below are Claude's, for the author's
review.*

## Read first

1. `CLAUDE.md`.
2. The spec, §9.10's preconditions ("the dest array on all PEs in the team is
   ready to accept the result"; "the source array at the local PE is ready to
   be read by any PE") and §9.10.8-§9.10.10.
3. Design §5.5 (as built), §5.8, §4.10; `internode.h`'s data half and its
   contract; `internode-generic.c`'s header comment.
4. `collectives.c` (`data_collective`, `member_copy`), `reductions.c`,
   `broadcast.c`, `barrier.c`.

## Decisions

1. **fcollect gathers into the lead's dest, then the leads exchange, then the
   members copy out** - §5.5's two levels. The lead, once every member has
   arrived, zeroes its dest and copies each local member's block to its
   rank's place: O(L n) of its own, under the O(P n) each member spends
   copying the result out, so not the node's bottleneck - and no member ever
   writes the lead's dest, which a slower member may still be reading for the
   collective before. The dest then holds the node's blocks and zero
   elsewhere, and goes to the internode phase; then the release and the
   members' copy-out, as for a broadcast. *(The first draft had each member
   copy its own block in, in parallel, guarded by the last copy-out's
   counter; the lead's copies are the simpler of the two at no cost that
   shows.)*
2. **The internode phase is a new operation at the seam,
   `osoe_internode_collect(team, epoch, buffer, bytes, routine)`**: on entry
   `buffer` holds this node's blocks at their places and zero elsewhere; on
   return, every node's. A backend with an allgather does one; **the generic
   implementation is the OR reduction over bytes** that the tree already
   runs - blocks at disjoint places, zero elsewhere, OR to their
   concatenation - so no new tree protocol, no new divergence check, and no
   write into another node's memory outside the internal region. Its cost is
   stated rather than hidden: each lead moves about 2k·P·n bytes where an
   allgather moves P·n, in `ceil(P·n / slot)` passes of depth `log_k N` - fine
   for the corpus's gathers of a few words per PE, and the reason the seam
   exists for a backend to do better.
3. **Writes into another node's dest are not used.** The spec's precondition
   makes them legal, and SOS relies on it; but the lead's dest is also where
   its node's members copy their result from, so a fast node's write for the
   next collective can land under a slow node's copy-out of this one. The
   internal region's slots were built to rule that out, and this slice keeps
   them the only thing one node writes in another for a collective.
4. **collect's blocks have per-PE sizes**, so every PE first fcollects the
   lengths (eight bytes per PE) into room the symmetric heap lends for the
   call - taken and given back by every member alike, which leaves the
   allocator as it was (§4.4) - and places its block at the prefix sum of
   those before it; then it is fcollect's path with the total. The lengths are
   O(P) per PE, as dest already is. A scan (§9.10.10, not built) would give
   each PE its place in O(1) memory, and is the better tool when it exists.
5. **alltoall and alltoalls are direct puts, then a quiet and a team sync.**
   Every block goes from its source PE to its dest PE, which no tree shortens,
   and nothing of the library's reads dest, so the hazard of 3 does not arise:
   the precondition is the application's to keep. Intranode puts are copies.
   The sync is the team's, with its quiet first, so each PE returns with every
   block addressed to it landed.
6. **The active-set forms follow slice 3**: the whole job or a fatal naming
   the set; pSync never touched; each is its team form on `SHMEM_TEAM_WORLD`.
7. **Types follow Table 5** (the RMA types), as the spec says for all four,
   with the `mem` forms in bytes; the C11 generics dispatch on the dest type.

## Tests

fcollect and collect: every root rank's block in place, from one element to
more than a slot's worth of dest; collect with sizes that differ, including
zero; in place is not allowed by the spec and not tested. alltoall and
alltoalls with strides; the deprecated forms with a static pSync; a partial
active set refused by name. One node at 1, 2 and 4 PEs, both maps, tier 3's
shapes; the headers as C99, C11 and C++. And back to back on one dest array -
fcollect after fcollect after a reduction, with no barrier between - which is
decision 3's hazard.

## Not in this slice

Split teams (§7.3); an internode allgather better than the OR reduction, which
is a backend's to offer through the seam.
