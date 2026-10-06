# 2026-10-06: the single-node build (v0.2 step 1, parts 2 and 3)

The second and third parts of v0.2's first step, built on
`docs/prompts/v0.2-step1-single-node-prompt.md` (the brief), Parts 2 and 3, on
the first part's seam (`docs/decisions/2026-10-06-single-node-seam.md`), from
the author's decisions for v0.2 (`docs/decisions/2026-10-05-v0.2.md`, items 2
to 5). A builder who configures with `-DOSOE_SINGLE_NODE=ON` gets a library and
tools that need no liburing, no io_uring and no kernel newer than Rocky Linux
9's, that refuse a job of more than one node by name, and that pass every
one-node test the full build passes; the full build is unchanged.

## What was built

1. **The option**, `option(OSOE_SINGLE_NODE ... OFF)` in the top-level
   `CMakeLists.txt`, said in the configure summary - a line naming the build,
   and in the full build liburing's version - and, wherever the version is
   printed, after it: `OSOE_VERSION_STRING` is `0.1.0 (single-node build)` in
   that build (`cmake/OsoeVersion.cmake`'s `OSOE_VERSION_TEXT`), so every tool's
   `--version`, `oshcc --showme:version`, `SHMEM_INFO`'s first line and the
   `SHMEM_VERSION=1` line say it; the library's file name and soname keep the
   bare version. On, there is no `find_package(PkgConfig)` and no
   `pkg_check_modules(LIBURING)`, no liburing link anywhere (`base/`, `shmem/`,
   the probe, the tests, `CheckStaticLink.cmake`, which now takes
   `SINGLE_NODE` and forwards the build's kind to its nested configure, as
   `CheckStrictC11.cmake` does), `tools/echo-uring` and `base/ring-check.c` are
   not built, and the files differ as the next items say. Off, nothing changes.
2. **The refusing backend**, `shmem/src/transport-single-node.c`, built in
   place of the internode backend's twelve files, none of which is compiled:
   every entry point of `internode-backend.h` and of `internode.h`. Reachable
   on one node: the check every PE makes before its node joins, which refuses
   a job of more than one node with one line - "this is osoe's single-node
   build (OSOE_SINGLE_NODE), which runs a job of one node, and this job spans
   N nodes: the full build runs it" - telling no one, so nothing is made in
   `/dev/shm`; and `internode.h`'s four calls a job of one node makes, the
   default fan-out (`OSOE_INTERNODE_FANOUT_DEFAULT`, which both backends now
   return, so that a one-node job is laid out alike in both builds) and init,
   fini and a team number taken again, which it hands to the generic phase as
   AM does. Unreachable: everything else, which ends the job naming the entry
   point and saying that reaching it is a defect in the part every job uses.
3. **The classic futex**, `base/futex-classic.c`, built in place of
   `base/futex-futex2.c` (the old `futex.c`, renamed): the six functions over
   `FUTEX_WAIT_BITSET` and `FUTEX_WAKE_BITSET`, the private flag for the
   private scope, the absolute `CLOCK_MONOTONIC` deadline `osoe_deadline`
   makes, `FUTEX_BITSET_MATCH_ANY` for the plain calls; the errors handled and
   the timeouts counted as the futex2 file does. The two share nothing but
   `base/futex.h`, which says so. `base/tests/test-futex.c`, run in both
   builds against whichever file is built, now pins the whole contract the
   brief lists: the start-up check passing, the deadline honoured, a stale
   value returning at once, a private wait woken from another thread, a shared
   one woken from another process on a shared mapping, and a bitset wait woken
   only by a wake sharing a bit with it.
4. **The init check says what it checks**: `osoe_futex_requirement()`, beside
   `osoe_futex_check()`, gives the facility the refusal names - the full
   build's text as it was ("Linux 6.7 or later, for the futex2 calls"), the
   single-node build's "the futex calls FUTEX_WAIT_BITSET and
   FUTEX_WAKE_BITSET, which a sandbox (seccomp) must allow", since every
   kernel since 2.6.25 has them and only a sandbox can refuse them.
5. **The label `internode`** (`cmake/OsoeInternode.cmake`): every test of a
   job of more than one logical node, of the internode backend's own files, or
   linking liburing - 383 of the full build's 721 in the debug preset - carries
   it, and in the single-node build none of them is registered, nor its
   program built: `ctest -L internode` on the full build runs exactly what the
   single-node build lacks. A subagent applied the brief's rule to 46 blocks and 12 loop guards
   (`shmem/tests`, `tools/{bench,wrapper,pacer}`), by edit scripts that
   regenerate the files from a pristine snapshot, every property but the label
   left as it was; the session then put back into both builds what the rule had
   caught by accident - the launcher's three node-map tests, which run no
   library; the five loops of `.invalid.*` settings tests, one-node refusals
   that ran the rendezvous's white-box program and now run `shmem_test_init`;
   the two white-box programs' one-node tests, which link liburing only where
   the build has it (`OSOE_LIBURING_LINK`); and the one-node job under a filter
   that denies io_uring. **Audited two ways**, as the brief asks: a script that
   reads every registered test's command, environment and program - the sources
   it compiles, its includes, liburing, seccomp filters, maps inside scripts -
   lists every test the rule makes `internode` that lacks the label, and lists
   none on the full build in all five presets, having listed the 338 the tree
   had before the edit; and the full build's debug suite with io_uring disabled
   (`kernel.io_uring_disabled=2`) failed 343 of its 718 tests, every one of them
   labelled - `env.probe` aside, which both builds register, each with its own
   probe. The single-node build's suite has no test that fails for want of the
   backend (item 9 and below). The README's "about 340" was right: 343. After
   the adversarial read's tests (below) the full build has 729 tests, 384 of
   them `internode`, and the single-node build 349: the other 345 and four of
   its own.
6. **The probe** checks what its build's library uses (`tools/probe/`): the
   checks both builds make in `probe.c`, the full build's futex2 and io_uring
   checks in `probe-full-build.c`, the single-node build's classic futex in
   `probe-single-node.c`, chosen by the build and not by `#ifdef`.
7. **Tests of the single-node build's own** (registered only there):
   `shmem.single-node.refuse-nodes`, `osoe-launch -n 2 -m 0,1`
   under `refusal.sh`: both PEs end by themselves, status 1, naming the build
   and the node count, nothing left in `/dev/shm`; `shmem.refuse.no-futex`,
   under `deny-futex`, a seccomp filter denying `futex(2)` with `EPERM`:
   `shmem_init` refuses by name with the classic futex's requirement, nothing
   left behind; and `env.probe.no-futex`, the probe failing its classic check
   under the same filter - these two not under AddressSanitizer (item 9). New
   in both builds, the install tests' one-node
   counterparts, the single-node build's only way to them:
   `wrapper.statics.one-node`, `wrapper.installed.one-node` - the installed
   `oshcc` and the installed `osoe-launch` - and `tools.pacer.installed.one-node`.
8. **`osoe-launch` is installed**, in both builds: the desktop's way to run a
   job, which the brief's verification runs from the prefix.
9. **What the first build on Rocky Linux 9 found that the static audit did
   not**: GCC 11's `-Wtype-limits`, in `-Wextra`, flagged `wait.c`'s
   signedness test `(T)-1 < (T)0` for every unsigned `T` - a warning, not an
   error, but the tree builds warning-free. `OSOE_SIGNED(T)`, `(T)-1 < (T)1`,
   says the same and is silent. Nothing else: the
   single-node build then built and passed its debug suite on Rocky at the
   first run, 339 of 339 (its two C++ header tests need a C++ compiler, which
   the VM has not). The audit's other expectations held, but one: Rocky 9.8
   ships CMake 3.31, where the audit expected 3.26 from 9.3.

   **And what the first sanitizer runs found**, two things no audit could:
   - **GCC 11's ThreadSanitizer lays memory out in an older way** than GCC
     14's: application memory only the low 8 GiB, a 20 GiB band at
     `0xaaaa_0000_0000` and the top 4 GiB (read from a TSan program's
     `/proc/self/maps` on the VM), so none of §4.1's four heap base candidates
     was free, and 223 of the 336 tests failed at start-up with "no candidate
     heap base is free". A fifth candidate, `0x1_0000_0000`, in the low 8 GiB,
     is tried last (`node.c`, design §4.1); `shmem_align`'s promise from `B`'s
     alignment is now up to 4 GiB, which `malloc.c` checks against the real
     `B` in any case. The full build has the same list: under GCC 14 its first
     candidate maps as before.
   - **Under AddressSanitizer with GCC 14** (Debian 13) the futex filter's two
     tests ended before `shmem_init`: the sanitizer's runtime takes futex
     locks of its own as a process starts, and glibc ends a process whose
     futex call fails as no futex call should ("The futex facility returned an
     unexpected error code"). They are not registered under AddressSanitizer;
     GCC 11's runtime ran them, and so does ThreadSanitizer's.

## The Rocky Linux 9 VM

`osoe-rocky`, from Lima's `rocky-9` template (`dev/lima/osoe-rocky.yaml`;
`dev/lima/create.sh osoe-rocky` picks it by name): 4 CPUs, 4 GiB, a 16 GiB
disk; `gcc make cmake python3` and GCC's sanitizer runtimes, and no liburing
headers. `uname -r` 5.14.0-687.10.1.el9_8.0.1.aarch64; Rocky Linux 9.8 (Blue
Onyx); glibc 2.34 (`ldd --version`); GCC 11.5.0 20240719 (Red Hat 11.5.0-14);
CMake 3.31.8; Python 3.9.25. Its runtime `liburing` 2.12 package is installed
as `qemu-guest-agent`'s dependency; nothing of osoe's links it (`ldd`).

## Choices made without the author

Each is Claude's, under the standing mandate; the undo follows.

1. **The generic phase is every build's**, and the refusing backend hands
   `internode.h`'s init, fini and a team number taken again to it, as AM does:
   so a job of one node keeps its team numbers' counters the same way in both
   builds. *Undo:* the refusing backend makes the three calls nothing, and the
   single-node build leaves `internode-generic.c` out.
2. **`OSOE_INTERNODE_FANOUT_DEFAULT`**, one constant both backends return for
   the default fan-out, so that a job of one node is laid out alike and step 2's
   change of the default is made once. *Undo:* each backend returns its own.
3. **`osoe-launch` is installed, in both builds**: the desktop's way to run a
   job, which the brief's verification runs from the prefix; on a cluster a
   convenience beside `srun`. *Undo:* install it in the single-node build only,
   or not at all, as before.
4. **The version text carries the build**: `OSOE_VERSION_STRING` itself is
   `0.1.0 (single-node build)` in the single-node build, so every place that
   prints the version says it with no change of its own. *Undo:* a separate
   macro, printed beside the version where wanted.
5. **The probe by files, not `#ifdef`**: each build's probe checks what its
   library uses; the single-node build's leaves out `TCP_NODELAY`, since its
   library makes no socket. *Undo:* one file with the build's branch.
6. **The `.invalid.*` settings tests run `shmem_test_init`, in both builds**:
   the refusal they check is the bootstrap's, which every program's
   `shmem_init` reaches, and the single-node build must refuse those values
   too. *Undo:* back to `shmem_test_job`, labelled `internode`.
7. **Tier 3 with the single-node build is a configure error**, rather than a
   preset that registers nothing; the README's preset list says so. *Undo:*
   let it register nothing.
8. **Tests of the internode backend's own files are `internode`** - `am.c`'s,
   the connections', the rendezvous's - since the single-node build does not
   contain the code they test. *Undo:* compile those files into their tests in
   both builds.
9. **The desktop section says what is true of a job of one node where the
   brief's item 2 said otherwise.** `/dev/shm` must hold about 263 MiB a PE,
   the capacity check's figure (four PEs about 1,050 MiB, as the quick start
   says), where the brief had "about 300", which is the container advice
   Building keeps. And the brief's "`OSOE_AGENT_CPUS` and the slice matter
   when PEs outnumber cores" holds of a job of many nodes, where the pacer's
   `--cpu-bind=none` numbers were measured: in a job of one node the progress
   thread has nothing to move and sleeps on its wake word, a sweep at the
   longest (`transport-am.c`), and the slice is ignored before Linux 6.12
   anyway. The README says nothing in the library spins and that the two
   settings are not needed there. *Undo:* the brief's two sentences.
