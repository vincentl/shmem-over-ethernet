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
| `allgather_model.py` | Bruck's allgather among node leads for fcollect's internode phase, the same decision (§5.8) |
| `scan_model.py` | The prefix scans' tree over a team's ranks, with per-PE slots shared by every team and READY per team number (§5.8, phase 1 step 10); `scan_timing.py` times it against §5.8's runs |
| `scan_pipeline_model.py` | A large scan's pieces, D in flight (§5.8), and since the team redesign's step E its large pieces through the members' lanes, granted as each enters, its small ones through each team number's own slots; `scan_grant_timing.py` times the grant's cost, once a scan: a hop when the members enter together, up to a round trip when a member with children enters after them |
| `landing_extent_model.py`, `copy_out_claim_model.py` | §4.11's two open points the author decided on 2026-09-25: landing-zone extents claimed by CAS over a bitmap, and one copier per landed nonblocking get |
| `pe_quiet_model.py` | §4.11 item 2's rule that a full quiet never piggybacks on a `pe_quiet`, which §10 had left unmodeled |
| `team_block_model.py` | A team's block of a node's pool (the team redesign's step D, D3-D5): claimed, held by its members, its name's map entry cleared by its lead as it leaves, given back by the last holder - record, generation, then the bitmap - and the engine relearning a block whose generation moved |
| `split_numbers_model.py` | Two splits at once on one PE under `SHMEM_THREAD_MULTIPLE` (step D, D7, as redesigned after the adversarial read of 2026-10-03): nothing held across a reduction; each member takes its agreed numbers pending, waiting only for a higher parent number, and a second reduction keeps them or runs the round again. Its `token` case is the first design, the mask token, deadlocking the read's program |
| `pool_dry_model.py` | A split whose node's pool is empty (step D): busy in its first round and while a block is on its way back - a team leaving it, or its last holder's give-back not yet in the bitmap, told by the members count a claim zeroes - and dry only otherwise |
| `posted_bitmap_model.py` | Which team blocks have an arrival posted (step D, D5): a bit a block behind a summary word, set after the handoff and before the wake, and taken by the agent by exchange, so that a pass with nothing posted is one load |
| `lane_model.py` | The pipeline's large slots a lead thread's own *lane*, lent to one collective at a time (the team redesign's step E, E4, E5, E9): a parent grants each child its lane's index as it enters; a child puts only into a lane its grant names, and names its own before its first arrival, which the parent reads as it takes that arrival - not at its first release, as the proposal had it, which a tree turned to another root in the team's next collective breaks (`release_lane_late`); a broadcast needs no grant; small collectives through each team number's own slots. Two threads of a node leading two teams at once, and a team's collectives moving between threads; `grant_timing.py` times the grant's cost, once a collective: up to a hop when the leads enter together, less a child's intranode time, and up to a round trip when a parent enters after its children, less its own |
| `lane_scan_model.py` | The scans' lanes beside the reductions' (step E's second part, round six's review): every member of a scan in a tree over the team's ranks, the scans' own counters, grant and lane words per team number, on the same lane per thread as the reductions; the lane a grant names, the lane word read with a child's first total, two threads of a node scanning two teams at once. Its breakages: the scans counting on the reductions' words (`scan_shared_words`), no grant, a lane word read early, and every scan of a node through one lane (`scan_one_lane`, which the suite could not see); a lane word read late holds, since a scan's tree never turns |
| `selftest.py` | Checks that the reductions change no verdict; rerun it after changing the engine |

**Where they run.** Every model is a ctest in the release preset only: a model
is Python and gives the same verdict whichever preset built the library. They
carry the label `model` beside `tier1`, and take about half an hour (29
minutes in the development VM, 2026-10-03), so the
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
   must hold.
3. **Abstract what is already proven.** Use `nd` for a nondeterministic
   environment (a network, a progress thread) and `await` for a wait whose wake
   protocol another model covers.
4. **Break the design on purpose.** Every rule the design states should have a
   variant that drops it and fails with a trace. A model in which nothing can fail
   proves nothing.

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
