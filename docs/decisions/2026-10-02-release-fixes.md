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

## Part 3: start-up says what it waits for, and a refusal tells the node (D2)

Branch `startup-loud`: the round-three review's R8, N1 and N2, on the author's middle
option. No wait gives up because time has passed (§3.15); a notice is not an end.

1. **Every start-up wait is said** after 30 s, and every minute after - one line a node:
   the lead names the local PEs not joined or mapped, with their ranks; local id 1,
   while the lead has made no segment, or a leftover of another launch stands in its
   place, names the lead; a lead calling another node names it, its host-list entry, the
   address and ports dialled and the dial's last error, which `dial` used to throw away;
   a lead waiting to be called names the nodes it waits for; a mesh connection that has
   stopped names the peer's address and port. `OSOE_TEST_NOTICE_SECONDS` shortens the
   30 s for tests.
2. **Every PE checks every setting that shapes the node or the job before its node
   joins** (3a.1). A Sonnet sweep found seven checked by the lead alone - not the brief's
   two - and the wire's by leads alone, after the rendezvous, never on one node. Every
   PE now reads and checks each, through `transport.h` for the backend's own;
   `OSOE_MAX_THREADS` and `OSOE_GET_LANDING_BYTES` are compared between a node's PEs, as
   the teams' and contexts' already were (the README marks them *checked*); a non-lead
   whose connection-area settings outgrew its lead's, which waited for ever, is refused
   by name. Six settings of the agent and the detector are checked after the join, and
   end the job through the abort word.
3. **A refusal before the segment tells the node.** The lead makes its segment before it
   checks `/dev/shm`; a PE that refuses before its node joins runs a last word - `failed`
   in the segment, or from a lead with none a *tombstone*, a header with `failed` set, or
   from any other PE `failed` in the lead's segment, waited for within the abort grace.
   The kernel checks tell nobody: every local PE refuses the kernel itself, before
   anything is made, as §1 promises - armed, the tombstone's wake was the futex2 call that
   had failed (`shmem.refuse.old-kernel`, found when the suite first ran whole).
4. **The listener keeps callers part-way through the hello in a set of 16**, and with
   `OSOE_JOB_SECRET` judges a hello's counts and node claim only after the caller has
   proved the secret. A full set stops accepting, and callers wait in the backlog, rather
   than answering NOT_YET as the brief proposed: callers retry either way.
5. **A failed start's leftovers.** `osoe-stat --gc` removes a control segment no PE
   joined once the process that made it (`node.creator_pid`, a new metric) is gone and
   nothing maps it, and a tombstone once nothing maps it; it lists and shows a tombstone
   as one.
6. **Fixed when the suite first ran whole after the rebase**: `init.c` reached below the
   backend boundary (`shmem.boundary`, and the strict C11 build through `liburing.h`).
7. **The tests** (a Sonnet subagent): a driver, `refusal.sh`, runs a job with `-k` and
   requires every rank to end by itself with a failure, or the given number left to the
   launcher's timeout with their notices said first; 16 refusal tests converted to it and
   27 new - every setting's refusal on every PE and on one, the `/dev/shm` refusal, a
   PE's own environment, a PE that never calls `shmem_init`, mislaunched counts with and
   without a secret - and a tier 3 test of a wrong address for a peer. Mutations, each
   seen to fail: the room check before the segment (only with the tombstone gone too),
   the tombstone skipped, each comparison removed, the connection-area size refusal
   removed, the notices' parts removed one by one, the claims judged before the proof,
   the set of one caller. Before and after on `main`: every case the brief listed left a
   PE to the timeout, or ran silently to exit 0, before; all end now. The tests found
   three waits more: one swept once a round of 32 dials, each most of a second of ARP's
   for an unroutable peer, so its notice came near 50 s (it sweeps between dials now);
   one told a lead that never came, after its no-progress limit, for 10 s more (it tells
   nobody now); and the connection area's two waits were silent (noticed now). Left as
   they are, and stated: a refusing node's neighbours in the tree end with it, but a node
   that has not reached it waits, saying so; a secret's mislaunched non-leaf waits naming
   a child that does not exist.
