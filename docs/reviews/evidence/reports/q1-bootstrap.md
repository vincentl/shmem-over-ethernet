# Q1 — Bootstrap, lifecycle, error model, and testing review

Reviewer scope: design §6, §3.13, §3.15, §8, §9; `dev/lima/*`, `CMakeLists.txt`, `CMakePresets.json`, `shmem/`, `tools/probe/probe.c`.
Repository HEAD: `9475e45`. Nothing in the repository was modified. The build ran on a `git archive` copy in the VM at `/tmp/osoe-q1`.

Evidence sources (all fetched 2026-09-16 with `curl`; plain text saved under `scratchpad/exp/bootstrap/`):

- Slurm man pages: https://slurm.schedmd.com/srun.html, https://slurm.schedmd.com/sbatch.html, https://slurm.schedmd.com/heterogeneous_jobs.html, https://slurm.schedmd.com/slurm.conf.html, https://slurm.schedmd.com/mpi_guide.html
- Slurm source, `master` branch: `src/common/env.c`, `src/slurmd/slurmstepd/{slurmstepd_job.c,task.c,mgr.c}`, `src/srun/launch.c`, `src/plugins/mpi/pmi2/{mpi_pmi2.c,setup.c,pmi.h}`, all from https://raw.githubusercontent.com/SchedMD/slurm/master/…
- Ubuntu packages: https://packages.ubuntu.com/noble/liburing-dev, …/resolute/liburing-dev, and the Launchpad API
- GitHub runner images: https://raw.githubusercontent.com/actions/runner-images/main/images/ubuntu/Ubuntu2404-Readme.md, `Ubuntu2404-Arm64-Readme.md`, `Ubuntu2604-Readme.md`, `Ubuntu2604-Arm64-Readme.md`
- OpenSHMEM 1.6 text: `scratchpad/spec16.txt`

Every web fetch succeeded.

---

## Summary table

| # | Finding | Severity | Confidence |
|---|---|---|---|
| B1 | §6.2 has no way for other PEs to find rank 0: there is no KVS, rank 0's host is not known in advance, and "retry on bind conflict" leaves its port unknown | **blocking** | high |
| B2 | §6.1 uses `SLURM_NODELIST` (the job's node list) together with `SLURM_NODEID` (a position in the step's node list). Any step that runs on a subset of the allocation gets the wrong hostnames | **blocking** | high |
| B3 | §6.1's claim that the node map comes from the environment is false for cyclic, plane, and arbitrary distributions, when a topology plugin is configured, and under `srun -E` | **blocking** | high |
| B4 | Every PE connects to rank 0. At design scale that is about 77k fds and a burst of SYNs. The measured cost is listen overflows and seconds to minutes of SYN retries, and it breaks the "scale with node count" rule | design change needed | high |
| B5 | No node-local bootstrap: which process owns the sockets, rings, and progress threads is unstated, and a `memfd` cannot be shared by "exchanging a key" over TCP | design change needed | high |
| B6 | Setup of the node-pair connections is unspecified: who connects to whom, how simultaneous connects are resolved, and when it happens | clarification needed | high |
| B7 | Running the binary directly in an `sbatch` script, or under `mpirun`, supplies misleading variables and the job hangs. There is no rendezvous timeout | design change needed | high (sbatch) / low (mpirun) |
| B8 | §3.13 says to exchange the handler-table checksum "through PMI", which contradicts §6 | minor (doc) | high |
| L1 | §6.4 has a non-last `shmem_finalize` only decrement a count. The spec requires it to act like `shmem_barrier_all` | design change needed (small) | high |
| L2 | §6.4 is silent on concurrent or re-entrant init, the latched thread level, the same-thread finalize rule, and `start_pes` implicit finalization at exit | clarification needed | high |
| L3 | Re-init as "a fresh job" works under Slurm once B1 is fixed. The reasoning in the design is sound | sound, with a caveat | medium |
| E1 | One crashed PE hangs the whole job until its time limit: Slurm's defaults do not kill the step, and a leaked reservation stalls a shared segment for good | design change needed | high |
| E2 | The design has no mechanism for `shmem_global_exit`: PEs doing pure computation receive no notification, and killing them with signals breaks the spec's exit-status rule | design change needed | medium-high |
| E3 | Silent node loss: no keepalive, `TCP_USER_TIMEOUT`, or heartbeat is specified | clarification needed | medium |
| T1 | The x86 CI leg on `ubuntu-24.04` cannot configure: noble ships liburing 2.5. `ubuntu-26.04` runners exist and ship 2.14 | **blocking for §8.5 CI as written** | high |
| T2 | §6.2 says the bootstrap "is exercised by every test", but tiers 1–2 never exercise the Slurm variables. Tier 1 is single-process with no rendezvous | design change needed (tests) | high |
| T3 | Invariant 7 cannot fail in any test while identical-VA mapping is always on | design change needed (tests) | high |
| T4 | Mapping invariants to tiers: most concurrency invariants are caught only by asserts or injected faults, not by the tiers. Details in the table | clarification needed | medium |
| T5 | Tests have no timeout, so a hang (invariants 1, 4, 8, 10, 13) sits until ctest's 1500 s default | minor | high |
| T6 | The probe checks less than §3.3 and §8.5 say it verifies; there is no NEEDED-libraries test for invariant 15 | minor | high |
| S* | Sound items, with evidence | — | — |

---

## Part 1 — Slurm environment (§6.1)

### What each variable actually is

*Observed in source* (`env.c`, `slurmstepd_job.c`, `task.c`, Slurm `master`), with *documented* text from srun.html:

