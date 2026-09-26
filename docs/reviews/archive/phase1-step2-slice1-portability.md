# Phase 1, step 2, slice 1 — portability review ("Will it run anywhere else?")

Reviewer: subagent `portability`, 2026-09-20, against `main` at `7ff9343` (slice merge `d542c63`).
Evidence: `docs/reviews/evidence/phase1-step2-slice1/portability/`. All builds and mutations were made in
a private copy under `build/scratch/review-portability/`; the main tree's build directories were not touched.

Claim labels: *documented* (a primary document was read this session), *observed in source*, *measured*
(run this session), *recalled* (memory; unverified). Timings and single-run test results in the shared VM
are "measured under contention".

## Verdict

**As C, it is more portable than its evidence suggested; as a thing someone else can launch, it is not there
yet, and that is mostly a gap rather than a risk.**

The strongest new evidence is positive. I booted a second VM on Debian 13's **16 KiB-page arm64 kernel**
(47-bit user address space, 32 MiB huge pages — the same three properties as the Raspberry Pi 5 kernel,
*documented* below) and the whole suite passed, 131 of 131, under the debug, release and asan presets
(*measured*). clang 19 builds the tree with zero warnings and passes 131 of 131 (*measured*). Every library
and tool source passes a musl syntax check (*measured*). A third-party program compiles against the
installed tree alone under `-std=c11 -pedantic -Wall -Wextra` with no diagnostics, also through
`<mpp/shmem.h>` under `-std=c99`, and also as C++17 (*measured*). The page-size question, which was the
one I expected to bite, does not.

What is missing is everything between "a correct library" and "a job on a cluster": the library **refuses
to start under Slurm by design** (`bootstrap.c:50`), the installed tree contains **no launcher at all**, a
static build installs an **unlinkable archive**, the kernel checks §1 says "moved into init" **did not move**,
and the **trust boundary is not stated anywhere** while the receive path already contains one write that a
peer controls (F1, reproduced). None of these is a design error; each is a decision or a piece of work that
should be made before step 3 puts the same parser on a socket, because step 3 is where F1 stops being a
robustness bug and becomes a remote write primitive.

Of the four untested dimensions, my ranking by *risk* (chance that something already written is wrong) is:
sockets and trust > Slurm > second machine > Pi 5 / 16 KiB pages. By *gap* (work simply not done):
Slurm > sockets > install > the rest.

---

## Findings

### F1. A get reply's `chunk_offset` is a peer-controlled write offset, and no test can see it
- **Location:** `shmem/src/progress.c:44-50` (destination), `:173-190` (completion); `landing()` in
  `shmem/src/progress-host.c:70-76`. Sibling reviewers report the same line; this finding adds the
  reproduction.
- **Failure scenario:** a peer sends `OSOE_AM_GET_REPLY` with `origin_pe` = any PE local to the receiver,
  `chunk_offset = X`, any token. `destination()` returns `slot->data + X` with no comparison of `X +
  payload_bytes` against the landing slot's size (one AM chunk), no check that a get is outstanding, and no
  comparison of `token` with the one issued. The parser then `memcpy`s the payload there (`am.c:206-207`).
  The landing zones live in the control segment, so `X` reaches the next PE's slot, the rest of the control
  segment, and whatever is mapped after it. `chunk_offset` is `uint32_t`: 4 GiB of reach, forwards.
- **Evidence (measured, 3 of 3 runs):** `forged-get-reply.patch` adds a 256-byte canary after
  `test-progress.c`'s landing storage and posts one unsolicited reply with `chunk_offset = HEAP + 32`. Every
  run prints `64 bytes beyond the landing zone were overwritten by a forged get reply` on both nodes, and
  **the test still exits 0** — the forged reply also sets `length`/`token` on the slot without disturbing
  the real get that follows. Contrast `resolve()` (`progress-host.c:49-67`), which bounds-checks puts, gets
  and rmw against the heap and says why (*observed in source*): the put path is sound, the reply path was
  missed.
- **Severity:** design change needed before step 3 (today the only "peer" is the same binary over shared
  memory, so it is a latent defect; on a socket it is a remote arbitrary-offset write into the control
  segment). **Confidence:** high.
- **Related, from reading `am.c`/`progress.c` (observed in source, not mutated):** `rmw.width` is not
  checked to be 4 or 8 and the target offset's alignment is not re-checked on the receiving side
  (`progress.c:145-151`, `rmw.h:65-75` — any width other than 8 is treated as 32-bit); `rmw.op` outside the
  enum silently does nothing; a message `len` is not compared with the bytes left in its segment
  (`am.c:161-167`), so an over-long message is only caught at `end_segment` after its payload has been
  applied. The last is memory-safe (the heap bounds check holds) but it means "framing error detected at
  once" (`am.h`) is not quite true. Siblings list `message_count`/`total_len`; I confirm `total_len < 32`
  and `message_count == 0 && total_len > 32` are both caught.

### F2. The trust boundary is not stated anywhere
- **Location:** design §3.4, §3.13, §3.15, §6.2; absent.
- **Evidence (measured):** `grep -n -i 'trust\|hostile\|malicious\|authent\|attacker\|untrusted\|security'`
  over the design and README returns only the two "mode 0600" lines (§3.14 line 932, §6.2 line 3357) and
  three unrelated ptrace lines. The pattern was checked to match (it finds "permission").
