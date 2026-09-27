# Task: the stress tester, phase 3 - the rest of the surface

**Status: written on 2026-09-27, at the end of phase 2, from
`docs/design/osoe-pacer-proposal.md` §4.5, §4.6, §4.8 and §5, and what
phases 1 and 2 built and found (`docs/decisions/2026-09-27.md`).** Phases 1
and 2 left `osoe-pacer` running six exercises - `sort`, `alltoall`,
`mailbox`, `teams`, and `threads` and `print` when named - each with an
oracle and a plant for every check. Phase 3 covers what none of them calls:
every atomic type and operation, the RMA routines beyond bulk puts and
gets, the locks, the memory routines, and the collectives at every size.
Then it makes "fully exercised" a number. An interposition shim counts every
routine the exercises call, against every routine the header declares.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1).
- **For the tool itself: nothing that grows faster than O(N) a PE**, N the
  job's size, and an oracle - with a plant that fires it - for everything it
  measures.

## Read first

1. `CLAUDE.md`, the naming rule especially, and its rule on delegating
   mechanical work: the type lists, the shim's generator and the ctest lines
   are the proposal's own examples of it.
2. The proposal: §3, §4.5, §4.6, §4.8, §5's phase 3, §6 (coverage by
   family).
3. `docs/decisions/2026-09-27.md`, both phases. Phase 1: the interface and
   where it differs from the brief, the plants and how their tests assert
   every line, the oracle review. Phase 2: plants that a collective can
   absorb, and how each kind was forced (item 11); the oracle review's
   "accepts, still" column, which this phase closes where it can; the
   layout's partners (item 13).
4. `tools/pacer/exercise.h`, `osoe-pacer.c`, and `mailbox.c` whole - the
   newest exercise - before writing another.
5. `shmem/include/shmem.h` and `pshmem.h`: the typed routines are macro
   generated, and the C11 generics dispatch to them. The shim is generated
   from `pshmem.h`; the `pshmem_` strong / `shmem_` weak pairing
   (`shmem/src/export.h`, checked by `cmake/CheckExports.cmake`) is what lets
   it interpose.
6. The spec, `docs/reference/OpenSHMEM-1.6.pdf`: §9.3 (memory management,
   `malloc_with_hints`, `realloc`, `align`, `addr_accessible`), §9.6 (RMA,
   the strided, block-strided and sized forms), §9.7 (AMOs, Tables 6 to 8),
   §9.8 (`signal_add`, `signal_set`), §9.10 (every collective), §9.12
   (`fence`, `quiet`, `pe_quiet`), §9.13 (locks), §10 (the profiling
   interface).
7. Prior art: bale's `histo_src`, `randperm_src` and `ig_src`
   (`corpus/bale/src/bale_classic/apps`) for the AMO oracles;
   `shmem/tests/test-differential.c` for order-independent oracles; PRK's
   `Stencil` and `Transpose` (`corpus/prk/SHMEM`) for the halo and the
   strided shapes; SOS's unit tests (`corpus/tests-sos/test/unit`) for each
   AMO's typed and C11 forms; `osoe-bench`'s collectives, for what
   `collectives` must not repeat.

## What to build

1. **`atomics`** (proposal §4.5). A table spread over the PEs, and oracles
   that do not depend on order:
   - adds and increments, whose totals are known;
   - xor, or and and, applied in pairs over two passes, so that the table
     returns to its start;
   - fetching adds of one on shared words, whose fetched values are then a
     permutation of 0 to the total, so their sum has a closed form whatever
     the order. That checks what `threads` discards (phase 2's oracle review:
     "a fetch-add's returned value");
   - `compare_swap` as bale's `randperm` uses it: every PE claims random empty
     slots until it holds K, after which every slot is claimed once and the
     total is N × K;
   - `swap` passing tokens whose sum is conserved;
   - `atomic_fetch` and `atomic_set` on float and double;
   - the nonblocking fetching forms in batches closed by a quiet.

   Every type of Tables 6 to 8 goes through the C11 generics, driven by one
   macro list, so that every typed routine is in the binary and called. Then
   the same on a private context and on a serialized one. It reports updates
   a second, remote and local.
