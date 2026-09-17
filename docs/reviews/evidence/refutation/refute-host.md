# Refutation: which process hosts the progress thread, and memory shared across processes

Findings covered: transport B1, semantics B1, omissions O1, bootstrap B5.
Design doc: `docs/design/shmem-ethernet-design.md` (1712 lines). Line numbers below refer to it.

**Experiment status.** I wrote a VM check of single-issuer submission from (a) another thread, (b) a forked child that inherits the ring, and (c) a sibling that receives the ring fd over SCM_RIGHTS. The user rejected that tool call, so **it was not run**. I did not retry it. The io_uring claims below come from the 6.12 source already fetched to `exp/q1-transport/ksrc/` (observed), plus what I recall of `io_uring/tctx.c`. The Yama claims are either observed by earlier reviewers (scope 0 only) or recalled.

---

## 1. Doc search: does any text answer "which process"?

Searched terms: coherence, process, helper, daemon, fork, SCM_RIGHTS, pidfd, election, lead, segment key/name, am_progress_start.

| Text | Where | What it implies |
|---|---|---|
| "PEs are processes." | L25 | The progress thread must live in one process: a PE's or some other. |
| `am_progress_start(am_ep_t *ep, int nthreads)` | §2.1 L123 | The threads are started by a library call on an endpoint, so they run in whichever process calls it. This leans toward a PE process and away from a daemon. It does not say which PE. |
| "Writers are PE threads." | §3.5 L281 | Outbound buffers must be in memory shared across processes. The doc never says so. |
| "One ring per progress thread, `IORING_SETUP_SINGLE_ISSUER`. Only the progress thread submits." | §3.3 L187–188 | Only one task may submit on a ring. |
| "PE side: `atomic_fetch_add(&ep->wake, 1)` then `futex_wake`" | §3.10 L478 | PEs *signal* the progress thread. They do not submit. |
| "A small fixed pool per node … not one thread per PE" / "shares the coherence domain with local PEs" / "The origin side never needs a helper, because the calling thread is inside the library by definition" / "each connection has exactly one owner" | §3.12 L548–557 | This rules out one thread per PE. "Coherence domain" means a shared mapping, which fits both a lead-PE host and a helper process, so it chooses neither. The origin-side sentence assumes the calling PE can receive its own replies. It cannot, because the stream has one owner (see §3). |
| "The progress thread shares the coherence domain with every local PE" | §4.1 L623 | Same as above. |
| "SLURM_LOCALID — the election input" / "local-lead election" | §6.1 L1307, L1312 | Some node-level election exists. The doc never says what it elects. §5.2's "local lead" is per team, for collectives. |
| Phase 1 `shmem_init` forces "…shared segment, election" | §7 L1388 | Same gap. |
| "ranks, addresses, and the segment key are exchanged" | §6.2 L1322 | The segment is found by a *key*. No mechanism is given. |
| "Shared segment name keyed on logical node id" | §8.1 L1449 | The segment has a *name*, which a bare memfd does not have. |
| "Shared `memfd`/hugetlbfs segment" | §4.1 L621 | hugetlbfs files have paths. memfds do not. |
| "Write replies directly into the destination rather than staging **where possible**." | §4.7 L903 | The "where possible" hedge admits staging, but no staging is designed. |
| "`bytes_landed` — progress thread adds as chunks land" | §4.6 L785 | This counter is written from another process, so it must be in shared memory. The doc doesn't say so. |
| Phase 1.5 CMA "Needs `PR_SET_PTRACER` … PEs are siblings" | §4.3 L670 | The doc knows that sibling PEs cannot reach each other's private memory. |
| Deferred: "nonblocking variants" | §7 L1410 | `_nbi` gets are **not** phase 1. |

**Conclusion.** No text names the host process. The strongest hints (§2.1, §6.1 "election") point to threads inside one elected PE, presumably `LOCALID==0`. Nothing states it.

## 2. io_uring single issuer (observed in source, 6.12)

- `exp/q1-transport/ksrc/io_uring_io_uring.c:3675–3676`, in `io_uring_create`: with `IORING_SETUP_SINGLE_ISSUER` and without `R_DISABLED`, `ctx->submitter_task = get_task_struct(current)`. The owner is a **task**, meaning a thread, not a process.
- L1290 `WARN_ON_ONCE(ctx->submitter_task != current)` and L3192–3193 assert the same owner on other paths. L3308 notes that the submit path reaches `__io_uring_add_tctx_node_from_submit()`.
- *Recalled* (`io_uring/tctx.c`, not in the fetched files): `__io_uring_add_tctx_node_from_submit` returns `-EEXIST` when `SINGLE_ISSUER && ctx->submitter_task != current`.

So `io_uring_enter` with submissions fails for every task except the creator. That includes other threads in the same process, a forked child, and a sibling given the fd over SCM_RIGHTS. **A SINGLE_ISSUER ring cannot be driven from another process.** The design never asks for that, though: §3.3 and §3.10 already say that PEs only wake the progress thread.

