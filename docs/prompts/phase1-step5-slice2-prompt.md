# Implement phase 1, step 5, second slice: the collectives' internode seam

You are continuing **§7.1 step 5** of **osoe** (OpenSHMEM Over Ethernet),
"Collectives". The first slice built reductions and broadcast on §5.5's
accumulator. Their internode phase - the node leads meeting over a k-ary tree -
runs in `collectives.c`, above the transport boundary, over put, fenced rmw and
a wait on `OSOE_REGION_INTERNAL`. That is correct on every backend, and no
backend can replace it: it is the SHMEM layer's own code, and it receives the
operation as a C function, which no NIC or switch can run. This slice puts it
behind `internode.h`, where the barrier's internode phase already is, and keeps
today's code as the implementation every backend may use.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

*Written by Claude on 2026-09-24, after the author asked whether the tree's
layers - core, AM, osoe, shmem - are cleanly separated. The answer found this:
§5.8 deferred "a separate internal collective-transport header ... until
reductions land and show what it must contain", `internode.h` forecast the same
header ("It is not built: there are no data-carrying collectives yet"), and the
first slice landed reductions without it. Fixing it before collect, fcollect and
alltoall means each of them is written against the seam instead of adding one
more internode phase a RoCE port must move. The decisions below are Claude's, for
the author's review.*

## Read first

1. `CLAUDE.md`.
2. `shmem/src/internode.h` whole: addressing by team, the release upcall, and
   the paragraph "Why this shape survives data-carrying collectives", which this
   slice replaces with what was built.
3. §4.10: no capability queries above the boundary, binding at build time,
   every blocking call sweeps.
4. §5.5's as-built paragraph and §5.8 whole, rules 1-4 especially.
5. `docs/reviews/roce-transport.md` §6 "Not reachable through this boundary" and
   §8.4: negotiated collective offload stays rejected, a *mandatory operation with
   a generic fallback* does not.
6. `collectives.c`, `internode-am.c`, `wait.c`, `cmake/CheckBoundary.cmake`.

## What exists, and what does not

- **The barrier's half of `internode.h`**: a team's node set given once,
  `arrive(team, epoch)` down, a `released(team, epoch)` upcall back up, all
  asynchronous; AM implements it with the agents' tree (`internode-am.c`).
- **The data half does not exist.** `internode_phase` and `lead_piece` in
  `collectives.c` are the whole internode phase of both data collectives, and
  their per-team state (tree neighbours, expected arrival and release counts)
  sits in `struct osoe_team`.
- **The combine functions are one per C type and operator** (`reductions.c`),
  so `int` and `int32_t` have two identical copies of each.
- **The wait on an internal word is in `wait.c`**, the SHMEM layer's
  `shmem_wait_until` file, so anything below the API that waits must include an
  API header to do it.

## Decisions

1. **The data half is a blocking call on the lead's application thread**, not
   the forecast's arrival carrying a payload and a release upcall carrying the
   result. §5.8 rule 2 puts O(n) combining on the lead's thread, which is
   blocked in the call anyway; an upcall on the progress thread would only hand
   the data back to that thread. The barrier's half stays asynchronous: nothing
   in it is O(n). `osoe_internode_reduce(team, epoch, buffer, count, operation,
   element, routine)` and `osoe_internode_broadcast(team, epoch, root_index,
   buffer, bytes, routine)`, called once per node per collective by the team's
   local lead after the intranode phase, with the node's partial result (or,
   on the root's node, the root's data) in `buffer`; they return with the
   result in `buffer` on every participating node. They block, so they sweep
   (§4.10).