10. **The statics sentences of the decisions of 2026-10-05 (item 9) are
    corrected**, two of Claude's own claims there being wrong: "a wait on such
    a variable would never be woken" - osoe's waits look again at every sweep,
    and a copy could be followed by a wake through the control segment as a
    put's is - is gone, the three other reasons standing; and osoe does not
    "map another process's private memory" but makes a PE's statics memory
    its node-mates can map, Linux having no call that maps memory a process
    keeps private. *Undo:* item 9's wording.

## Verification

On the tree that merges, `d70f256` (the documents' last commit after it changes
no file a build reads), unless a line says otherwise.

**The full build** in `osoe` (Debian 13, kernel 6.12, GCC 14), the floor pins
on: debug 729 of 729, release 729 of 729, AddressSanitizer 726 of 726,
ThreadSanitizer 500 of 500; `tier3` 113 of 113; `osoe-pacer`'s collectives at
three values of `OSOE_SMALL_COLLECTIVE_BYTES` (128, 1024 and 16384), its teams
and threads, and ten minutes of `mix` on four logical nodes of two PEs: no
failure in any, in 60, 66, 64, 121 and 609 seconds. The models, 30 of 30, on
`84e46fc`, whose whole verification - the four presets (721, 721, 718, 492),
`tier3` (113), the same pacer runs and the models - passed too: no model or
model-checker file changed since.