## 3. Would an obvious host choice work without changing other decisions?

**(a) A progress thread in every PE.** This contradicts §3.12 ("not one thread per PE") and §3.1/§9 inv. 3 (one connection per node pair, one receiver). Each PE would need its own sockets, or several processes would be receiving on one stream. **Not viable.**

**(b) Threads inside the local lead PE (`LOCALID==0`).** This is the reading that §2.1 and §6.1 suggest. It is consistent with the stream model, with single issuer, and with one owner per connection. Its costs:

1. **Get replies for non-lead PEs.** The reply chunks arrive in the lead's address space. The origin's `dest` is private memory (spec §9.6.1.5, "Local address of the data object to be updated"), so the lead cannot write into it directly without CMA. That rules out §4.7's "directly into the destination", and it contradicts the §3.12 sentence that the origin side needs no helper.
   - The obvious repair is to put the provided buffers, or a per-PE staging area, in the shared node segment and have the blocked origin PE copy out.
   - Doing that pins receive buffers until a possibly descheduled PE runs. At 256 oversubscribed PEs that means `ENOBUFS`: receiving stops for the whole node pair.
   - The alternative is to copy into staging, which needs a sized origin-side buffer.
   - Either way it breaks the spirit of §3.7 ("Any scheme where inbound processing can be halted by …") and the one-sided §3.8 memory argument. It is a real design decision, not a mechanical one.
2. **Liveness.** If the lead crashes, or is stopped by `gdb -p` (the §3.14 hang-debugging workflow) or `SIGSTOP`, the whole node loses the transport. §3.15 covers connection loss but not a PE that stops while its connections stay open.
3. **§3.11 "cpuset isolation for the progress thread"** conflicts with Slurm binding the lead task's CPUs. This is *recalled* Slurm behaviour.
4. **Re-init and finalize (§6.4).** The lead's threads must outlive the lead's own `shmem_finalize` until the others finish. The spec's implicit barrier in the last finalize (§9.1.4) mostly covers this.

**(c) A helper process forked by the lead.** This fits the "coherence domain" wording. It still has problem (b)1. It also needs parent-death handling, a CPU placement, and a re-init story, none of which §6 mentions.

**Result.** Every option except (a) is implementable, but each one forces the get-landing decision. That decision reopens §3.8/§4.7 and needs one invariant added to §9. The finding survives.

---

## Transport B1: progress-thread host undefined; get replies can't reach private `dest`; PE-side seal_and_submit conflicts with single issuer; lifecycle and scheduling

**Verdict: SURVIVES (blocking) for the core. Two sub-points are downgraded.**

- **Core (sub-points 1 and 3): survives.** Section 1 shows no text names the host. Section 3 shows that the obvious choices either contradict §3.1/§3.12 or force an undesigned get-landing and liveness decision.
- **Sub-point 2 (seal_and_submit vs single issuer): downgraded to clarification.**
  - The kernel fact holds (section 2).
  - But the doc already gives the obvious reading. §3.5 writers seal (the `/* seal; try other buffer */` comment, and the states are atomics in shared memory). §3.5 L307 says "The progress thread seals, and submits". §3.10 L478 gives the PE-side wake.
  - So `seal_and_submit()` in §4.6 reads naturally as "seal (CAS), bump the wake word, futex_wake". No implementer would call `io_uring_enter` from a PE.
  - Fix: rename the function to `seal_and_signal` and add a comment. Which wake word to use is transport D2 and is not re-judged here.
