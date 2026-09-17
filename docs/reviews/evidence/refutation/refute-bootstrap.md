# Refutation pass: bootstrap, lifecycle, error model, testing

Scope: q1-bootstrap.md findings B1, B2, B3, B4, B7, L1, E1, E2, T1, T2, T3, and q1-omissions.md O4, O8 (profiling part), O10.
Design doc: `docs/design/shmem-ethernet-design.md` at HEAD 9475e45. Line numbers below are file lines.
Sources I fetched this pass (2026-09-16) are saved under `scratchpad/exp/refute-boot/`:
- `src/common/slurm_step_layout.c`, `src/api/step_launch.c`, `src/common/hostlist.c`, `src/stepmgr/stepmgr.c`, all from https://raw.githubusercontent.com/SchedMD/slurm/master/...
- `actions/runner-images` README and the Ubuntu2604/2404 readmes.
- Launchpad API `getPublishedSources` for liburing (noble, resolute).

I re-read the reviewer's saved copies of `env.c`, `task.c`, `slurmstepd_job.c`, `srun.txt`, `sbatch.txt`, and `slurm.conf.txt` in `scratchpad/exp/bootstrap/`.

Labels: **documented** (man page or spec text), **observed in source**, **recalled**.

**Not done:** the user rejected the VM rerun of the B4 connection-burst experiment (a paced-arrival variant). B4 is judged by reading `burst.c` and the design text, without new measurements.

---

## Verdict table

| ID | Verdict | New severity |
|---|---|---|
| B1 rank 0 cannot publish its address | SURVIVES-DOWNGRADED | design change needed (small; an obvious KVS-free fix exists) |
| B2 SLURM_NODELIST is job-level, NODEID is step-level | SURVIVES-DOWNGRADED | design change needed (one-variable fix) |
| B3 TASKS_PER_NODE gives counts, not placement | SURVIVES-DOWNGRADED | design change needed |
| B4 all PEs connect to rank 0 | SURVIVES | design change needed (the timing numbers are overstated) |
| B7 binary run directly in sbatch hangs; no timeout | SURVIVES-DOWNGRADED | minor (usability) |
| L1 / O10 non-last finalize must act as barrier_all | SURVIVES | design change needed (one line) |
| E1 one crash hangs the job | SURVIVES-DOWNGRADED | clarification needed (require `srun -K`, add timeouts); in-library liveness is optional |
| E2 / O4 shmem_global_exit | SURVIVES | design change needed |
| T1 CI on ubuntu-24.04 cannot configure | SURVIVES-DOWNGRADED | minor (doc/CI label; no CI file exists yet) |
| T2 bootstrap coverage claim is false | SURVIVES-DOWNGRADED | clarification needed (test plan) |
| T3 invariant 7 untestable | SURVIVES-DOWNGRADED | minor (add a test mode) |
| O8 profiling vs shmem.exports conflict | conflict REFUTED; the omission of the profiling interface SURVIVES | clarification needed |

---

## B1: no channel for rank 0 to publish its address

**Restatement.** §6.2 says rank 0 "publishes" its address and port. With no KVS, PMI, or shared filesystem there is nowhere to publish to. Rank 0's host cannot be computed, and "retry on bind conflict" makes the port unknowable.

**Doc text.**
- L1322–1324: "Rank 0 binds a listening socket; every other PE connects … One round trip, no KVS".
- L1332–1335: "Under Slurm's default block distribution, rank 0 is on the first expanded host. **Do not rely on the default.** Have rank 0 publish its resolved address and port, and have everyone else learn it the same way regardless of distribution policy."
- L1336–1337: "Port: an environment variable with a default, plus retry on bind conflict".

Nothing else in §6 names a channel. The defect is real as written.

**Is SLURM_PROCID 0 always on step node 0?** No. Observed in source, `slurm_step_layout.c` (master), function by function:

