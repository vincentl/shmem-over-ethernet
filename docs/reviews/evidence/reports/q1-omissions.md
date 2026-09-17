# Q1 — Omissions red team: OpenSHMEM 1.6 requirements vs. osoe design

Reviewer scope: I read spec §1–§8, §9.1–§9.5 in full, §9.6.1.1/§9.6.1.5/§9.6.2.2, §9.7 intro plus compare_swap and fetch_add, all of §9.8, the §9.9 intro, the §9.10 intro plus §9.10.1, the §9.11 intro plus wait_until, test, test_all and signal_wait_until, all of §9.12, §10, and Annexes A–D, F.1 and G.1–G.2. The spec text comes from `scratchpad/spec16.txt`, and line numbers below refer to that file. I read the design doc (`docs/design/shmem-ethernet-design.md`, 1712 lines) in full, plus `shmem/include/shmem.h` and `shmem/src/info.c`.

Evidence labels: **[spec Lnnnn]** means spec text I quoted from the extraction. **[doc Lnnn]** means a design doc line. **[observed]** means a VM experiment run on the Lima `shmem` VM (Debian 13, 6.12 aarch64). The code is in `scratchpad/exp/omissions/`. **[recalled]** means from memory and not verified.

---

## 0. Summary table

| # | Omission | Severity | Conf. |
|---|---|---|---|
| O1 | No decision on **which process hosts the progress thread**. Get replies, fetching-AMO results and `_nbi` gets must reach *private* origin memory in another process. Hosting also sets the lifecycle, signal and cpuset behaviour | **blocking** | high |
| O2 | **Thread level (§9.2) never chosen.** Per-PE state in §4.6 (`dirty[]`, `ndirty`, `high_seg`, `bytes_issued`) is unsynchronized. `shmem_init_thread`/`query_thread` are missing from phase 1 | **blocking** | high |
| O3 | §4.8 says fence/quiet do not order or complete stores through `shmem_ptr`. The spec says they **do**: Table 14, `SHMEM_CTX_NOSTORE`, and §9.10.1 on barrier_all. The §8.1 force-remote flag turns this into a real ordering bug | design change needed | high |
| O4 | **`shmem_global_exit` and abnormal PE exit across nodes** are unaddressed. "Abort loudly" (§3.15) has no way to reach other nodes, and blocked PEs hang | design change needed | high |
| O5 | The **symmetric heap is per PE** (§8, §9.2 item 2). The doc never says how L per-PE heaps sit in "the" node segment: sizing, VA layout, or the key the §4.9 watch table uses | design change needed | high |
| O6 | **How sibling PEs get the node segment** is not designed: memfd passing, naming, collisions between concurrent jobs, crash cleanup, hugetlb fallback | clarification needed | high |
| O7 | Symmetric **statics** (spec §3) are rejected in phase 1, so most spec examples cannot run. Non-conformance is not stated, and the phase-1.5 CMA writer is the progress thread, not a sibling PE | clarification needed | high |
| O8 | Mandatory surface missing from every phase list: profiling interface (`pshmem_*`, `pshmem.h`, `shmem_pcontrol`), `shmemx.h`, `start_pes` with implicit finalize, `_my_pe`/`shmalloc`…, `SMA_*`, `SHMEM_VERSION`/`INFO`/`DEBUG`, the `SHMEM_SYMMETRIC_SIZE` grammar, C11 generics, `mpp/shmem.h`. The profiling interface conflicts with the §10 "only spec API symbols exported" rule | clarification needed (profiling export policy: design change) | high |
| O9 | `SHMEM_TEAM_SHARED` is a **required predefined team**, not optional. Team queries on predefined teams and `shmem_ctx_create` semantics are also missing | clarification needed | high |
| O10 | §6.4 lifecycle: a non-last `shmem_finalize` must perform a **barrier_all**. It must be called on the init thread. The last one destroys teams and contexts | clarification needed | high |
| O11 | fork/exec after init. The child inherits shared maps and atexit teardown | clarification needed | medium |
| O12 | Signal masking on the progress thread; EINTR and deadline recomputation in PE waits | minor | medium |
| O13 | Slurm CPU binding vs. §3.11's "cpuset isolation for the progress thread" | clarification needed | medium |
| O14 | Heterogeneity: §4.2 claims support, but byte order, `long double`, **page size (Pi 4 4K vs Pi 5 16K)** and allocator determinism are unaddressed | clarification needed | medium |
| O15 | Memory accounting: heap × L commitment, Slurm memcg charging of shared pages, `ulimit -n` | minor | low–medium |
| O16 | Small spec items: `signal_fetch`/`signal_wait_until` missing, `addr_accessible`/`pe_accessible`, malloc size 0 skips the barrier (affects the debug checksum), `shmem_pe_quiet` signature | minor | high |
| C1–C6 | Wrong or imprecise spec citations in the design doc (§2 below) | minor (C1 clarification) | high |

What the doc handles well is in §4.

---

## 1. Omissions (detail)

### O1 — Progress-thread host process is unspecified (blocking)

**Location:** design §3.12, §4.6 "Data structures", §4.7 last paragraph, §3.3 (sole issuer / pbuf ring).