**The single-node build**, the four presets each, its pacer smoke tests among
them, then `cmake --install` into a prefix, `oshcc` from it compiling the
statics test program and two examples of `corpus/openshmem-examples` (`hello`,
`bcast1`), each run with the installed `osoe-launch` at 8 and 32 PEs on the
VM's four cores - at `SHMEM_SYMMETRIC_SIZE=16m`, since 32 PEs at the default
heap ask 8.2 GiB of `/dev/shm` and 8 ask 2.1, more than the VMs' half of their
memory (the step's review, scale Q3-2) - and a job of two logical nodes refused:

| | Debian 13, liburing hidden (`osoe-load`) | Rocky Linux 9.8 (`osoe-rocky`) |
|---|---|---|
| debug | 349 of 349 | 347 of 347 |
| release | 349 of 349 | 347 of 347 |
| AddressSanitizer | 346 of 346 | 344 of 344 |
| ThreadSanitizer | 346 of 346 | 344 of 344 |
| installed: version text | `0.1.0 (single-node build)` | the same |
| installed: 3 programs at 8 and 32 PEs | all exit 0 | all exit 0 |
| two logical nodes | refused by name on both PEs, exit 1, nothing in `/dev/shm` | the same |
| `internode` tests registered | 0 | 0 |
| an ELF file linking liburing | none | none |

AddressSanitizer runs three fewer: the futex filter's tests (item 9). Rocky's
suites are two fewer than Debian's, the headers' C++ test needing a C++
compiler the VM has not. No build of either printed a warning. The model
checker's 30 tests in a single-node configure on Rocky's Python 3.9: 29 passed
at `-j4`, and `model.abort` was killed by the kernel for memory - 1.4 GiB in a
VM of 4 beside three other models; alone it passed, in 22 minutes.

What these runs found on the way, all fixed before the tree above: GCC 11's
ThreadSanitizer's heap base and GCC 14's AddressSanitizer's futex locks (item 9
of what was built); `stat.segment`'s timeout under GCC 11's AddressSanitizer,
whose leak check costs every process half a second at exit (an empty program
takes 0.57 s to exit with it and 0.003 without), and the test runs `osoe-stat`
thirteen times; and the signal test's count under ThreadSanitizer, which holds
a signal for a handler until its own next intercepted call.