- `_init_task_layout` dispatch order: `NODECYCLIC` → `_task_layout_cyclic`, `ARBITRARY` → `_task_layout_hostfile`, `PLANE` → `_task_layout_plane`, `node_ranks` present → `_task_layout_topo`, otherwise `_task_layout_block`.
- **block:** `tids[i][j] = task_id++` for i from 0 upward. Task 0 is on node index 0. This also holds with pack_nodes.
- **cyclic:** the outer loop starts at j=0, i=0 and assigns taskid 0 to node 0 (`cpus[i] >= 1` is forced). Task 0 is on node 0.
- **plane:** the distribution loop starts at i=0. Task 0 is on node 0.
- **topology** (the default distribution when a topology plugin supplies `node_ranks`): `order_map` is `qsort_r` by `node_ranks`, and task ids are handed out in `order_map` order. **Task 0 is on `order_map[0]`, which need not be node 0.** This matches srun.html (documented): "tasks are distributed following topology order rather than node index order."
- **arbitrary:** `slurm_step_layout_create` sets `node_list` to the `hostlist_uniq`'d list, and `hostlist_uniq` calls `qsort` on the ranges (hostlist.c). `_task_layout_hostfile` walks that sorted list, and each task id is its position in the hostfile. With `SLURM_HOSTFILE` = `pi03,pi01`, node 0 is pi01 but task 0 is on pi03. **Not node 0.**

**Is there a KVS-free scheme that fits the doc?** Yes. Observed in source:

- `SLURM_NODEID` = `nodelist_find(msg->complete_nodelist, conf->node_name)` (slurmstepd_job.c:267).
- `complete_nodelist` = `ctx->step_resp->step_layout->node_list` (step_launch.c:353–354).
- `SLURM_STEP_NODELIST` is set from the same `step->step_layout->node_list` (env.c:1417–1420).

So "the host at index 0 of `SLURM_STEP_NODELIST`" and "the task with `SLURM_NODEID==0`" always agree, under every distribution. Het jobs with `--het-group` overwrite both with combined values (env.c:1412–1415).

The reviewer's fix is therefore valid:
- The listener is `NODEID==0 && LOCALID==0`.
- The port is deterministic: a fixed base, or a hash of `(SLURM_JOB_ID, SLURM_STEP_ID)`, over a short candidate list, with a handshake that checks job and step id.

Every task can compute this without publishing anything. It fits §1's dependency budget and the "no KVS" line. The doc even gestures at it ("everyone else learn it the same way"), but pins the role to *rank* 0.

**Why downgraded from blocking.** The defect is one paragraph of design text. The fix needs no new dependency or mechanism, and it does not disturb any other section.

**Tightest statement.** §6.2 assigns the rendezvous to PE rank 0, whose host is not derivable from the environment under topology-ordered or arbitrary distributions (observed in source). It also makes the port non-deterministic, and gives no channel to publish either.

**Fix.** The rendezvous host is `SLURM_STEP_NODELIST[0]`, with the listener at `NODEID==0, LOCALID==0`. Use a deterministic port candidate sequence keyed on job and step id; the handshake verifies job, step, and wire version. Note NodeName vs NodeAddr resolution (slurm.conf.html) and provide an address override.

---

## B2: SLURM_NODELIST is job-level, SLURM_NODEID is step-relative

**Restatement.** §6.1 (L1307) takes hostnames from `SLURM_NODELIST`. Inside an allocation, a step on a subset of nodes gets the job's list, while `NODEID` indexes the step's list.

**Verification.**
- Documented, srun.html (`srun.txt`):
  - `SLURM_NODEID`: "The relative node ID of the current node."
  - `SLURM_STEP_NODELIST`: "List of nodes allocated to the step."
  - `SLURM_JOB_NODELIST`: "List of nodes allocated to the job."
  - The man page's own example runs `echo $SLURM_JOB_NODELIST; srun -lN2 -r2 hostname` (srun.txt:4648–4649).
