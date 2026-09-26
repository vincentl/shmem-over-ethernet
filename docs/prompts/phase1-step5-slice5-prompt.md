# Implement phase 1, step 5, fifth slice: pipelined pieces through the internode tree

You are extending **§7.1 step 5** of **osoe** (OpenSHMEM Over Ethernet),
"Collectives". The generic internode phase (`internode-generic.c`) moves a data
collective's buffer through the tree of node leads in 16 KiB pieces, and each
piece makes a whole pass - up the tree, then down - before the next starts.
This slice keeps D pieces in flight instead.

*Approved by the author on 2026-09-25 (`docs/decisions/2026-09-25.md`, item 2),
after the Pi cluster measured a 1 MiB broadcast over four nodes at 36.5 ms and a
16 x 1 MiB fcollect at 773 ms. The protocol is decided and model-checked:
design §5.8, "Pipelined pieces", and `tools/modelcheck/pipeline_model.py`. The
allgather that replaces fcollect's gather is the next slice, not this one.*

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks.**

## Read first

1. `CLAUDE.md`.
2. Design §5.4 (epochs, bounded skew), §5.5 (as built: the generic phase), §5.8
   ("Pipelined pieces" - the four rules - and "An allgather for fcollect", for
   what comes next).
3. `tools/modelcheck/pipeline_model.py`: its node loop is the loop to build, and
   its breakages are the mistakes to avoid.
4. `internode-generic.c` whole, `internal-region.h`, `region-wait.h`
   (`OSOE_COMPARISON_GREATER_OR_EQUAL`), and `internode.h`'s contract.

## Decisions

1. **`OSOE_COLLECTIVE_PIPELINE`** is D: pieces in flight per level, default 2,
   between 1 and 8, read at each session's first init beside
   `OSOE_COLLECTIVE_FANOUT` and `OSOE_MAX_TEAMS` and agreed at the rendezvous as
   they are (`struct agreement` in `rendezvous.c`), since it fixes the internal
   region's offsets. The agreement's up header grows, so its magic moves on
   again (`osoeup-2` to `osoeup-3`, with the size assert that ties them). D = 1
   is today's protocol, and the model checks that too.
2. **The internal region**, which phase 1 step 7's first slice gave a block per
   team number, slots first and words after (`internal-region.h`). A team
   number's slot block becomes `(fanout + 1) x D` slots - `OSOE_INTERNAL_SLOTS_PER_CHILD`,
   today a constant 1, becomes D: D per child position, piece k in slot k mod
   D, then D release slots. Its words become `fanout` arrival counters - one
   per child position - and one release counter, each alone on the region's
   fixed `OSOE_INTERNAL_LINE_BYTES` line. At D = 2, 32 teams and a fan-out of 4
   the region is about 5 MiB a PE; say so where the capacity check, design
   §5.8 and the runbook (§1's memory line) give its size.
3. **The loop** is the model's. A lead keeps `up`, the next piece to send up,
   and `down`, the next release to consume:
   - while `up < pieces` and `up < down + D`: wait until every child position's
     counter shows piece `up` (at least), check each slot's header, combine
     (reduction and gather) or skip (broadcast, whose pieces go up empty), put
     the partial into the parent's slot `(position, up mod D)` with its header,
     and add one to the parent's counter for this position, fenced after the
     put;
   - otherwise: wait until the release counter shows piece `down`, check the
     release slot's header, copy it into the buffer, forward it to every child
     - a put into the child's release slot `down mod D`, then a fenced add to its
     release counter - and only then count it consumed.
   - The root sends nothing up: for each piece it waits for every child's copy,
     combines, and forwards.
4. **Counters are cumulative over the team's life and compared as at least.**
   Each lead keeps, per team, a base per child position and one for releases,
   and adds to them at a collective's end what its own tree says each received:
   the pieces, for every occupied position, and for the release base when it is
   not the root. Trees differ between collectives - a broadcast's is rotated to
   its root - so a position is fed by different children over time, and the
   bases are what keeps that straight (the model's rotated cases).
5. **The header check stays**, per slot: epoch, piece, collective and bytes. It
   is what names a divergence when the pieces meet.
6. **A counter pinned, not a time.** Add to the team's published tree state the
   most pieces this lead has had in flight at once in its last collective - the
   largest `up - down` it saw - so that a test can assert the overlap happened
   (at least 2 at D = 2 on a many-piece broadcast, exactly 1 at D = 1) without
   timing anything (the lesson of 2026-09-24: pin by counters).

## Not in this slice

The allgather for fcollect; team creation; any change to the intranode phase,
to the barrier's tree (which carries no data), or to `internode.h`'s contract.

## Tests

Every existing test passes unchanged, in all four presets, at the default D.
New, on `0,0,1,1`, `0,1,2,0,1,0` and a five-node map, at D = 1, 2 and 4 through
the environment: a reduction, a broadcast from a non-zero root, and an fcollect,
each of many pieces (1 MiB and an odd size that leaves a short last piece),
checked element by element; back-to-back collectives with different roots; the
in-flight counter as in decision 6; and a rendezvous disagreement on
`OSOE_COLLECTIVE_PIPELINE`, ended by name.

## Verification

Debug and asan in the VM, foreground, under the lock; the main session runs
release, tsan, tier 3's collective tests (the `pi` link shows the gain),
mutations - the model's three breakages, plus a base not advanced for a
rotated tree - and the load loop.

## Stop and ask

If the loop needs an ordering the model does not have; if the region's growth
trips the capacity check on a map the suite runs; or if D must differ between
teams.