8. **The adversarial read** (a fresh agent, given the branch and Part 3's brief). Sound:
   the last word, the order of `create_control`, every setting validated by every PE, the
   proof before the claims, `--gc`'s new rules. Findings, and what was done:
   - *A failed start's leftover made the next start with the same ids fail* (medium), 4
     times in 10: a node-mate read its `failed` before the new lead reclaimed it - a
     requeued Slurm job without the epilog, a reused launcher pid, a hand-set job id. The
     header records the launch (`SLURM_RESTART_COUNT`, or `OSOE_LAUNCH`, which
     `osoe-launch` sets afresh; layout 28), and a node-mate waits past a ready or failed
     segment of another launch, saying so, as `tell_the_lead` does
     (`shmem.start-up.same-ids`, five starts of five).
   - *A lead that died with its segment made left its node-mates silent* (low): liveness
     starts after the join; they watch its pid during it now
     (`shmem.start-up.lead-ends-in-join`).
   - *README and design sentences that were not true*: other nodes do not all keep
     waiting for a refusing node; "every start-up wait says" is one line a node; a
     refusal reaches its node-mates within 10 s of its lead's segment; the settings
     checked after the join. Rewritten.
   - *Tests that passed with their property broken*: a non-lead's notice after the
     segment exists is forbidden now; an unproved hello claiming a node outside the job
     is sent; tier 3's wrong address must be named within 20 s, not 60; `--gc` removes a
     tombstone the library made. Each fails with its fix undone.
   - *A process on a lead's host answering on its port first can end a caller* that has
     a secret, which takes it for a job with another secret: stated in the README's Trust
     paragraph, not changed - a real mismatch must still end naming itself.

**Verification**, on the tree that merges (`7458fb8`, rebased onto Part 2), in `osoe-load`,
pins on: debug 657 of 657, release 657 of 657, asan 656 of 656, tsan 441 of 441; tier 3's
start-up and global-exit tests twice, 22 of 22. Before the rebase, on `bcbc7e0`, the same
presets and tier 3's start-up tests passed whole, and the read's four new tests each failed
with its fix undone; one release run had failed only `shmem.floor`'s get pin, under the
host's load from two reviewers' builds (4.70 times the floor against 3.0), and passed three
of three idle.

## Part 4: the README and the records

The documentation-only items of the brief's Part 4, each part's own README sentences
having travelled with it. A peer drafted every item read-only against `main` at
`9bbac4f` - the passage it replaces, the text, what it rests on - with four readers
checking roughly 600 citations and numbers, the Pi numbers recomputed from the review's
raw files (`build/scratch/release-fixes/part4-drafts.md`); a Sonnet subagent applied the
drafts, a second finished them after the first was stopped, with seven corrections a
citation check found; this session read the README's diff whole and regrouped the
commits so that none leaves the README's lists broken, as two intermediate ones had.

- **Taken without the author**: the Pi session's raw files committed, 137 of 138 -
  `/home/pi` is the cluster's stock account, as in 13 files already tracked - with
  trailing spaces stripped and the nodes' hardware addresses removed from
  `01-node-facts.txt`; `ip link`'s output, those addresses alone, left out. The second
  review's "SHARP is InfiniBand's" left out, as nothing in the tree proves it. The
  review's C5 (`iput` strides of zero or less, mismatched allocation sizes) not added: no
  draft carried its evidence.
- **Changed from the drafts**: "the echo moved 10 to 14.5" to 10 to 12.7, the drafter's
  six idle runs - the 14.5 has no raw log; "at most 16 PEs a node" to 15, the widest
  test, with timing at 4 on the Pi and the barrier alone to 32 in the VM; the review's
  B1 "would have ended", a prediction from the counts and not a run.
- **The records**: `docs/pi-cluster-results.md`'s "every line within 5 %" and the
  slice's "no difference", both false of their own raw files, corrected; design §3.14,
  §3.15, §4.11, §5, §6.2, §4.1 and §3.4 where they disagreed with the code; the
  decisions index's `OSOE_AGENT_CPUS`.
- **Left**: design §5.7's split "borrowed from the heap", which step D rewrites; stale
  comments in `team.c`, `metrics.c` and `connection-area.c`.
- **The final reads** (the brief's), on `main` at `1eee438`: a stranger's read of the
  whole README by a fresh reader taking the part of an MPI and OpenMP programmer new to
  SHMEM, and a check of every claim against its source by six readers in ranges -
  about 730 claims, 60 flagged. Their fixes are Part 6's, below.

## Part 5: the repository

Branch `repository-scrub`, by a Sonnet subagent, rebased onto `main` after Part 4 and
its audits run again over the whole tree: no tracked file names the author's home
directory (218 places before, `<repo>` and `~` in their place; a control finds the
pattern where it is); every source file carries `SPDX-License-Identifier:
LGPL-3.0-or-later` but the specification's four examples, kept with its notice (160 files
before, 4 after); the captured Slurm fixtures name no development machine (`vm-0`,
`192.0.2.1`), and their README says the VM that `dev/lima` makes has no Slurm. The
release checklist moved into the tree, `docs/release-checklist.md`, a draft until the
author reads it. The debug suite on the rebased branch: 657 of 657.

## Part 6: the whole

`main` at `1eee438`, Parts 1 to 5 merged, verified as the brief asks; then what those
runs and Part 4's final reads found, fixed on `release-part6`.

- **The four presets in `osoe`**, configured with `-DOSOE_TEST_PINS=ON`, four tests at
  a time: debug 657 of 657, asan 656 of 656, tsan 441 of 441, and release 656 of 657 -
  `shmem.floor`'s three fastest-trip pins at 3.34, 3.23 and 3.07 against 3.0, while the
  suite under load ran in `osoe-load` on the same host. Alone in `osoe-load`, five runs
  passed every pin: the fastest trips 1.9 to 2.3 times the floor's, the stream
  0.20 to 0.21 of it (`docs/design/evidence/release-fixes/floor-idle.txt`). `tier3`: 104
  of 104. `models`: 24 of 24 in 29 minutes, one at a time (`model.abort` 592 s,
  `model.abort_exit` 532, `model.tree_barrier` 309).
- **The stranger's path, from a clean clone, in `osoe`** - not `osoe-load` as the brief
  has it, since `osoe` is the VM with Slurm: `cmake --workflow --preset release`, exit 0
  in 591 s, 657 of 657; configured without Python, CMake named the two things it leaves
  out and 656 of 656 passed; `cmake --install` put `oshcc`, `osoe-pacer` and
  `osoe-stat` in the prefix; `ring.c`, built by the installed `oshcc` and run by `srun
  -K1 --cpu-bind=none -N1 -n4`, printed its four lines and exited 0; the global-exit
  example under `srun -K1`, three runs, each exited 7 with its handler run once and its
  four lines.
- **The README's command, idle in `osoe-load`**: the build 61 s, one file at a time; the
  tests 442 s, one at a time, 657 of 657.
- **Under eight busy loops in `osoe-load`**: 656 of 657, `shmem.reply` past its 60 s -
  the review's K3. Its four threads waited for their tickets with `sched_yield`, and
  under the loops two runs in ten stalled past 120 s at 0.1% of a CPU: starved, not
  spinning. On Linux's EEVDF scheduler a yield pushes the yielder's deadline back a
  slice, and only a sleep resets it - 100 yields and then a sleep stalled as well. A
  waiter that sleeps at once ends under load, but let the test-then-decrement mutant
  pass 9 runs of 10: the queue then stands, and the free credit is seldom contended. The
  waiter now watches with plain loads 10,000 times and then sleeps on the credit's word
  as the library does: idle 0.01 s, the mutant caught 20 runs of 20; under the loops 20
  runs within 0.25 s. (What it misses under the loops, the adversarial read below
  measured.)
- **The final reads' fixes in the code.** `env.probe` makes `shmem_init`'s own io_uring
  check, through the same function (`base/ring-check.c`), so that the README's "every
  facility osoe relies on" is true: it had tried neither an `R_DISABLED` ring nor the
  `SEND` opcode. With io_uring denied it fails that check by name. And the agent
  thread's start had a 10 s bound, which would have ended a job whose machine was too
  loaded to run a new thread by then; it waits without one (§3.15).
- **The README, against the final reads**: what has run, and where (16 PEs across
  machines; the VM daily and the Pi cluster the target); GCC or Clang, the packages, a
  node that only runs jobs, and the distributions that fall short; the x86-64 sentence
  confined to the tree it was true of; the build's time and `-j`; the prefix on every
  node, and the linker script among what is installed; C++ through `OSOE_CC`; the lead
  defined, a loopback name, the network used, the path that `srun` needs; `-c 2`
  struck, which no run supports, and the binding's numbers in one place with what
  unbinding costs; a batch script with its secret; `osoe-stat` on each node; an index
  of what to do when a job fails; the design's claims made exact (the collectives'
  levels, fence, the boundary, a futex wake, a team's costs, the waiting row,
  `MULTIPLE`'s locks, the tree's bytes); the team limits marked temporary; the landing
  zone and the nonblocking fetching atomics; limits not listed before (65,535 PEs a
  node, descriptors, `-static`, heterogeneous jobs, MPI); RemoveIPC's remedies untried;
  numbers recomputed from raw files (the mailbox's 256,000, `shmem_ptr`'s 28 µs, the
  loopback stream from a committed run);
  the SOS sweep's counts with their tree and PE count; the Pi's offload setting, F1, and
  a line for a cluster that stalls the same way; the models' 29 minutes and the six
  `MULTIPLE` models opt-in; the keepalive budget; the grace a constant; "the rows
  above"; the test hooks' rows as the code has them (`OSOE_FORCE_REMOTE` leaves the
  traffic in shared memory; the quiet check and the perturbation act on contexts that
  threads share), and `OSOE_TEST_NOTICE_SECONDS`, `OSOE_LAUNCH` and `OSOE_CORPUS`
  listed; a job's ids free again once it has started; `OSOE_HEAP_BASE` in place of the
  candidates; the `tier3` preset named; the Pi's round trips in microseconds; io_uring's
  sysctl among the requirements; the two meanings of *checked*. Design §3.12 says a
  fetching atomic's reply takes no extent of the landing zone.
