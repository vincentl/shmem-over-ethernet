# 2026-10-02 on: the release fixes

The fixes the round-three review named, built on `docs/prompts/task-release-fixes.md`
(the brief), which carries the plan `docs/prompts/phase1-release-fixes-plan.md` (the
plan) and the author's decisions of 2026-10-02 (`2026-10-02.md`). This record grows a
section a part as each merges; the team redesign has records of its own.

## The decisions it builds on

| | Decision | Taken |
|---|---|---|
| D1 | `shmem_global_exit`: only the caller carries the status | the author, 2026-10-02 (Part 2) |
| D2 | start-up: every PE validates every setting; the segment before the refusals; the 30 s notice; the tests with `-k` | the author, 2026-10-02 (Part 3) |
| D3 | team numbers: held per PE, then counters per team | the author, 2026-10-02 (the team briefs) |
| D4 | `shmem.floor`'s pins checked only when asked | the plan's recommendation, not struck (Part 1) |
| D5 | `OSOE_AGENT_SLICE_US` stays 100, both sides documented | the plan's (Part 4) |
| D6 | `OSOE_MAX_THREADS` stays 16, compared PE by PE | the plan's (Parts 1 and 3) |
| D7 | the models under a label of their own | the plan's (Part 1) |
| D8 | the version 0.1.0 | the plan's (Part 1) |
| D9 | the evidence's paths scrubbed to placeholders; SPDX lines on every source file | the plan's (Part 5) |

## Part 1: the small code fixes

Built on branch `release-small-fixes`, from five branches, four of them by Sonnet
subagents and one, the record's words, in this session; each subagent's report was
checked against its diff before its commits were taken.

1. **The C11 forms of `shmem_signal_add` and `shmem_signal_set`** (C1). Macros that
   choose by argument count, in `shmem_sync`'s and `shmem_atomic_add`'s style. C++ has
   no context-first form: the specification gives it none, and the overloads first
   built here broke a program that takes the routine's address (the adversarial read,
   below). Tests: both four-argument forms in `test-signal.c`, both families in
   `shmem.generic-arity.*`, the headers' C and C++ tests. Mutations: the old header (the
   four-argument calls stop compiling; the arity and header tests fail), the add routed
   to the set (the signal tests fail, a deadlock proved), the complaint emptied (the
   arity test fails). `shmem.exports` unchanged: 3,595 symbols before and after.
2. **The "blocked in" record's routine name as four atomic words** (K1). `blocked.c`
   holds the protocol, `base/atomic-name.h` the packing the writer and both readers
   share; the bytes and the layout number are unchanged. The brief counted three
   readers; there are two - the PE's own agent reads an atomic pointer, never the
   array. `shmem.blocked-record` runs the library's writer against both readers on two
   CPUs. Its mutation is the readers' loads made plain, which fails ten runs of ten in
   `osoe-load` (9,970 to 754,148 torn copies in 2 s); the brief's mutation, the
   writer's stores made plain, tears nothing on these cores (LSE read-modify-writes),
   nor does `osoe-stat`'s reader, which looks its fields up between loads. The test's
   first version could not fail at all: its loader copied each word out by a call,
   about a hundred instructions between the name's loads and the generation's; the
   loader now copies whole words by a fixed size.
3. **The silent-peer check's gate tested** (K5). `shmem.stopped-bulk.0-0-1`: 64 MiB
   toward a node stopped for 15 s. With the gate it completes after the node is
   continued (15.1 to 15.4 s, idle and under eight busy loops); without it the job
   ends at 12.1 to 12.4 s (28 runs) - not at about 4 s, the brief's figure, which is
   the silence the message names; the persist timer's probes put the floor of the stop
   at 13 s, so 15 s leaves 2.8 s. The test also refuses to pass if the put returned
   before the node was continued.
4. **The buffer-shrink notice once a node** (P3), from the lead; `shmem.am-size.notice`
   counts three lines on three nodes of three PEs (nine with the mutation). The
   arithmetic of the ceiling is pinned in `test-am-size.c`: 1,025 nodes fit the default
   budget and 1,026 do not.
