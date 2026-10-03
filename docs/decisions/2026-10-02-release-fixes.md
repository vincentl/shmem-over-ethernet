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

## Part 2: a global exit's status is the caller's, and the caller leaves last (D1)

Branch `global-exit-status`, built in two stages: D1 as the author decided it, every PE
but the caller of `shmem_global_exit(status)` leaving with 0; then, when building it
showed `srun -K1` could still return 143, the caller leaving last - the author's answer
the same night (`2026-10-02.md` item 9).

1. **The status** (D1). The model first: `abort_exit_model.py`'s STATUS restated - a
   process whose program called ends with a status some caller gave, every other with
   0 - with two variants that must fail, `status_everywhere` (the code as it was) and
   `caller_zero`. The restatement hid one old variant, `finalize_returns` (a second
   `exit` beside the agent's), which STATUS had caught only because every process
   carried the status; a property ONE_END - a process is ended by the thread that
   claimed it - now catches it for its own reason. The code is the plan's two measured
   lines and the comments of `abort.c`, `abort.h`, `init.c` and `fatal.c` (a fatal that
   loses to a global exit leaves with 0 too). `osoe-launch`'s status became `srun`'s:
   the highest exit code of any rank, 128 plus the signal for a rank killed by one it
   did not send, 1 for its own findings when no rank reported more. Tests:
   `global-exit.sh` requires one nonzero line, the caller's, and the launcher's
   status; new `zero`, and without `-k`, as `srun -K1` runs, `kills.L4` and
   `kills.slow-handler`. Mutations: non-callers carrying the status fail 14 of 23
   global-exit tests, the caller leaving with 0 15, the launcher's old 1 15. Under the
   VM's Slurm (`srun -K1 --cpu-bind=none -N1 -n4`): handlers of 0 ms to 5 s on PE 0,
   300 ms on PE 3, status 0, and without `-K1` - `srun` returned the status every time,
   every handler finished and stdout was flushed.
2. **Found, and put to the author.** `srun -K1` kills every task still running at the
   first nonzero exit, and a killed task counts 143, which `srun`'s status then is. On
   two logical nodes a non-lead caller left before the other node's PEs had ended in 2
   and 7 runs of 30 (a lead caller in none of 20): its agent ended it at once, while
   its lead still had the goodbye to send.
3. **The first adversarial read** (a fresh agent, given the branch at `443b2f7` and the
   brief's property). The change's own lines held: every way a process ends on a
   global exit computes one status from the disposition and `called`, and `called`
   reaches the agent as §3.16 says. Eight findings: the race of item 2 on one node too
   (a non-caller whose flush waits on stdin's lock, one with megabytes buffered, 2 of
   30 runs of `kills.L4`'s own setup) - put to the author with the rest; the tests
   could not see a late end (a mutant ending every other PE 8 s late passed 8 of 10:
   every mode now bounds the end at 6 s, but the grace's own, and the mutant fails 13
   of 25); two callers on a node, the second killed inside its handlers (none of the
   values passed, against §9.1.6) - the same fix; the grace's status tested by
   nothing (two modes now hold stdout's lock for good, on every other PE and on the
   caller, and each mutant fails one); `osoe-launch` discarding a rank's own signal
   when it died before the launcher's SIGTERM (it keeps which signals it sent now);
   racing callers' statuses (stated); the model has no grace thread (informational);
   and a PE ended by its own thread dying of SIGPIPE (the ending thread holds SIGPIPE
   back now, as the agent holds every signal).
4. **The caller leaves last** (the author's answer). The model first:
   `abort_exit_model.py` gains `srun -K1` as a thread that kills every process still
   running after the first nonzero exit, and the properties NO_KILL and JOB_STATUS;
   the word carries the declarer's rank. Every built case passes with it and with
   liveness (to 9.6 million states, two callers on one node); two callers on two nodes
   at once fails NO_KILL, the residual stated; each rule's variant fails as named
   (`no_carrier_wait`, `no_lead_wait`, `no_remote_wait` and the code before,
   `called_carries`, NO_KILL; `wait_every_node` and `no_exclusion` STUCK;
   `caller_zero` JOB_STATUS). Three old cases turned: with the carrier outliving its
   lead the declarer's wakes always finish first, so `nonlead` passes without sweeps
   or pidfds (`no_carrier_wait` still shows Part B's finding), `no_rewake` passes (the
   rewake is a second line), and `pidfd_fatal` shows its violation on node B. The
   code: the carrier is the PE the word names; it waits for every other PE of its node
   by pidfds (`osoe_liveness_outlast`, no lock, no allocation) before its end; a lead
   of a job of several nodes waits for its node's others but the carrier before its
   `_exit`, its grace started first and waiting too; the carrier's node's lead waits
   after its goodbyes for every other node's end of stream, but a node whose goodbye
   named another caller (`peer_abort_origin`). Bounds 20 s and 30 s. A first build
   started a lead's grace after its wait and stacked graces (30 s in `held-stdout`).
   Tests: without `-k` no rank may be killed and every line must be written; new,
   non-lead callers on either node, two callers on one node, every other PE holding
   stdout.
5. **Measured after it**, under the VM's Slurm: a caller on PE 0, 1 or 3, ten runs
   each, returned 7 thirty times of thirty; two callers with 7 and 8 and handlers of
   300 and 450 ms returned 7 or 8 ten of ten, the carrier's handler finishing every
   time; a PE blocked in `fgets` on a stdin that never ends returned 7 in 10 to 11 s,
   three of three (143 before); every other PE holding 16 MiB of stdout returned 7 five
   of five, every line written. Under `osoe-launch` without `-k` on `0,0,1,1`, a
   non-lead caller on node 0 and on node 1, thirty runs each: no rank killed, 7 every
   time.
6. **The second adversarial read** (a fresh agent, given the model and code commits
   `023b8b9` and `c42961d` and the property). Sound: the carrier and status rules, no
   cycle of waits on a node, every wait bounded, no lock or allocation on the end path,
   no new fatal while polling during the abort, fatals unchanged, and further runs
   under `KILLS=yes` clean. Five findings:
   - *Three nodes and a relayed goodbye* (high): a node told by a third sent the
     carrier's node a goodbye and ended its stream at once, which that node took for
     its PEs having ended; `srun -K1` then killed them, 8 runs in 40 with a PE there
     held. The model, of two nodes, relays no goodbye and ends a stream only at a
     process's end. On a global exit the engine now leaves the stream to the process's
     end (`keep_stream`); `kills.held-other-node.0-2-2-1` fails 4 runs in 20 with the
     shutdown put back, and passed 10 of 10 as built.
   - *Two of the three waits could be deleted with every test passing* (medium): the
     held-stdout test holds every PE, leads too, whose graces lined the ends up.
     `kills.held-other-node.0-0-1-1` holds one non-lead of the other node and fails
     with either lead's wait gone.
   - *The bounds* (low-medium): a lead of the carrier's node starts its grace after its
     goodbyes and its wait for the other nodes, five graces at the outside, not four as
     written; `abort.h`, §3.15 and the README say the bounds as they are, and that
     processes held in several places at once can run one out.
   - *A lead that called and lost had its handlers cut* when its wait outran its grace
     (low): it waits before its grace starts now, as the carrier does.
   - *`osoe_liveness_outlast` opened each pidfd only on reaching its PE* (low): a PE
     reaped meanwhile could be looked for by a recycled pid; it opens a batch of 64
     before waiting on any.
7. **Under tsan** the two grace tests hung: tsan's `_exit` takes stdout's lock. A program
   whose main thread holds the lock and whose second thread calls `_exit` after a second
   ends in 1.0 s built plainly and not at all with `-fsanitize=thread`; the held-stdout
   tests are left out of the tsan build.

**Verification**, on the tree that merges (`80fbdc7`), in `osoe-load`, pins on: debug 627
of 627, release 627 of 627 (the models left to their preset), asan 626 of 626, tsan 411 of
411; tier 3's global-exit tests twice, 18 of 18. The model whole on the host, every case as
expected (9 min 30 s).

## Still with the author

- **The release checklist** (`docs/release-checklist.md`, with the repository scrub) is
  a draft until the author has read it.