2. **`rma`** (§4.6). The halo on a ring: a bulk put of round k's pattern to
   the right neighbour, `shmem_fence`, then an update of the neighbour's flag;
   the neighbour waits until the flag is at least k and checks the whole
   buffer, so a put overtaking its fence is caught on either path, on-node
   or across. The flag's update alternates `shmem_atomic_add`,
   `shmem_signal_add` and `shmem_signal_set` - the standalone signal routines
   nothing calls yet - with their `ctx_` forms on a context. Sizes from 8 B to
   1 MiB. Then:
   - `iput` and `iget` with strides, `ibput` and `ibget` with block sizes and
     strides;
   - the sized `put32`, `put64`, `put128` and their gets, and `p` and `g`;
   - `get_nbi` with many in flight, then one quiet; `pe_quiet` per target;
     `put_nbi` bursts;
   - `shmem_ptr` loads and stores toward node-mates, with a fence.

   Every transfer's content is (source, target, offset, round)-determined and
   read back.
3. **`locks`** (§4.8): L distributed locks. Each PE loops choosing one,
   taking it by `set_lock` or a `test_lock` spin, then does a get, an
   increment and a put of the protected count at the lock's home -
   deliberately not atomic, since the lock is what protects it - then
   `clear_lock`. Oracle: each count equals its acquisitions, summed over the
   PEs at a checkpoint. It reports acquisitions a second and the longest
   wait.
4. **`memory`** (§4.8): a seeded sequence of `malloc`, `calloc`, `align`,
   `realloc`, `free` and `malloc_with_hints` with both hints, the same on
   every PE since they are collective. Each block is filled and checked after
   every realloc, checked with `addr_accessible`, and its address checked
   alike on every PE. The sequence goes up to the budget and back down, many
   times. The address check must stay O(N) a PE, or better: a hash of the
   sequence's addresses, compared by a reduction, rather than an fcollect of
   every block.
5. **`collectives`** (§4.8): every collective at sizes from 8 B to 1 MiB, on
   the world and on a strided team, median and fastest, each result
   verified - the sweep `osoe-bench` does not do, and the benchmark half of
   `teams`. Its plants use phase 2's lesson: a change each kind's result
   cannot absorb.
6. **The shim** (§5's phase 3). A script generates it from `pshmem.h`: a
   strong `shmem_X` for every routine, counting its calls and forwarding to
   `pshmem_X`, linked into a second target only (`osoe-pacer-counted`, or as
   the brief's reader judges), never into the tool by default. At finalize it
   reports how many of the declared routines were called, the count of each,
   and the list not called. The first report, from a default run of every
   exercise, is the author's to sort. The library exports some 4,857 `shmem_`
   entry points, most of them typed (`docs/design/successor-api-stage-setting.md`,
   L44): the generator is the work, and its output is never edited by hand.
7. **Plants**, one for every new check, asserted line by line through
   `cmake/PacerPlant.cmake`.

The new exercises join the default set: every exercise but `threads` and
`print` when none is named.

Not in this phase: `mix`, the soak, the library's counters in the stream,
node-folded labels, the second route of `parallel_print`, the deprecated
forms, installation.

## Order of building, each part running before the next

1. `atomics`: the type list first, one oracle at a time, then the contexts;
   its plants.
2. `rma`, the halo first; its plants.
3. `locks`, then `memory`; theirs.
4. `collectives`; its plants.
5. The shim and its first report.
6. The tests, the runbook's §6, the measurements, the notes.

## Tests

As phases 1 and 2, in the same block of `tools/pacer/CMakeLists.txt`, whose
place in every preset is still the author's to decide:

- each new exercise `--quick` over the four maps, which the smoke tests do
  once the exercises join the default set;
- a plant test for each check;
- a test that the counted target builds, runs `--quick`, and prints its
  report.

The quick runs must stay about a minute a preset in release, all exercises
together.

## The measurement

At 4 × 4 on the Pi, in release: ten minutes each of `atomics`, `rma`, `locks`,
`memory` and `collectives`, and the shim's report from a default run of every
exercise, with each node's retransmission timeouts before and after
(`build/scratch/pacer/pi-runs-phase2.sh` is the shape). On the VM, the new
exercises on the four maps in debug and asan.

## Stop and ask

- The tool finds a failure in the library. Record a reproduction; whether to
  fix it on the branch is the author's call.
- A check no plant can fire, or a plant caught by a check it was not meant for.
- Anything in the tool that grows faster than O(N) a PE.
- A type of Tables 6 to 8, or any routine the specification declares, that the
  library's header lacks: that is a conformance finding, not something to
  shape the list around.
- The shim's list of routines not called. Which are worth an exercise and
  which are accepted - the cache routines are no-ops by design - is the
  author's to decide.
- The quick runs cannot be kept to about a minute a preset in release.