- Documented, sbatch.html: `SLURM_NODELIST`: "List of nodes allocated to the job. See SLURM_JOB_NODELIST."
- Observed in source, `env.c` `env_array_for_step`:
  - L1412–1415: only for het steps is `SLURM_NODELIST` overwritten.
  - L1417–1418: otherwise `env_array_append_fmt(dest, "SLURM_JOB_NODELIST", ...)`, which does not overwrite an inherited value.
  - L1420: `SLURM_STEP_NODELIST` is always overwritten.
- slurmstepd could have overwritten `SLURM_NODELIST` later: task.c:365 calls `setup_env(step->envtp, false)`, and `setup_env` sets `SLURM_NODELIST` when `env->nodelist` is set (env.c:763–764). I checked this. `envtp` is `xmalloc`'d (zeroed) in slurmstepd_job.c:396, and nothing in task.c, slurmstepd_job.c, or mgr.c assigns `envtp->nodelist`. So the value is not overwritten.

The finding holds. When srun creates its own allocation, job list = step list and the bug is invisible. It bites exactly on `srun -r`, `srun -N<k>` subsets, `-x`, and multiple steps inside one `sbatch`, which is the normal batch pattern.

**Downgraded** from blocking to design change needed: the fix is to name a different variable.

**Fix.** Use `SLURM_STEP_NODELIST`, `SLURM_STEP_NUM_NODES`, and `SLURM_STEP_NUM_TASKS`. `SLURM_NTASKS` is only a cross-check (see B3 on `-E`).

---

## B3: SLURM_TASKS_PER_NODE gives counts, not placement

**Restatement.** §6.1 (L1306, L1312–1313) claims "the node map falls out of the environment". Per-node counts give the rank→node map only for block-like, node-ordered layouts.

**Verification.**
- Observed in source (above): cyclic interleaves task ids across nodes. Topology order assigns contiguous blocks, but in `node_ranks` order rather than list order. Arbitrary follows hostfile order. In all three, the count vector alone does not recover which ranks are on which node.
- Documented, srun.txt:1177: cyclic example "tasks one and four on the first node…". srun.txt:1153: topology order. srun.txt:1210: arbitrary "laid out … in the order of the file."
- **Is SLURM_TASKS_PER_NODE step-level under srun?** Yes, unless `srun -E/--preserve-env`. Observed in source:
  - env.c:1440–1445: `tpn` is built from `step->step_layout->tasks`, and `if (!preserve_env) overwrite SLURM_TASKS_PER_NODE`.
  - env.c:1447: `SLURM_STEP_TASKS_PER_NODE` is always set.
  - env.c:1459–1461: `SLURM_NTASKS` is also skipped under `-E`.
  - Het steps overwrite it with combined counts (L1432–1437).
  - The man page still says "in the same order as SLURM_JOB_NODELIST" (srun.txt:4458), which is stale for subset steps.
- **Supporting evidence the reviewer missed.** §8.1 (L1461–1462) describes the map `0,0,0,1,2,2` as "exercising uneven node sizes and non-contiguous rank-to-node mapping". That map is contiguous, so the doc's own test pole would not catch this class.

**Downgraded** from blocking to design change needed. The rendezvous already broadcasts "the full node map" (L1323), so the fix belongs inside the existing mechanism: each node reports `SLURM_GTIDS` (documented: "Global task IDs running on this node"; observed set in task.c:325), one record per node. What is wrong is the claim that no agreement step is needed, not the architecture.

**Fix.**
- Drop the "falls out of the environment" claim.
- Build the map from per-node `SLURM_GTIDS` at rendezvous.
- Use `SLURM_STEP_TASKS_PER_NODE` only as a cross-check.
- Fix the §8.1 wording, and add a cyclic pole such as `0,1,2,0,1,2`.

---

## B4: every PE connects to rank 0

**Restatement.** At 256 PEs × hundreds of nodes, one listener accepts about 77k connections and fds. That contradicts §1 "Nothing may scale with PE count where it can scale with node count".

**Doc text.** L21–24 give the scaling rule and the "256 PEs per node and hundreds of nodes" target. L1322 has "every other PE connects". The finding against the doc's own rule stands on text alone.