- **Failure scenario:** §6.2's handshake "carries the job and step IDs, so a client that reaches another
  job's server is rejected". Slurm job ids are sequential and public (`squeue`), the port sequence is a
  deterministic hash of them, and nothing else authenticates a connection. Any host that can reach the
  port can join as a node, after which the one-sided model gives it read and write of every heap *by
  design* — and F1 gives it more than the heaps. Whether that matters depends on a statement nobody has
  made: is the cluster LAN trusted?
- **What I recommend the design say** (three sentences would do): (1) all PEs of a job trust each other
  completely — a one-sided API cannot do otherwise; (2) other local users are untrusted, which mode 0600
  on every object already enforces (*measured*: all six objects of a two-node job are `-rw-------`,
  `stale-segments.out`); (3) the network is *either* declared trusted, *or* the handshake carries a
  secret. A secret is nearly free: Slurm propagates the submitting environment to every task
  (*documented*, srun "propagates the environment"), so an `oshrun`-style wrapper can set
  `OSOE_JOB_SECRET=$(head -c16 /dev/urandom | xxd -p)` before `srun` and every task inherits it with no
  KVS. And independently of (3): **a peer's bytes must never be able to write outside a heap or a landing
  slot**, because a buggy peer is certain even where a hostile one is not — that is F1.
- **Severity:** clarification needed (becomes design change needed if the answer is "LAN not trusted").
  **Confidence:** high that it is unstated.

### F3. The probe's checks did not "move into init"; on a pre-6.7 kernel the library aborts mid-rendezvous and leaks
- **Location:** design §1 ("the `tools/probe` checks, moved into init"); `shmem/src/init.c` (no check),
  `base/futex.c:40-46,52-56`.
- **Evidence (observed in source):** nothing under `shmem/src` or `base/` calls `uname`, probes a syscall,
  or touches io_uring; `libosoe` does not link liburing at all (`shmem/CMakeLists.txt:38`), so the
  configure-time `liburing>=2.6` requirement currently guards only `tools/probe`.
  **(measured):** `deny-futex2.c` is a seccomp wrapper that makes syscalls 454-456 return `ENOSYS`, which is
  what Linux < 6.7 does. Under it, a 1-PE and a 2-PE job both die with
  `osoe: futex_wake failed: Function not implemented`, `SIGABRT`, and the launcher reports
  `left behind: /dev/shm/osoe.<job>.0.0.0.heap.0` and the control segment.
- **Failure scenario:** Raspberry Pi OS Bookworm on a Pi 5 — the OS most Pi 5s in the field still run, with
  a 6.6 kernel (*recalled*) — gets a core dump and stale heaps instead of "osoe needs Linux 6.7 (futex2);
  this is 6.6". It is *refusal*, not misbehaviour, which is the important half; but it is refusal without
  the message §1 promises, after shared objects exist.
- **Severity:** clarification needed now; the io_uring opcode checks become mandatory at step 3, where the
  failure mode of a missing `IORING_OP_FUTEX_WAIT` is an `-EINVAL` CQE in the idle loop — a spin or a hang,
  not an abort. **Confidence:** high.

### F4. `tools/probe` does not test the forms the design uses
- **Location:** `tools/probe/probe.c:73-129`.
- **Evidence (observed in source):** the io_uring futex check waits on a **private, stack** word with a
  **stale value** (`FUTEX2_PRIVATE`, expects `-EAGAIN`). §3.10's wake word is in a **shared** mapping and
  is bumped by **other processes**; a kernel could pass the probe and fail that. The cross-process futex
  check uses `memfd_create` + `fork`, where the library uses `shm_open` by name between unrelated processes.
  Not probed at all: `pidfd_open` and a pidfd on the ring (§3.15), `MAP_FIXED_NOREPLACE` being honoured
  (§4.1; `node.c:144-145` handles a kernel that ignores it, which is good, but nothing tests it), huge
  pages, `/dev/shm` capacity. Return values of `ftruncate`, `mmap`, `io_uring_queue_init_params` (line 137)
  and `socket`/`bind`/`connect` are ignored, so on a machine where one of them fails the probe crashes or
  reports a misleading line rather than a `FAIL`.
