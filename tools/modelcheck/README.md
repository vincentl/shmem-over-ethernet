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
| `landing_extent_model.py`, `copy_out_claim_model.py` | §4.11's two open points the author decided on 2026-09-25: landing-zone extents claimed by CAS over a bitmap, and one copier per landed nonblocking get |
| `selftest.py` | Checks that the reductions change no verdict; rerun it after changing the engine |

**Where they run.** Every model is a ctest in the release preset only: a model
is Python and gives the same verdict whichever preset built the library. The
five of §4.11's `SHMEM_THREAD_MULTIPLE` designs (`get_completion` but for its
phase-1 case, `dirty_set`, `watch_list`, `landing_extent`, `copy_out_claim`) run
only with `OSOE_MULTIPLE_MODELS=ON`,
since phase 1 ships up to `SERIALIZED` and they were three quarters of every
release run; turn it on when §4.11 is scheduled or `engine.py` changes.

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