## The adversarial read

Two reads before the merge, each by a fresh agent: one adversarial, of the
commit `84e46fc` against the brief and the rule the step is held to - one tree,
two builds, no drift - which built both builds in `osoe-x86`, its x86-64 build
under Rosetta among them; and one of every sentence Parts 2 and 3 changed in the
README, against the tree. Their findings, and what became of each:

1. **A signal ended the single-node build's timed waits early** (fixed). A
   timed `FUTEX_WAIT_BITSET` comes back with `EINTR` from every handler,
   `SA_RESTART` or not - the kernel restarts it through a restart block, which a
   handler turns into `EINTR` - where futex2's wait restarts under
   `SA_RESTART`; `futex-classic.c` returned 0, as for a wake. Its callers make
   a fresh sweep deadline each pass, so under a profiler's timer (`SIGPROF`
   every 10 ms) a blocked PE never swept: in a one-node job of 2.5 s, PE 1's
   sweeps stood at 0 against the full build's 24, and `transport.watch.wakes`
   and `.spurious` at 234 against 0 - and `osoe-stat` reads a PE asleep with
   its sweeps standing still as a dead one. Not a hang: the agent, which blocks
   signals, still ended the PE. Now both futex files make the wait again to the
   same absolute deadline, which is what the kernel's own restart does - the
   full build's too, which starved the same way under a handler without
   `SA_RESTART` - and `futex.h` says a signal does not end a wait.
   `test-futex.c` pins it: a wait of 300 ms under a 10 ms timer ends at its
   deadline, with `SA_RESTART` and without.
