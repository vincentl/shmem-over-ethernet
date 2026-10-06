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
   backend (item 9 and below). The README's "about 340" was right: 343.
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
   under the same filter. New in both builds, the install tests' one-node
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
   preset that registers nothing. *Undo:* let it register nothing.
8. **Tests of the internode backend's own files are `internode`** - `am.c`'s,
   the connections', the rendezvous's - since the single-node build does not
   contain the code they test. *Undo:* compile those files into their tests in
   both builds.

## Verification

@@VERIFICATION@@

## The adversarial read

@@ADVERSARIAL@@