| Variable | Level | Semantics | Source |
|---|---|---|---|
| `SLURM_PROCID` | step (het-offset) | `task->gtid + het_job_task_offset` | task.c `exec_task` |
| `SLURM_LOCALID` | step, per node | `task->id`, the task's index on this node, dense 0..n_local−1 | task.c |
| `SLURM_NODEID` | **step** (het-offset) | `nodelist_find(msg->complete_nodelist, conf->node_name) + het_job_node_offset`. `complete_nodelist` is the **step's** node list, so the value is dense 0..`SLURM_STEP_NUM_NODES`−1 in step-hostlist order | slurmstepd_job.c:267, task.c:331. srun.html: "The relative node ID of the current node." |
| `SLURM_NTASKS` | step, **unless `srun -E/--preserve-env`** | `env_array_for_step` overwrites it with the step task count only `if (!preserve_env)` | env.c ~1458 |
| `SLURM_STEP_NUM_TASKS`, `SLURM_STEP_NUM_NODES` | step, always | always overwritten | env.c ~1424–1430 |
| `SLURM_TASKS_PER_NODE` | step, **unless `-E`** | step layout counts `if (!preserve_env)`. The man page still says "in the same order as SLURM_JOB_NODELIST" | env.c ~1440; srun.html |
| `SLURM_STEP_TASKS_PER_NODE` | step, always | "Number of processes per node within the step." | env.c ~1447; srun.html |
| `SLURM_JOB_NODELIST` | **job** | non-het steps use `env_array_append_fmt`, which does not overwrite an inherited value, so inside `sbatch`/`salloc` this remains the job list | env.c ~1418 |
| `SLURM_NODELIST` | **job** (non-het) | not set by `env_array_for_step` for non-het steps, so it is inherited from the allocation. `sbatch.html`: "List of nodes allocated to the job" | env.c ~1413–1420; sbatch.html |
| `SLURM_STEP_NODELIST` | step | "List of nodes allocated to the step." | env.c ~1420; srun.html |
| `SLURM_GTIDS` | step, per node | "Global task IDs running on this node. Zero origin and comma separated." Set by slurmstepd whatever the MPI plugin | task.c:325; srun.html |
| `SLURM_HET_SIZE`, `*_HET_GROUP_<n>` | job | per-component copies. With `--het-group=0,1` the combined `SLURM_JOB_NODELIST`/`SLURM_NODELIST` and `SLURM_TASKS_PER_NODE` are overwritten for the combined step | heterogeneous_jobs.html; env.c ~1411–1437 |

Each of these is set by slurmstepd's `setup_env`/`exec_task`, independent of the `--mpi` plugin. So `--mpi=none` still exports them (*observed in source*: task.c sets them unconditionally; `mpi_g_slurmstepd_task` only adds plugin variables). **Sound:** §6.1's use of PROCID, LOCALID, and NODEID is correct, and all three are step-relative and dense.

### B2 — `SLURM_NODELIST` is the wrong list (blocking, high)

**Location:** §6.1 table row `SLURM_NODELIST | Hostnames, for the rendezvous address`; §6.2 first bullet.

**Counterexample:** inside `sbatch -N4` (nodes `pi[01-04]`), run `srun -N2 -r2 ./app` or `srun -N2 -x pi01,pi02 ./app`.
- Tasks see `SLURM_NODELIST=SLURM_JOB_NODELIST=pi[01-04]` and `SLURM_STEP_NODELIST=pi[03-04]`.
- On `pi03`, `SLURM_NODEID=0`. Indexing `SLURM_NODELIST` with it gives `pi01`, a node that runs no task.
- Any address derived from "NODEID k → k-th host of SLURM_NODELIST" is wrong.
- `srun --relative` is documented as "Run a job step relative to node n of the current allocation" (srun.html).
- Several steps sharing one allocation is the normal pattern: srun.html and heterogeneous_jobs.html both show `srun … &` pairs.

**Fix:** use `SLURM_STEP_NODELIST` exclusively, with `SLURM_STEP_NUM_NODES` and `SLURM_STEP_NUM_TASKS` for sizes. Keep `SLURM_NTASKS` only as a cross-check, because `-E` makes it job-level.

### B3 — The node map is not in the environment (blocking, high)

**Location:** §6.1: "SLURM_TASKS_PER_NODE | Local member counts for every node" and "The node map falls out of the environment before a single socket is opened."

Per-node counts give the rank→node map only if ranks are contiguous per node **and** node order matches rank order. The *documented* distributions break this:

- **cyclic:** "tasks one and four on the first node, task two on the second node, and task three on the third node" (srun.html `-m`). Counts `2,1,1` cannot distinguish that layout from block. Cyclic "is the default behavior if the number of tasks is no larger than the number of allocated nodes."
- **plane:** "the taskids assigned to each node depend on the plane size."
- **arbitrary:** "tasks will be laid out on the nodes in the order of the file" (`SLURM_HOSTFILE`). A host can appear several times, non-contiguously.
- **Default distribution with a topology plugin:** "If a topology plugin is configured and the user does not specify a distribution method, tasks are distributed following topology order rather than node index order." Even block then assigns rank ranges in a node order different from the order of `SLURM_TASKS_PER_NODE`.
- **`srun -E`:** `SLURM_TASKS_PER_NODE`/`SLURM_NTASKS` keep job-level values (env.c `preserve_env`). Counts and size then disagree with the step.