**What I could and could not check.** The user rejected the VM rerun. Reading `exp/bootstrap/burst/burst.c`:
- The server does blocking `accept` then a blocking 16-byte `read` per connection, serially.
- All N clients `connect` in a tight loop, so every SYN arrives within milliseconds on zero-RTT loopback.

That is a worst case. Real srun launch spreads task start over time, and a real rendezvous would accept in a loop via epoll or io_uring. **The absolute times (e.g. ">300 s") are therefore not representative, and I would not cite them.**

The structural points do not depend on the timings:
- ~77k fds in one process. The soft `RLIMIT_NOFILE` of 1024 was observed by the reviewer. The library can raise soft to hard, but the hard limit is site-dependent.
- A listen backlog capped by `somaxconn`.
- O(PEs) handshakes, where O(nodes) suffices.

At Pi scale (tens of nodes × 4 PEs) there is no problem.

**Verdict.** Survives as design change needed, with medium confidence in the magnitude claims.

**Fix.** Only `LOCALID==0` per node talks TCP to the rendezvous and carries its node's GTIDS (merges with the B3 fix). Local PEs meet node-locally. Clients retry with jittered backoff.

---

## B7: binary run directly in sbatch hangs; no rendezvous timeout

**Restatement.** Running the binary without srun inside a batch script gives one process with `SLURM_NTASKS>1`. It waits forever for peers.

**Verification.**
- Documented, sbatch.txt: `SLURM_NTASKS`, `SLURM_NODEID` ("ID of the nodes allocated."), `SLURM_NODELIST`, and `SLURM_TASKS_PER_NODE` are all present in the batch environment.
- `grep -i timeout` over the design finds only the §3.10 sweep and §4.9 deadlines, nothing in §6.
- Observed in source: `env_array_for_step` always sets `SLURM_STEP_NUM_TASKS` (env.c:1430). In the `env_array_for_batch_job` region (env.c ~1215–1345), the only task-count variable written is `SLURM_NTASKS` (L1302). That supports the reviewer's detector.

**Downgraded** to minor. This is launcher misuse. The consequence is a hang until the time limit instead of a message. The spec asks nothing here, and the fix is small.

**Fix.**
- Refuse Slurm mode when `SLURM_STEP_NUM_TASKS` is absent.
- Add `OSOE_BOOTSTRAP_TIMEOUT`, with rank 0 or the rendezvous host naming the missing nodes.
- Detect duplicate PROCIDs at rendezvous. The `mpirun` case is recalled, unverified.

---

## L1 / O10: non-last shmem_finalize must perform a barrier

**Restatement.** §6.4 L1360–1361: "`shmem_finalize` decrements; the last one tears down." The spec requires non-last calls to act as `shmem_barrier_all`.

**Spec (documented).** spec16.txt:1413–1416, §9.1.4: "The last call to shmem_finalize performs an implicit global barrier … All other calls to shmem_finalize perform an operation semantically equivalent to shmem_barrier_all and return without freeing any OpenSHMEM resources." It is also "a collective operation that requires all PEs to participate" (L1407).

**Attempted refutation.** No other text in §6.4 or §7 (the L1389 row) mentions a barrier on finalize. The doc's "decrement" wording directly contradicts the spec. A PE doing `init; init; putmem; finalize` would see no completion guarantee at the inner finalize.

**Verdict.** Survives. The severity is a design change, but a one-line one.

**Fix.** Every `shmem_finalize` performs barrier_all (the §5.6 quiet plus rendezvous). The last one then tears down. The §5.4 epoch counter must count these barriers.

---

## E1 (and O4 part 2): one crashed PE hangs the job

**Restatement.** Slurm does not kill the step by default, and the design has no liveness detection. A PE crash stalls survivors until the time limit.