**What the doc says.** "A small fixed pool per node … Target-side progress is fully discharged by a thread that shares the coherence domain with local PEs" [doc L548–557]. "`bytes_landed` — progress thread adds as chunks land" [doc L785]. "Write replies **directly into the destination**" [doc L903]. It also says "PEs are processes" [doc L25]. Nowhere does it say *which process* the thread lives in: a designated PE, every PE, or a helper process forked by the first local PE.

**Why it matters (spec).**
- Blocking get: "The routines return after the data has been delivered to the dest array on the local PE", and `dest` is "Local address of the data object to be updated" [spec §9.6.1.5, L3812ff]. That is **private** memory of the origin PE.
- `_nbi` get: "At the completion of shmem_quiet, the data has been delivered to the dest array on the local PE" [spec §9.6.2.2].
- Fetching AMOs: "return after the data has been fetched from the target PE and delivered to the calling PE" [spec §9.7 intro].
- §4.1: "Other PEs should be able to communicate … with that computationally-bound PE without that PE issuing any explicit OpenSHMEM calls".

**Failure scenarios.**
1. The progress thread lives in local PE 0's process. PE 3 calls `shmem_getmem(buf_on_stack, …)`. The reply arrives in PE 0's address space (its sockets, its provided-buffer ring). The thread cannot write `buf_on_stack` in PE 3 without CMA (`process_vm_writev`, which is subject to Yama; see O7) or a shared staging area. "Write directly into the destination" is impossible, and §3.8's "no copy until send time" memory argument has no origin-side counterpart. An origin-side staging budget for 1 MB replies is not designed.
2. The same host, when PE 0 is stopped by a debugger (`gdb -p` stops all threads) or `SIGSTOP`. Every other PE on the node loses target-side progress and every remote node stalls on this node. The design's own §3.14 "`am_dump_state` when a 64-PE job hangs" workflow attaches a debugger, which causes this.
3. The same host, when PE 0 segfaults. Its sockets close, but PEs 1..L-1 on the node are alive with no transport and no detection path (see O4).
4. A helper process forked at init. srun did not launch it, so it needs its own death-of-parent handling (`PR_SET_PDEATHSIG`), its own cgroup/cpuset (O13), and a way to be re-created on §6.4 re-init.

**Resolution sketch (not prescriptive).** Pick the host explicitly. Then define the reply landing path: for example, provided buffers carved from the shared node segment, so the origin PE copies from them itself, or a per-PE shared reply area. Define who reaps a PE's `bytes_landed`, and make those counters shared, since they are written cross-process. Add this to §9 as an invariant.

Severity **blocking** (phase 1 includes `getmem` and fetching AMOs). Confidence high.

### O2 — Thread level not decided; per-PE state is unsynchronized (blocking)

**Spec.** "There are four levels of threading" [§9.2, L1855ff]. `shmem_init_thread` "returns 0 upon success; otherwise, it returns a nonzero value" [§9.2.1]. Further requirements:
- "In the FUNNELED, SERIALIZED, and MULTIPLE thread levels, the shmem_finalize call must be invoked by the same thread that called shmem_init_thread" (item 1).
- "Blocking OpenSHMEM calls will only block the calling thread … will not prevent them from executing other OpenSHMEM calls when the thread level permits" (item 3).
- "In the SHMEM_THREAD_MULTIPLE thread level, all OpenSHMEM calls are thread-safe" (item 4).

`shmem_query_thread` "may be called at any time". Neither routine appears in design §7, and grep finds no `THREAD_` in the doc.

**Design gap.** §3.5 says "Writers are PE threads", which is multi-writer safe. But §4.6 keeps `pe->dirty[]`, `pe->ndirty`, `pe->high_seg[]` and `bytes_issued` as plain per-PE fields, and `shmem_quiet` ends with `pe->ndirty = 0` [doc L804–810].

**Failure scenario (MULTIPLE).** Thread T1 is inside `shmem_quiet` after the wait loop. Thread T2 issues `putmem` to node 7, appends 7 to `dirty` and increments `ndirty`. T1 then executes `ndirty = 0`. T2's next `shmem_quiet` does not wait for node 7's ack, and a later get on node 7 observes stale data. The spec's §9.12.2 note makes quiet cover only "operations posted prior to calling shmem_quiet" by the calling thread, but losing T2's *later* operation from every future quiet is a real bug. `bytes_issued += n` from two threads is a data race as well. Separately, `shmem_barrier_all` must not be called concurrently by threads [§9.10.1], and the §5.4 private `k`/`d` counts are per PE, which is fine under that rule.

**Needed.** A decision on the provided level. SINGLE or FUNNELED is the cheapest in phase 1, and `shmem_init` may "choose to support any one of the defined thread levels" [§9.2.1 Notes]. If MULTIPLE is intended, the §4.6 structures need to be atomic or per-thread. Either way, `shmem_init_thread`/`shmem_query_thread` belong in phase 1, because the refcount of §6.4 is shared with them.

Severity **blocking** for the §4.6 data structures. Confidence high.

### O3 — Memory stores *are* ordered and completed by fence/quiet (design change needed)

**Design.** "Accesses through the returned pointer are ordinary loads and stores, not OpenSHMEM operations. `fence` and `quiet` do not order them. Completion routines do not apply." [doc L924–927]. Also "a local store via `shmem_ptr` and a remote put to the same location have no ordering relationship" [doc L931–933].