`SLURM_STEP_TASKS_PER_NODE` fixes only the last case. No environment variable gives the global rank→node map. `SLURM_GTIDS` gives it **only for the local node**. With `--mpi=pmi2`, the full map exists as the KVS key `PMI_process_mapping` (*observed in source*: pmi2 `setup.c:408`), which the design has removed.

**Consequences if unfixed:** wrong local member counts, so §5.2 arrival targets never reach `(k+1)·L` and barrier hangs; wrong node participation; a wrong "lowest team rank among local members" lead.

The design's **own** §8.1 mixed map (`0,0,0,1,2,2`) is contiguous, so the tests would not show this. A cyclic map would (see T2).

**Fix:** stop claiming the map comes from the environment. At rendezvous each node reports `SLURM_GTIDS` (its local ranks, in `LOCALID` order), and the full map is assembled from that. This is exactly what §6.2's broadcast of "the full node map" can carry. The work is one line per node, not per PE (see B4).

### Heterogeneous jobs and MPMD

- **Het with `--het-group=0,1`** (*observed in source*): PROCID and NODEID get het offsets, and `SLURM_JOB_NODELIST`/`SLURM_NODELIST`/`SLURM_STEP_NODELIST`/`SLURM_TASKS_PER_NODE` are overwritten with combined values. Using `STEP_NODELIST` plus GTIDS-based mapping works here. Without `--het-group`, each component is a separate step with its own PROCID space, so an unaware app forms separate "worlds", which is correct.
  - *Unverified, low confidence:* if two components share a physical host, the host could appear under two NODEIDs. It would then be treated as two logical nodes, which is correct but loses the intranode path.
- **MPMD (`--multi-prog`)**: different executables per rank. The spec allows non-SPMD programs (spec §4: "While not required by OpenSHMEM, in typical usage, PEs are executed using a single program…"). §3.13's checksum covers handler-table equality, which a shared libosoe gives even across different executables. It should also cover wire-format version, page size (Pi 4 at 4K mixed with Pi 5 at 16K; see §8.5), and `SHMEM_SYMMETRIC_SIZE`. Phase-1 statics are rejected, so different executables are otherwise harmless. Minor.

### B7 — Launches that set misleading variables (design change needed)

- **Binary run directly in an `sbatch` script** (high confidence). sbatch.html documents `SLURM_NTASKS` ("Set to value of the --ntasks option"), `SLURM_PROCID`, `SLURM_NODEID`, `SLURM_LOCALID`, `SLURM_TASKS_PER_NODE`, and `SLURM_NODELIST` in the batch environment.
  - With `#SBATCH -n16`, one process starts with PROCID=0 and NTASKS=16. It binds, waits for 15 peers, and hangs until the job time limit.
  - **Detector** (*observed in source*): `SLURM_STEP_NUM_TASKS`/`SLURM_STEP_TASKS_PER_NODE` are set only by `env_array_for_step` (the srun path); `env_array_for_batch_job` does not set them. If they are absent, refuse Slurm mode ("run under srun") or fall back to a singleton.
- **`mpirun`/`prterun` inside an allocation** (*recalled, unverified*): Open MPI starts its daemons with `srun`, one per node, and forks the application ranks itself. The application inherits the daemon's `SLURM_PROCID`/`SLURM_NTASKS`, which are node counts. Under the `SLURM_STEP_NUM_TASKS` check, the fork shows N processes on a node all claiming the same PROCID. **Detect duplicate PROCIDs at rendezvous** and abort with a message.
- **No rendezvous timeout anywhere in §6.** Add `OSOE_BOOTSTRAP_TIMEOUT`. On expiry, rank 0 prints the missing ranks or nodes and aborts non-zero. That turns every case above, plus a node that fails to launch, into a diagnosis instead of a time-limit kill.

### B1 — "Rank 0 publishes its resolved address" has no channel (blocking, high)

**Location:** §6.2 bullets 2–3.

With no KVS, no PMI, and no shared filesystem assumed, "publish" has nowhere to publish to. Non-zero ranks cannot compute rank 0's host (B3: under cyclic, topology, or arbitrary layouts, rank 0's node is not known from the environment). "Retry on bind conflict" makes the port unknowable as well. As written, the rendezvous cannot start.

**A deterministic fix that fits the dependency budget:**

1. **Rendezvous host = step node 0, not rank 0.** `SLURM_NODEID` is the index in `SLURM_STEP_NODELIST` order (above), so every task can compute `hostlist_first(SLURM_STEP_NODELIST)`, whatever the distribution. The listener is the task with `NODEID==0 && LOCALID==0`. Only the **first** hostlist element is needed, though the full parser is still useful.
2. **Port = a deterministic candidate sequence**, e.g. `base + hash(SLURM_JOB_ID, SLURM_STEP_ID, i) mod range` for i = 0..K. The listener binds the first free candidate. Clients try candidates in order, and the handshake carries `(job id, step id, wire version, table checksum)` so a wrong listener is rejected.
   - The job/step id also separates concurrent steps on one node, which §6.2's retry was meant to handle.
   - `srun --resv-ports`/`SLURM_STEP_RESV_PORTS` is *documented* as "Used for native Cray's PMI only" and needs `MpiParams=ports=` configured, so it is not a general answer.
