# modelcheck: exhaustive interleaving models

A small explicit-state model checker for osoe's lock-free and futex protocols,
in Python 3 with the standard library only. It grew out of the review's models
(`docs/reviews/evidence/`) and was first used for design §4.11. The copies in
`docs/design/evidence/multiple/` are the record of that design and stay as they
were run. Develop new models here.

| File | Role |
|---|---|
| `engine.py` | The explorer; its docstring is the reference for the instruction set |
| `get_completion_model.py`, `dirty_set_model.py`, `watch_list_model.py` | Worked models: each checks a design and deliberately broken variants |
| `pipeline_model.py` | Pieces of a data collective pipelined through the internode tree, the author's decision of 2026-09-25 (§5.8) |
| `allgather_model.py` | Bruck's allgather among node leads for fcollect's internode phase, the same decision (§5.8); collect's places from each node's total (P29); and P30's - each lead's base and the total from a scan of the nodes' totals among the leads, each step carrying a record of its byte ranges, its arithmetic held for every node count to 70 (`docs/design/collect-scan-proposal.md`, built by the second cloud session); every breakage names the property it must fail |
| `exchange_model.py` | collect's node totals by a direct exchange among the leads (P33): each lead puts its total into a slot of every other and adds to its counter, fenced after, one hop where P29's tree takes two levels each way; nothing waits before writing, so two sets of slots and a counter each by the exchange's parity, on two to four leads, with and without the gather after - a collect of nothing has none, and the gather alone would order the next exchange's writes after every read. Its breakages: one set of slots (`one_slot`, SLOT) and one counter (`one_counter`, VALUE), each failing without a gather and holding with one |
| `scan_model.py` | The prefix scans' tree over a team's ranks, with per-PE slots shared by every team and READY per team number (§5.8, phase 1 step 10); `scan_timing.py` times it against §5.8's runs |
| `scan_pipeline_model.py` | A large scan's pieces, D in flight (§5.8), and since the team redesign's step E its large pieces through the members' lanes, granted as each enters, its small ones through each team number's own slots; `scan_grant_timing.py` times the grant's cost, once a scan: a hop when the members enter together, up to a round trip when a member with children enters after them. Under x86-TSO too, its puts buffered (T29), all but `summed`, whose first failure there is RESULT where it names VALUE: each fails under both orders, and which the search meets first moves with the buffers' flush steps. |
| `landing_extent_model.py`, `copy_out_claim_model.py` | §4.11's two open points the author decided on 2026-09-25: landing-zone extents claimed by CAS over a bitmap, and one copier per landed nonblocking get |
| `pe_quiet_model.py` | §4.11 item 2's rule that a full quiet never piggybacks on a `pe_quiet`, which §10 had left unmodeled |
| `team_block_model.py` | A team's block of a node's pool (the team redesign's step D, D3-D5): claimed, held by its members, its name's map entry cleared by its lead as it leaves, given back by the last holder - record, generation, then the bitmap - and the engine relearning a block whose generation moved. And the hold of the team's local release (T16; the step D review's G1): the engine that closes the destroy's epoch queues the RELEASE for the child on a control queue, from which it leaves at any time, and applies the release that lets the members go only once none is queued; `no_hold` applies it at once, the members leave, the block is claimed again and the engine learns it with the RELEASE still queued (QUEUED, `known_team`'s fatal). Under x86-TSO too (T29): each store the library makes `release` buffered, a sync's arrival a locked add that drains them. |
| `split_numbers_model.py` | Two splits at once on one PE under `SHMEM_THREAD_MULTIPLE` (step D, D7, as redesigned after the adversarial read of 2026-10-03): nothing held across a reduction; each member takes its agreed numbers pending, waiting only for a higher parent number, and a second reduction keeps them or runs the round again. Its `token` case is the first design, the mask token, deadlocking the read's program. Under x86-TSO too (T29), state for state as under SC: every store of the design is `seq_cst` or locked. |
| `pool_dry_model.py` | A split whose node's pool is empty (step D): busy in its first round and while a block is on its way back - a team leaving it, or its last holder's give-back not yet in the bitmap, told by the members count a claim zeroes - and dry only otherwise. Under x86-TSO too (T29): the claim's and the namer's `release` stores buffered. |
| `posted_bitmap_model.py` | Which team blocks have an arrival posted (step D, D5): a bit a block behind a summary word, set after the handoff and before the wake, and taken by the agent by exchange, so that a pass with nothing posted is one load. Under x86-TSO too (T29): the handoff's `release` store buffered. |
| `lane_model.py` | The pipeline's large slots a lead thread's own *lane*, lent to one collective at a time (the team redesign's step E, E4, E5, E9): a parent grants each child its lane's index as it enters; a child puts only into a lane its grant names, and names its own before its first arrival, which the parent reads as it takes that arrival - not at its first release, as the proposal had it, which a tree turned to another root in the team's next collective breaks (`release_lane_late`); a broadcast needs no grant; small collectives through each team number's own slots, collect's leads' scan of the node totals among them on a pre-order tree of its own (P30), its bases counted by the reductions' tree failing (`scan_heap_bases`). Two threads of a node leading two teams at once, and a team's collectives moving between threads; `grant_timing.py` times the grant's cost, once a collective: up to a hop when the leads enter together, less a child's intranode time, and up to a round trip when a parent enters after its children, less its own. Under x86-TSO too (T29): the puts buffered, each fenced add draining them. |
| `lane_scan_model.py` | The scans' lanes beside the reductions' (step E's second part, round six's review): every member of a scan in a tree over the team's ranks, the scans' own counters, grant and lane words per team number, on the same lane per thread as the reductions; the lane a grant names, the lane word read with a child's first total, two threads of a node scanning two teams at once. Its breakages: the scans counting on the reductions' words (`scan_shared_words`), no grant, a lane word read early, and every scan of a node through one lane (`scan_one_lane`, which the suite could not see); a lane word read late holds, since a scan's tree never turns |
| `shape_timing.py` | Not an interleaving model: the cost of a large reduction's or broadcast's tree shape on a link (v0.2 step 2) - the pipelined loop simulated over a k-ary tree of leads, fitted to the Pi's table of 2026-09-26, and the closed form `collective-shape.c` computes, checked against it and against two ways for links to share; its grid of the chooser's answers is `shmem.shape.grid`'s. And collect's places' four ways (P31): each one's cost in the same closed form, the default's choice among them - the exchange on few nodes and never past 16 (P35, from H24), each node's total through the tree on more, the scan where that vector is a large reduction - pinned (PLACES), its grid `shmem.shape.places-grid`'s, and `shape_timing.py places` printing the defaults by node count; provisional until H25 measures them |
| `circulant_model.py` | Träff's circulant reduce-scatter and allgather among a team's node leads (v0.2 step 3): the rounds' arithmetic for every node count from 2 to 70, held element by element and, through its digests, `collective-circulant.c`'s; and the protocol - each round through a row of the receiver's lane, credited for the pieces its sender's window waits on, the reduce-scatter's grants a lane a round, the allgather's a count, its lanes carried by the reduce-scatter's first pieces, every counter its own - on two to five leads and beside a small tree reduction, with the property a team number's zeroing leans on: once every lead has returned, nothing written into a lead's words moves (QUIET). Eight breakages, each failing. `rabenseifner_model.py`, the fold the step's brief first described, is kept as the comparison's evidence - up to 3.5 vectors on a fold lead's link - and was not built |
| `visit_model.py` | Not an interleaving model: one connection's visits searched exhaustively from `progress.c`'s step order - flush's steps, `busy()`, the visit rule - under the zero-copy send (v0.2 step 4), written by its fourth adversarial read: a quiet's acknowledgement request and the close's goodbye are never stranded when both buffers come back in one visit, with one buffer lent at a time and a connection that owes a seal kept busy. Its breakages: one lent without the busy terms (STUCK at a quiet) and every send by reference as first built (STUCK at both). `reservation_model.py`'s `zero_copy` cases hold the buffer's lending itself |
| `control_ack_model.py` | Not an interleaving model: a control wire's acknowledgements between two leads, searched exhaustively from `progress.c`'s steps (v0.2 step 4, Part 2) - every segment carrying ARRIVE or RELEASE acknowledged before its sender can look quiet, which `deadlock_model.py`'s `control_wire_acked` assumes; no answer that asks, nothing owed with its busy bit clear, no message after a goodbye, and quiet and the close always reachable. Written by Part 2's second adversarial read; ten breakages, each failing the check written for it, and `control_busy` without its request term holding, the term redundant |
| `carrier_model.py` | Not an interleaving model: two callers of `shmem_global_exit` on two nodes, each its node's carrier, their abortive goodbyes crossing (v0.2 step 4, Part 2) - with two wires a pair they arrive on different wires, and a carrier that judges each wire alone waits for the other to the bound (`two_wires`, failing); skipping both of a peer's wires once its abort naming another caller has arrived on either holds. Written by Part 2's second adversarial read; Part 3 added `control_only`, the rule with helpers - the lead judges each peer by its control wire alone, the wire it holds - which holds too |
| `abort_owners_model.py` | Not an interleaving model: a peer's abort across owners (v0.2 step 4, Part 3), two nodes each with an agent on the control wire and a helper on the data wire, written by Part 3's adversarial read. An ending node's agent waits for its helper's abortive goodbye before its process ends, and a reader excuses an end of stream while its node's abort is declared: with the wait the excuse is not needed for the ending node's own goodbyes (`wait_no_excuse` holds), and each is needed in a case of its own - without the wait a peer's helper takes the data wire's end before its agent has applied the control wire's goodbye (`no_wait`, failing), without the excuse a node already ending takes an abrupt peer's end for a death (`abrupt_p_ending_no_excuse`, failing). Its send side, by the second read: a reading owner also sends, and once a reset is excused a flush that offered a segment again on the failed wire raised the fatal the excuse spared - a reset under a stream holds, the flush as it was fails (`abrupt_p_ending_reset_stream_unchecked`) |
| `helper_request_model.py` | The lead's requests to a helper thread (v0.2 step 4, Part 3): a request word the lead's agent alone writes, an answer word the helper alone writes, and the helper's wake word, bumped after the request is stored and loaded by the helper before it - so a request is never slept through and needs no second look (§4.9's rule). A quiet request answered after a whole pass, naming the request it answers; a stop acknowledged at once, then done after the close, and only then the ring freed; a helper that never answers found by its turns rather than by a time - the lead reads the helper's seq_cst count of turns after its seq_cst store of the stop, nudges it once for each count it sees, and ends the job naming a helper that has turned twice since without answering. Breakages: the request loaded first, a wake without the bump, done before the close's last ring use, a deaf helper whose turns nobody counts, an answer taken for the wrong request, the count read before the stop is posted, the count a plain store under TSO |
| `deadlock_owners_model.py` | The deadlock detector with a helper thread (v0.2 step 4, Part 3): a node's engine two owners - the agent with the control plane (the tree's arrivals, the probes, the root's rounds), a helper with the data wires - neither writing the other's state; a sample's first half posts a quiet request the helper answers after a pass begun after it, its activity sums both owners' applied counts, and its second half waits for the answer. `deadlock_model.py`'s argument owner by owner, its seven scenarios holding with liveness (2.3 million states the largest); breakages: the helper's applied count left out of the activity, an answer without a pass after the request, the second half not waiting for the helper's answer |
| `mesh_model.py` | The mesh's start-up schedule (H23, the author's decision A8; `docs/design/mesh-schedule-proposal.md`): each lead calls the leads at the offsets 1 to (N - 1) / 2 after it, round the circle, and an even N's opposite pairs from the lower lead, one offset a slot in an order shuffled from the job's ids; one loop answers the listener and drives at most W calls of its own, a call no earlier than its slot and never skipped. Every lead's connections searched at two to five leads, in every order of the offsets, a lead reaching the mesh at any moment: no refusal (RULE), no wire twice (ONCE), no lead waiting for ever (STUCK), every pair's wires at both ends (MESH); the order before H23 - accept every lower node, then call each higher one in turn - holds too. Six breakages, each failing: calls made blocking (STUCK, the offset-1 calls a cycle), the hello's old check (RULE), both or neither of an opposite pair calling (ONCE, MESH), a listener closed while callers are to come (STUCK), and a call dropped once its slot has passed (STUCK). `digest` writes the slots' orders into `shmem/tests/mesh-schedule-digest.txt` (`model.mesh.digest`), which `shmem.mesh-schedule` holds `mesh-schedule.c` to; the loop is `wire.c`'s `osoe_wires_open` |
| `mesh_timing.py` | Not an interleaving model: the mesh's start-up timed over tier 3's Pi-like link, 100 µs each way, every lead a serial server - the schedule against the order before H23, every lead entering at once and as the rendezvous's tables reach it. `check` (registered): each slot a permutation (each lead its own order fails), every caller within the listen backlog to 4,097 nodes, the schedule four times the old order at 64 and 256 nodes (the old order through the same check fails it), and at most 20 SYNs a round trip at a listener as the tables arrive (the offsets in increasing order fail it, 31). `table` prints 64 to 4,097 nodes, by hand |
| `selftest.py` | Checks that the reductions change no verdict; rerun it after changing the engine |