- **The adversarial reads** (a fresh agent, given the diff and the properties: the
  credit test still catches the two-step take and ends under load; the probe fails
  wherever `shmem_init` refuses for io_uring; no path leaves the agent's start waiting
  in silence; every changed sentence true). The code's three held - no lost wake in the
  waiter, the probe's check the library's own, no path on which the agent never
  starts while the process lives - and it found, all fixed but the one refuted, with
  what a second fresh reader found in the fixes to the waits and the ring check:
  - *Under the busy loops the waiter often misses the mutant*: caught in 58 runs of 90
    in release and 6 of 20 in debug, every time under asan and tsan; idle 50 of 50 in
    release, two of them by the timeout, and 20 of 20 in each other preset. The figure
    "7 of 10" above this record first gave was of a waiter watching 1,000 times, not
    the one committed. The test's comments now say what was measured: it guards the
    suite's idle run. Under tsan and the loops it took 5 to 32 s of its 60, the
    margin left as it is, since no run puts tsan under load.
  - *The agent's start was a start-up wait that neither swept nor said what it
    waited for*: it now sweeps for the abort word - the local PEs' liveness is the
    agent's from there - ends at `OSOE_NO_PROGRESS_TIMEOUT` where that is set, and on
    the node's lead says what it waits for after 30 s, one line a node. The non-leads'
    wait for the lead's agent already swept; it stays silent, as `job.c`'s matching
    wait is - a notice there, which this part first added, a second reader found
    doubling the lead's own and naming the wrong cause - and now ends at the timeout
    too, and keeps its sweep's deadline across a wake, which a signal every few
    milliseconds had held off.
  - *`shmem_init`'s io_uring check never enabled the ring it made `R_DISABLED`, nor
    entered it*, both of which the agent does on its first pass: under a filter
    refusing only one of them, `env.probe` passed - or crashed, reading a completion
    that never came - and a two-node job ended after a PE had left `shmem_init`. The
    check now enables the ring and makes the agent's own call, a wait for completions
    with a zero timeout; it reads the list of opcodes for an error of its own, naming
    it; and it names the `kernel.io_uring_disabled` sysctl only where the ring's
    creation is what failed, the one step the sysctl refuses. The probe reads no
    completion it did not get, and writes a line at a time. Contrived, since every
    kernel from 6.7 has these calls, and now tested: `deny-io-uring` refuses
    `io_uring_setup`, the enabling or the entering, and `env.probe.no-io-uring-*`,
    `shmem.refuse.no-io-uring-*` and `shmem.no-io-uring.one-node` check the probe's
    failure, the job's refusal by name before anything is made, and a one-node job
    that runs.
  - *The README*: the Pi's round trips taken from a run with the offload on - the runs
    with it off give 172 to 176 µs against 158 to 159, 9 to 11%; F1's stalls reach 52
    s, and the offload is not their settled cure; the slice's "1.3" and `node`'s "a
    fifth", which this pass had replaced with the means of runs whose setting was
    changed partway through, restored from the overnight record; the team row's costs
    are in members, not nodes; the fan-out's 1.5 times needs a pipeline 4 to 8 deep;
    alltoall and collect's lengths among the collectives that are not logarithmic; the
    SOS sweep's "other two on one map", which no record holds, struck; the batch script
    sets `OSOE_NO_PROGRESS_TIMEOUT`; a start's leftovers, which a later start by the
    same ids reclaims; and a dozen narrower words. The models' comments in their
    `CMakeLists.txt` and README, stale on why the six are opt-in, corrected.
  - *Refuted*: that the README's timings lacked committed evidence - this record, which
    gives them, merges with them.