3. **Name resolution:** step node lists contain Slurm `NodeName`s. slurm.conf.html: `NodeAddr` is "Name that a node should be referred to in establishing a communications path… used as an argument to the getaddrinfo()", and it may differ from NodeName. On a multi-homed Pi cluster the resolved address may be on the management network. Provide `OSOE_BOOTSTRAP_IF`/address override and document it. *Clarification needed.*
4. **Alternative escape hatch that still links nothing:** Slurm's `mpi/pmi2` plugin gives every task `PMI_FD`, `PMI_RANK`, `PMI_SIZE`, and `PMI_JOBID` (*observed*: `mpi_pmi2.c:90–96`), over a socketpair to slurmstepd (`setup.c:362`). Speaking the PMI-2 text wire protocol directly over `PMI_FD` needs no `libpmi2`. It provides a KVS (publish/lookup) and `PMI_process_mapping`, which removes B1 and B3 at once. *The wire-protocol details are recalled, not verified here.* §6.3 frames PMI-2 only as a link-time backend, and a wire client dissolves that dependency objection.

### Hostlist parser, "call it fifty lines"

Correct for the one-dimensional forms `pi[01-04,07]`, zero padding, and comma-joined prefixes `a[1-2],b3`. *Recalled, unverified:* Slurm hostlists also allow suffixes after the bracket and several bracket groups in some versions. Tier-1 fixtures should come from `scontrol show hostnames` on a real cluster. Minor.

### B4 — Rank 0 accepting every PE (design change needed, high)

§6.2: "Rank 0 binds a listening socket; every other PE connects." At the §1 design point (256 PEs per node × 300 nodes ≈ 76,800 PEs), rank 0 needs ~76,800 simultaneous fds and absorbs a SYN burst of the same size. This contradicts §1: "Nothing may scale with PE count where it can scale with node count."

**Limits** (*observed in the VM*; *documented* for Slurm):

- Soft `RLIMIT_NOFILE`=1024 (hard 524288).
- `somaxconn`=4096, `tcp_max_syn_backlog`=512, `tcp_syn_retries`=6 (≈127 s before `ETIMEDOUT`, *recalled* backoff 1+2+4+8+16+32+64).
- Slurm propagates the submit shell's **soft** limits by default: "If neither PropagateResourceLimits or PropagateResourceLimitsExcept are configured … the default action is to propagate all limits" (slurm.conf.html).
- So, unless init raises the soft limit, rank 0 hits `EMFILE` at connection 1021.

**Experiment** (`scratchpad/exp/bootstrap/burst/burst.c`, run in the VM): N nonblocking clients from 127.1.x.y sources (256 per source IP, modelling nodes) connect at once to one listener that accepts and reads a 16-byte hello serially.

| N | backlog | per-conn work | time to all hellos | ListenOverflows | SYN retransmits |
|---|---|---|---|---|---|
| 2,000 | 4096 | 0 | 0.14 s | 0 | 0 |
| 20,000 | 4096 | 0 | 5.3 s | 15,902 | 34,443 |
| 20,000 | 4096 | 50 µs | 7.4 s | 15,902 | 42,204 |
| 60,000 | 4096 | 0 | 19.7 s | 55,902 | 171,776 |
| 60,000 | 4096 | 20 µs | **> 300 s (killed by `timeout`)** | 155,432 | 233,480 (+8,877 syncookies) |
| 60,000 | 128 | 0 | **> 300 s (killed by `timeout`)** | 174,595 | 290,835 (+45,889 syncookies) |

This is loopback with zero RTT, and the times come from SYN retransmit backoff. Across a real 1 GbE fabric with ~300 senders, accept-side stalls will be worse, and the two >300 s rows show that as little as 20 µs of per-connection accept work, or a small backlog, pushes rendezvous past the 127 s SYN-retry `ETIMEDOUT` window.

**Fix:**
1. **Only one PE per node (`LOCALID==0`) talks TCP to the rendezvous.** That is ~300 connections, which the 2,000 row shows is trivial.
2. **Local PEs meet on a node-local socket instead** (see B5).
3. **Rendezvous connects retry with jittered backoff** under the B7 timeout.

### B5 — Missing node-local bootstrap and hosting process (design change needed, high)

The design never says which **process** hosts the progress-thread pool, the io_uring rings, and the node-pair sockets. PEs are separate Slurm tasks (siblings, not parent and child), and a `grep` for fork/daemon/helper/owner/SCM_RIGHTS/pidfd in the design finds nothing relevant. Consequences:

- **Segment sharing:** §4.1 uses a `memfd`, which has no name. §6.2 "exchanges the segment key" over TCP to rank 0, but a memfd can only reach a sibling by `SCM_RIGHTS` over a Unix socket, or via `/proc/<pid>/fd/N`. The latter needs ptrace access, and §4.3 itself notes Yama `ptrace_scope=1` blocks siblings. So a node-local Unix socket is required. An abstract name keyed by `jobid.stepid.nodeid` also fixes the §8.1 "segment name keyed on logical node id" collision across concurrent jobs, and leaves nothing in `/dev/shm` after a crash.
- **Hosting choice** must be explicit. Options:
  - (a) Threads in the `LOCALID==0` PE. That PE's death kills the node's transport (E1). Its CPU affinity also comes from Slurm task binding, so the progress thread shares the lead's bound core. That is at odds with §3.11 "use cpuset isolation for the progress thread", because the step cgroup contains only allocated CPUs (*recalled*: task/cgroup behaviour).
  - (b) A helper process forked from the lead. It survives the lead's user code, but inherits the fork-in-threaded-process hazards and must die with the step.
  - Either way, the ring's `IORING_SETUP_SINGLE_ISSUER` binds submission to the creating task, which holds under (a) or (b).
- **Recommendation:** record the choice in §3.12/§6, and state the order: node-local Unix rendezvous → lead-only TCP rendezvous → node-pair mesh → broadcast.

