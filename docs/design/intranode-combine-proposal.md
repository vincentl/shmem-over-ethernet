# The intranode combine, sliced over the node's members

*A proposal, written by the lead on 2026-11-02 at the author's word of that
morning (`docs/decisions/2026-11-01.md`, answer 6): the first of the four
design changes the large-cluster playbook names (`docs/prompts/large-cluster-playbook.md`,
C5 and the first row of its §5) to be modelled here before any cluster session.
Modelled and timed; not built. The author decides whether it is built, and
when.*

## 1. The problem

A reduction's intranode phase is §5.5's: every member arrives, the node's lead
waits for them all, then combines its L members' sources one after another
into its own `dest` - in the descriptor's order, so that a floating sum rounds
the same way every time - and only then hands the vector to the internode
phase (`collectives.c`, `data_collective_at`). That combine is O(L × bytes) on
one thread, before a byte leaves the node:

| PEs a node | A 64 KiB reduction's combine | A 1 MiB reduction's combine |
|---|---|---|
| 4 | 256 KiB of reads | 4 MiB |
| 16 | 1 MiB | 16 MiB |
| 64 | 4 MiB | 64 MiB |
| 256 | 16 MiB | 256 MiB |

At one core's rate through memory - 10 to 20 GB/s on a server's DDR, more
where the vectors fit a cache - the 256-PE node's 1 MiB combine is 13 to 25
ms, longer than the whole internode phase on a fast link (a 1 MiB reduction
across eight Google Cloud nodes took 5.9 ms by the chooser's tree, P37). H12
measured the shape of it on two cloud nodes at 16 to 256 PEs a node: a 64 KiB
reduction 296 µs at 16 and 1,287 at 256, where the broadcast, whose intranode
phase is a copy-out every member does for itself, went 200 to 368.

Two more costs sit on the same thread: the release, one `futex_wake` of every
waiting member, O(L) kernel work in the lead (`osoe_epoch_wake_all`; the
barrier's release is a chain, O(log L) deep); and, with `OSOE_CHECK_COLLECTIVES`
on, the lead's read of L - 1 members' call words. Neither is this proposal's
subject; the first is a candidate for the chained wake the barrier has.

## 2. The proposal

Once every member has arrived, **member i combines slice i** - the i-th of L
near-equal runs of elements, at element boundaries - of every member's source,
in the descriptor's order, into slice i of the lead's `dest`, and adds to a new
counter, `combined`. The lead combines its own slice, waits for `combined` to
reach the epoch's target, and runs the internode phase, the release and the
copy-outs exactly as today.

```c
/* every member, after osoe_call_say */
if (osoe_epoch_release_bump_eq(&b->arrived, (k+1) * L))
    osoe_epoch_wake_all(&b->arrived);               /* the last arriver wakes every waiter */
osoe_epoch_wait_eq(&b->arrived, (k+1) * L, routine);  /* was: the lead alone */
/* my slice of the lead's dest: elements [from, to) */
if (source != dest || !lead)
    memcpy(lead_dest + from, lead_source + from, (to - from) * width);
for (m = 1; m < L; m++)
    combine(lead_dest + from, source_of(local_ids[m]) + from, to - from);
osoe_epoch_release_bump_eq(&b->combined, (k+1) * L);  /* stamped: the lead waits on it */

/* lead */
osoe_epoch_wait_eq(&b->combined, (k+1) * L, routine);
... internode phase, release, wait for copied: as today
/* member */
... wait for released, copy out, bump copied: as today
```

**What does not change.** The result is bit for bit today's: each element is
combined once, from the lead's source first and then the members in
descriptor order, by whichever member owns its slice - rule 3 of §5.8 holds as
it did. The internode phase, the release, the copy-outs, the epochs and their
equality waits, the grants a large reduction sends as the lead enters
(`osoe_internode_reduce_enter`, still before the lead's wait), the P24 check,
the deprecated broadcast's root rule and the team of one are untouched. The
broadcasts are untouched: their intranode phase is already every member's own
copy. A small reduction gains nothing and is left serial by a threshold
(§5); the change is the large reductions'.

**What changes in the design notes.** Invariant 16, "before release, the local
lead is the only writer of its `dest`", becomes "before release, every
element of the lead's `dest` has one writer, which combines it whole" - the
property the single-writer rule was for. §5.5's three reasons for a single
writer are each met another way (§3 below), and its sentence that "parallel
combines would ping-pong the same cache lines anyway" is answered by the
slices: a line belongs to one slice, and only its owner writes it. The layout
gains one counter a team block, `combined`, alone on its line: a layout bump
(the control segment's version), and `osoe-stat`'s view of a member waiting on
`arrived` or a lead on `combined` wants the blocked record to name them.

## 3. Why it is safe: §5.5's three reasons, each met

§5.5 rejected members combining into the lead's `dest` for three reasons. The
model (`tools/modelcheck/intranode_combine_model.py`) is written to them, with
a property for each and a broken variant that drops the rule and fails it.

1. **Lost updates** - concurrent read-modify-writes of one element. The slices
   are disjoint: an element has one writer an epoch (WRITER). The variant that
   gives two members one slice fails it.
2. **In-place corruption** - a member combining before the lead has called
   reads the lead's source while the application still prepares it. Every
   member now waits for every arrival before it reads any source, so invariant
   17 (push, never pull) holds for the members as it did for the lead
   (SOURCE); the in-place cases of the model alias each member's source and
   dest. The variant that combines after its own arrival alone fails it.
3. **Epoch clobber** - a lead that returns early lets the next epoch's writers
   seed its `dest` while a slow member still copies out. The lead still leaves
   last; and the new argument, which the sliced design needs and the serial one
   did not: the next epoch's slices start only once every member has arrived
   at it, which each does only after its own copy-out of this one (RESULT).
   The variant whose lead does not wait for the copy-outs fails it in place -
   the next epoch's source is the dest - and holds where the application
   leaves the lead's `dest` alone, which is why the in-place case is the one
   that matters.

**An argument the model found.** Every member now waits on `arrived` by
equality, as the lead alone did; §5.4's equality waits are sound only under
bounded skew, and a member racing into the next epoch moves the counter past
the value a slow member still waits for. The skew is bounded by `combined`:
the lead releases only once every member has bumped it, and a member bumps it
only after passing its wait on `arrived`, so no member can arrive for epoch
e + 1 while one still waits for e's arrivals. The model's `serial` control
variant, first written with the members waiting on `arrived` but without
`combined`, was STUCK for exactly that reason; as built, members wait for
nothing but the release, and the variant now does the same and holds.

**Under `SHMEM_THREAD_MULTIPLE`** a thread of each member calls the collective
and combines that member's slice; nothing new crosses threads of one PE.

**The model's cases**: the sliced design at two to four members over two
epochs, separate and in place, under sequential consistency and x86-TSO -
the application's stores to its source, the combine's and the copy-out's are
plain stores drained by the locked adds that follow them, as the library's are
(§3.16) - every case holding with the engine's liveness check; the serial
design as built, holding; five breakages, each failing the property it is
named for. Thirty cases, 400,000 states the largest (four members in place
under TSO), seven seconds on a laptop. Registered as `model.intranode_combine`.

## 4. What it costs

- **The lead's critical path**: O(bytes / L) of combining plus one wait,
  instead of O(L × bytes). Each member: O(bytes) of reads, L × bytes / L, where
  it read nothing before its copy-out.
- **Memory traffic**: the same L × bytes of reads and bytes of writes a node,
  now from L cores at once - bounded by the node's memory bandwidth where the
  vectors do not fit the caches, not by one core's. On a two-socket node half
  of every member's reads cross the sockets, as the lead's did.
- **One wake more**: the last arriver wakes every waiter on `arrived`, where
  today it wakes the lead alone (or nobody, since the lead usually spins to
  its wait and sleeps). Members that arrive together rarely sleep; under skew
  the wake is one `futex_wake(INT_MAX)` by the last arriver, as the release
  is - or a chain. A measurement's question (§5).
- **Memory**: one counter's line a team block.
- **Code**: about sixty lines in `collectives.c`, the counter in `team.h` and
  the block's layout, the threshold's setting, the blocked record's detail,
  the design notes' §5.5 and invariant 16, the README's "Scaling with nodes"
  row and a test that fails without it (a reduction whose result or whose
  lead's combine count shows the slices).

## 5. What the probe measured

`docs/design/evidence/intranode-combine/combine-timing.c`, standalone: L
processes share L source vectors and one dest; process 0 combines every source
serially, then each process combines its slice; the median of 21 rounds, in
the development VM (four cores, Apple silicon under Lima; `run-vm.txt`):

| L | 64 KiB: serial, sliced µs | 256 KiB | 1 MiB | 4 MiB |
|---|---|---|---|---|
| 1 | 1.1, 1.0 | 5.7, 5.1 | 17.5, 17.5 | 70, 70 |
| 2 | 3.2, 2.1 | 15.6, 7.2 | 65, 34 | 256, 128 |
| 4 | 8.5, 5.0 | 80, 23 | 145, 37 | 636, 362 |
| 8 | 18.7, 6.3 | 77, 20 | 320, 124 | 1,329, 368 |
| 16 | 37.7, 12.7 | 160, 58 | 658, 179 | 2,726, 880 |
| 32 | 76, 46 | 329, 172 | 1,335, 314 | 5,572, 2,174 |

The serial time grows linearly in L, as the arithmetic says; the sliced time
at L up to the four cores is a quarter to a third of it at 256 KiB and 1 MiB
(3.5 to 4.0 times), and past the cores the gain stays near the cores' count -
2.6 to 4.3 times at 8 to 32 processes - since L processes share four cores
and the slices run as wide as the cores allow. At 4 MiB the four-core gain is
1.8 to 2.5: four sources of 4 MiB are past the cache, and the memory system,
not a core, is the bound. At 64 KiB and L of 2 or 4 the gain is 1.5 to 1.7,
the barrier's cost a good part of 5 µs. On a cluster node with a core a PE the
gain is L-fold until the node's memory bandwidth binds, by the same
arithmetic: at 256 PEs a 1 MiB reduction's 13 to 25 ms of combining becomes a
few hundred microseconds of reads per PE, in parallel.

The probe's barrier yields the CPU on every look; its first form spun, and at
eight processes on four cores measured the scheduler's slice, 8 ms, which the
library's futex waits do not pay. The numbers are of the memory system and the
scheduler, not of osoe; the suite's timing is the measurement that decides
(§7).

## 6. Alternatives weighed

- **A tree over the node's members** (§5.3's sketch, "a two-level intranode
  tree"): log₂ L levels, each a member combining one whole vector: O(bytes ×
  log₂ L) on the critical path, 8 vectors deep at 256 PEs, and L / 2 members
  writing their own scratch. The slices are O(bytes) deep and write no
  scratch. The tree spreads the arrival counter's contention, which the slices
  do not touch; §5.3's own trigger for that is a measurement of `sync_all`
  at 64 cores or more, not this.
- **Groups**: members in groups of g, one combiner a group into its own
  scratch, the lead combining the L / g partials: √L-fold at best, with
  scratch. Dominated.
- **The internode phase sliced too** - each member's slice sent by its own
  thread: the single-issuer rings forbid an application thread's send (§3.12),
  and the lanes are the lead's. Out of scope; C4 of the playbook.
- **Leaving it**: the cost is the lead's and grows with L; at 4 PEs a node it
  is nothing, at 16 small, at 256 the largest single term of a reduction's
  time by the arithmetic of §1.

## 7. Questions for the author

*Answered 2026-11-03*: the author took every recommendation below as his answer (`docs/decisions/2026-11-03.md`).

1. **Build it?** The model holds, the probe shows the arithmetic; the author
   decides whether the change - about sixty lines, a layout bump, a counter,
   invariant 16 amended - is built, and when: before the first cluster session
   (so that the PEs-a-node ladder times it), or after that session's numbers
   show the lead's combine binding. *Recommended*: after the first session's
   PEs-a-node rung at 64 PEs, built here for the second; the VM cannot time
   it past four cores, and the change is to a protocol the suite tests
   everywhere.
2. **The threshold.** Slicing a small reduction costs a wait and buys nothing;
   the small class (at most `OSOE_SMALL_COLLECTIVE_BYTES`, 1 KiB) stays
   serial, and above it a slice of at least a cache line - 128 bytes an
   element run - is the least worth a member's wait. A setting
   (`OSOE_INTRANODE_SLICE_BYTES`, the least bytes a slice, members beyond
   it combining nothing), or the fixed rule? *Recommended*: the fixed rule -
   slices of at least 1 KiB, the serial combine below L × 1 KiB - with the
   setting only if the ladder's numbers ask for one.
3. **The arrival's wake.** The last arriver wakes every waiting member -
   `osoe_epoch_wake_all`, O(L) in the arriver, which is an application thread
   and not the node's network - or the chain, O(log L) deep. *Recommended*:
   wake-all, as the one-node release does, and measure.
4. **The release's wake**, beside it: the data collectives' release is one
   wake of every member on the lead's thread, O(L); the barrier's is a chain.
   Not this proposal's, but the same measurement would show whether it
   matters at 256. *Recommended*: measured in the same run, changed only if
   it shows.

## 8. The record

The model and its cases: `tools/modelcheck/intranode_combine_model.py`,
registered as `model.intranode_combine`; the probe and its run:
`docs/design/evidence/intranode-combine/`; the decision record:
`docs/decisions/2026-11-02-intranode-combine-model.md`.