- **What I added (measured):** `probe-extra.c` makes the missing checks — uring `FUTEX_WAIT` on a shared
  word in a named shm mapping woken by `futex_wake` from another process; `pidfd_open` + `POLL_ADD`;
  `MAP_FIXED_NOREPLACE` refusing an occupied address with `EEXIST`. All pass on the 4 KiB VM
  (`probe-extra.out`). Not run on the 16 KiB VM (the file was outside that VM's mount and the budget ended);
  run it on the Pi.
- **Severity:** minor. **Confidence:** high.

### F5. Launch under Slurm is refused; the `OSOE_` table in §6.2 is no longer what the library reads
- **Location:** `shmem/src/bootstrap.c:49-56, 62`; design §6.2 table; `init.c:80-82` (`SHMEM_INFO` text).
- **Evidence (observed in source):** `SLURM_JOB_ID` set and `OSOE_PROCID` unset is a fatal "launch under
  Slurm is not yet supported". The library requires **`OSOE_STEP_NUM_NODES`**, which is in neither §6.2's
  table nor the `SHMEM_INFO` help text. **(measured):** a by-hand two-PE launch of the installed hello
  needs all eight variables including that one. No hostlist parser exists (§6.2 promises one "with unit
  tests in tier 1"); `base/node-map.c` parses only `OSOE_NODE_MAP`'s comma list. `RLIMIT_NOFILE` is not
  raised (no `setrlimit` in the tree). The "rendezvous" is a shared-memory object (`job.c:96-104`), so even
  with a wrapper script mapping `SLURM_*` to `OSOE_*`, **day one on the cluster is single-node only**.
- **Severity:** gap, as the what-changed note says; recorded as *clarification needed* only for the
  undocumented ninth variable. **Confidence:** high.

### F6. What Slurm's documentation does and does not confirm about §6.1
Read this session from slurm.schedmd.com (Slurm 24.11, the version in Debian 13 — *documented*,
packages.debian.org: `slurm-wlm 24.11.5-4+deb13u1`).
- **Confirmed (documented):** `SLURM_GTIDS` exists ("global task IDs running on this node");
  `SLURM_STEP_NUM_TASKS`, `SLURM_STEP_NUM_NODES`, `SLURM_STEP_NODELIST` are step-scoped and distinct from
  the `SLURM_JOB_*` ones; under `--distribution=cyclic` "tasks one and four on the first node", i.e. ranks
  on a node are **not contiguous**, which is exactly why §6.1 builds the map from `SLURM_GTIDS` and not
  from `SLURM_TASKS_PER_NODE` (`2(x3),1` format confirmed). The design's reasoning here is sound, and
  `base/node-map.c` plus `launch.c` already exercise non-contiguous maps (`0,1,2,0,1,0`).
- **Not confirmed:** that `SLURM_NODEID` is *step*-relative. The documentation says only "the relative
  node ID of the current node". The design cites Slurm's `env.c`; I could not verify it. It is load-bearing
  (it selects the rendezvous server and indexes `SLURM_STEP_NODELIST`). First-day check below.
- **A trap the design half-knows (documented, sbatch.html):** `SLURM_PROCID`, `SLURM_LOCALID` and
  `SLURM_NODEID` **are set for the batch script itself**. §6.2's bare-`sbatch` test keys on
  `SLURM_STEP_NUM_TASKS` being absent, which is the right variable; an implementation that keyed on
  `SLURM_PROCID` would turn every mis-launched batch script into a successful one-PE job — the exact
  failure §6.2 exists to prevent. Worth a unit test when the Slurm path is written.
- **Heterogeneous jobs (documented, heterogeneous_jobs.html):** one `srun a : b` step gives globally unique
  `SLURM_PROCID`s, but each component has its own job id (`SLURM_JOB_ID_HET_GROUP_<n>`) and node list
  (`SLURM_JOB_NODELIST_HET_GROUP_<n>`). §6.2 names objects and authenticates the handshake by job id and
  takes the server from "the first host of `SLURM_STEP_NODELIST`"; whether either is consistent across
  components is unverified. **Recommendation: refuse when `SLURM_HET_SIZE` is set**, with a message, until
  someone has a het job to test against. A het job is also the one place a big-endian or 4 KiB/16 KiB mix
  could appear, so refusing costs nothing real.
- **Hostlists (documented, slurm.conf via search; secondary):** `linux[0-64,128]` and **multiple bracket
  groups in one name**, `rack[0-63]_blade[0-41]`, plus zero padding. A "fifty line" parser must do the
  cartesian product and preserve width; test fixtures should come from `scontrol show hostnames` on the
  real cluster.
- **Ports:** `--resv-ports` needs `MpiParams=ports=` in slurm.conf and is documented as "for native Cray's
  PMI only"; `SLURM_STEP_RESV_PORTS` is not in the current srun output-variable list (*documented absence*,
  one fetch). The design's hashed candidate ports avoid depending on it, which is right.
- **Peers are found by resolving a Slurm `NodeName`.** Slurm lets `NodeName` differ from `NodeHostname` and
  `NodeAddr` (*recalled*), and hobby Pi clusters commonly resolve names through `/etc/hosts` or mDNS only on
  the head node. `SLURM_LAUNCH_NODE_IPADDR` is an address but of the *srun* host, not node 0. If the first
  host of the step's list does not resolve on every compute node, rendezvous times out after 60 s with a
  message that should name the host and the `getaddrinfo` error.
- **Severity:** clarification needed (NODEID, het refusal). **Confidence:** medium; raised by one `srun
  -N2 -r2 env` on any Slurm install.

### F7. Removing a control segment's name from outside kills a healthy job at finalize
- **Location:** `shmem/src/node.c:509-510`.
- **Evidence (measured, by accident, then understood):** in the first `stale-segments.sh` run the objects
  were removed with `rm` while PEs were still alive; each node's last finalizer then died with
  `cannot remove control segment /osoe.<job>.0.0.0: No such file or directory`. `shm_unlink` returning
  `ENOENT` means the goal is already achieved; treating it as fatal converts a harmless event into a
  nonzero job exit after the application's work is done.
- **Why it is likely on the target (recalled — verify):** systemd-logind's `RemoveIPC=yes`, the Debian
  default, deletes a non-system user's POSIX shared memory when their **last login session ends**. A user
  who `ssh`es into a compute node to look at a running job and logs out again triggers it; Slurm tasks are
  not logind sessions unless `pam_slurm_adopt`/`pam_systemd` make them so. Mappings survive, so the job
  keeps running — and then aborts in `shmem_finalize`; late `shm_open`s by name (a non-lead PE that is slow
  to join) would instead time out. Slurm's `job_container/tmpfs` plugin gives each job a private
  `/dev/shm` (*recalled*), which avoids this but makes the segment **invisible to `osoe-stat` run from a
  normal login shell** — §3.14's "diagnose from outside without cooperation" then needs `nsenter` or
  `srun --jobid --overlap osoe-stat`.
- **Severity:** minor for the `ENOENT` (one-line fix: tolerate it); clarification needed for the
  operational note. **Confidence:** high for the code, medium for RemoveIPC.

### F8. `osoe-stat --gc` and the launcher's leak check know one of the four kinds of object
- **Location:** `tools/stat/stat.c:624-667`; design §3.14 / §6.2 ("for `osoe-stat --gc` or a Slurm epilog
  to remove").
- **Evidence (measured, `stale-segments.out`):** a two-node job killed with `SIGKILL` leaves six objects:
  two control segments, two `.conn.`, one `.job.`, one `.wire.`. `--gc` removes the two control segments
  and prints "is not an osoe control segment" for the other four, which stay. The design text predates the
  connection area being a separate object. Step 3 removes `.job.` and `.wire.`; `.conn.` (the largest,
  and at 500 nodes about 500 × 2 × `OSOE_AM_BUFFER`) remains.
- **Severity:** minor. **Confidence:** high. The epilog glob `rm -f /dev/shm/osoe.$SLURM_JOB_ID.*` does
  cover everything and is the better recommendation.

### F9. `OSOE_AM_HUGEPAGES=1` can never succeed; heap huge pages in §4.1/§6.2 do not exist
- **Location:** `shmem/src/connection-area.c:233-241`; design §4.1 "Costs", §6.2 last paragraph of the
  node-local part.
- **Evidence (measured, 16 KiB VM, 8 free 32 MiB huge pages):** every PE prints "could not be mapped with
  huge pages (Invalid argument); using ordinary pages" and `HugePages_Free` stays 8. `MAP_HUGETLB` is only
  valid for anonymous or hugetlbfs-backed mappings (*recalled*, mmap(2)); the fd here comes from `shm_open`,
  i.e. tmpfs, so the result is `EINVAL` on every kernel. The fallback is graceful and loud, which is right.
  **(observed in source):** `node.c` contains no hugetlbfs path at all, while §6.2 says in the present tense
  "heaps use it under the same names". On the Pi 5 a huge page is **32 MiB** (*documented*,
  docs.kernel.org arm64 hugetlbpage: 16K granule → 32M), so a 256 MiB default heap is 8 pages and any size
  not a multiple of 32 MiB needs rounding the code does not yet have. Whether the Pi kernel enables
  `HUGETLBFS`/`TRANSPARENT_HUGEPAGE` could not be established from `bcm2712_defconfig`.
- **Severity:** minor (a dead option and a design sentence ahead of the code). **Confidence:** high for the
  behaviour, medium for the cause.

### F10. A static build installs an archive nobody can link
- **Location:** `base/CMakeLists.txt:2` (`osoe_base STATIC`, "Never installed"), `shmem/CMakeLists.txt:38`.
- **Evidence (measured):** `-DBUILD_SHARED_LIBS=OFF` builds and installs `lib/libosoe.a`; linking the
  hello against it fails with over a hundred undefined references (`osoe_deadline_in` ×27,
  `osoe_futex_wake` ×25, `osoe_environment_u32` ×12, …) because `libosoe_base.a` is neither merged nor
  installed. `BUILD_SHARED_LIBS` is an advertised option (`CMakeLists.txt:21`). Fix: make `osoe_base` an
  `OBJECT` library.
- **Severity:** minor (defect, mechanical fix). **Confidence:** high.

### F11. A third party can compile against the install but cannot run what they built
- **Location:** `tools/launch/launch.c:3` ("Not installed"), `shmem/CMakeLists.txt` install rules.
- **Evidence (measured):** the staged prefix holds `libosoe.so*`, four headers and `bin/osoe-stat`. No
  `osoe.pc`, no CMake package config (`install(TARGETS osoe)` has no `EXPORT`), no `oshcc`, no `oshrun`,
  no `osoe-launch`. The hello runs bare to "no launcher environment … Start programs with osoe-launch" —
  a program that is not in the install — and under Slurm to F5's refusal. The specification's annex
  describes `oshcc`/`oshrun` as the reference implementation's wrappers, not a requirement (*documented*,
  spec text lines 9702-9723 of the pdftotext output), so this is convention, not conformance; but every
  existing OpenSHMEM build script and test suite calls `oshcc` and `oshrun`.
- **Recommendation:** install `osoe-launch` (it is already the documented way to run on one machine); ship
  an `osoe.pc` (it is three lines, and the project already requires pkg-config); make `oshcc` a shell script
  over `pkg-config` and `oshrun` a shell script over `srun`/`osoe-launch` — the latter is also where F2's
  job secret and `srun -K1` (§3.15) belong.
- **Severity:** clarification needed. **Confidence:** high.

### F12. The stated language level is not the one the tree needs
- **Location:** design §1 ("a C11 compiler"), `CMakeLists.txt:23-25` (`CMAKE_C_EXTENSIONS ON`).
- **Evidence (measured):** with `CMAKE_C_EXTENSIONS OFF` (`-std=c11`) the build fails in
  `base/deadline.c`, `shmem/src/agent.c`, `watchdog.c` and eight test files: `clock_gettime`,
  `CLOCK_MONOTONIC`, `nanosleep`, `pthread_sigmask`, `kill`, `ftruncate`, `setenv` are undeclared because
  those files define no feature-test macro, while ten others define `_GNU_SOURCE`. Four `-pedantic`
  warnings for empty `__VA_ARGS__`. GNU extensions actually used by the library are few and guarded by
  nothing: `__typeof__` and `__attribute__((weak, alias, visibility))` in `export.h`,
  `__attribute__((format))`, `gettid`. All are supported by gcc and clang, which are the only compilers
  that matter on Linux.
- **The `<stdbit.h>` question is moot today:** nothing in the tree includes it (*measured*: grep, and
  independently the musl syntax pass — musl 1.2.5/1.2.6 ship no `<stdbit.h>`, *documented*). §3.14's
  sentence about `stdc_leading_zeros_ull` describes histograms that do not exist yet. When they do:
  glibc ≥ 2.39 only (*documented*: Debian 13 has 2.41, Ubuntu 24.04 2.39, Debian 12 2.36, RHEL 9 2.34),
  neither gcc nor clang ships the header, and musl would become unbuildable for the sake of one
  count-leading-zeros. A six-line portable loop in `base/` keeps Alpine alive and still honours §3.16's
  objection to builtins.
- **Severity:** minor — say "gnu11, gcc or clang" and add `_GNU_SOURCE` (or `_POSIX_C_SOURCE=200809L`)
  uniformly. **Confidence:** high.

### F13. "An unaligned 64-bit atomic faults on aarch64" is not true on this machine
- **Location:** design §3.4; comments at `am.h:68-71`, `connection.h:91-93`, `rmw.h:62-63`.
- **Evidence (measured):** `unaligned-atomic.c` performs `atomic_fetch_add` on a `uint64_t` at offsets 0-7;
  all eight succeed with gcc 14 defaults (outline atomics, LSE at run time), with `-mno-outline-atomics`
  (LL/SC) and with `-march=armv8.1-a` (inline LSE), on the Apple-silicon VM. Linux does not enable EL0
  alignment checking (*recalled*), so the Cortex-A76 should behave the same; verify on the Pi.
- **Consequence:** none for correctness — the alignment rule and `osoe_symmetric_word_offset`'s check are
  right regardless, and the spec requires aligned AMO targets. But the stated *reason* is wrong, and the
  belief that a misaligned AMO "crashes rather than degrades" is what makes the missing receive-side
  alignment check in F1's related list look safe. It silently performs a misaligned, possibly
  cache-line-straddling update instead.
- **Severity:** minor. **Confidence:** high here, medium for the A76.

### F14. ThreadSanitizer cannot run on the target's address-space width
- **Evidence (measured, 16 KiB VM):** every test of the tsan preset dies at start-up with
  `FATAL: ThreadSanitizer: unsupported VMA range — Found 47 - Supported 39, 42 and 48` (gcc 14.2).
  The Pi 5 kernel is `CONFIG_ARM64_16K_PAGES=y`, `CONFIG_ARM64_VA_BITS_47=y` (*documented*,
  raspberrypi/linux `rpi-6.12.y` `bcm2712_defconfig`, fetched by a subagent).
- **Consequence:** the tsan tier can only ever run in the development VM or on a Pi booted with
  `kernel=kernel8.img` (4 KiB pages; *documented*, config.txt documentation). Not a defect; write it in §8.5
  so nobody spends a day on it.
- **Severity:** minor. **Confidence:** medium-high (same VA width, different kernel build).

### F15. 32-bit hosts are refused at compile time, which is right, and nowhere said
- **Evidence (observed in source):** `amo.c:51-56` asserts `int64_t` is `long` and `size_t` is `unsigned
  long`; the heap-base candidates (`node.c:39-44`) all exceed 4 GiB. Raspberry Pi OS still ships a 32-bit
  userland that runs on the 64-bit kernel. The README's requirements list should say "64-bit userland".
- **Severity:** minor. **Confidence:** high (not compiled for armhf — no cross toolchain in the VM).

### F16. Endianness is host order on the wire, detected only by accident
- **Evidence (observed in source):** every AM header is a host-order struct moved with `memcpy`. There is
  no byte-order mark. A big-endian peer *would* be rejected, because `osoe_am_table_checksum` hashes the
  in-memory bytes of `uint32_t fixed` (`am.c:42-45`) and so differs by byte order — an accident, and one a
  tidy-minded change to hash values instead of bytes would remove. Every plausible target is little-endian.
  One sentence in §3.4 ("host order; little-endian only; the handshake magic is the byte-order check")
  and a magic number in the step-3 handshake close it.
- **Severity:** minor. **Confidence:** high.

### Cited from sibling reviewers, not re-derived
Shared objects are sized with `ftruncate` only and never compared with `/dev/shm` capacity, so
`L × SHMEM_SYMMETRIC_SIZE` beyond tmpfs is `SIGBUS` on first touch (my `probe-extra` prints the free space
for this reason; a Pi 5's `/dev/shm` is half of 4-16 GiB). `SMA_SYMMETRIC_SIZE` is ignored
(`bootstrap.c:103` reads only the `SHMEM_` spelling, although `spec_env` exists twenty lines above).

---

## Gap versus risk, per untested dimension

*Gap* = work known not to be done. *Risk* = something already written that may be wrong there.

| Dimension | Gap | Risk | Evidence and residual |
|---|---|---|---|
| **16 KiB pages (Pi 5)** | none found | **low** | 131/131 under debug, release, asan on a 16 KiB / 47-bit kernel (*measured*). `page_round` uses `sysconf`; no literal 4096 in `shmem/src` or `base/` (*measured* grep). Residual: tsan unusable (F14); huge pages are 32 MiB (F9). |
| **Address-space layout** | none | low | Heap landed at `0x1000_0000_0000` on the 47-bit kernel (*measured*). Candidates are 64 KiB- and 32 MiB-aligned. A 39-bit kernel reaches only the 4th candidate (`0x10_0000_0000`); untested — Pi 4 `kernel8.img` VA width not verified. `MAP_FIXED_NOREPLACE` failure is loud and names the remedy. Leads on different nodes may choose different `B`; §4.2 says the wire is offsets, not verified by me. |
| **Older kernel / OS** | init-time check (F3) | low-medium | Refuses, but by `abort()` with leaks. Bookworm fails at CMake configure on liburing (2.3, per §1) before it gets that far, which is the better message. |
| **Cortex-A76 vs Apple core** | — | low | Same ISA level for what is used (LSE present on both; gcc outline atomics choose at run time). Store-buffer and weak-memory behaviour differ in degree, not kind, from the VM; the VM is already aarch64, which is the hard case. Unmeasured: outline-atomics call overhead per counter bump; build with `-mcpu=cortex-a76` on the Pi and compare. |
| **x86-64** | never built | low-medium | `OSOE_CACHELINE` changes (64 vs 128) so every shared layout differs, but no layout is shared across machines; futex2 syscall numbers 454/455 are the same on x86-64 (*recalled*). TSO hides ordering bugs rather than creating them. One CI build would retire this. |
| **clang / musl** | — | low | clang 19: zero warnings, 131/131 (*measured under contention, one run*). musl: syntax-only pass of all sources (*measured*); not linked or run. |
| **32-bit, big-endian** | unsupported | none | Refused at compile time (F15) / rejected by accident (F16). Say so. |
| **Second physical machine** | the socket wire (step 3) | **medium** | Everything "internode" today shares a kernel, a clock, a page cache and `/dev/shm`. Specific things that cannot have been exercised: the job rendezvous is a shm object (`job.c`), wire teardown is a shm handshake, `kill(pid,0)` liveness, futex wakes across "nodes". None of that code survives step 3, so the risk is in what replaces it, not in what exists. |
| **Sockets / hostile or buggy peer** | the transport | **high** | F1 is real now and remote at step 3; F2. The stream parser itself is careful (length-terminated segments, every framing field cross-checked, malformed-segment tests); truncation is explicitly deferred to the socket owner (`am.h`), correctly. |
| **Slurm** | the whole launcher path (F5) | **medium** | The *reasoning* in §6.1 is confirmed where I could check it (GTIDS, cyclic, step vs job lists). Unverified: `SLURM_NODEID` step-relativity, het jobs, NodeName resolution (F6). Operational: RemoveIPC and private `/dev/shm` (F7), `KillOnBadExit`. |
| **Install / third party** | pkg-config, wrappers, launcher (F11) | low | Headers are clean under C99/C11 pedantic and C++17 (*measured*); static build broken (F10). |

### Should PMIx be used?

*Documented:* Debian 13's Slurm (24.11.5) build-depends on `libpmix-dev` and ships `libpmi2-0t64`, so
`srun --mpi=pmix` and `--mpi=pmi2` are very probably available on Raspberry Pi OS Trixie; which plugin
package carries them was not verified.

**Not by default, and the design's reason stands** — a link dependency on `libpmix2` is exactly what §1
exists to avoid, and the environment gives rank, size, locality and the node list for free. What PMIx would
buy: no port hashing and no hostname resolution (F6), a fence that is a barrier, het jobs and non-Slurm
launchers (`prterun`) for nothing, and — the one that matters at the second scale tier — a **scalable
exchange**. §6.2's server receives one connection per node and broadcasts the rank→node map to each: at
500 × 256 that is 128,000 × 4 B = 512 KB to 499 leads ≈ 256 MB, about 2 s on 1 GbE and fine; at
4,096 × 256 it is 4 MB to 4,095 leads ≈ 16 GB through one NIC — over two minutes on 1 GbE against a 60 s
default timeout, and ~1.3 s on 100 GbE. So for the Ethernet tier the home-grown rendezvous is adequate, and
for the RoCE tier the sites in question all have PMIx. Keep §6.3's thin interface, make the optional
backend PMIx rather than PMI-2 (PMI-2 is the one being retired), load it with `dlopen` so the default build
still links nothing, and — cheaper than either — send the map as **ranges**: under block distribution a
node's GTIDS is one interval, so the broadcast is 8 bytes per node, not 4 per PE. The per-PE
`local_of_rank` array (`node.c:391`, `world_size × 4` bytes *per process*: 512 KB at 128k PEs, 4 MB at 1M,
× 256 PEs per node = 1 GiB per node) has the same cure and is the one bootstrap structure that scales with
PE count per PE; the scale reviewer owns the rest of that arithmetic.

---

## First day on the Pi cluster — ordered by likelihood of biting

1. **(certain) It will not start under `srun`.** `bootstrap.c:50` refuses. Single-node runs only, via
   `osoe-launch` from the build tree (it is not installed) or a wrapper that maps `SLURM_PROCID`,
   `SLURM_STEP_NUM_TASKS`, `SLURM_NODEID`, `SLURM_STEP_NUM_NODES`, `SLURM_LOCALID`, `SLURM_GTIDS`,
   `SLURM_JOB_ID`, `SLURM_STEP_ID` onto `OSOE_*` with `--nodes=1`. Multi-node is impossible until step 3.
2. **(very likely) Wrong OS generation.** Check `cat /etc/os-release`, `uname -r` (need ≥ 6.7),
   `pkg-config --modversion liburing` (need ≥ 2.6). Bookworm fails at configure; a Bookworm userland with a
   newer kernel is the case §1 declares unsupported.
3. **(do first on a good OS) `getconf PAGESIZE` and the probes.** Expect 16384. Run `tools/probe` and
   `probe-extra` from this review's evidence directory. Then `cmake --workflow --preset debug`, `release`,
   `asan`; expect 131/131 as in the 16 KiB VM.
4. **(certain) The tsan preset fails at start-up** with "unsupported VMA range … Found 47" (F14). Do not
   debug it. To run tsan on the hardware, boot `kernel=kernel8.img`.
5. **(likely with 4 cores) Timing-fragile tests.** Four A76 cores, four PEs, an agent thread each, and a
   polling progress thread on the lead: the what-changed note already expects more of these. Run the suite
   under `stress-ng --cpu 8` before believing a pass.
6. **(likely) `/dev/shm` against heap size.** `df -h /dev/shm`; `L × SHMEM_SYMMETRIC_SIZE` plus the
   connection areas must fit, or the first touch is `SIGBUS`, not an error.
7. **(likely on a shared cluster) `RemoveIPC` and private `/dev/shm`.** `grep RemoveIPC
   /etc/systemd/logind.conf`; `scontrol show config | grep -i JobContainer`. Start a job that sleeps, `ssh`
   to its node and log out, and see whether `/dev/shm/osoe.*` survives and whether finalize still exits 0
   (F7). Check `osoe-stat --list` from a login shell sees the job at all.
8. **(when the Slurm path is written) Record the environment as fixtures.** `srun -N2 -n4 env | grep
   ^SLURM_`, the same with `--distribution=cyclic`, `plane=2`, and `sbatch -N4 --wrap 'srun -N2 -r2 env'`
   to settle `SLURM_NODEID` step-relativity (F6); `scontrol show hostnames "$SLURM_JOB_NODELIST"` for
   hostlist fixtures; a bare `sbatch` without `srun` to see `SLURM_PROCID=0` set and
   `SLURM_STEP_NUM_TASKS` absent.
9. **(step 3) Name resolution and firewall.** From every compute node, `getent hosts` the first host of the
   step's list; check the hashed port range is open between nodes.
10. **(operability) `srun -K1`** or `KillOnBadExit=1`, and an epilog `rm -f /dev/shm/osoe.$SLURM_JOB_ID.*`
    (F8 — `--gc` alone leaves the connection areas).
11. **(performance, later) Build with `-mcpu=cortex-a76`** and compare against the default outline
    atomics; `OSOE_AM_HUGEPAGES` does nothing (F9). Run `unaligned-atomic.c` to settle F13 on the A76.

---

## What is sound, and what convinced me

- **Page-size independence.** Four presets on a 16 KiB kernel, three clean and the fourth failing for a
  reason outside the project. The code asks `sysconf` once and rounds; section alignment is by cache line,
  not page. I expected findings here and have none.
- **Choosing `B`.** A short candidate list tried with `MAP_FIXED_NOREPLACE`, a fallback for kernels that
  ignore the flag, the lead publishing its choice, and a failure message that names `OSOE_HEAP_BASE`. It
  adapted to a 47-bit address space nobody had listed, which is the argument §4.1 makes for it.
- **Offsets on the wire, never addresses** (§4.2), with a receive-side bounds check on the heap that says
  what disagreement it implies (`progress-host.c:49-67`). This is what makes heterogeneous page sizes,
  ASLR and different `B` per node non-issues.
- **§6.1's reading of Slurm** — placement is not in the environment; build the map from `SLURM_GTIDS`; use
  `SLURM_STEP_*` throughout — matches the documentation everywhere I could check it, and the logical node
  map means the non-contiguous case is already tested daily. "No singleton fallback" is the right call and
  the documented batch-script variables (F6) show why.
- **Object naming and permissions.** `job.step.node.session` in every name, `O_EXCL` creation with a
  message for a stale name, mode 0600 on all six objects (*measured*), heap names unlinked once mapped, and
  a launcher that fails a clean job that leaks. A local attacker who pre-creates a predictable name gets a
  denial of service with a clear message, not a hijack, because every name is created `O_EXCL` by a lead on
  the same node before anyone opens it.
- **The public headers.** Clean under `-std=c11 -pedantic -Wall -Wextra`, under C99 via `<mpp/shmem.h>`,
  and as C++17; hidden visibility with an export test; the `pshmem_` strong / `shmem_` weak pairing is the
  arrangement that makes static linking and interposition both work.
- **Dependency discipline.** libc and pthreads only at link time today; clang and musl both accept the
  source unmodified. The project's founding claim — builds from the distribution's packages — holds.

## Lessons for a successor API

- **Launch is part of the interface whether the specification admits it or not.** OpenSHMEM specifies
  `shmem_init()` with no arguments and says nothing about how a PE learns its rank, so every implementation
  invents a bootstrap, every user learns `oshrun`, and portability across launchers is folklore. A successor
  should specify a minimal *environment contract* (rank, size, node index, local index, local rank list, job
  identity, optional secret) that any launcher can satisfy with `env`, and let `init` take an optional
  struct for embedders. §6.2's `OSOE_` table is already most of that contract.
- **`SHMEM_SYMMETRIC_SIZE` as one number per PE, fixed before init,** forces the implementation to reserve
  `L ×` that in shared memory it cannot validate against `/dev/shm`, huge-page availability or page size
  until first touch. A successor should let the heap be created (and grown) by a call that can *fail*.
- **Same-address symmetric memory** (needed only for `shmem_ptr` arithmetic and symmetric statics) is what
  drags in fixed-address mapping, sanitizer address maps, VA-width candidates and `-no-pie`. Handle-plus-
  offset addressing, which §4.2 already uses on the wire, would delete all of §4.1's "Choosing `B`".
- **One-sided semantics leave no room for a trust boundary between PEs**, so the only boundary available is
  job membership. A successor should say so in its model section, and should make job identity (and an
  optional credential) a first-class bootstrap input rather than something each implementation hashes out
  of a scheduler's job id.
- **Compiler wrappers (`oshcc`) exist because the specification never defined how to find the library.** A
  successor should bless `pkg-config` and stop there.

## Not investigated

- Nothing was run on a Raspberry Pi, a second machine, under Slurm, on x86-64, or over a socket. The
  16 KiB VM is Apple silicon under Virtualization.framework with Debian's kernel, not the Pi's.
- `probe-extra` and `unaligned-atomic` were not run on the 16 KiB VM; `OSOE_AM_HUGEPAGES` was not tried on
  the 4 KiB VM (the `EINVAL` cause is recalled, not proven).
- The 16 KiB and clang suite runs are single runs per preset, under contention; no load testing.
- musl was syntax-checked only; no musl liburing, link or run. No armhf or big-endian cross-build.
- Whether different `B` on different nodes is exercised by a test (§8.2's heap-base-shift mode) — not checked.
- Receive-side validation of `get_request.length`, `barrier.from_node` and reply tokens beyond what F1
  lists — read, not mutated.
- Slurm facts not confirmed against a primary page this session: `SLURM_NODEID` step-relativity, hostlist
  multi-bracket syntax (secondary source), `--overlap` semantics, `RemoveIPC`, `job_container/tmpfs`,
  `NodeName` vs `NodeAddr`. Web access worked; these were simply not found stated plainly, or not fetched.
- Whether the Pi 5 kernel enables hugetlbfs/THP, and the Pi 4 kernel's VA width.

## Evidence files

`docs/reviews/evidence/phase1-step2-slice1/portability/`: `page-size-16k-vm.yaml`,
`page-size-16k-results.txt`, `probe-extra.c`/`.out`, `deny-futex2.c`, `forged-get-reply.patch`,
`unaligned-atomic.c`, `stale-segments.sh`/`.out`, `third-party-hello.c`.

Housekeeping: the throwaway Lima instance **`osoe-16k`** (2 CPUs, 3 GiB, 12 GiB disk) is stopped, not
deleted, in `~/.lima/osoe-16k`; it mounts only `build/scratch/review-portability`. Remove it with
`limactl delete osoe-16k`, or keep it — a 16 KiB-page tier costs one `limactl start`. clang and musl-tools
were installed in the shared `osoe` VM.

## Sources

- slurm.schedmd.com: `srun.html`, `sbatch.html` (output environment variables, `--distribution`,
  `--resv-ports`), `dist_plane.html`, `heterogeneous_jobs.html` — Slurm 24.11.
- packages.debian.org/trixie: `slurm-wlm` 24.11.5-4+deb13u1 (Build-Depends `libpmix-dev`), `libpmi2-0t64`,
  `liburing2` 2.9-1, glibc 2.41.
- raspberrypi.com config.txt documentation (`kernel_2712.img`, 16K page size);
  github.com/raspberrypi/linux `rpi-6.12.y` `arch/arm64/configs/bcm2712_defconfig`
  (`ARM64_16K_PAGES`, `ARM64_VA_BITS_47`); docs.kernel.org arm64 `hugetlbpage` (16K → 32M).
- glibc 2.39 release announcement (`<stdbit.h>`); musl release notes through 1.2.6 (no `<stdbit.h>`);
  liburing CHANGELOG (futex support in 2.6); man7.org history sections for `futex_waitv` (5.16), futex2
  and `IORING_OP_FUTEX_*` (6.7), multishot recv (6.0), `PBUF_RING` (5.19), `MAP_FIXED_NOREPLACE` (4.17),
  `pidfd_open` (5.3), `memfd_create` (3.17). All fetched by a Sonnet subagent this session and treated as
  its report, spot-consistent with the design's own figures.