### B6 — Node-pair connection setup unspecified (clarification needed)

At 300 nodes that is 44,850 connections and 299 sockets per node. Nothing says:

- **Direction.** Proposal: lower NODEID connects to higher, which avoids duplicate pairs and simultaneous-open ties. With the other rule each node accepts at most 299 connections, well inside a 4096 backlog.
- **Timing.** Eager setup at init is implied by the §3.3 "SQ = 2×connections, assert at init" sizing. Lazy setup would invalidate that assert.
- **Handshake.** It should re-verify job/step id and NODEID, so a stale connection from a previous job, or a previous init cycle (L3), is rejected.
- **Setup order.** `TCP_NODELAY`, plus the E3 timeouts, must be set before the first byte.

Eager full mesh also costs ~300 fds per node-lead and N²/2 handshakes at init, which is modest at 300 nodes. State the ceiling.

### B8 — §3.13 "exchange through PMI" (minor, high)

Contradicts §6 ("No PMI link dependency") and §10 ("Nothing links against PMIx or PMI-2 on the default path"). Carry the checksum, together with wire version, page size, and `SHMEM_SYMMETRIC_SIZE`, in the rendezvous hello.

### Minor: environment prefix contradiction

§6.2 says non-Slurm launch uses "the same variables under a `SHMEM_` prefix that takes precedence". §8.1 uses `SHMEM_NODE_MAP`. §10 says implementation variables are `OSOE_` "since the spec reserves `SHMEM_`". The spec's wording (§8) is softer: "The implementations of the specification are free to define additional variables." Pick `OSOE_` consistently. Minor, high.

---

## Part 2 — Lifecycle (§6.4) against spec §9.1.1, §9.1.4, §9.1.5, §9.2.1

Spec text (quoted from `spec16.txt`):

- §9.1.1: "The shmem_init and shmem_init_thread initialization routines may be called multiple times within an OpenSHMEM program. A corresponding call to shmem_finalize must be made for each call to an OpenSHMEM initialization routine. The OpenSHMEM library must not be finalized until after the last call to shmem_finalize and may be re-initialized with a subsequent call to an initialization routine." Also: "It is a collective operation that all PEs must call before any other OpenSHMEM routine may be called, except shmem_query_initialized".
- §9.1.4: "This is a collective operation that requires all PEs to participate in the call." "The last call to shmem_finalize performs an implicit global barrier … **All other calls to shmem_finalize perform an operation semantically equivalent to shmem_barrier_all** and return without freeing any OpenSHMEM resources."
- §9.1.5: "This function may be called at any time, regardless of the thread safety level or the current initialized state".
- §9.2 rule 1: "the shmem_finalize call must be invoked by the same thread that called shmem_init_thread." §9.2.1 Notes: "The OpenSHMEM library may not be able to change the level of threading support provided after the first initialization call has been made."
- §9.1.13 `start_pes`: "the OpenSHMEM library is implicitly finalized when the program exits. Implicit finalization is collective and includes a global synchronization".

### L1 — Non-last finalize must barrier (design change needed, small; high)

§6.4: "`shmem_finalize` decrements; the last one tears down." The spec requires every non-last finalize to be semantically `shmem_barrier_all`, which includes the quiet of §5.6.

**Failure scenario:** a PE calls `init; init; put_nbi…; finalize` and expects completion at the inner finalize. Nothing is quieted, so a peer reading the target after its own inner finalize sees stale data. Conformance bug.

### L2 — Clarifications (high)

- **Concurrent init.** "Subsequent calls increment and return" is wrong if a second thread calls init while the first is still inside bootstrap: it returns before the library is usable. Use a once-style gate (mutex plus a state word), not a bare atomic count. `shmem_query_initialized` must stay lock-free-readable (§9.1.5), and should report nonzero only after bootstrap completes.
- **Thread level:** latch at first init. Later `shmem_init_thread` calls return the latched `provided` value (§9.2.1 Notes).
- **Same-thread finalize** (§9.2 rule 1): decide whether to assert it in debug builds.
- **`start_pes`** is required (§9.1.13). It needs an `atexit` handler that performs the collective final barrier. That handler runs after `main` returns, when a user thread may still be alive, and must interact correctly with the refcount. Not mentioned in §6.4 or §7. Also decide whether an app that calls `shmem_init` and exits without `shmem_finalize` gets the same `atexit` treatment. SOS-style implementations do something here (*recalled*).
- **`max_teams` latch**: §6.4 already flags this. It is sound as an open item.

### L3 — Re-init as a fresh job (sound, with caveats; medium)

Under Slurm, re-init is possible because init is collective (§9.1.1): every PE re-enters init, and since the environment is unchanged, PE numbering is identical. §6.4's "re-run the whole of §6" is right. Caveats:

- It depends on B1's deterministic rendezvous. A "retry on bind conflict" port cannot be rediscovered.
- The previous cycle's TCP connections may be in TIME_WAIT. Bind the listener with `SO_REUSEADDR`, and make the B6 handshake include an init generation so a straggler from cycle k cannot join cycle k+1.
- The §4.1 `MAP_FIXED_NOREPLACE` range must be fully unmapped at last finalize, or re-init "fails loudly" by construction.
- *Recalled, low confidence:* io_uring ring teardown releases registered files and buffers asynchronously (`io_ring_exit_work`). Close sockets explicitly before `io_uring_queue_exit` so ports are freed on time.

Spec §4 ("concludes its use … when all PEs make their final call to shmem_finalize") and §9.1.1 ("may be re-initialized") pull in slightly different directions. Supporting re-init is the conforming reading.