**Where they run.** Every model is a ctest in the release preset only: a model
is Python and gives the same verdict whichever preset built the library. They
carry the label `model` beside `tier1` - all but `model.circulant`, which has
`model` alone - and take about an hour (40 tests in 62 minutes in the
development VM, 2026-10-11, the longest `model.reservation` at 14; 41 now,
`model.wire` retired with the wire it checked and `model.exchange` and
`model.shape_timing.places-grid` added on 2026-10-13; 44 since `model.mesh`,
`model.mesh.digest` and `model.mesh_timing` on 2026-10-19), so the `release` test
preset leaves that label out and the `models` preset runs it
alone (`ctest --preset models`). Without Python 3 none is registered. The
six of §4.11's `SHMEM_THREAD_MULTIPLE` designs (`get_completion` but for its
phase-1 case, `dirty_set`, `watch_list`, `landing_extent`, `copy_out_claim`,
`pe_quiet`) run only with `OSOE_MULTIPLE_MODELS=ON`,
since the first three alone were about 18 minutes of every release run when
the models ran in it, and phase 1 then stopped at `SERIALIZED`. The library
provides `MULTIPLE` now: turn it on when §4.11's code or `engine.py` changes,
and before a release; on 2026-10-03 the six took 22 minutes.

## Writing a model