**Spec.**
- Table 14 lists "Memory Store" with X under both Fence and Quiet. Its footnote 9 says ordering and delivery of stores "are ensured only on contexts created with certain options" [L8882ff].
- `SHMEM_CTX_NOSTORE`: "Quiet and fence operations performed on the given context are not required to enforce completion and ordering of memory store operations" [§9.5, L3175 region], and "No options are enabled on the default context".
- §9.10.1: `shmem_barrier_all` "ensures completion of all previously issued memory stores and remote memory updates".
- §9.12.4: "All Put, AMO, store, and nonblocking Put routines on symmetric data issued to same PE are guaranteed to be delivered before Puts (to the same PE) issued after the fence call."

So on the default context, fence and quiet *must* cover stores.

**Concrete failure.** Take the §8.1 force-remote flag with a same-logical-node pair, while `shmem_ptr` still returns non-NULL. §8.1 only says it must be NULL *across* logical nodes.
```
shmem_putmem(x, a, 8, 1);   /* routed over TCP, still queued */
shmem_fence();              /* design: seal + submit only, no wait */
int *p = shmem_ptr(x, 1); *p = b;   /* store happens immediately */
/* put lands later: x == a on PE 1, violating fence (put issued before, must be delivered before) */
```
In normal mode, intranode put is a synchronous `memcpy`, so ordering holds for free. The fix is cheap:
- (a) make `shmem_ptr` return NULL whenever intranode traffic is force-routed remote, or
- (b) make fence a per-peer quiet for PEs reachable by pointer;
- and correct the §4.8 text.

The weak-memory remark in §4.8 also changes. Quiet on the default context should include whatever makes the calling thread's own stores visible. On Linux a futex or syscall inside quiet already is such a point [recalled]. The spec limits this to the calling thread [§9.12.2 Notes].

Severity design change needed (normal mode is conforming by accident; the documented rationale is wrong). Confidence high.

### O4 — `shmem_global_exit` and abnormal termination (design change needed)

**Spec §9.1.6.** "When any PE calls shmem_global_exit, it results in the immediate notification to all PEs to terminate … the exit status returned to the environment shall be one of the values passed … all PEs must terminate regardless of their current execution state." It is not in any design phase list, and grep finds no `global_exit`. §3.15 says only "abort the job loudly".

**Failure scenarios.**
1. The spec's Example 8 pattern: PE 0 calls `shmem_global_exit(EXIT_FAILURE)` while PEs 1..N-1 are blocked in `shmem_finalize`/`barrier_all` waiting on `released` futexes (§5.4). If the implementation just calls `exit()` on PE 0, remote nodes see a TCP EOF, but on PE 0's *own* node the other PEs share no socket with PE 0 and wait forever. That holds unless the progress thread is in PE 0, in which case they lose transport silently.
2. Any PE crashes (SIGSEGV). Under srun, remaining tasks are not killed unless `--kill-on-bad-exit` or the cluster default is set [recalled: `KillOnBadExit` defaults to 0], so the job hangs until its time limit.

**Needed.**
- A GLOBAL_EXIT control AM flooded to all nodes.
- A local PE table (pids) so the receiving side can terminate local PEs, or a shared "abort word" that every PE futex-waits on alongside its real wait. Every PE-side wait in §3.10/§4.9/§5.4 needs a way to be interrupted.
- A rule that connection EOF or error means global abort with a distinguishable status.
- A liveness check for local PEs, e.g. a pidfd per local PE polled by the progress thread through io_uring.

Exit-status propagation "one of the values passed" needs rank 0's launcher-visible exit code, or all PEs exiting with the same status.

Severity design change needed. Confidence high.

### O5 — The symmetric heap is per PE; node-segment layout undefined (design change needed)

**Spec.**
- `SHMEM_SYMMETRIC_SIZE` "Specifies the size (in bytes) of the symmetric heap memory **per PE**" [§8, L1186ff].
- "Each PE has a single symmetric data segment and symmetric heap" [§9.2 item 2].
- Symmetric addresses: "Symmetric addresses are only valid at the PE where they were generated" [§3.1].

**Design.** "Shared `memfd`/hugetlbfs segment, mapped by every local PE" [doc L622]. "Reserve a fixed virtual range with `MAP_FIXED_NOREPLACE`" [doc L641]. "Identical-VA mapping is pursued as well" [doc L651].

**Gaps.**
- Segment size is L × SIZE plus node structures. At L = 256 and SIZE = 1 GiB that is 256 GiB of *VA per process* if every PE maps every sibling heap, which `shmem_ptr` and intranode memcpy need. That is fine with 48- or 52-bit VA (the VM kernel is `CONFIG_ARM64_VA_BITS=52` [observed]). On a 39-bit-VA kernel (512 GiB total) it fails. I have not verified which VA size Raspberry Pi OS kernels use [recalled: some Pi defconfigs historically used 39-bit — unverified].
- "Identical VA" can only mean *my own* heap sits at the same VA in every PE. Siblings' heaps must be at other VAs, so intranode put is `memcpy(base[pe] + off, …)`, not "raw pointer arithmetic" on the symmetric address. The doc should say this.
- §4.9 `watch_find(node->watch, addr)`. The waiter's `ivar` is a VA in the waiter's process, and the progress thread (another process) or a sibling PE applies the AMO at a different VA. The key must be `(pe, offset)` or a canonical segment offset. As written, keying on `addr` loses every cross-process wake, which then surfaces only as timeout counters.
- Heap exhaustion: `shmem_malloc` must return NULL on all PEs identically. The deterministic allocator gives this for free (sound), but it is not stated.