- **The branch, verified** in `osoe-load`, configured with `-DOSOE_TEST_PINS=ON`, four
  tests at a time, as it merged: debug 664 of 664, release 664 of 664, asan 663 of
  663, tsan 448 of 448. Under eight busy loops, 662 of 664, where two earlier runs of
  the branch that day had passed every test: `env.echo-uring`'s stream pin, 0.42 of a
  blocking stream against at least 0.50 - a timing pin measured under load, which Part
  1 left unswitched since it held idle ten runs of ten, and which this record leaves
  so - and `shmem.strict-c11`, the whole library rebuilt, past its 180 s; its timeout
  is 600 s now (the round-three review's K3), and under the loops again, beside three
  other build checks, it took 89 and 42 s.
- **Taken without the author**: the six `SHMEM_THREAD_MULTIPLE` models stay opt-in, as
  the brief says, and the README says so. Run once on this branch in `osoe-x86`, all six
  passed, every broken variant failing as it should (`dirty_set` 345 s, `get_completion`
  867, `watch_list` 28, `landing_extent` 3, `copy_out_claim` 17, `pe_quiet` 52: 22
  minutes in all). The checklist gains a run of them
  on the tagged tree, and the README's commands run on the Pi cluster as written - the
  ring with `-K1 --cpu-bind=none` across two nodes, the batch script, `env.probe` on
  every node, `osoe-stat` on a running job's nodes - none of which a run has done yet.
- **Removed**: `build/scratch/wt/review-r3`, `build/scratch/wt/part6` and
  `build/scratch/part6-clone`. Kept until the tag, at the author's word: `osoe-load`
  and `osoe-x86`.

## Still with the author

- **The release checklist** (`docs/release-checklist.md`, with the repository scrub) is
  a draft until the author has read it.
