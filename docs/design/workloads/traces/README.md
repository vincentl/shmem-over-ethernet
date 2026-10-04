# Virtual traces of reference workloads

A trace records **one iteration of a hot loop** in a public OpenSHMEM code: the
OpenSHMEM calls it makes, in order, with element sizes, target PEs, repeat counts,
and context use. Each trace backs one reference workload in the design notes
(§8.6) and cites the pinned source it was read from.

Traces are *virtual*: derived by reading source, not by running it. Nothing in
the corpus is built. The design notes (§8.6) give the reasons, and describe the
dynamic path through the profiling interface that can later check these traces
against real runs.

## Sources

The corpus is listed in [`tools/corpus/manifest.tsv`](../../../../tools/corpus/manifest.tsv)
and fetched by `tools/corpus/fetch.sh` into `corpus/` at the root of the main
checkout (shared by all worktrees, ignored by git), each entry at exactly its
pinned commit or tarball digest. The entries and pins come
from the usage study, `docs/reviews/evidence/q2/q2-usage.md` §0.

Corpus licenses vary, and none of them is this repository's. Traces therefore carry
citations, never code: a routine name, a size, a `file:line`. Short identifiers
in comments (a variable name, a macro) are the most a trace quotes.

## Files

| File | Workload | Read from |
|---|---|---|
| [`U1-gups.trace`](U1-gups.trace) | U1 scalar remote access | OSB GUPs (HPCC RandomAccess) |
| [`U2-prk-stencil.trace`](U2-prk-stencil.trace) | U2 halo exchange | PRK SHMEM Stencil |
| [`U3-graph500.trace`](U3-graph500.trace) | U3 remote atomic per element | OSB Graph500 BFS |
| [`U4-prk-p2p.trace`](U4-prk-p2p.trace) | U4 put-with-signal | PRK SHMEM Synch_p2p |
| [`U5-isx.trace`](U5-isx.trace) | U5 aggregated alltoallv | ISx SHMEM |
| [`U7-bale-convey.trace`](U7-bale-convey.trace) | U7 phase boundaries | bale convey, simple conveyor |
| [`U8-bale-convey-local.trace`](U8-bale-convey-local.trace) | U8 intranode direct access | bale convey, local porter |

There is no U6 trace. U6 is U1 and U3 on one context per thread and needs
`SHMEM_THREAD_MULTIPLE`, which osoe has provided since phase 1 step 8 (§4.11),
though nobody has written the trace; OSB `GUPs_threaded/SHMEMRandomAccess.c:254-272`
is the obvious source.

## Format

A trace is a UTF-8 text file with the suffix `.trace`. It is line-oriented:
fields are separated by blanks, `#` starts a comment that runs to the end of the
line, and blank lines are ignored. A comment on a step line is that step's note.

A file has a header, then a body.

### Header

Header lines come first, one per line, in this order:

| Line | Meaning |
|---|---|
| `trace <id>` | Identifier; the file name without `.trace` |
| `workload <U#>` | The §8.6 workload this trace backs |
| `source <name> <commit>` | Manifest entry name and its full pinned commit SHA (or tarball SHA-256). Every citation below is relative to `corpus/<name>/` at that pin |
| `loop <cite> <text>` | Where the loop is, and what one iteration is |
| `threads <n>` | OpenSHMEM-calling threads per PE in this loop |
| `build <text>` | Compile-time choices the trace assumes: the default build unless stated. Optional; repeatable |
| `param <NAME> <value>` | A symbol used in sizes or counts: its default, a range, or `data` when the input decides it. Optional; repeatable |

### Body

The body is a sequence of steps and blocks, run in order by **every PE** unless
a `role` block says otherwise.

| Line | Meaning |
|---|---|
| `call <routine> <elem> <count> <target> <ctx> <cite>` | One OpenSHMEM call |
| `direct <access> <elem> <count> <target> <ctx> <cite>` | A load or store through a pointer returned by `shmem_ptr`; `<access>` is `load` or `store`. Not a call, but it replaces one (U8) |
| `repeat <count> <text>` … `end` | The enclosed steps run `<count>` times, in order, each pass completing before the next |
| `when <text>` … `end` | The enclosed steps run only under the stated condition |
| `role <name> <text>` … `end` | The enclosed steps run only on the PEs described |

Blocks nest; `end` closes the innermost.

Fields:

- **`routine`**: spelled exactly as the source calls it, including legacy and
  deprecated names (`shmem_long_fadd`, `shmem_put64`, the pSync `*_to_all`
  reductions). A trace does not modernize names, so it stays greppable against
  the source. When the source calls a local wrapper, the trace lists the
  OpenSHMEM calls inside the wrapper and cites them there, naming the caller in
  a comment.
- **`elem`**: bytes per element, for an LP64 target. For the `*mem` routines,
  `1`. `-` when the routine carries no payload.
- **`count`**: elements per call: an integer, a param, or arithmetic on them
  (`RADIUS*W*8`, `P-1`). `-` when there is no payload.
- **`target`**: which PE the call addresses.

  | Value | Meaning |
  |---|---|
  | `self` | The calling PE: a wait on its own memory, or an operation addressed to itself |
  | `neighbour` | A PE fixed by a topology (grid or chain), named in the note |
  | `random` | A PE chosen by data: a hash, a random number, `index % npes` |
  | `each` | Inside a `repeat` over PEs, a different PE on each pass; the note says whether the calling PE is included |
  | `all` | A collective over the team the routine names (world for the pSync forms) |
  | `-` | The routine addresses no PE (`shmem_quiet`, `shmem_fence`) |

- **`ctx`**: the context the call runs on.

  | Value | Meaning |
  |---|---|
  | `default` | Spelled without a context argument, where the spec also has a `shmem_ctx_` form |
  | `thread` | An explicit context private to the calling thread |
  | `shared` | An explicit context used by several threads |
  | `-` | The routine has no context form in the spec (collectives, `wait_until`, `shmem_ptr`) |

- **`repeat` count**: an integer, a param, arithmetic on them, or a range. `a..b` is
  inclusive; `a..` has no upper bound (a retry loop). `0..` includes not running.
- **`cite`**: `path:line` or `path:first-last`, relative to the source checkout.
  Extra lines that the step also stands for go in its note. When a macro builds
  the call, so the cited line does not name the routine, the note says
  `spelled at :N` for the line in the same file that does.

### What a trace leaves out

- Computation and local memory access, except `direct` accesses through a
  `shmem_ptr` pointer.
- `shmem_my_pe` and `shmem_n_pes`: they are constant reads (q2-usage §1).
- Calls outside the iteration, such as the barriers around a timed region. A
  comment in the header can name them.
- Paths not taken by the default build. A `build` line or comment names the
  alternatives.

## Adding or changing a trace

1. Add the code base to `tools/corpus/manifest.tsv` with a full commit SHA, and
   fetch it.
2. Read the loop at that commit. Take the default build unless the workload needs
   otherwise, and record the choice on a `build` line.
3. Cite every step. When the source contradicts q2-usage or §8.6, the trace
   follows the source and says so in a comment; q2-usage itself is a read-only
   record.
4. Run `tools/corpus/check-traces.py`. It checks the format, the pins, that every
   cited line exists, and that each call names its routine on its cited line.
5. Moving a pin means re-reading every trace that cites that entry, updating its
   line numbers, and running the check again.
