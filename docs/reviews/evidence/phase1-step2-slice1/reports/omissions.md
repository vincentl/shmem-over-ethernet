# Omissions red team — phase 1, step 2, slice 1

Scope. I read spec §3–§8, §9.1–§9.3 and §10 end to end, and grepped the rest of
the spec for the routines named below. I then looked for requirements and
operational facts that no code, no test and no design-doc section addresses.
All experiments ran in a private copy (`build/scratch/review-omissions/tree`,
debug preset) inside the shared Lima VM (Debian 13, 6.12, aarch64, 4 cores).
Programs and run scripts are in
`docs/reviews/evidence/phase1-step2-slice1/omissions/`.

Evidence labels: **[documented]** spec or design text; **[source]** observed in
source; **[measured]** a VM experiment of mine (all timings measured under
contention); **[recalled]** from memory, unverified. Web access was not used, so
every claim about Slurm or kernel behaviour that I did not measure is
**[recalled]**.

Items the lead says siblings already reported are cited in one line in §5 and
are not re-derived here.

---

## 0. Ranked summary

| # | Omission | Group | Severity | Confidence |
|---|---|---|---|---|
| 1 | A non-lead PE opens the control segment **by name without checking who owns it**; another local user can pre-create it. Denial of service is measured. A hijack is argued from source | security | design change needed (small) | high for the denial of service, medium for the hijack |
| 2 | **No signal story at all**: `scancel` (SIGTERM to every PE) leaves every named object behind. The design doc never mentions SIGTERM, SIGINT, SIGPIPE or `scancel` | operational | design change needed | high |
| 3 | A PE that leaves with **`exit(0)` without finalize** hangs the rest of the job in a release build. `srun -K1`, the documented production answer, fires only on a *nonzero* exit | operational | design change needed | high (hang measured), medium (`-K` semantics recalled) |
| 4 | **Core files are L × heap size per PE**, written non-sparse. Measured 513 MiB on disk for a 2-PE job with 64 MiB touched per PE | operational | clarification needed | high |
| 5 | When `/dev/shm` runs out, the victim is **the node lead's agent**, applying someone else's put. It dies of SIGBUS with no message | operational | design change needed (extends a sibling's finding) | high |
| 6 | `osoe-launch` neither forwards signals nor kills its children when it dies; SIGTERM to the launcher **orphans running PEs** | operational / test infrastructure | clarification needed | high |
| 7 | Diagnosis "from outside the job" (§3.14) assumes `osoe-stat` can see the job's `/dev/shm`. Under Slurm's `job_container/tmpfs` plugin it cannot | operational | clarification needed | medium (recalled) |
| 8 | CPU placement under Slurm binding: 4 PEs + 4 agents + the lead's internode progress on 4 cores. There is still no launch requirement and no start-up check | operational | clarification needed | medium |
| 9 | "Survives heterogeneous nodes" (§4.2) is still undefined. The job rendezvous exchanges no ABI word | spec / operational | clarification needed | high |
| 10 | The spec's own generic RMA names (`shmem_put`, `shmem_g`) are absent, so Examples 5 and 7 do not compile. The header comment does not tell a user which generics exist | spec | clarification needed | high |
| 11 | `SHMEM_SYMMETRIC_SIZE=0` is accepted, giving a job in which every `shmem_malloc` fails. `1e3` is rejected although the spec says "floating point value" | spec | minor | high |
| 12 | A fork child keeps every sibling heap mapped and writable. Only `shmem_finalize` is guarded, not the other routines | operational | minor | high |
| 13 | Step 3 has no address-family or interface-selection rule (IPv6-only clusters, `127.0.1.1` hostnames, `wlan0` versus `eth0`) and no SIGPIPE rule | operational (step 3) | clarification needed | medium |
| 14 | RoCE tier: nothing mentions `RLIMIT_MEMLOCK`, which is the classic Slurm + verbs start-up failure | operational (goal 4) | clarification needed | medium (recalled) |
| 15 | No licence, so nothing can legally be reused — including the successor-API lessons this review is meant to feed | process | clarification needed | high |
| 16 | No version identity: `0.0.0`, soname `.so.0`, no build id in `SHMEM_VERSION` output, no ABI policy, no cross-node library-version check beyond the handler checksum | process | clarification needed | high |
| 17 | The installed tree cannot start a job: no launcher is installed and Slurm launch is refused. There is no `pkg-config` file and no compiler wrapper | process | clarification needed (expected at this step) | high |
| 18 | The four-preset gate is manual and Linux-only; nothing records that it ran. §10 "Thorough 1.6 spec read" is still listed as *open — blocking* | process | minor | high |
| 19 | `dlopen`/`dlclose` of `libosoe` with a registered `atexit`; spec §3 "must not be defined in a DSO" | spec | minor | low |

What I found sound is in §6.

---

## 1. Spec requirements unaddressed

### 10 — The generic RMA names the spec's examples use do not exist

- **Location:** spec §9.6.1.1 and §9.6.1.6 (C11 `shmem_put`, `shmem_g`), Examples 5 and 7; `shmem/include/shmem.h:440-667`.
- **Failure scenario:** `generic-spec-examples.c` is the spec's Example 5 and 7 call lines. It fails with `implicit declaration of function 'shmem_put'` and `'shmem_g'` **[measured]**.
- **Evidence:**
  - Design §7.5 defers the typed RMA cross product **[documented]**, so the generics over it cannot exist yet.
  - Nothing tells a *user* which generics are present. The header comment says only "the header grows with the implementation".
  - `SHMEM_INFO` lists environment variables, not the supported surface.
- **Severity:** clarification needed. The typed put/get family is mechanical (§7.5) and it is what almost every OpenSHMEM program's first line of communication uses. A sibling counts 7 of 53 examples running; this is the second reason after statics.
- **Confidence:** high.

**What is sound here.** Every generic that *is* defined is correct for every type in the spec's tables.

- `generic-all-types.c` instantiates all 14 AMO generics and all 14 wait/test generics, with and without a leading context.
- It covers every type of Tables 6, 7, 8 and 12, including `const T *` for `shmem_atomic_fetch`.
- It compiles clean under `gcc -std=c11 -pedantic -Wall -Wextra -Werror` and the same with clang **[measured]**.
- The arity-counting trick works and needs no GNU extension.
- The choice of no `default:` association is right: a wrong type is a compile error.

### 11 — `SHMEM_SYMMETRIC_SIZE` edge cases

- **Location:** spec §8; `shmem/src/bootstrap.c:103-107`, `base/size.c`.
- **Evidence [measured]** (`run` section "env grammar" in my session; program `env-size.c`):

  | Value | Result |
  |---|---|
  | `3.1M` | 3250586 (the spec's own example, exact) |
  | `.5m` | 524288 |
  | `20kk` | 20480 |
  | `1.5` | 2 (ceiling, correct) |
  | `-5`, `abc`, empty, `0x10`, `1 g` | fatal with a good message |
  | `0` | **accepted**; heap 0 bytes, job starts |
  | `1e3` | **rejected** |

- **Failure scenario:**
  - `SHMEM_SYMMETRIC_SIZE=0` is usually a script's unset-variable arithmetic gone wrong. Every `shmem_malloc` then returns NULL on every PE, and the first dereference is a segfault far from the cause.
  - `1e3` is a "floating point value" by any C reading (`strtod` accepts it). The spec's grammar is loose enough that rejecting it is defensible, but nothing records that choice.
- **Severity:** minor. **Confidence:** high.

### 9 — Heterogeneity is claimed and never defined

- **Location:** design §4.2 line 1484 ("survives ASLR, PIE, heterogeneous nodes") **[documented]**; `shmem/src/job.c:38-43, 193-204` **[source]**.
- **Evidence:**
  - The job-wide rendezvous publishes and checks exactly two facts: `symmetric_size` and the handler-table checksum.
  - A grep for byte order finds no endianness word, no `sizeof(long)`, and no `long double` representation in `shmem/src`, `base/` or the design doc. The only hits are two "little-endian" comments on magic numbers.
  - Wire and segment headers are native byte order.
- **Failure scenario:** in step 3, take a mixed job (x86-64 login node plus aarch64 Pis, or any future big-endian peer).
  - The handler checksum passes, because both sides run the same source.
  - Every length field is then misparsed.
  - §3.4's redundant fields would probably catch it as "malformed segment", which is the wrong diagnosis.
- **One thing fixed since round one [source]:** page size no longer leaks into symmetry.
  - `node.c:388-389` keeps `heap_size` (what the allocator sees) equal to the parsed value.
  - Only the *mapping* is rounded to `sysconf(_SC_PAGESIZE)`.
  - So a Pi 4 (4K) plus Pi 5 (16K) job has identical allocator state.
- **Severity:** clarification needed. Either delete the word "heterogeneous" or add one ABI word to the rendezvous. **Confidence:** high.

### 19 — `dlopen` of the library

- **Location:** spec §3 ("must not be defined in a dynamic shared object") **[documented]**; `shmem/src/deprecated.c:47` (`atexit`) **[source]**.
- **Scenario:** a Python or Julia binding `dlopen`s `libosoe`.
  - glibc runs a DSO's `atexit` registrations at `dlclose` **[recalled]**.
  - If that holds, `dlclose` after `start_pes` would run a collective finalize at an arbitrary moment.
- **Evidence:** not tested.
- **Severity:** minor. **Confidence:** low; a ten-line experiment would settle it.

---

## 2. Operational realities unaddressed

### 1 — Shared-memory names are predictable and the openers trust the name

- **Location:** `shmem/src/node.c:201` (create, `O_EXCL`, 0600), `node.c:304-307` (non-lead open: `shm_open(name, O_RDWR, 0)`, then `fstat` checked for **size only**). The same pattern is at `wire.c:198-204`, `job.c:121-124`, `connection-area.c:209-212` **[source]**. Design §3.14 "Mode 0600" and §6.2 **[documented]**.
- **What is sound:**
  - Every *creator* uses `O_CREAT|O_EXCL` with mode 0600.
  - An existing name is refused, never reused, so a second user cannot read a job's data through the front door.
- **Failure scenario A, denial of service [measured]** (`run-core-and-squat.sh`):
  1. User `nobody` runs `: > /dev/shm/osoe.424242.0.0.0`.
  2. The victim's `shmem_init` dies with "already exists, probably left by a crashed job … remove it and retry".
  3. The victim cannot remove it: `/dev/shm` is sticky and the file is not theirs.
  4. Slurm job ids are sequential and public (`squeue`) **[recalled]**. A shell loop on any shared node therefore blocks every future osoe job there, and the message sends the user looking for a crash that never happened.
- **Failure scenario B, hijack [argued from source, not demonstrated]:**
  1. A non-lead PE polls until the name exists, then waits on `ready` and checks magic, layout and job description.
  2. An attacker who creates the name first, mode 0666, with a plausible header and `ready = 1`, gets non-lead PEs to map *the attacker's* segment.
  3. The lead's `O_EXCL` fails and the lead exits, but it cannot publish `failed` into a segment it never opened.
  4. From there the attacker controls `heap_base` and the sibling heap names the victims map.
  5. Victim puts land in attacker-readable memory, and victim-process memory corruption follows.
- **Fix:** after every `shm_open` without `O_CREAT`, call `fstat` and require `st_uid == geteuid()` and `(st_mode & 077) == 0`. That is four call sites. Reword the "already exists" message to name the owner.
- **Severity:** design change needed (small). The trust boundary for *local* users is stated nowhere; §3.14's "only the job's user can read it" is true of objects the job created and says nothing of objects it opens.
- **Confidence:** high for A. Medium for B; an end-to-end demonstration would raise it.

### 2 — Signals: nothing is designed, and `scancel` leaks

- **Location:** absent.
  - The design doc has 0 hits for `scancel`, `SIGTERM`, `SIGINT`, `SIGPIPE`.
  - In source, the only signal code in the library is the agent's mask (`agent.c:91-98`).
- **Failure scenario [measured]** (`run-signal-experiments.sh`, case B):
  1. SIGTERM goes to all four PEs of a two-node job, which is what `scancel` delivers to a step **[recalled]**.
  2. All die at once by default disposition.
  3. **All six named objects remain**: 2 control, 2 connection areas, the job segment and the wire.
  4. Cleanup is then left to an epilog nobody has written, or to `osoe-stat --gc`, which a sibling shows removes only the control segments.
- **Arithmetic for a Pi:**
  - The connection area's default budget is 256 MiB per node (`connection-area.h:62`).
  - Tmpfs pages persist after the processes die.
  - On a 4 GiB Pi 5, `/dev/shm` defaults to 50% of RAM = 2 GiB **[recalled]**.
  - A user who cancels eight busy jobs has therefore filled it, and the ninth dies of SIGBUS (finding 5).
  - Under Slurm the job id never recurs, so a leak is the failure mode there, not the sibling's `File exists`.
- **Also unaddressed:**
  - Whether the library may install handlers at all. An application's own SIGTERM checkpoint handler must keep working.
  - SIGPIPE in step 3. The agent blocks it, but §3.5 makes PE threads writers; whether a PE thread ever issues a `send` is a step-3 decision that needs `MSG_NOSIGNAL` or a stated rule.
- **Suggested shape:** no handlers in the library. Instead:
  - a complete `osoe-stat --gc`;
  - a shipped epilog one-liner;
  - optionally, unlink the connection-area, wire and job names once all parties have mapped them, as heaps already are. Only the control segment needs a name for `osoe-stat`.
- **Severity:** design change needed. **Confidence:** high.

### 3 — `exit(0)` without finalize hangs a release job, and `-K1` does not save it

- **Location:** design §3.15 and the watchdog message ("in production the launcher tears the job down instead (srun -K1)") **[documented]**; `watchdog.c` **[source]**.
- **Failure scenario [measured]** (case C):
  1. PE 3 of 4 calls `exit(0)` after a barrier, as an early "nothing to do" return does.
  2. With `OSOE_WATCHDOG=0`, the release default, the other three sit in `shmem_finalize`'s barrier until the launcher's timeout kills them.
  3. With the watchdog on, siblings abort promptly (case C2).
- **Why `-K1` does not help:** `--kill-on-bad-exit` acts when a task exits with a **nonzero** code or a signal **[recalled]**. `exit(0)` is neither, so on the cluster this job burns its whole wall-time limit.
- **Deferred item:** step 4's pidfd liveness would cover it, so this is a deferral to confirm rather than a new mechanism. Two things are still missing:
  - the design's stated production answer is wrong for the zero-exit case;
  - a cheap partial fix is available now. `shmem_init` registers an `atexit` hook that, if the library is still initialized in the initializing pid, publishes the abort word and `_exit`s nonzero.
- **Severity:** design change needed. **Confidence:** high on the hang; medium on `-K` semantics until checked against `srun(1)`.

### 4 — Core dumps include every sibling heap, non-sparse

- **Location:** absent. There are 0 hits for `madvise`/`MADV_DONTDUMP` in `shmem/src`, and 0 for "core dump" in the design doc.
- **Evidence [measured]** (`core-size.c`, `run-core-and-squat.sh`):
  - Setup: 2 PEs, default 256 MiB heaps, each touching 64 MiB; PE 0 calls `abort()`.
  - Result: `core.<pid>` is **520 MiB apparent, 513 MiB on disk**, with `coredump_filter=0x33`.
  - The core is own heap + sibling heap + connection area; pages never touched are written as zeros.
- **Arithmetic:**
  - Core size ≈ L × `SHMEM_SYMMETRIC_SIZE` per crashing PE.
  - Pi, L = 4, 1 GiB heaps: 4 GiB per core. A collective failure crashes all four, giving 16 GiB onto an SD card or NFS.
  - 256 PEs × 1 GiB: 256 GiB per core, × 256.
  - With systemd-coredump the write is also compressed on the crashing node's CPUs **[recalled]**.
- **Fix:** `MADV_DONTDUMP` on sibling heap mappings and on the connection area and wires. A PE's own heap is arguably wanted. The control segment stays, since it is small and is the post-mortem.
- **Severity:** clarification needed. **Confidence:** high.

### 5 — `/dev/shm` exhaustion kills the wrong process, silently

- **Location:** `node.c:120` (`ftruncate`, sparse), `agent.c:91` (SIGBUS deliberately left unblocked) **[source]**. A sibling reports the missing capacity check; this adds *who dies and how*.
- **Evidence [measured]** (`heap-overcommit.c`, `run-small-dev-shm.sh`):
  1. A private mount namespace has a 64 MiB `/dev/shm`. Three PEs on map `0,1,1`, `SHMEM_SYMMETRIC_SIZE=48m`.
  2. `shmem_init` succeeds and `shmem_malloc(40 MiB)` succeeds on all three.
  3. PE 0's `putmem` to PE 2 succeeds.
  4. During the put to PE 1, **rank 1 dies of signal 7**, with no osoe message and no control-segment pointer.
  5. Rank 1 is node 1's lead, whose agent applies internode puts for the node. The PE that over-allocated and the PE that issued the put are both fine.
- **Pi arithmetic:**
  - 4 GiB Pi 5: `/dev/shm` = 2 GiB.
  - Default 4 × 256 MiB heaps + 256 MiB connection budget = 1.25 GiB, which fits.
  - `SHMEM_SYMMETRIC_SIZE=512m` does not: 2 GiB + 256 MiB.
- **Under Slurm `ConstrainRAMSpace`:**
  - Tmpfs pages are charged to the cgroup of the first toucher **[recalled]**.
  - The first toucher of a sibling's heap page is often the lead's agent, so the memcg OOM victim is again the lead.
- **Fix options:**
  - `posix_fallocate` the heap at init. That costs commitment and gives an honest `ENOSPC` with a message.
  - Or a `statvfs` check plus a SIGBUS handler that at least prints the segment name.
- **Severity:** design change needed. **Confidence:** high for tmpfs; low for the memcg attribution.

### 6 — `osoe-launch` orphans its PEs

- **Location:** `tools/launch/launch.c:259-341` **[source]**: no handler for SIGTERM, SIGINT or SIGHUP, no `PR_SET_PDEATHSIG`, no process group.
- **Evidence [measured]** (case A):
  - SIGTERM goes to the launcher mid-job.
  - Three seconds later all 4 PEs are still running.
  - They finish their 30 s and clean up on their own.
- **Why it matters here:**
  - The project's three flakes were load-induced, and "the full suite was never the cause, only the thing that happened to be busy".
  - An interrupted test run (Ctrl-C on `ctest`, a killed agent session) leaves PEs spinning for up to their own runtime.
  - A hung one stays for ever, because the 60 s launcher timeout died with the launcher.
  - That is unaccounted load on the next run.
  - It also skips `remove_leftovers`.
- **Severity:** clarification needed (test infrastructure, not the library). **Confidence:** high.

### 7 — `osoe-stat` and a private `/dev/shm`

- **Location:** design §3.14 ("an external tool reads it … without the job's cooperation") **[documented]**.
- **Scenario:**
  - Slurm's `job_container/tmpfs` plugin gives each job a private `/dev/shm` and `/tmp` in its own mount namespace **[recalled]**.
  - It fixes findings 1 and 2 for free, because the mount vanishes with the job.
  - It breaks §3.14: an operator's `ssh node; osoe-stat` sees an empty `/dev/shm`.
  - They must enter the job's namespace, with `srun --overlap --jobid … osoe-stat` or `nsenter`.
- **Evidence:** nothing in the doc, the README or the `osoe-stat` usage text mentions it.
- **Severity:** clarification needed. A paragraph of operator documentation plus a test once a cluster exists. **Confidence:** medium.

### 8 — CPU placement on a 4-core node

- **Location:** design §3.12, lines 895-897 ("measure it rather than assume") **[documented]**. This is first-round O13, acknowledged but not resolved.
- **Evidence [measured, under contention]** (`run-idle-cpu.sh`): 4 PEs asleep in the application, 2 threads per PE.

  | Node map | CPU over 10 s | Reading |
  |---|---|---|
  | One node | **0 ticks** | Agents really do sleep. Sound |
  | Two nodes (`0,0,1,1`) | **55 ticks**, ≈ 5.5% of one core | The step-2 polling progress loop. Documented as temporary |

- **What is unaddressed:**
  - Under `--cpu-bind=cores`, the task/affinity default on many sites **[recalled]**, each task gets one core and its agent inherits the mask.
  - The lead's core then runs the lead's compute thread *and* all of the node's internode progress.
  - In a bulk-synchronous code every barrier waits for the slowest PE, so the job runs at lead speed on every node.
  - No launch recipe exists (`--cpus-per-task`, `--cpu-bind=none`, or 3 PEs + 1 spare core on a Pi).
  - `SHMEM_DEBUG` does not print the effective affinity mask, which would make the situation visible in one line.
- **Severity:** clarification needed; becomes measurable in step 3. **Confidence:** medium.

### 12 — What a fork child can still do

- **Location:** `init.c:187` (the pid check in `pshmem_finalize`); `shmem/tests/test-fork.c` **[source]**.
- **What `test-fork.c` promises:**
  - A child's `shmem_finalize` is a no-op and the control segment keeps its name.
  - The child leaves with `_exit`, so the `atexit` path is untested there.
- **Evidence [measured]** (`fork-in-pe.c`, cases D and D2):
  - `system()`, `popen()`, and `fork()` + `exit()` all work under both `shmem_init` and `start_pes`. The child's `atexit` finalize is correctly inert. Sound.
  - The child's store to the symmetric heap is visible to the parent (`heap after child store=8`).
- **What is unaddressed:**
  - The mappings are `MAP_SHARED` with no `MADV_DONTFORK`.
  - A daemonizing child pins every local heap and connection area after the job ends, and can scribble on a later reader's post-mortem.
  - Only `finalize` checks the pid. A child calling `shmem_putmem` writes to a reservation with the parent's identity.
- **Severity:** minor. `MADV_DONTFORK` on everything but the control segment would close it. **Confidence:** high.

### 13 — Step 3 addressing and SIGPIPE

- **Location:** design §6.2 "Global" **[documented]**.
- **Evidence:** the doc has 0 hits for IPv6, `getaddrinfo`, or interface selection.
- **Scenarios:**
  - Debian-family installs map the hostname to `127.0.1.1` in `/etc/hosts` **[recalled]**. A lead that resolves *its own* name to advertise an address publishes loopback.
  - A Pi has `wlan0` and `eth0`. The route chosen by name resolution may be the wireless one.
  - An IPv6-only cluster needs `AF_UNSPEC` throughout, and the port-candidate handshake needs to work per family.
- **Severity:** clarification needed before step 3's first socket. **Confidence:** medium.

### 14 — `RLIMIT_MEMLOCK` at the RoCE tier

- **Location:** absent. The doc has 0 hits for `memlock`/`ulimit`.
- **Background:**
  - The first round measured that io_uring rings do not need memlock on 6.12.
  - Verbs memory registration does.
  - Slurm propagates the submitting shell's `ulimit -l` unless configured otherwise **[recalled]**.
  - That is the most common first-day failure of any RDMA library under Slurm.
- **Relevance:** goal 4. Registering L × heap per node is exactly the quantity finding 5 computes.
- **Severity:** clarification needed; one sentence in §10's RoCE list. **Confidence:** medium.

---

## 3. Project and process gaps

### 15 — Licence

- **Location:** README "License"; design §10 "Still open — project" **[documented]**.
- **Evidence:**
  - "No rights are granted."
  - A grep for `Copyright`/`SPDX` in `shmem/`, `base/` and `tools/` finds nothing **[source]**.
  - The corpus is read and never copied (§8.6), which is sound practice and keeps the tree clean of foreign licences.
- **Consequence:**
  - Nothing here can be reused by anyone, including the design notes and the "lessons for a successor API" this review feeds.
  - A successor-API document derived from an unlicensed tree inherits the question.
  - Choose before step 3, the first code that may borrow socket idioms.
- **Severity:** clarification needed. **Confidence:** high.

### 16 — Version identity and ABI policy

- **Evidence:**
  - `CMakeLists.txt:4` sets `VERSION 0.0.0`; the installed soname is `libosoe.so.0` **[measured]**.
  - `SHMEM_VERSION` prints "osoe, OpenSHMEM 1.6", with no library version and no build id **[source, `init.c:98`]**.
  - Layout versions exist per object **[source]**:
    - `OSOE_NODE_LAYOUT 7`
    - `OSOE_JOB_LAYOUT`
    - `OSOE_CONNECTION_AREA_LAYOUT`
  - Each is checked on open. That is sound, and better than most projects at this stage.
- **What is missing:**
  - A statement of which of the three ABIs is promised to whom: link ABI to applications, shared-memory ABI between PEs of one job, and the segment ABI to `osoe-stat`.
  - A cross-*node* library-identity check. Two nodes running different builds with an unchanged handler table pass today. A changed segment format over the step-3 socket has no layout word to fail on unless the wire handshake carries one.
  - A build id in the `SHMEM_VERSION`/`SHMEM_DEBUG` line, so a bug report can say what ran.
- **Severity:** clarification needed. **Confidence:** high.

### 17 — What `cmake --install` produces

- **Evidence [measured]:**
  - The installed tree holds `bin/osoe-stat`, four headers, and `lib/libosoe.so{,.0,.0.0.0}`.
  - A program builds against it with `-I`/`-L`/`-losoe`.
  - There is no `osoe.pc`, no CMake package file, and no `oshcc`-style wrapper. Annex B's wrapper is the reference implementation's, not a requirement **[documented]**.
  - `osoe-launch` is deliberately "Not installed".
  - `bootstrap.c:50` refuses `SLURM_JOB_ID`.
- **Consequence:** an installed osoe can run a job only by hand-setting nine `OSOE_` variables per process. That is expected before step 3, but the README's "Running a program" should say so in its first line.
- **Link to statics:** the compiler wrapper is already planned with symmetric statics (§4.3). Until then a `pkg-config` file is ten lines.
- **Severity:** clarification needed. **Confidence:** high.

### 18 — The gate, the changelog, and the open "blocking" item

- **Evidence:**
  - `CMakePresets.json` conditions every preset on Linux, so there is no host build at all **[source]**.
  - CI is deferred (§10).
  - The gate is four manual workflows.
  - Tsan does not cover the multi-node path (what-changed note).
- **Actual regression risk:**
  - Nothing records *that* the gate ran for a given commit.
  - The load loop that found all three flakes is not part of it.
  - A `dev/gate.sh` would make the manual gate auditable without CI. It would run the four presets plus N iterations under `stress`, and write a dated log whose hash goes in the merge commit message.
- **Changelog:** there is none. The what-changed note and commit messages are better than most, so it is unnecessary until there is a user.
- **Open item:** design §10 lines 4242-4248 still list "Thorough 1.6 spec read" under *Still open — blocking*. The project has merged two steps past an item it calls blocking. Do the read (the conformance deliverable is most of it) or reclassify.
- **Severity:** minor. **Confidence:** high.

---

## 4. First-round omissions: status

| Round-one item | Status now | Evidence |
|---|---|---|
| O1 progress-thread host | **Fixed.** The lead's agent is the node's engine; every PE has an agent | §3.12 table; `init.c:126-131` [source] |
| O2 thread levels | **Fixed.** `init_thread`/`query_thread` exist, up to SERIALIZED; MULTIPLE designed in §4.11 | `init.c:214-241` [source] |
| O4 `global_exit`, abnormal exit | **Not fixed; deferred to step 4.** The development watchdog covers a dead local PE only. See finding 3 | `watchdog.c:22` [source]; case C [measured] |
| O5 per-PE heaps, watch key | **Fixed.** Named per-PE heap objects; own heap at a common base, siblings elsewhere | `node.c:113-170` [source] |
| O6 segment naming and cleanup | **Partly.** Names carry job.step.node.session; heaps are unlinked after mapping; hugetlb falls back. Cleanup after a kill is not fixed (finding 2) | [measured] |
| O7 statics | **Not fixed; now stated** as known non-conformance with a clear runtime message | §7.4 [documented]; message seen in `fatal-under-start-pes` run [measured] |
| O8 mandatory surface | **Mostly fixed.** `pshmem_` strong / `shmem_` weak; `shmemx.h`; `mpp/shmem.h`; `start_pes`; the environment variables; AMO and wait generics. The residue is in §5 | `preload-profiler.c` [measured] |
| O9 `SHMEM_TEAM_SHARED` | **Fixed.** `shmem_team_n_pes(SHMEM_TEAM_SHARED)` follows the node map: 3, 3, 1, 1, 2, 1 across the maps tried | `ring.c` [measured] |
| O10 finalize is a barrier | **Fixed** | `init.c:191-194` [source] |
| O11 fork | **Fixed for teardown.** The residue is finding 12 | cases D, D2 [measured] |
| O12 signals on the agent | **Fixed.** All asynchronous signals are blocked before `pthread_create` | `agent.c:91-98` [source] |
| O13 CPU binding | **Acknowledged, not resolved** (finding 8) | §3.12 [documented] |
| O14 heterogeneity | **Page size fixed; the rest open** (finding 9) | `node.c:388-389` [source] |
| O15 `RLIMIT_NOFILE` | **Documented (§6.2), no code yet.** There is no `setrlimit` in the tree. That is correct while there are no sockets; shm descriptors are closed after `mmap` | grep [source] |
| O16 `signal_fetch`, `signal_wait_until`, accessibility queries, `pe_quiet` signature | **Fixed** | `shmem.h:172-173, 374-377, 395-396` [source] |

How the O8 profiling check ran [measured]:

- `preload-profiler.c` is an `LD_PRELOAD` tool that defines `shmem_putmem`, `shmem_barrier_all` and `shmem_finalize`, and calls the `pshmem_` spellings.
- It intercepted a two-node run and counted **exactly the application's calls** (1 put, 2 barriers).
- It did not count the barriers inside `shmem_malloc`, `shmem_free` and `shmem_finalize`.
- The weak-alias arrangement therefore works for preloaded tools as well as linked ones, and the library's internal calls go to `pshmem_` as §7.4 says.

---

## 5. Already reported by siblings (not re-derived)

- **`osoe_fatal` → `exit()` → `start_pes`'s `atexit` finalize.** One additional observation from `run-fatal-under-start-pes.sh` **[measured]**:
  - The dead PE's implicit finalize *enters the barrier the survivors are waiting in*.
  - `shmem_barrier_all` therefore **returns on PEs 1 and 2 although PE 0 never called it**. The survivors hang one collective later.
  - That is a wrong answer before it is a hang, and it occurs with the watchdog on as well as off.
- `SMA_SYMMETRIC_SIZE` is ignored. Confirmed in passing: `SMA_SYMMETRIC_SIZE=1m` alone gives the 256 MiB default **[measured]**. `SMA_DEBUG` alone works.
- The 1.4-deprecated AMO names are missing.
- `<mpp/shmemx.h>` is missing.
- `shmem_pcontrol` is declared only in `pshmem.h`. Confirmed: `pcontrol-from-shmem-h.c` fails to compile **[measured]**.
- Calls before init segfault.
- Shm objects are never checked against `/dev/shm` capacity. Finding 5 adds the victim and the diagnosis.
- `osoe-stat --gc` leaves the connection, job and wire objects. Confirmed by `run-gc-experiment.sh`, which prints "is not an osoe control segment" for each **[measured]**.
- Data-path waits are bounded by `OSOE_BOOTSTRAP_TIMEOUT`.
- Only 7 of the spec's 53 examples run.

---

## 6. What is sound

- **Degenerate and uneven job shapes all work [measured]** (`ring.c`: put, get, AMO, two barriers, malloc/free, finalize). Shapes tried:
  - 1 PE;
  - 3 PEs on one node;
  - `0,0,0,1,1`;
  - `0,0,0,1` (L = 3 and 1);
  - one PE per node × 4;
  - `0,1,1,2,2,2,2` (P = 7, L = 1, 2, 4);
  - interleaved `1,0,1,0,2,2`;
  - 3 single-PE nodes.

  Every one exited 0 and left nothing in `/dev/shm`. Uneven L is genuinely supported by the node map, the barrier and `TEAM_SHARED`.
- **C++ callers [measured]:**
  - All four headers compile under `g++` and `clang++ -std=c++17 -Wall -Wextra -pedantic` with no diagnostics.
  - The predefined handles work in static initializers (spec §7).
  - A C++ PE links and runs.
  - The C11 generics are correctly invisible to C++ (spec: "C11"). A C++ use of one is a clean "not declared" error suggesting the typed name.
- **The `SHMEM_SYMMETRIC_SIZE` grammar** matches every example in spec §8 exactly, including the ceiling **[measured]**.
- **Idle cost is zero** on a single node: agents block rather than poll **[measured]**.
- **Clocks [source]:** `CLOCK_MONOTONIC` is used only for local deadlines and for `blocked_since`, which `osoe-stat` reads on the same host. I found no cross-node clock comparison.
- **Creation discipline [source]:** every named object is created `O_EXCL`, mode 0600. A stale name is refused rather than adopted.
- **Launcher cleanup [measured]:** in every failure case I produced, `osoe-launch` reported and removed leftovers when it survived. Its "clean job left something behind" check is a good test of the naming rules.
- **Fortran** is correctly absent (removed in 1.5).

---

## 7. Lessons for a successor API

Surface that exists for historical reasons and that every implementation must nevertheless carry:

1. **`start_pes` and implicit finalization.**
   - They force a collective into `atexit`.
   - That collides with any library that also owns fatal exit (§5, first bullet).
   - One deprecated routine dictates the whole process-exit design.
2. **No error returns.**
   - `shmem_init` returns `void`, and RMA and collectives return nothing.
   - Every failure must therefore be fatal, so the library must own process termination.
   - That is where findings 2, 3 and 5 and the `global_exit` deferral all come from.
   - A successor should return status from init and from collectives, and leave process exit to the application and launcher.
3. **`shmem_global_exit`** requires an out-of-band, any-state kill channel between all PEs.
   - A library without launcher cooperation cannot do that cleanly.
   - Define it as a launcher service or drop it.
4. **The type cross product.**
   - There are 526 routines, almost all `type × operation × {ctx, no ctx}` expansions.
   - C11 generics layered on top need arity-counting macros only because the context is an *optional leading* argument, and C++ gets nothing.
   - A successor takes `(ptr, width-or-datatype)` and an explicit context always. That is roughly 40 routines.
5. **The `pshmem_` name shift** doubles the symbol table to serve a 1990s link trick.
   - It cannot cover macros (the spec admits this in §10 item 3).
   - It is undefined for the unprefixed deprecated names; the design's recorded non-conformance.
   - A callback-based tools interface replaces all of it.
6. **Symmetric statics** need a compiler wrapper, `-no-pie`, and address agreement, and forbid DSOs.
   - Most spec examples depend on them, so an implementation without them fails 46 of 53 examples while being otherwise complete.
   - A successor should offer heap only, or explicit registration.
7. **Heap size by environment variable only**, fixed at start, per PE.
   - The memory commitment (finding 5) is decided outside the program, with no query and no error path.
   - A successor uses an init-time attribute struct with a failure return.
8. **World-collective `malloc`/`free` with a mandatory barrier.**
   - At a million PEs every allocation is a global synchronization.
   - Team-scoped allocation should be the primitive.
9. **Reference-counted init/finalize where a non-last finalize is a barrier**, plus finalize-on-the-init-thread: lifecycle rules that exist to let libraries nest, and cost every implementation an epoch-count subtlety (invariant 26).
10. **`SHMEM_TEAM_SHARED` defined by what `shmem_ptr` returns** couples team membership to a mapping policy, so an implementation cannot choose one without fixing the other.
11. **Dead weight:** `SMA_*`, `_SHMEM_*`, `mpp/`, the `pSync` constants, the six cache no-ops and `shmem_pcontrol`. Roughly a tenth of `shmem.h`.

---

## 8. Not investigated

- Anything under real Slurm. `scancel` signal delivery, `-K` semantics, `job_container/tmpfs`, memcg charging and the default CPU binding are all **[recalled]**.
- `dlopen`/`dlclose` (finding 19), and the Yama ptrace scopes that matter to §4.3 statics.
- Raspberry Pi OS kernel VA width versus the heap base candidates.
  - `node.c:40-43` tries 2^44, 3·2^44, 2^40 and 2^36.
  - Only the last fits a 39-bit address space, and it leaves 64 GiB below it.
  - I did not determine which VA width the Pi 5 16K-page kernel uses.
- Whether `shmem_finalize` from a thread other than the initializing one is detected (§9.2 rule 1).
- `shmem_malloc(0)` and the no-barrier rule.
- The end-to-end hijack of finding 1.
- The fd count at scale (no sockets exist yet).
- Stdio ordering through `srun`.
  - `osoe-launch` passes stdio through untouched.
  - Library diagnostics carry a `PE n:` prefix and are written with a single `write(2)`, which is as good as it can be without launcher help.