2. **A wake of none woke one in the classic file** (fixed): `FUTEX_WAKE_BITSET`
   with 0 wakes one, futex2 none. No caller passes 0; both files now return at
   once for n of 0 or less, `futex.h` says so, and `test-futex.c` pins it with
   a waiter that one call, not a loop, would show woken.
3. **The start-up check tried the private scope alone** (fixed), in both files,
   where every wait between a node's PEs is shared: under a sandbox that allows
   private futex calls only, as glibc's own locks need, the check passed and the
   job died of `SIGABRT` at its first shared wait, its objects made. It now
   makes a wake and a wait in each scope, and `deny-futex --shared` and
   `deny-futex2 --shared` - a filter refusing the calls without the private
   flag - give each build a test that the job is refused by name with nothing
   made: `shmem.refuse.no-shared-futex`, and the full build's
   `shmem.refuse.no-shared-futex2`.
4. **Tests labelled `internode` only for the map they launch** (partly fixed):
   one-node variants now run in both builds of the installed launcher's signal
   forwarding (`shmem.launcher-signals.{term,int,kill}.one-node`,
   `launcher-signals.sh` taking `one-node`), of a death after init stranding no
   named object (`shmem.kill-after-init.{KILL,TERM,kept}.one-node`), and of a
   compile-only call followed by a link (`wrapper.compile-then-link.one-node`).
   Still two nodes only, and so absent from the single-node build: init's
   reclaiming of a dead start's objects (`shmem.leftovers.*`, whose script
   plants the job tables and connection area a job of one node never opens),
   `shmem.dead-pe.lead-in-init`, `shmem.job.0*`, and the pacer's planted
   defects, whose `PacerPlant.cmake` launches `0,0,1,1`. Each would need its
   script or its expectations changed for one node, which this step leaves to
   a later one; the single-node build still runs the pacer's own one-node test.
5. **Documents** (fixed): `launch.c` and `tools/CMakeLists.txt` called
   `osoe-launch` a test tool not installed; `docs/README.md` called `osoe` the
   one VM `dev/lima/` defines; `osoe-rocky.yaml` said no liburing is installed
   (its runtime is, with `qemu-guest-agent`); the desktop section ran the
   installed launcher without saying it ends a job after 60 s, which it now
   says with `-t`; and below Linux 5.3, which has no `pidfd_open`, every PE of a
   job printed that it watches by pid - now the node's lead alone, once.
   `liveness.c` also gained `pidfd_open`'s number for headers older than 5.3,
   as the futex2 file has its calls'.
6. **What the brief's "Off, nothing changes" no longer says** (recorded): the
   full build changed in three ways a reader of that sentence would not expect
   - `osoe-launch` is installed (choice 3), the `.invalid.*` tests run
   `shmem_test_init` (choice 6), and tests were added to it, the install tests'
   one-node counterparts and the one-node variants above among them. Nothing
   of its library changed but the futex fixes above, which both builds take.
7. **Which refusal a job of more than one node meets first** (an observation,
   left): the bootstrap's own refusals - a bad `OSOE_PORT_RANGE`, say - come
   before the single-node build's, and are told to the node, which leaves its
   tombstones, as in the full build. "Before anything is made in `/dev/shm`"
   holds for a job whose settings are otherwise valid, which is what the README
   says it of.

What the adversarial read found sound, with the evidence: every function of
`transport-single-node.c` but its check is unreachable on one node, each guard
named; the full build's `-LE internode` tests are the single-node build's less
its own three in every preset; the single-node build compiles none of the
internode files, and no ELF file of it links liburing (111 do in the full
build), `libosoe.so` needing libc alone and importing no socket or io_uring
call; the exported symbols are the same; a live four-PE job's control segment
is byte-for-byte the same layout, 221 metrics, in both builds; the refusal
holds under `-m 0,1`, `-s`, `0,0,1,2` and `OSOE_FORCE_REMOTE=1`, nothing left
in `/dev/shm`; no internode file reads the environment, and every setting is
refused alike on one node in both builds; and the two futex files agree on the
private flag, the deadline's clock and time namespace, the bitset and the
errors, but for findings 1 and 2 and the classic call's timer slack.

The README read found nine sentences wrong or loose, and the session checked
each against the tree before fixing it (`362bb89`): `/dev/shm`'s figure, the
agent's settings (choice 9), the suite's counts (338 of 721 and three of its
own), the distributions run against those that should run, Ubuntu 22.04's
CMake, the tests that fail without io_uring (`env.probe` aside), the pacer's
programs by build, the link-time dependencies, and the statics sentences
(choice 10).