---

## Part 3 — Error model (§3.15) and exit

### E1 — A single PE crash hangs the job (design change needed, high)

**Slurm defaults, documented:**
- slurm.conf.html `KillOnBadExit`: "With the default value of 0, if one of the processes is crashed or aborted the other processes will continue to run".
- `WaitTime`: "The default value is 0, which disables this feature."
- srun.html `-K`: "If this option is not specified, the default action will be based upon the Slurm configuration parameter of KillOnBadExit."
- *Observed in source*, `src/srun/launch.c` ~436: `if (task_state_first_abnormal_exit(...) && _kill_on_bad_exit()) _step_signal(SIG_TERM_KILL);` — no kill otherwise.

**Scenario:**
1. A non-lead PE on node A segfaults between CAS and `filled += n` (§3.5).
2. `filled` never equals `head`, so that segment is never submitted.
3. §3.6's stall becomes permanent for every PE on A targeting that peer node.
4. The node-pair TCP connection stays healthy, because the process owning it did not die, so §3.15's "connection dying → abort loudly" never fires.
5. The safety sweep (§3.10) resubmits only sealed-and-complete buffers.
6. Every PE ends up in futex waits with sweep timeouts, and the job burns its time limit.

The same happens when a PE dies holding any intranode collective slot: a §5.4 `arrived` target is never reached.

**Fix:**
- The progress thread monitors every local PE's liveness. Pidfds from the node-local rendezvous (B5), polled on the existing ring with `IORING_OP_POLL_ADD` (pidfd polling needs Linux 5.3+, *recalled*), cost one SQE per local PE. Size the SQ for them.
- On unexpected exit (not after finalize): `SIGKILL` the remaining local PEs, send an abort AM on every node-pair connection, and exit non-zero.
- Remote progress threads do the same on receipt, or on EOF/RST.
- Document `srun -K1` as recommended, but do not rely on it.

### E2 — `shmem_global_exit` over this design (design change needed, medium-high)

Spec §9.1.6:
- "it results in the immediate notification to all PEs to terminate."
- "shmem_global_exit flushes I/O and releases resources in accordance with C/C++ language requirements for normal program termination."
- "the exit status returned to the environment shall be one of the values passed to shmem_global_exit".
- Notes: "all PEs must terminate regardless of their current execution state."

Problems:
- **Reaching PEs that are computing.** §3.12 ("The origin side never needs a helper") means there is no thread in each PE process to receive the notification. A PE in pure computation never enters the library. Options:
  - A per-PE watcher thread blocked on a futex word in the node segment, which calls `exit(status)`. That runs `atexit` and stdio flush concurrently with a running `main` thread, which is racy but common practice (*recalled*).
  - A signal, whose handler cannot legally call `exit`.
  - The design must pick one.
- **Exit status under Slurm.** If survivors are killed by signal (for example by the E1 kill path, or by Slurm with `-K1` after the first PE exits with `status`), srun reports the signal (*observed*: `launch.c` keeps a signalled `local_global_rc` in preference to exit codes). The spec's "shall be one of the values passed" is then violated. So each PE must `exit(status)` itself, and the kill fallback must come only after a grace period.
- **Propagation.** Send over node-pair connections (abort AM, control queue), then set a node-segment futex word for local PEs. It must not depend on the calling PE's own node transport being healthy. If this PE's node is the problem, `exit` anyway; E1's liveness path will propagate it.

### E3 — Silent node loss (clarification needed, medium)

A node losing power sends no FIN. With data in flight, Linux retransmits until `tcp_retries2`, ~15 min (*recalled*). An idle connection never notices; keepalive defaults to 2 h (*recalled*). Slurm will eventually act: srun.html `--no-kill`: "The default action is to terminate the job upon node failure", after `SlurmdTimeout`, whose value was not checked. Set `TCP_USER_TIMEOUT` plus keepalive, or piggyback a heartbeat on §3.10's 100 ms sweep, so "abort loudly" happens in seconds.

---

## Part 4 — Testing (§8)

### T1 — CI toolchain on `ubuntu-24.04` cannot configure (blocking for §8.5 as written, high)

- `packages.ubuntu.com/noble/liburing-dev`: **2.5-1build1**. Launchpad shows only `2.5-1build1 Release` for noble; nothing in updates or backports.
- `CMakeLists.txt` stops with FATAL_ERROR below 2.6, so both `ubuntu-24.04` and `ubuntu-24.04-arm` fail at configure.
- The kernel is not the problem: both 24.04 images report `6.17.0-1022-azure`, and 26.04 images report `7.0.0-1012-azure`.
- `ubuntu-26.04` and `ubuntu-26.04-arm` runner images exist (runner-images repo), and `packages.ubuntu.com/resolute/liburing-dev` is **2.14-1**.
- **Fix:** use the 26.04 runners, consistent with §1's "require a new enough OS".
- `kernel.io_uring_disabled` on the runners: *not verifiable from here*. The VM shows 0. Keep `env.probe` as the first CI test so a disabled io_uring fails with a clear line.
- The x86 leg also compiles `memory_order_acq_rel` (§3.5), which GCC 14 on Debian aarch64 accepts (below). The x86 headers were *not checked*.

### T2 — Bootstrap coverage claim is false (design change needed, high)

§6.2: "The tier-1 and tier-2 harnesses of §8 use this path, which means the bootstrap is exercised by every test."