**Verification.**
- Documented, slurm.conf.txt:2379–2385: "KillOnBadExit: If set to 1, a step will be terminated immediately if any task is crashed or aborted … With the default value of 0, if one of the processes is crashed or aborted the other processes will continue to run". The default of 0 is confirmed.
- srun.txt:1907–1913: `-K, --kill-on-bad-exit` overrides it per step.
- The design has no text on PE liveness. §3.15 (L609–614) covers only connection death.

**Why downgraded.**
1. The spec places no requirement on behaviour after a PE crashes. This is a usability and operability issue, not conformance.
2. Under Slurm, the hang is removed by `srun -K1`, or by a site `KillOnBadExit=1`. The reviewer's `launch.c` excerpt shows srun signals the step on the first abnormal exit when kill-on-bad-exit is enabled.
3. The design can require that flag, e.g. in a launch wrapper, or refuse to start without it, at near-zero cost.

**What remains:**
- Non-Slurm launches (tier-2 harness, ssh) hang too, which T5's missing test timeouts make worse.
- The reviewer's specific stall path (crash between CAS and `filled += n`) is plausible from §3.5 as described.

**Fix.**
- Document `srun -K1` as required.
- Add bootstrap and test timeouts.
- Optionally, pidfd liveness on the progress thread. That becomes attractive once E2 needs a local notification path anyway.

---

## E2 / O4: shmem_global_exit

**Restatement.** No mechanism terminates all PEs, including computing ones, with the passed status.

**Verification.**
- Spec §9.1.6 (documented, quoted by the reviewer and consistent with spec16.txt): "immediate notification to all PEs to terminate … exit status returned to the environment shall be one of the values passed … all PEs must terminate regardless of their current execution state."
- `grep global_exit` on the design finds nothing, and it is absent from the §7 phase lists (L1385–1430).
- By §7's own criterion ("what forces an architectural decision", L1382), it qualifies. It needs an asynchronous path into a PE that may never call the library, and §3.12 (L551–552) declares "The origin side never needs a helper".