1. **Threads are programs.** A program is a list of `(label, instruction)` pairs
   passed to `assemble`. Jumps name labels with a leading `@`. The instructions
   mirror what C11 atomics and futexes can do: `load`, `store` (plain or
   serializing), locked `add`, `cas`, `or`, `andn`, `fwait`, `fwake`.
2. **State the guarantee with ghosts.** Record what the specification talks about
   in ghost memory (keys starting with `$`), such as "this call returned" or "this
   byte landed". Check it with a `check` instruction at the point the guarantee
   must hold, whose message begins with the property's name and a colon -
   `ORDER: team 0: ...` - so that a failure says which property failed.
3. **Abstract what is already proven.** Use `nd` for a nondeterministic
   environment (a network, a progress thread) and `await` for a wait whose wake
   protocol another model covers.
4. **Break the design on purpose.** Every rule the design states should have a
   variant that drops it and fails with a trace. A model in which nothing can fail
   proves nothing. Each broken variant names the property it must fail - a check's
   name, or LOST WAKE, STUCK or TRAPPED - and `report(res, expect=...)` compares:
   a variant that fails some other way, its own check no longer reached, is as
   unexpected as a design that fails (`engine.py`'s "Properties"). The models
   that search with their own code compare the same way, with `engine.judge`.
5. **Run it under x86-TSO, with the library's orders.** A `store` is `serial=True`
   when the library makes it `seq_cst` (an `xchg` on x86) and `serial=False` when it
   makes it `release` or plain: under `explore(..., tso=True)` the second waits in the
   thread's buffer until a flush step, a locked instruction or a syscall. Mark each
   store as the C code makes it (§3.16), with a comment naming the file and function,
   and run every case under both orders, expecting the same verdict. Where the
   library has a locked instruction the model leaves abstract - a sync's or a
   reduction's arrival - put a locked `or` of nothing in the TSO run
   (`team_block_model.py`'s `fence`); and write bytes with a `store` and a `gpre`
   ghost atomic with it, not an `nd` step, which never buffers
   (`scan_pipeline_model.py`).

## Keeping it fast

- **Delete dead locals** (`set` to `DEL`). States that differ only in a stale
  local are otherwise distinct.
- **Fold ghost bookkeeping** into the adjacent real step with `gpre` (a call
  starting) and `gpost` (a call returning).
- **Declare interchangeable threads** with `symmetric=`.
- **Start small.** Size grows roughly geometrically with each operation or thread
  added. Two threads of each role, with one or two operations each, find every
  defect found so far. Check a larger configuration once the small ones pass.
- **Watch the progress lines.** Depth or a memory value that keeps climbing means
  an unbounded loop in the model. `max_seconds`, `max_states` and `max_depth` end a
  run with a verdict instead of a hang.
- **Budget memory.** Roughly 100 bytes per state, so 50 million states needs
  about 5 GB.