- Tier 1 is "In-process AM backend, queues not sockets, single-threaded", so no rendezvous runs.
- Tier 2 uses the `SHMEM_`/`OSOE_` override, so none of the Slurm-specific logic runs: STEP vs JOB lists, `-E`, cyclic GTIDS, het offsets, the batch-script detector, duplicate PROCIDs. B2, B3, and B7 would all reach the cluster untested.

**Fix:**
- Add tier-1 **environment fixtures**: captured `env` dumps from `srun` under block, cyclic, plane, arbitrary, `-r/-x` subset steps, `-E`, het `--het-group=0,1`, and a bare `sbatch` script. Feed each to the pure env→map function, and assert either the map or a refusal.
- Make the tier-2 launcher emulate Slurm (set `SLURM_*` exactly as `env.c` would, including a cyclic layout), rather than only the override prefix.
- Add a cyclic §8.1 pole such as `0,1,2,0,1,2`. The listed mixed map `0,0,0,1,2,2` is still contiguous and misses B3's class.

### T3 — Invariant 7 cannot fail in tests (design change needed, high)

§4.2 pursues identical-VA mapping "as well", with `MAP_FIXED_NOREPLACE` at a fixed range. Every test process therefore has identical heap addresses, and a wire message that leaks a raw pointer passes every tier.

**Fix:** a test/debug option that maps each logical node's heap at a different base, and disables `shmem_ptr` fast paths in that mode. Run it in tier 2.

### T4 — Invariants against tiers

The "realistic?" column judges whether the test catches a violation reliably, not whether the test can run.

| # | Invariant | Tier / mechanism that can detect a violation | Realistic? |
|---|---|---|---|
| 1 | Handler may enqueue, never reserve | Not observable in tier 1 (no send completions to withhold) unless the in-process backend models "both buffers occupied". Tier 2/3 only under load. **Best detector:** make it unrepresentable (handler context type exposes no reserve) plus a debug assert "reserve on progress thread" | Only by construction or assert |
| 2 | One in-flight send per connection | Tier 1 cannot (no io_uring). Tier 2/3: small `SO_SNDBUF` does produce short sends. *Experiment*: io_uring `send` of 1 MB with `SO_SNDBUF=4096` completed short in 20/20 attempts (`res=32741`), blocking and nonblocking socket alike (`exp/bootstrap/shortsend.c`). So the poll-arm/retry paths where reordering can occur are reached. Detection relies on the receiver's §3.4 sequence assert and a sender-side "in-flight" assert. A `MSG_WAITALL` variant was inconclusive (harness hang) | Yes via asserts; tests alone unlikely |
| 3 | Single stream per node pair | Structural: connection table has one fd per peer; assert at setup | Assert |
| 4 | Progress thread never waits for filled==head | Tier 2 oversubscribed (e.g. 16 PEs on 4 vCPUs) plus §3.14 fault injection delaying between CAS and `filled +=`. Needs test timeouts (T5) | Yes with injection |
| 5 | Signals land after payload | Tier 1 deterministic ordering test through the real chunker. **Tier 3 netem reorder does not help:** TCP re-sequences, so the app only sees different segmentation and timing | Tier 1 yes; tier 3 no |
| 6 | Allocator deterministic | Tier 1 unit test plus §4.4 checksum at barrier | Yes |
| 7 | Wire format independent of VA | No tier as designed (T3) | **No, until T3** |
| 8 | Wakeup affects only how soon | Fault injection "drop wake" at the wake site, then assert completion within the sweep and count sweep-submits | Yes with injection |
| 9 | Barrier bounds need blocking collectives | Not testable until nonblocking collectives exist; compile-time/type guard (§5.4) | N/A |
| 10 | Epoch equality needs bounded skew | Tier 2 oversubscribed with an injected delay of the lead; a violation shows as a hang. Exhaustive interleaving model is better | Partial |
| 11 | AMO descriptors carry values | Tier 1: force reservation failure (injected alloc fail) so an AMO result is deferred, then a later put to the same location | Yes if tier 1 exercises deferral |
| 12 | CQ sized from pbuf ring | Init assert. Add: assert CQ overflow counter (`*ring.cq.koverflow`) and `IORING_SQ_CQ_OVERFLOW` are zero at quiescence; `IORING_FEAT_NODROP` otherwise hides it | Yes with the added assert |
| 13 | Watch insert before check | §4.9 itself says no test will show it. Needs an exhaustive interleaving model, or a debug yield point between insert and predicate | Model only |
| 14 | TCP_NODELAY always | Trivial `getsockopt` assert at setup; tier 3 one-way latency test with netem delay as a backstop | Yes |
| 15 | Nothing links but libc/liburing | No test yet. Add a `readelf -d`/`NEEDED` allowlist test beside `shmem.exports` | Easy, missing |
| 16 | Lead single writer, leaves last | Tier 2 in-place reductions plus injected lead delay before exit | Yes with injection |
| 17 | Read source only after peer called | Tier 2: a PE fills its source immediately before calling, after a delay; pull implementations read stale data | Yes |
| 18 | 32-bit futex, absolute deadlines | Probe (U64 `EINVAL` confirmed below), unit test of the deadline helper, timeout-rate counter | Yes |

**Weak memory ordering on Apple Silicon (§8.3, §8.5 "exercised on every run"):**
- *Observed:* Debian GCC 14.2 on aarch64 emits different code for the orderings: `memory_order_relaxed` load → `ldr`, `memory_order_acquire` → `ldar`, fetch_add release → `__aarch64_ldadd4_rel`, CAS `memory_order_acq_rel` → `__aarch64_cas4_acq_rel` (`exp/bootstrap/mo.c`). So the §3.5 annotations compile and matter to codegen on this toolchain.
- Running on native aarch64 makes violations **possible**, not **observed**. Whether M-series cores under `vz` expose store-buffer-visible effects at a test-observable rate is *unverified*.
- The design's "Native aarch64, so weak memory ordering is exercised on every run" overstates this.
- Treat §3.5/§4.9 ordering as something to model-check, or to assert with a debug-only validation, not as covered by the VM loop. Medium confidence.