**Attempted refutation.** Could `srun -K1` plus a plain `exit(status)` in the caller satisfy it? Slurm would then kill the other tasks by signal, and srun reports a signalled status in preference to exit codes (reviewer's `launch.c` observation; I did not independently re-read that code). That would not reliably give "one of the values passed". It also does not work outside Slurm. So I cannot refute this.

**Verdict.** Survives: design change needed.

**Fix.**
- Pick the per-PE notification mechanism: a watcher thread on a node-segment futex word that calls `exit(status)`, or equivalent.
- Flood a GLOBAL_EXIT control message over node-pair connections.
- Make every blocking wait interruptible by the abort word.
- Allow a kill fallback only after a grace period.

---

## T1: CI on ubuntu-24.04 cannot configure

**Restatement.** §8.5 (L1542) names `ubuntu-24.04` and `ubuntu-24.04-arm`. Noble ships liburing 2.5, and CMake requires 2.6 or later.

**Verification.**
- Launchpad API (fetched): noble has `2.5-1build1 Release Published`. `2.5-1ubuntu1..3` exist only in Proposed (superseded or deleted). There is no 2.6+ in updates, security, or backports. packages.ubuntu.com has no noble-backports entry.
- Resolute: `2.14-1 Release Published`.
- `CMakeLists.txt:35`: `pkg_check_modules(... "liburing>=2.6")`, then FATAL_ERROR.
- PPAs: not checked. Any PPA would contradict §1's "distribution's liburing" rule (L1634, L1671) anyway.
- Runner images, from the runner-images README (fetched):
  - `ubuntu-26.04` (x64) and `ubuntu-26.04-arm` exist, **badged "preview"**.
  - Their readmes report OS 26.04.1 LTS and kernel 7.0.0-1012-azure.
  - The 24.04 image runs kernel 6.17.0-1022-azure.

**Ways around it.**
- Switch the labels to the 26.04 runners (preview status is a stability caveat).
- Run a `debian:trixie` container job on the 24.04 runner. The kernel is new enough, but the default Docker seccomp profile may block io_uring syscalls (recalled, unverified), so it needs `--security-opt seccomp=unconfined`.

**Downgraded** to minor. The repo has no `.github/` directory, so no CI exists to be broken. The defect is one table row naming the wrong label, which contradicts the doc's own §1 "require a new enough OS" policy.

**Fix.** Change L1542 to `ubuntu-26.04` and `ubuntu-26.04-arm` (note preview status), or a Debian 13 container.

---

## T2: bootstrap not exercised by tests

**Restatement.** §6.2 L1341–1342, "the bootstrap is exercised by every test", is false for the Slurm-specific logic.

**Doc.**
- §8.2 L1470: tier 1 is an "In-process AM backend, queues not sockets", so no rendezvous runs.
- L1339–1340: non-Slurm launch uses "the same variables under a `SHMEM_` prefix".

The shared-code-path argument covers the socket rendezvous. It does not cover how `SLURM_*` variables are interpreted, which is where B2, B3, and B7 live.

**Verdict.** Survives, downgraded to clarification needed (test plan, not architecture).

**Fix.**
- Tier-1 fixtures of captured `srun` environments: block, cyclic, arbitrary, subset step, `-E`, het, bare sbatch.
- A tier-2 launcher that emits `SLURM_*` exactly as env.c/task.c would.
- A cyclic §8.1 pole.

---

## T3: invariant 7 untestable while all maps are identical

**Restatement.** Invariant 7 (L1615, "wire format never depends on identical virtual addresses") cannot fail in tests, because §4.2 (L651–653) maps identical VAs everywhere.

**Attempted refutation.** Tier 2 logical nodes are separate processes on one host. With `MAP_FIXED_NOREPLACE` at a fixed range, every process gets the same heap VA, so a leaked raw pointer would still decode correctly. Only a real heterogeneous cluster, or a failed fixed mapping, would expose it. Survives.

**Downgraded** to minor. This is a missing test mode, not a design defect.

**Fix.** A debug option that offsets the heap base per logical node and disables raw-pointer fast paths. Run it in tier 2.

---

## O8 (part): profiling interface vs the shmem.exports test

**Restatement.** The spec requires `pshmem_*` entry points (with weak `shmem_*`). The §10 policy "only spec API symbols are exported", enforced by `shmem.exports`, supposedly forbids them.

**Is the profiling interface mandatory?** Yes. Documented, spec16.txt:9384–9403, Chapter 10: "places the following requirements on implementations. 1. … must provide a mechanism through which all of the OpenSHMEM defined functions may be accessible with a name shift … prefix pshmem_ … 5. A no-op routine, shmem_pcontrol, must be provided … 6. … defined in pshmem.h."

The spec requires a name-shifted entry point. It does **not** mandate weak aliases specifically; they are the usual mechanism (recalled).

**Is there a conflict?** Refuted.
- §10 (L1673) says "only **spec API** symbols are exported". `pshmem_*` names are defined by the spec (Chapter 10), so the policy permits them.
- The enforcement is a regex, not a design rule. `cmake/CheckExports.cmake` has `set(ALLOWED "^shmem_")`, and its header comment says "Extend ALLOWED as deprecated unprefixed spec routines … are implemented."
- Weak symbols show as `W` in `nm -D --defined-only`. The script strips the type letter with `^[0-9a-fA-F]* *[A-Za-z] +`, so weak `shmem_*` aliases already pass.
- Accommodating `pshmem_` is a one-character regex change: `^p?shmem_`.

**What survives.** The profiling interface (`pshmem_*`, `pshmem.h`, `shmem_pcontrol`) is mandatory and appears in no phase list. There is also a small design point: internal barriers (malloc, finalize) should call internal symbols, not interposable `shmem_barrier_all`, so a profiler does not double-count them. Clarification needed.

**Fix.**
- List the profiling interface in §7 as mechanical but mandatory.
- State the alias mechanism: strong `pshmem_*`, weak `shmem_*`.
- Route internal calls to hidden `osoe_` functions.
- Change the `ALLOWED` regex to `^p?shmem_`.