2. **The operation and the element are values, not code.** `enum
   osoe_operation`: and, or, xor, max, min, sum, prod. An element is a kind -
   signed integer, unsigned integer, floating, complex - and a width in bytes.
   That is the vocabulary offload interfaces use (libfabric's `fi_datatype` and
   `fi_op`, UCC's datatypes and reduction operations), where a function pointer
   can never be offloaded. The contract any implementation must meet: integer sum and
   product wrap modulo 2^N; floating of `long double`'s width means this
   platform's `long double`; every node ends with the same bits (§5.8 rule 3).
3. **Mandatory, with a generic implementation any backend may call.** Today's
   tree moves to `internode-generic.c`, on the boundary, as
   `osoe_internode_generic_reduce` and `_broadcast`. Each backend defines
   `osoe_internode_reduce` and `_broadcast`; AM's call the generic ones. A
   backend may offload some calls and hand the rest to the generic ones - per
   call, inside the backend - so nothing above the boundary asks what a backend
   supports (§4.10), and this is not the negotiated offload the RoCE review
   rejects.
4. **Addressing stays by team.** A broadcast's root is the index of the root's
   node in the node set the team was registered with (`osoe_internode_team`),
   not a node number and not a PE.
5. **Combine functions are generated per element, not per C type**: thirteen
   elements (int8 to int64, uint8 to uint64, float, double, long double,
   complex float, complex double), looked up by (operation, element), and used
   by both the lead's intranode combine and the generic internode phase. The
   element of each spec type comes from its C type - `sizeof`, signedness,
   list - so `char` is whichever `char` this platform has.
6. **The generic phase owns its state and the internal region.** Its per-team
   tree and counts move out of `struct osoe_team` into its own table, indexed by
   team; after this slice the SHMEM layer no longer names `OSOE_REGION_INTERNAL`.
7. **Waiting on a word of one's own region moves below the API**: `wait.c`'s
   `wait_at` becomes `osoe_region_wait` in `region-wait.c`, with its own
   comparison enum that `wait.c` maps the `SHMEM_CMP_` constants onto. So the
   generic implementation includes no API header, and `CheckBoundary.cmake`
   splits its "above" list into the API and the runtime and checks that
   **nothing but the API includes an API header** - the one-way rule the layering
   answer proposed, true before this slice and checked from it.
8. **§5.8 rule 4 is amended**: the intranode phase's algorithm is chosen above
   the boundary, the internode phase's by the backend. The generic
   implementation's future crossover to a ring (§5.8's large reductions) is
   made inside it.
9. **Every piece carries a header its receiver checks** - the team's epoch, the
   piece's number and bytes, and what the collective is - so that PEs calling
   different collectives, or the same one with different arguments, end the
   job by name. The first slice's review found that no epoch crossed nodes in a
   data collective, where §5.5 says it is asserted on receipt; the generic
   implementation's slot layout is this slice's, so the fix is too. The four
   waits still record one blocked detail, the epoch: telling them apart is a
   segment layout change, and not worth one here.

## Order of work

### Part A — the seam

`internode.h`'s data half with its contract; the operation and element types;
the combine table by (operation, element), generated from shmem.h's lists, and
`reductions.c` passing (operation, element) instead of a function.

### Part B — the generic implementation

`internode-generic.c`: `find_lead_tree`, `lead_piece` and `internode_phase`
from `collectives.c`, their state from `struct osoe_team`, the internal
region's layout; the broadcast's root taken as an index into the team's node
set. AM's two definitions in `internode-am.c`. `collectives.c` calls the seam.

### Part C — the wait and the boundary lists

`region-wait.c`; `wait.c` over it; `CheckBoundary.cmake`'s lists, with the new
files classified and the API-header check added. Check that the new check fails
on a deliberate violation before believing it passes.

### Part D — tests and notes

The first slice's reduce and broadcast tests, unchanged, on every map and
preset: the seam's only implementation is the one they already exercise, and
they are what a second backend must pass. A test that PEs calling different
collectives end the job with the header check's words; the internal region's
white-box test on the region wait. The three mutations, re-pointed at
the moved code. The notes: `internode.h`'s forecast paragraph replaced by the
as-built contract and why it is blocking; §5.8's deferred header recorded as
built and rule 4 amended; §4.10's "Outside the boundary: Collectives" bullet;
a §10 entry; the layering of `CheckBoundary.cmake`'s lists.

## Not in this slice

A second backend, and offload of anything; trees for split teams (the world
team only, as now, §7.3); §5.8's ring and scan algorithms; collect, fcollect,
alltoall and alltoalls, which come next and add their entries to this seam as
they are written.