- **Sub-point 4 (Slurm cpuset): survives as clarification.** It relies on recalled Slurm behaviour and depends on the host choice.
- **Sub-point 5 (fd passing doesn't help): agreed.** Section 2.

**Tightest statement.** The design never names the process that owns the rings, sockets, provided buffers, and progress threads. It also never says which structures must therefore live in the shared node segment: segment buffers and their states, wake words, `bytes_landed`, and deferred queues. For non-lead PEs, get-reply delivery has no possible path as written.

**Fix direction.** In §3.12, name the host: lead-PE threads or a forked helper. List the shared-memory residents. Add a §9 invariant: "PEs never submit; they seal and wake."

## Semantics B1: get replies and fetching-AMO results land in private memory the progress thread can't reach; CMA fails under Yama 1

**Verdict: SURVIVES-DOWNGRADED to design change needed. The hosting question stays blocking under transport B1 / O1.**

- **Get replies: survives.** No host except (a) can write the origin's private `dest` without CMA. §4.7's "where possible" hedge admits staging but designs none.
- **Why downgraded.**
  - Phase 1 contains only **blocking** `shmem_getmem`. `_nbi` gets are deferred (§7 L1410).
  - For blocking gets there is an obvious bounded answer: land in shared per-PE staging or in shared pbufs, and have the blocked caller copy out.
  - The remaining design work is the stall/`ENOBUFS` interaction and the origin-side memory bound (section 3 (b)1). That is a design change, not an unimplementable gap.
  - The unbounded-staging argument only bites once `_nbi` gets arrive.
- **Fetching-AMO results: downgraded to clarification.** A fixed-size result word in a shared per-PE slot is trivial and bounded. At most one fetching AMO is outstanding per blocked caller thread.
- **CMA under Yama 1: accurate but low weight.**
  - Observed by the reviewer only at scope 0 → success. The scope-1 failure is expected: `process_vm_writev` uses `PTRACE_MODE_ATTACH_REALCREDS`, which Yama 1 restricts to descendants (*recalled*).
  - §4.3 L670 already records this restriction for CMA, so nobody reading the doc would choose CMA for gets.
  - The §4.3 claim that Yama "defaults to 1" is distribution-dependent (C7, minor).

**Tightest statement.** §4.7's "write directly into the destination" is impossible for any origin PE other than the host process. §4.6's `bytes_landed` must live in shared memory. The copy-out step and its receive-stall and memory consequences are undesigned.

**Fix direction.** Add a shared per-PE reply area. The blocked caller copies out. Define `bytes_landed` as "copied". Rule CMA out explicitly. Defer the `_nbi` staging bound to when nonblocking variants arrive.

## Omissions O1: progress-thread host process unspecified

**Verdict: SURVIVES (blocking). Same defect as transport B1.**

The scenarios hold for the lead-host reading, which is the one the doc hints at:
- Scenario 1: private `dest`.
- Scenario 2: a debugger or SIGSTOP on the host stalls the node. §3.14's own hang-dump workflow triggers this.
- Scenario 3: host crash while the other PEs live on.
- Scenario 4: helper lifecycle.

Scenario 1 is subject to the phase-1 blocking-only downgrade above. Merge O1 with transport B1.

## Bootstrap B5: memfd can't be shared between sibling tasks by sending a key over TCP; needs a node-local UNIX socket or pidfd_getfd

**Verdict: SURVIVES-DOWNGRADED to clarification needed.**

- **Hosting half:** a duplicate of transport B1. Count it once.
- **Segment-sharing half.** As literally written, "memfd" plus a "segment key" is broken: a memfd has no name. But the "must use a UNIX socket" conclusion is overstated.
  1. §4.1 offers hugetlbfs, and §8.1 speaks of a segment *name*. A named object (a hugetlbfs file, or `shm_open` under `/dev/shm`) keyed by `jobid.stepid.nodeid` needs only a string, and a string can travel over the §6.2 TCP rendezvous. It can also just be computed from the environment.
  2. Even with a memfd, the key can be `(pid, fd)`, and siblings open `/proc/<pid>/fd/<n>`.
     - Observed at scope 0 in `exp/omissions/sib.c`: `open(/proc/8197/fd/5): OK`, and the mapping read back.
     - At Yama 1 this should still work. The `/proc` fd link check uses `PTRACE_MODE_READ_FSCREDS`, and Yama restricts only `PTRACE_MODE_ATTACH` (*recalled*, not tested; testing would need a sysctl change, which is not allowed).
     - `pidfd_getfd` does need ATTACH, so the reviewer is right that it fails at Yama 1 (*recalled*).
  3. What remains:
     - **Name collision** when concurrent jobs share a node: §8.1 keys only on logical node id. This is real and is fixed by adding job and step ids.
     - **Crash cleanup** of named `/dev/shm` objects: real, a clarification.
     - hugetlbfs needs reserved pages (observed `HugePages_Total: 0`), so a fallback must be stated.

**Tightest statement.** §4.1/§6.2 do not say how a sibling task attaches to the segment. A bare memfd cannot be found by a key. The §8.1 name must include job and step ids, and crash cleanup must be stated.

**Fix direction.** Use a named segment (`shm_open` or hugetlbfs) named `osoe.<jobid>.<stepid>.<nodeid>`. Unlink it once all local PEs have attached, or at the first finalize. Alternatively keep the memfd and pass `/proc/<pid>/fd/<n>`. A node-local UNIX socket is optional. It becomes useful if the design also wants pidfd-based liveness (bootstrap E1).

---

## Verdict table

| ID | Verdict | New severity |
|---|---|---|
| transport B1 (core: host undefined, get landing, lifecycle) | SURVIVES | blocking |
| transport B1.2 (PE seal_and_submit vs SINGLE_ISSUER) | SURVIVES-DOWNGRADED | clarification (§3.5/§3.10 give "seal + wake") |
| transport B1.4 (Slurm cpuset) | SURVIVES-DOWNGRADED | clarification (recalled Slurm behaviour) |
| semantics B1 (get replies / AMO results to private memory) | SURVIVES-DOWNGRADED | design change (phase 1 is blocking gets only; AMO result slot is trivial) |
| omissions O1 | SURVIVES (duplicate of transport B1) | blocking |
| bootstrap B5 (memfd over TCP, needs UNIX socket) | SURVIVES-DOWNGRADED | clarification (named shm/hugetlbfs or `/proc/pid/fd` works; the real issues are the job-id in the name and cleanup) |