**Tier 3 environment claim (§8.5) — sound, verified.** In the VM (kernel `6.12.95+deb13-cloud-arm64`):
- `sch_netem.ko.xz` is present and loads, as does `veth`.
- Two netns joined by veth took `netem delay 5ms 1ms loss 1% reorder 25% 50%` and MTU 576, and ping over them showed 5/5 received with rtt avg 6.4 ms.
- It also works **unprivileged**: `unshare -rn` then veth plus `tc qdisc add … netem` succeeded, so tier 3 need not require sudo in the VM. On Ubuntu runners AppArmor may restrict unprivileged user namespaces (*recalled*), but sudo is available there.
- Reminder from invariant 5: netem reorder is invisible to the application over TCP. Its value is segmentation, loss-induced stalls, and timing.

### T5 — No test timeouts (minor, high)

`shmem/tests/CMakeLists.txt` and `tools/probe/CMakeLists.txt` set no `TIMEOUT`. The ctest default is 1500 s (*recalled*). Hang-shaped failures (invariants 1, 4, 8, 10, 13; E1) should fail in seconds. Set a per-label `TIMEOUT`, and have the rendezvous timeout of B7 honour it.

### T6 — Probe coverage against claims (minor, high)

- §3.3 says the probe verified "exactly 8 data CQEs … then one terminal -ENOBUFS". The probe checks only `ncqe > 1` and "an ENOBUFS seen". Observed output `cqes=9 with_F_MORE=8` matches the claim, but a regression to 2 CQEs would still pass. Assert `more == NBUF && ncqe == NBUF+1`.
- The cross-process futex check uses parent and child after `fork`. PEs are siblings. Futex keys on a shared mapping are inode+offset either way (*recalled*), so this is likely fine.
- The probe does not check `RLIMIT_NOFILE` against the planned connection count, or netns/netem availability for tier 3. §8.5's "verified" netem claim is not backed by the probe; it is confirmed above.
- `probe.c` ignores `ftruncate`/`bind`/`listen` return values. No warnings under the current flags. Minor.

---

## Part 5 — Build in the VM

`cmake --workflow --preset debug` on a `git archive HEAD` copy at `/tmp/osoe-q1`, kept out of the repository's `build/`:

```
-- Checking for module 'liburing>=2.6'
--   Found liburing, version 2.9
[100%] Built target probe
1/3 Test #1: shmem.info .......................   Passed    0.00 sec
2/3 Test #2: shmem.exports ....................   Passed    0.01 sec
3/3 Test #3: env.probe ........................   Passed    0.31 sec
100% tests passed, 0 tests failed out of 3
```

`release`, `asan`, and `tsan` workflows: all exit 0 with 3/3 passed, and no compiler warnings in any log.

Probe output:
```
info kernel 6.12.95+deb13-cloud-arm64 aarch64, page size 4096, liburing 2.9
ok   futex2 U32 wait (value mismatch)
info futex2 U64 wait  r=-1 errno=Invalid argument -> U64 NOT implemented
ok   cross-process futex on memfd mapping   woke=1 child_exit=0
ok   SINGLE_ISSUER + CQSIZE                 r=0 sq=64 cq=4096
ok   opcode FUTEX_WAIT / RECV
ok   uring futex_wait stale value -> -EAGAIN  res=-11
ok   register provided buffer ring / TCP_NODELAY
ok   multishot recv: >1 CQE from one SQE    cqes=9 with_F_MORE=8
ok   buffer ring exhaustion -> -ENOBUFS
PROBE PASSED (0 failures)
```

Minor doc drift: §8.5 says `limactl shell osoe …`, and `create.sh` defaults the instance to `osoe`. The instance in use here is named `shmem`. Harmless, but workflow text should say `<instance>`.

---

## Sound, with evidence

- **S1.** `SLURM_PROCID`, `SLURM_LOCALID`, and `SLURM_NODEID` are correct choices: step-relative, dense, het-offset, and set whatever `--mpi` is (*observed*: task.c, slurmstepd_job.c).
- **S2.** §6.2's "Do not rely on the default [block distribution]" is right, and more so than stated: with a topology plugin even the default is not node-index order (srun.html).
- **S3.** Removing the PMIx/PMI-2 **link** dependency is achievable; B1's deterministic scheme or a PMI-2 wire client keeps it.
- **S4.** Tier-3 kernel facilities exist in the cloud kernel and work unprivileged (verified above).
- **S5.** Kernel requirements are met by the CI kernels (6.17, 7.0) and the VM (6.12). Only the userland liburing version fails on 24.04 (T1).
- **S6.** Build system: presets, Linux-only guard, in-source refusal, and export check all work. Four presets build and test clean.
- **S7.** Re-init as a fresh rendezvous is feasible under Slurm, since init is collective and numbering comes from an unchanged environment (L3), once B1 is fixed.
- **S8.** Small `SO_SNDBUF` does produce short io_uring send completions on loopback, so §8.2's mitigation reaches the partial-write path (experiment above).
- **S9.** §3.15's "abort the job loudly, don't recover" is the right policy. The gaps are in detection and propagation (E1–E3), not the policy.