Severity design change needed (the watch-table key is a correctness bug). Confidence high.

### O6 — Getting the node segment to sibling PEs; naming and cleanup (clarification needed)

**Design.** §6.2: "ranks, addresses, and the segment key are exchanged" over TCP to rank 0. §8.1: "Shared segment name keyed on logical node id". §4.1: memfd.

**Gaps and evidence.**
- A memfd has no name in any namespace. srun tasks are siblings, and file descriptors cannot travel over the §6.2 TCP rendezvous. The options are:
  - `/proc/<pid>/fd/<n>` open: works at Yama `ptrace_scope=0` [observed: `open(/proc/8197/fd/5): OK`, mapping read back the sibling's data]. The check is `PTRACE_MODE_READ_FSCREDS`, which Yama level 1 does not restrict [recalled].
  - `pidfd_getfd`: works at scope 0 [observed]. It needs `PTRACE_MODE_ATTACH_REALCREDS`, so it fails for siblings under Yama 1 [recalled].
  - `SCM_RIGHTS` over an AF_UNIX socket: needs a node-local rendezvous (abstract socket name keyed by job/step and node) that §6 does not have.
- If `shm_open` is used instead, the name must include `SLURM_JOB_ID.SLURM_STEP_ID`, or two jobs sharing a node (or two logical-node test runs in parallel CI) collide. A PE crash leaves `/dev/shm/*` files pinning RAM until someone unlinks them. That argues for unlinking immediately after all local PEs attach, which then conflicts with §6.4 re-init.
- hugetlbfs: the mount exists in the VM, but `HugePages_Total: 0` [observed], so `MAP_HUGETLB` fails without admin reservation. A fallback to normal pages (or THP via `MADV_HUGEPAGE` on shmem, which needs `shmem_enabled`) is unspecified.
- Correction to §4.3: "Yama `ptrace_scope` defaults to 1" does not hold on Debian 13 cloud, where it is `0` [observed]. Ubuntu defaults to 1 [recalled]. The statement should name the distro.

Severity clarification needed. Confidence high on the facts, medium on the recalled Yama semantics.

### O7 — Symmetric statics and conformance claim (clarification needed)

**Spec §3.** "Global and static C and C++ variables" are symmetric data objects. Spec examples in §9.1.1, §9.1.4, §9.1.9, §9.7.1.3, §9.7.1.7, §9.8.3, §9.12.1, §9.12.2 and Annex A all put, get or AMO on `static` variables. Annex C lets an implementation "detect a Put to a non-symmetric variable and choose to abort". §9.8.3 Notes: "sig_addr … could be a global/static C variable".

**Design** phase 1: "Unsupported; error on statics" [doc L669].

That is conforming only in the sense that the application would have UB if statics weren't symmetric, and they *are* symmetric by the spec. **Phase 1 cannot run the spec's own hello-put example** (Example 5). Needed:
- State the non-conformance explicitly.
- Define the detection: a range check on `dest` outside heap regions, then `shmem_global_exit`-style abort with a message.
- Make `shmem_addr_accessible` return 0 for statics.

Phase 1.5 CMA: the writer into a target PE's `.data` for a *remote* put is the progress thread (O1), not "PEs are siblings". Its Yama relationship depends on O1's host choice (a helper forked by a PE is that PE's descendant, which *is* permitted at Yama 1).

Severity clarification needed. Confidence high.

### O8 — Mandatory interface surface absent from all phase lists (clarification; profiling = design change)

| Requirement | Spec evidence | Design |
|---|---|---|
| `pshmem_` alternate entry point for **every** function, `pshmem.h`, no-op `shmem_pcontrol` | §10 items 1, 5, 6 [L9377ff] | absent |
| `shmemx.h` "must exist, even if no extensions are provided" | §5 | absent |
| `start_pes` "Implementations must include it"; implicit collective finalization at exit | §9.1.13 | absent |
| `_my_pe`, `_num_pes`, `shmalloc`, `shfree`, `shrealloc`, `shmemalign`, `shmem_wait`, `shmem_fadd`/`cswap`/… | F.1 table, "Last Version Supported: Current" | absent |
| `mpp/shmem.h` header directory | F.1 "Header Directory: mpp … Current" | absent |
| `SMA_VERSION`/`SMA_INFO`/`SMA_SYMMETRIC_SIZE`/`SMA_DEBUG`, with `SHMEM_` winning | §8, F.1 | absent |
| `SHMEM_VERSION` (print version at start-up), `SHMEM_INFO` (print help), `SHMEM_DEBUG` | §8 table | absent (§4.4 mentions only SYMMETRIC_SIZE) |
| `SHMEM_SYMMETRIC_SIZE` grammar: float with k/m/g/t suffix, "3.1M" → 3250586, ".5m", "20kk" ignored trailing, invalid → nonzero from `shmem_init_thread` or terminate | §8 | not specified |
| C11 `_Generic` interfaces for the phase-1 routines (`shmem_put`, `shmem_atomic_fetch_add`, `shmem_wait_until`, …) | §9.6, §9.7, §9.11 | absent |
| `shmem_global_exit` declared `_Noreturn` for C11 | §9.1.6 | absent |

**Design conflict.** §10 closed "Project name" says `libosoe` is built with hidden visibility "so only spec API symbols are exported (enforced by the `shmem.exports` test)". The profiling interface requires `pshmem_*` to be exported too, and `shmem_*` should be weak aliases so a profiler library can interpose (§10.2.2 Examples 49/50). §10.3.1 also implies internal calls (the malloc barrier, finalize barrier) should go to internal or `pshmem_` symbols, or a profiler double-counts `shmem_barrier_all`.

**Failure scenario.** A TAU- or Score-P-style wrapper defines `shmem_putmem` and calls `pshmem_putmem`. Link fails, or the `shmem.exports` test rejects the build.

Also the current header comment "Only the routines osoe implements are declared" [`shmem/include/shmem.h`] is fine, but §5 requires `shmem.h` to "contain only the interfaces and constant names defined in this specification". The include guard `OSOE_SHMEM_H` is covered by exception 2 (implementation prefix). That is sound.

Severity clarification needed. The export policy is a design change needed (a small one). Confidence high.

### O9 — `SHMEM_TEAM_SHARED` is required; predefined-team queries; contexts (clarification)

**Spec.**
- §7 lists `SHMEM_TEAM_SHARED` as a predefined handle: "the team of all PEs that would mutually return a non-null address from a call to shmem_ptr or shmem_team_ptr for all symmetric heap objects".
- §9.4: "All predefined teams are valid for the duration of the OpenSHMEM portion".
- §3.2 footnote 4: TEAM_SHARED is "not having been created by a call to a shmem_team_split_* routine on SHMEM_TEAM_WORLD".

**Design** §4.1: "if `SHMEM_TEAM_SHARED` is ever implemented" [doc L636]. §7 "Plumb": only `SHMEM_TEAM_WORLD`.

**Consequence.** Because `shmem_ptr` returns non-NULL intranode, TEAM_SHARED *must* equal the logical-node members. Its definition ties it to exactly the §8.1 locality function. `shmem_team_my_pe/n_pes/translate_pe/get_config(SHMEM_TEAM_SHARED)` must work from init. The spec's own §3.2 Example 1 calls `shmem_team_create_ctx(SHMEM_TEAM_SHARED, …)` and `shmem_team_translate_pe(SHMEM_TEAM_WORLD, 0, SHMEM_TEAM_SHARED)`.

This is cheap, since the node map exists at bootstrap. But if force-remote mode (O3) makes `shmem_ptr` NULL, TEAM_SHARED membership must shrink to singletons consistently.

**Contexts.** `shmem_ctx_create` is required. The spec allows failure ("An unsuccessful context creation call is not treated as an error"), so returning nonzero with `SHMEM_CTX_INVALID` is a conforming phase-1 stub, and the doc should say so. Also required: `shmem_ctx_get_team`, and `SHMEM_CTX_INVALID` no-ops for fence, quiet and destroy [§9.12.1/§9.12.2/§9.5.3].

The doc's remaining open question "whether any team query must return remotely accessible memory" [doc L1679] can be closed. `team_my_pe` and `team_n_pes` return `int`, `get_config` fills a caller struct, and `translate_pe` returns `int`; none returns memory.

Severity clarification needed. Confidence high.

### O10 — §6.4 lifecycle misses spec obligations (clarification)

**Spec §9.1.4.**
- "All other calls to shmem_finalize perform an operation semantically equivalent to shmem_barrier_all and return without freeing any OpenSHMEM resources."
- "The last call … performs an implicit global barrier."
- "This call destroys all teams … all shareable contexts are destroyed."
- "including the symmetric memory heap and pointers initiated by shmem_ptr."
- §9.2 item 1: finalize must be on the thread that called `shmem_init_thread`.

**Design** §6.4: "`shmem_finalize` decrements; the last one tears down" [doc L1361]. That omits the barrier on non-last finalize. If a non-last finalize just decrements, a PE that calls it while peers have pending puts gets no completion, and the collective sequence (barrier counts `k`, §5.4) diverges from PEs whose finalize count differs. §5.4's `k` must count these barriers.

Also: the atexit implicit finalize for `start_pes` (O8); MPI init/finalize in arbitrary order [D.1.1]; and whether `SHMEM_SYMMETRIC_SIZE` is re-read at re-init (the same latch question as `max_teams`).

Severity clarification needed. Confidence high.

### O11 — fork/exec after init (clarification)

This is not mentioned. `system()`/`popen()` from a PE is common in real codes. The child inherits `MAP_SHARED` heap mappings and inherits any io_uring ring fd and sockets if the progress thread lives in that process (O1), but not the thread itself. If osoe installs `atexit` teardown (needed for `start_pes` implicit finalization and segment unlink), a `fork()`+`exit()` child runs it: it unlinks or unmaps shared state, may `shutdown()` TCP sockets (which affects the parent's connection, since shutdown acts on the socket, not the fd [recalled]), or sends a finalize barrier arrival.

**Needed.** Record `getpid()` at init and make all teardown hooks no-ops in other pids. Consider `MADV_DONTFORK` on the heap and `O_CLOEXEC`/`SOCK_CLOEXEC` on every fd, including the ring (`io_uring_setup` fds are close-on-exec by default [recalled]).

Severity clarification needed. Confidence medium.

### O12 — Signals on the progress thread; EINTR in PE waits (minor)

§3.10 handles `-EINTR` for the ring. §4.9 and §5.4 `futex_wait` loops re-check the predicate on any return, which is correct. What is missing:
- The progress thread(s) must block all asynchronous signals (`pthread_sigmask` before creation). Otherwise a process-directed `SIGINT`/`SIGALRM`/`SIGPROF` handler from the application or a sampling profiler can run on the progress thread, including non-async-signal-safe application code, while it holds a handler context. With a helper process, Ctrl-C forwarded by srun must still kill it.
- The absolute deadline (§3.10) must stay fixed across EINTR retries, not be recomputed, or a signal storm extends the safety sweep indefinitely.

Severity minor. Confidence medium.

### O13 — Slurm CPU binding vs. "cpuset isolation for the progress thread" (clarification)

§3.11: "Use cpuset isolation for the progress thread instead" [doc L539–540]. Under srun with `--cpu-bind` or task/affinity and the task/cgroup cpuset plugin, a thread created inside a task inherits that task's affinity mask and cgroup [recalled], so it cannot move to an isolated core. If hosted in PE 0 (O1), the progress thread competes with PE 0 on PE 0's core, which is exactly the §3.11 hazard. On an oversubscribed Pi (4 cores, many PEs) this dominates.

**Needed.** A stated launch requirement, e.g. `--cpu-bind=none`, a spare core per node reserved via `--cpus-per-task`, or `srun --overlap` for a helper step, and a startup check that prints the effective mask.

Severity clarification needed. Confidence medium.

### O14 — Heterogeneity claims (clarification)

§4.2 says offset addressing "survives … heterogeneous nodes". The following is unaddressed:
- **AM header byte order**: native vs. little-endian fixed.
- **`long double`**, which is a standard RMA type (Table 5): 16 bytes on aarch64 Linux as binary128, vs. 80-bit x87 padded to 16 on x86-64 [recalled]. Same size, different bytes.
- `shmem_pe_accessible` is defined as "a process running from the same executable file" [§9.1.7], which a cross-architecture job cannot satisfy.
- A mixed **Pi 4 (4K pages) + Pi 5 (16K pages, §8.5)** cluster is a plausible configuration. §4.4 forbids consulting "anything that varies across PEs". If the allocator rounds large blocks to `sysconf(_SC_PAGESIZE)`, or aligns the heap base to page size, heaps desynchronize on exactly that cluster, and §3.13's handler-table checksum does not catch it.

**Needed.** Declare homogeneous (endianness, ABI, page size) and exchange/assert these in the §6.2 rendezvous, or make the allocator's constants explicit and page-size independent.

Severity clarification needed. Confidence medium (high for the page-size point).

### O15 — Resource limits and accounting (minor; one item verified sound)

- **RLIMIT_MEMLOCK is not a concern** for rings and provided-buffer rings on 6.12. Four rings, each with a 32,768-entry pbuf ring and a 65,536-entry CQ, were created successfully with `ulimit -l 64` (64 KiB). One ring with 1024 entries was created with `ulimit -l 0` [observed, `exp/omissions/ml.c`]. The prompt's suspected failure does not occur on the target kernel. (Registered *fixed buffers* would be different; the design does not use them.)
- `ulimit -n` is soft 1024, hard 524288 [observed]. "Hundreds of nodes" of sockets plus pidfds (O4) plus rings in one process approaches 1024. Raise the soft limit to the hard limit at init.
- **Memory commitment**: L × `SHMEM_SYMMETRIC_SIZE` of shared memory per node. Under a Slurm memory cgroup, shmem pages are charged to the cgroup of the task that first touches them [recalled]. If one process (the lead or the progress thread) pre-faults or zeroes the segment, it absorbs the whole node's heap charge and is OOM-killed first. Say who touches the pages, or leave that to each PE.

Severity minor. Confidence: high on observed items, low on the memcg claim.

### O16 — Smaller spec items (minor)

- `shmem_signal_fetch` [§9.8.6] and `shmem_signal_wait_until` [§9.11.15] are the natural consumers of phase-1 `signal_add`/`signal_set`, but neither is listed. They are trivial given §4.9, but §9.8.1 requires them to be atomic with respect to the signal updates, which holds on the shared mapping.
- `shmem_pe_accessible`/`shmem_addr_accessible` [§9.1.7/§9.1.8]: not mentioned. `addr_accessible` must return 1 only for symmetric addresses. In phase 1 that means heap only.
- `shmem_malloc(0)`, `calloc` with a zero argument, and `align(…, 0)` perform **no barrier** [§9.3.1, §9.3.4, §9.3.6]. §4.4's debug checksum "compare at the barrier the call already performs" must still fold these calls, and must not assume a barrier happens.
- `shmem_free` "calls a barrier on entry", and malloc on exit [§9.3.2, §9.3.1]. The §5.4 epoch counting must include these implicit barriers.
- §4.6 says "Fence concerns puts, signalled puts, and non-fetching AMOs". Table 14 also marks *blocking* AMOs (fetching included) and memory stores. That is harmless for blocking ops, but the list is incomplete.

---

## 2. Citation check

| Doc claim | Verdict | Evidence |
|---|---|---|
| "1.6 change #10 … collective operations on the world team (§9.3.1–9.3.6)" [doc L731] | **Correct** | G.1 item 10 [L10350ff] |
| "§9.10.9 (1.6 change #17): 'The same source and dest arrays must be passed by all PEs.'" [doc L1208] | **C1 — quote truncated inside quotation marks.** Actual: "The same source and dest arrays must be passed by all PEs that participate in the collective." [L7384]. The change number and section are correct | L7384, G.1 item 17 |
| "1.6 change #18 … fence orders only operations issued on the same context (§9.12.1)" [doc L774] | **Correct** | G.1 item 18 |
| "`shmem_test_all` and friends, which 1.6 change #19 clarifies return 1 on an empty test set" [doc L1011] | **C2 — overgeneralized.** #19 covers only `shmem_test_all` and `shmem_test_all_vector`. On an empty set, `test_any`/`test_any_vector` return **SIZE_MAX** and `test_some`/`test_some_vector` return **0** [L8468, L8577, L8762, L8830]. Implementing "friends return 1" would be wrong | G.1 item 19 |
| "1.6 change #2 … (§9.1.1, §9.1.4, §9.1.5)" [doc L1354] | **Correct** | G.1 item 2 |
| "1.5 change #17" (ivar updated using AMOs) [doc L1015] | **Correct** | G.2 item 17 |
| "`shmem_team_ptr` … 1.6 §9.1.10"; WORLD identical to `shmem_ptr`, INVALID returns null [doc L910–917] | **Correct** | §9.1.10 |
| "§9.10.1 Notes: 'is equivalent to calling `shmem_ctx_quiet` on the default context followed by calling `shmem_team_sync` on the world team.'" [doc L1239] | **Correct, verbatim** | §9.10.1 Notes |
| "§9.12.3 … returning immediately when `npes` is 0" and the "might be equivalent to `shmem_quiet`" quote [doc L829, L846] | **Correct** | §9.12.3 |
| `void shmem_pe_quiet(shmem_ctx_t ctx, const int *target_pes, int npes)` [doc L837] | **C3 — wrong signature.** The spec has two routines: `void shmem_pe_quiet(const int *target_pes, size_t npes)` and `void shmem_ctx_pe_quiet(shmem_ctx_t ctx, const int *target_pes, size_t npes)`. `npes` is `size_t`, not `int` | §9.12.3 SYNOPSIS |
| "§9.4.8 only as 'a collective operation that destroys the team referenced by team'" [doc L1267] | **C4 — not verbatim.** Actual: "a collective operation that destroys the team referenced by the team handle argument team." The substance (scope unnamed) is correct | §9.4.8 |
| "§3.2 item 4" (AMOs vs. non-OpenSHMEM loads/stores undefined) [doc L635, L937] | **Correct** | §3.2 list item 4 |
| §3.2 atomicity summary and "SHMEM_TEAM_SHARED … separate atomicity domain" [doc L629–639] | **Correct** | §3.2 and footnote 4 |
| "§9.9 … Hints only. A conforming implementation may ignore them entirely" [doc L1422] | **Correct in substance.** Spec: "it is up to the implementation whether or not to apply any optimizations within a session" | §9.9 intro |
| "`shmem_sum_inscan`, `shmem_sum_exscan` (§9.10.10)" [doc L1423] | **Correct** (section titled SHMEM_SCAN) | G.1 item 1 |
| "`shmem_ibget`, `shmem_ibput` (§9.6.1.4, §9.6.1.8)" [doc L1424] | **C5 — pairing reversed.** §9.6.1.4 is SHMEM_IBPUT and §9.6.1.8 is SHMEM_IBGET. The set of sections is right | TOC L283, L287 |
| "1.6 §9.10.5 and §9.10.7–§9.10.10 all state 'the application does not need to synchronize to ensure that the source array is ready across all PEs prior to calling this routine.'" [doc L1199] | **Correct** (capital "The" in spec). Present in §9.10.5, .7, .8, .9, .10 [L6588, L6878, L7036, L7395, L7578]; matches G.1 item 14 | — |
| "`shmem_ctx_create` is not collective" [doc L751] | **Correct** | §9.5.1 |
| "`shmem_team_split_strided` is collective over the parent team" [doc L1247] | **Correct** | §9.4 "Team creation is a collective operation on the parent team object" |
| "destroy also destroys every shareable context … PRIVATE contexts user's responsibility … destroying INVALID is a no-op" [doc L1275–1280] | **Correct** | §9.4.8 |
| "`_nbi` completion is defined only at `quiet`" [doc L789] | **Correct for get_nbi** ("considered complete after a subsequent call to shmem_quiet"). Barrier_all and the implicit barriers also complete them, which is harmless | §9.6.2.2, §9.10.1 |
| "Yama `ptrace_scope` defaults to 1" [doc L670] | **C6 — not a spec citation, but factually wrong for the stated target.** Debian 13 VM: `0` [observed] | O6 |
| "Accesses through the returned pointer are ordinary loads and stores … fence and quiet do not order them" [doc L924] | **Contradicted by spec** — see O3 | Table 14, §9.5, §9.10.1 |

---

## 3. Requirement → design coverage grep (checklist)

| Spec requirement | In design? |
|---|---|
| shmem_init / finalize / my_pe / n_pes / query_initialized, repeated init | yes (§6.4); missing the non-last-finalize barrier (O10) |
| shmem_init_thread / query_thread / thread levels | **no** (O2) |
| shmem_global_exit | **no** (O4) |
| pe_accessible / addr_accessible | **no** (O16) |
| shmem_ptr / team_ptr | yes (§4.8); store-ordering claim wrong (O3) |
| info_get_version / get_name, version and vendor constants | implemented in code; not in doc (fine) |
| start_pes, implicit finalization, deprecated names, mpp/ header | **no** (O8) |
| malloc/free collective on world, implicit barriers | yes (§4.4, §4.5); zero-size no-barrier case missing (O16) |
| SHMEM_SYMMETRIC_SIZE per PE + grammar | partially (named only); per-PE semantics and grammar missing (O5, O8) |
| SHMEM_VERSION / INFO / DEBUG, SMA_* | **no** (O8) |
| Predefined teams WORLD **and SHARED**, INVALID handling | WORLD yes; SHARED treated as optional (O9) |
| team_my_pe / n_pes / translate_pe / get_config | **no** explicit mention (O9) |
| Contexts: DEFAULT, ctx_create success/failure, INVALID no-ops, NOSTORE semantics | DEFAULT yes; others no (O3, O9) |
| putmem / getmem (local completion semantics, private dest) | partially; private-dest delivery path missing (O1) |
| fetch_add / compare_swap | yes (§4.1, §3.8) |
| put_signal / signal_add / signal_set | yes; signal_fetch / signal_wait_until missing (O16) |
| wait_until / test | yes (§4.9); watch key issue (O5) |
| fence / quiet / pe_quiet | yes (§4.6); pe_quiet signature wrong (C3); stores (O3) |
| barrier_all ≡ quiet(default) + team_sync | yes (§5.6) |
| Profiling interface (pshmem_, pshmem.h, pcontrol) | **no**; conflicts with export policy (O8) |
| shmemx.h must exist | **no** (O8) |
| C11 generic interfaces | **no** (O8) |
| Fortran | not required (removed in 1.5, G.2 item 18). **Correctly absent** |
| MPI interop (D.1): init/finalize in any order, own progress | not mentioned. The refcount design is compatible; no action beyond not assuming PMI ownership |
| Annex C undefined behaviours (statics, null+len) | statics yes (O7) |

---

## 4. What the doc handles well (with evidence)

- **Allocation is world-collective and deterministic.** §4.4/§4.5 rest correctly on G.1 item 10 and §9.3.1 ("collective operation on the world team"). Identical call sequences give identical offsets, and on exhaustion every PE returns NULL identically, satisfying §9.3.3 "all PEs return a null pointer".
- **barrier_all vs. sync semantics** (§5.6) match §9.10 "Team-implicit collectives" and §9.10.1 Notes verbatim.
- **Collective source-readiness rule** (§5.5 "push, never pull") correctly derives from the §9.10.5/.7–.10 text quoted above, and the conclusion that pull-based alltoall is non-conforming follows.
- **Blocking-collective assumption** (§3.8 item 2, §5.4): the spec confirms "All OpenSHMEM teams-based collective operations are blocking routines" and "may not be called simultaneously by multiple threads on a given team" [§9.10 intro]. The equality-counter argument rests on real spec text.
- **Team destroy scoping** (§5.7): the spec indeed does not name the scope, and the leak-until-finalize choice is consistent with §9.1.4 ("teams … may be leaked until the final call").
- **`shmem_team_ptr`**: the signature and the WORLD/INVALID semantics are exactly right (§9.1.10).
- **Session routines as no-ops** are conforming (§9.9).
- **`shmem_malloc_with_hints` as an alias** is conforming (§9.3.5: "If the implementation cannot optimize, the behavior is same as shmem_malloc").
- **The §4.9 wait/test exclusivity reasoning** is correct per §3.2 ("Exclusivity is also guaranteed when the target PE performs a wait or test operation on the same location and with the same datatype") and G.2 item 17.
- **Outstanding-get cap causing blocking, not failure** (§4.6) is consistent with the spec, which puts no bound on `_nbi` operations.
- **Fortran is absent**, which is correct: the API was removed in 1.5 (G.2 item 18; F.1 "Entire Fortran API … Last Version Supported 1.4").
- **io_uring memory accounting**: no RLIMIT_MEMLOCK problem for rings and pbuf rings on 6.12 [observed], so the §3.3 sizing is not blocked by default limits.
- **Existing code** (`shmem.h`, `info.c`): the `_SHMEM_*` deprecated constants are present as §6/F.1 require, and the `_Static_assert` that `SHMEM_VENDOR_STRING` fits `SHMEM_MAX_NAME_LEN` enforces §9.1.12.

---

## 5. Experiments

`scratchpad/exp/omissions/` (run in the VM under `/tmp/omx`):
- `sib.c`: two sibling processes. A calls `memfd_create` and B obtains the fd via `/proc/<pid>/fd` and via `pidfd_getfd`. Both OK at `ptrace_scope=0`. I did not test under Yama 1, because changing the sysctl is a security-setting change.
- `ml.c`: N rings × pbuf-ring entries under varying `ulimit -l`. All succeeded, down to a limit of 0.
- Probes: `ulimit -l` 8192 KiB, `ulimit -n` 1024/524288, `HugePages_Total: 0` with hugetlbfs mounted, `CONFIG_ARM64_VA_BITS=52`, `vm.max_map_count` 1048576.