5. **`shmem.floor`'s pins only when asked** (R2, D4): `OSOE_TEST_FLOOR_PINS=1`, which
   `-DOSOE_TEST_PINS=ON` sets for the test - and, a choice the subagent made beyond the
   brief, for tier 3's three floor tests too, whose `sudo` strips the environment. The
   project's own runs configure with the option on. `env.echo-uring`'s pins are not
   behind the switch; measured on `osoe-load` below.
6. **Python 3 optional** (R5a): the counted build and the models are left out where it
   is not found; the whole suite passes without it (606 of 606 in that build). Also: a
   custom target now owns the counting shim's generation, which two targets compiled -
   a parallel-build race CMake documents, seen once by a subagent.
7. **The models under the label `model`** (D7): the `release` test preset leaves them
   out, the `models` preset runs them alone. The README's first command measured in
   `osoe-load`, idle: about 5 minutes 20 s (the build 1 min, the tests 4 min 16 s).
8. **The version 0.1.0** (D8) from `project(osoe VERSION ...)`: the start-up line, the
   first line of `SHMEM_INFO`'s text, every tool's `--version`, `libosoe.so.0.1.0`.
   A choice made without the author, by the subagent: `oshcc --version` stays the C
   compiler's, which configure scripts read to learn what `CC` is, and osoe's version is
   `oshcc --showme:version`. *Undo*: add `--version` to that case in
   `tools/wrapper/oshcc.in`, drop `wrapper.version-is-the-compilers`.
9. **`OSOE_MAX_THREADS`'s words** (C3): a thread takes its place at its first call after
   `shmem_init` that does more than report on the job, and gives it back only when it
   exits. Two tests beyond the brief, `shmem.thread-places.held` and `.freed`, tell the
   two readings apart, which the old exhaustion test could not.
10. **`osoe-stat --gc` and a failed start's names** (R5a): `.job` and `.conn` objects are
    removed once their creator is gone and nothing maps them; `shmem.leftovers.gc` stops
    a start mid-way and checks both are left alone while it lives and removed after.
    Every object a job makes in `/dev/shm` begins `osoe.<job>.`, so the README's epilog
    line `rm -f /dev/shm/osoe.${SLURM_JOB_ID}.*` covers them all.

**Verification**, on the tree that merges:

- The four presets in `osoe`, configured with `-DOSOE_TEST_PINS=ON`: debug 615 of 615,
  release 615 of 615 (the models left out by the preset), asan 614 of 614, tsan 404 of
  404. A first run failed `shmem.boundary` in all four: `blocked.c` was in none of
  `cmake/CheckBoundary.cmake`'s layer lists; listed in the runtime layer, the whole run
  was repeated.
- After the adversarial read's fixes (below), the four presets again, in `osoe-load`:
  the same counts. tsan's first run lost a link and `shmem.strict-c11` to "Too many
  open files in system" - the host's file table, which the three VMs' shared folders
  had filled (120,544 of 122,880); with their caches dropped it passed whole. The
  tests the fixes touch, 144 of them, passed in each preset first, and the `models`
  preset fails with no models (a build without Python) where it passed.
- The 24 models of the release preset's list, run on the host (Python 3.14), since the
  VMs were busy: every one passes (`abort` 804 s, `abort_exit` 752 s, `tree_barrier`
  474 s, the rest under 130 s each).
- In `osoe-load`, the stranger's machine: `env.echo-uring`'s pins held idle ten runs of
  ten, so they need no switch; under eight busy loops `shmem.blocked-record` and
  `shmem.stopped-bulk.0-0-1` passed five runs of five, and `shmem.floor` with its pins
  asked for failed the fastest-trip pin (3.03x and 3.25x against 3.0x) - as pins
  measured under load do; with the pins on and idle it passed three of three, and a
  stranger's build, the pins off, passed three of three under the same load.
- An adversarial read of the branch by a fresh agent, given the diff and each item's
  property: below, with what was done.

### The adversarial read

A fresh agent read the branch at `20b282c` against each item's property, given none of
this session's conclusions, and ran what it could in `osoe-load`. It found the
properties held - the macros' dispatch, the record's memory orders, the gate, the
notice, the pins, the build without Python, the label, the version, the thread places,
`--gc`'s ordinary use - and twelve things besides, most severe first:

1. **`osoe-stat --gc` removed whatever a crafted segment named** (medium; run as one
   user, root by reading). The branch's README advises `--gc` as root in an epilog, and
   it unlinked any name a control segment's heap and statics fields held, checking
   neither the name nor the owner. Fixed: a name is removed only if it is the
   segment's own, `<segment>.heap.<n>` or `<segment>.statics.<n>`, and its object is a
   regular one owned as the segment is. `stat.segment` builds a dead job whose names
   point elsewhere - an object of the test's, a look-alike of another segment's, a FIFO
   by the segment's own name - and requires each refused and the segment's own statics
   removed.
2. **`--gc` waited forever on a FIFO** by a segment's or a job table's name, which any
   user can make; a root epilog would stall. Fixed: every open is non-blocking, and an
   object that is not a regular one is refused. `stat.segment` plants both.
3. **C11 code that declares or defines `shmem_signal_add` or `shmem_signal_set` by
   name stops compiling** - a §10 profiling wrapper, an `extern` redeclaration. Inherent
   to the C11 form as a macro, as for `shmem_sync` before it: the README's "C only"
   bullet and §7.5 now say so, with the spelling that works, the name in parentheses.
4. **The C++ overloads broke code that takes the routine's address** (`&shmem_signal_add`,
   `std::thread`, `std::function`). The specification's C++ has no context-first form:
   removed, and `headers.cpp` takes both addresses.
5. **`shmem.blocked-record` guards one of the fix's three halves, in an optimized build
   only**: the readers' loads made plain fail it; the writer's stores, and `osoe-stat`'s
   word path, do not - which item 2 says. The README and the design notes said more;
   both now say exactly this.
6. **`osoe-stat` includes `base/`**, `atomic-name.h`, against §3.14's "nothing from
   `base/`". A choice made in this session: the header says how a name the library
   rewrites is loaded, as words, and nothing of where anything is, and one loader keeps
   the two readers from drifting apart. §3.14 and `segment.h` now say so. *Undo*: copy
   the loader into `tools/stat/segment.c`.
7. **The `models` preset passed with no models** (a build without Python): its
   `noTestsAction` is now `error`.
8. **The stress tester's first line said `version=1.6`**, the specification's, against
   the README's "everything that names a version says it": it reads `osoe-pacer 0.1.0
   library=osoe openshmem=1.6` now. The evidence of 2026-09-27 keeps the old line.
9. **`--gc` examined the first 64 names of a listing**, so 64 it could not remove hid
   the rest for good: it takes every name now.
10. **Two tests ran an unscoped `--gc`**, removing every dead osoe object of the user on
    the machine - another session's kept segment, a test running beside them. Both
    name their own objects now.
11. **`shmem.stopped-bulk`'s guard proves the put was held, not that the window shut**
    (theoretical): today it does shut, and the gate's mutation fails the test; 64 MiB is
    four times the smallest size the guard passes at, 16 MiB. Left as it is.
12. **Two races in `--gc`** (theoretical): a restart with the same ids between the
    creator check and the unlink, and PID namespaces that share `/dev/shm`. The control
    segment's rule has the same limits; left, and stated here.

And a note for the next release: `libosoe.so.0` is the soname of every 0.x version, so
whether 0.2 takes a new one is that release's to decide.

## Still with the author

- **A global exit's last PE** (put on 2026-10-02, Part 2; widened the same evening by
  Part 2's adversarial read): `srun -K1` kills a PE still ending when the caller has
  left, and then returns 143. First measured on two logical nodes for a caller that is
  not its node's first PE (2 and 7 runs of 30); the read found it on one node too - a
  PE whose flush waits on a lock, or has megabytes to write - and with two callers on a
  node, where Slurm kills the one that finishes second inside its handlers, so the
  status is none of the values passed (§9.1.6). Recommended: one PE carries the status
  and leaves last, waiting, within the grace, for its node's other PEs and then for the
  other nodes; the same-node half alone is about half a day, both about two days. The
  alternatives: a stated limit, or only a non-lead caller waiting for its lead. Part
  2's record gives the measurements.
- **The release checklist** (`docs/release-checklist.md`, with the repository scrub) is
  a draft until the author has read it.
