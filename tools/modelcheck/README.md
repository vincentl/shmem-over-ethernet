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
| `scan_model.py` | The prefix scans' tree over a team's ranks, with per-PE slots shared by every team and READY per team number (§5.8, phase 1 step 10); `scan_timing.py` times it against §5.8's runs |
| `scan_pipeline_model.py` | A large scan's pieces, D in flight (§5.8), and since the team redesign's step E its large pieces through the members' lanes, granted as each enters, its small ones through each team number's own slots; `scan_grant_timing.py` times the grant's cost, once a scan: a hop when the members enter together, up to a round trip when a member with children enters after them |
| `landing_extent_model.py`, `copy_out_claim_model.py` | §4.11's two open points the author decided on 2026-09-25: landing-zone extents claimed by CAS over a bitmap, and one copier per landed nonblocking get |
| `pe_quiet_model.py` | §4.11 item 2's rule that a full quiet never piggybacks on a `pe_quiet`, which §10 had left unmodeled |
| `team_block_model.py` | A team's block of a node's pool (the team redesign's step D, D3-D5): claimed, held by its members, its name's map entry cleared by its lead as it leaves, given back by the last holder - record, generation, then the bitmap - and the engine relearning a block whose generation moved |
| `split_numbers_model.py` | Two splits at once on one PE under `SHMEM_THREAD_MULTIPLE` (step D, D7, as redesigned after the adversarial read of 2026-10-03): nothing held across a reduction; each member takes its agreed numbers pending, waiting only for a higher parent number, and a second reduction keeps them or runs the round again. Its `token` case is the first design, the mask token, deadlocking the read's program |
| `pool_dry_model.py` | A split whose node's pool is empty (step D): busy in its first round and while a block is on its way back - a team leaving it, or its last holder's give-back not yet in the bitmap, told by the members count a claim zeroes - and dry only otherwise |
| `posted_bitmap_model.py` | Which team blocks have an arrival posted (step D, D5): a bit a block behind a summary word, set after the handoff and before the wake, and taken by the agent by exchange, so that a pass with nothing posted is one load |
| `lane_model.py` | The pipeline's large slots a lead thread's own *lane*, lent to one collective at a time (the team redesign's step E, E4, E5, E9): a parent grants each child its lane's index as it enters; a child puts only into a lane its grant names, and names its own before its first arrival, which the parent reads as it takes that arrival - not at its first release, as the proposal had it, which a tree turned to another root in the team's next collective breaks (`release_lane_late`); a broadcast needs no grant; small collectives through each team number's own slots, collect's leads' scan of the node totals among them on a pre-order tree of its own (P30), its bases counted by the reductions' tree failing (`scan_heap_bases`). Two threads of a node leading two teams at once, and a team's collectives moving between threads; `grant_timing.py` times the grant's cost, once a collective: up to a hop when the leads enter together, less a child's intranode time, and up to a round trip when a parent enters after its children, less its own |
| `lane_scan_model.py` | The scans' lanes beside the reductions' (step E's second part, round six's review): every member of a scan in a tree over the team's ranks, the scans' own counters, grant and lane words per team number, on the same lane per thread as the reductions; the lane a grant names, the lane word read with a child's first total, two threads of a node scanning two teams at once. Its breakages: the scans counting on the reductions' words (`scan_shared_words`), no grant, a lane word read early, and every scan of a node through one lane (`scan_one_lane`, which the suite could not see); a lane word read late holds, since a scan's tree never turns |
| `shape_timing.py` | Not an interleaving model: the cost of a large reduction's or broadcast's tree shape on a link (v0.2 step 2) - the pipelined loop simulated over a k-ary tree of leads, fitted to the Pi's table of 2026-09-26, and the closed form `collective-shape.c` computes, checked against it and against two ways for links to share; its grid of the chooser's answers is `shmem.shape.grid`'s |
| `circulant_model.py` | Träff's circulant reduce-scatter and allgather among a team's node leads (v0.2 step 3): the rounds' arithmetic for every node count from 2 to 70, held element by element and, through its digests, `collective-circulant.c`'s; and the protocol - each round through a row of the receiver's lane, credited for the pieces its sender's window waits on, the reduce-scatter's grants a lane a round, the allgather's a count, its lanes carried by the reduce-scatter's first pieces, every counter its own - on two to five leads and beside a small tree reduction, with the property a team number's zeroing leans on: once every lead has returned, nothing written into a lead's words moves (QUIET). Eight breakages, each failing. `rabenseifner_model.py`, the fold the step's brief first described, is kept as the comparison's evidence - up to 3.5 vectors on a fold lead's link - and was not built |
| `visit_model.py` | Not an interleaving model: one connection's visits searched exhaustively from `progress.c`'s step order - flush's steps, `busy()`, the visit rule - under the zero-copy send (v0.2 step 4), written by its fourth adversarial read: a quiet's acknowledgement request and the close's goodbye are never stranded when both buffers come back in one visit, with one buffer lent at a time and a connection that owes a seal kept busy. Its breakages: one lent without the busy terms (STUCK at a quiet) and every send by reference as first built (STUCK at both). `reservation_model.py`'s `zero_copy` cases hold the buffer's lending itself |
| `control_ack_model.py` | Not an interleaving model: a control wire's acknowledgements between two leads, searched exhaustively from `progress.c`'s steps (v0.2 step 4, Part 2) - every segment carrying ARRIVE or RELEASE acknowledged before its sender can look quiet, which `deadlock_model.py`'s `control_wire_acked` assumes; no answer that asks, nothing owed with its busy bit clear, no message after a goodbye, and quiet and the close always reachable. Written by Part 2's second adversarial read; ten breakages, each failing the check written for it, and `control_busy` without its request term holding, the term redundant |
| `carrier_model.py` | Not an interleaving model: two callers of `shmem_global_exit` on two nodes, each its node's carrier, their abortive goodbyes crossing (v0.2 step 4, Part 2) - with two wires a pair they arrive on different wires, and a carrier that judges each wire alone waits for the other to the bound (`two_wires`, failing); skipping both of a peer's wires once its abort naming another caller has arrived on either holds. Written by Part 2's second adversarial read; Part 3 added `control_only`, the rule with helpers - the lead judges each peer by its control wire alone, the wire it holds - which holds too |
| `abort_owners_model.py` | Not an interleaving model: a peer's abort across owners (v0.2 step 4, Part 3), two nodes each with an agent on the control wire and a helper on the data wire, written by Part 3's adversarial read. An ending node's agent waits for its helper's abortive goodbye before its process ends, and a reader excuses an end of stream while its node's abort is declared: with the wait the excuse is not needed for the ending node's own goodbyes (`wait_no_excuse` holds), and each is needed in a case of its own - without the wait a peer's helper takes the data wire's end before its agent has applied the control wire's goodbye (`no_wait`, failing), without the excuse a node already ending takes an abrupt peer's end for a death (`abrupt_p_ending_no_excuse`, failing). Its send side, by the second read: a reading owner also sends, and once a reset is excused a flush that offered a segment again on the failed wire raised the fatal the excuse spared - a reset under a stream holds, the flush as it was fails (`abrupt_p_ending_reset_stream_unchecked`) |
| `helper_request_model.py` | The lead's requests to a helper thread (v0.2 step 4, Part 3): a request word the lead's agent alone writes, an answer word the helper alone writes, and the helper's wake word, bumped after the request is stored and loaded by the helper before it - so a request is never slept through and needs no second look (§4.9's rule). A quiet request answered after a whole pass, naming the request it answers; a stop acknowledged at once, then done after the close, and only then the ring freed; a helper that never answers found by its turns rather than by a time - the lead reads the helper's seq_cst count of turns after its seq_cst store of the stop, nudges it once for each count it sees, and ends the job naming a helper that has turned twice since without answering. Breakages: the request loaded first, a wake without the bump, done before the close's last ring use, a deaf helper whose turns nobody counts, an answer taken for the wrong request, the count read before the stop is posted, the count a plain store under TSO |
| `deadlock_owners_model.py` | The deadlock detector with a helper thread (v0.2 step 4, Part 3): a node's engine two owners - the agent with the control plane (the tree's arrivals, the probes, the root's rounds), a helper with the data wires - neither writing the other's state; a sample's first half posts a quiet request the helper answers after a pass begun after it, its activity sums both owners' applied counts, and its second half waits for the answer. `deadlock_model.py`'s argument owner by owner, its seven scenarios holding with liveness (2.3 million states the largest); breakages: the helper's applied count left out of the activity, an answer without a pass after the request, the second half not waiting for the helper's answer |
| `mesh_timing.py` | Not a model, and registered as no test: the mesh's start-up timed (design §6.2, `wire.c`), today's order - each lead accepts its lower nodes, then calls the higher ones one at a time - against the slotted schedule of `docs/design/mesh-schedule-proposal.md`; run by hand, in about two minutes |
| `selftest.py` | Checks that the reductions change no verdict; rerun it after changing the engine |

**Where they run.** Every model is a ctest in the release preset only: a model
is Python and gives the same verdict whichever preset built the library. They
carry the label `model` beside `tier1`, and take about an hour (40 tests in
62 minutes in the development VM, 2026-10-11, the longest `model.reservation`
at 14; 39 since `model.wire` was retired with the wire it checked), so the
`release` test preset leaves that label out and the `models` preset runs it
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
