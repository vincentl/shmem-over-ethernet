# Adversarial review, round three: osoe before its first release

You are leading the third adversarial review of **osoe** (OpenSHMEM Over
Ethernet), the one its author asked for before the first release. Use subagents
(the Agent tool) for parallel investigation. You own the plan, the
cross-checking and the final synthesis. **Treat everything a subagent reports
as a claim to verify, not a conclusion.**

The first round reviewed a design; the second, an implementation of its first
two steps. This round reviews **the library its author means to publish**: an
OpenSHMEM 1.6 implementation over TCP and io_uring, about 28,000 lines of C in
the library and 17,000 in its tests, some 600 tests in four build presets, 28
protocol models, a stress tester, and a design record of 7,300 lines that has
been decided against code for two weeks. The question is not whether it could
be better - it could - but **whether it is what its README says it is**, and
what must change before a stranger relies on that.

## Context

- **Repository:** `<repo>`, `main` at the commit the
  author names when starting the review (`git log -1 main`). Review that commit:
  build it in a worktree of your own.
- **Read first**, in order: `README.md` (the public face: what osoe is, how to
  run it, and a list of every way it departs from the specification);
  `docs/README.md` (where to find what); `docs/decisions/README.md` (one line
  for every decision still in force); and the table in `docs/prompts/README.md`,
  which gives every piece of work since the second review with its outcome and
  merge commit - start at `phase1-step2-slice2-prompt.md`.
- **Design record:** `docs/design/shmem-ethernet-design.md`. Each section
  states a choice, why, and what was rejected; §1 gives the goals and the
  **release point** (six conditions); §9 the load-bearing invariants; §10 the
  open questions, of which "a thorough 1.6 read" is still marked blocking.
- **Dated records:** `docs/decisions/<date>.md`. **They and the commit messages
  are primary sources**: most record a defect a green suite missed, a rule
  broken on purpose to show a test could see it, or a measurement that exists
  nowhere else. A finding that contradicts one is worth having; so is one that
  shows a record claims more than the code delivers.
- **Specification:** `docs/reference/OpenSHMEM-1.6.pdf` (local, not in git; if
  absent, say so and mark every conformance finding unverified). `pdftotext
  -layout` is on the host.
- **Linux:** the host is macOS. A Lima VM named **`osoe`** (Debian 13, kernel
  6.12, aarch64, 4 cores) mounts the repository at the same path. Build and test
  with `limactl shell osoe cmake --workflow --preset debug|release|asan|tsan`.
  **Take `flock /tmp/osoe-vm.lock` around any run that measures or loads the
  VM**: other sessions use it too, and two runs building one tree corrupt each
  other. Build in a `git worktree` of your own, never in the main checkout.
- **The Pi cluster:** four Raspberry Pi 5 nodes under Slurm on gigabit Ethernet,
  `docs/pi-cluster.md` its runbook and `docs/pi-cluster-results.md` what it has
  measured. If you have access (`ssh pi-login`), you may run jobs from the head
  node; **change nothing on the cluster**, and do not log into compute nodes
  while a job starts there - systemd-logind's `RemoveIPC` deletes a user's
  shared memory when their last session on a node ends, which the runbook
  explains.
- **Tools:** `osoe-launch` (a job of logical nodes on one machine),
  `osoe-stat` (reads a running or hung job from outside), `osoe-bench`,
  `osoe-pacer` (the stress tester: `docs/design/osoe-pacer-proposal.md`),
  `oshcc`, and `tools/modelcheck/` (the interleaving engine and its models,
  with `README.md` for the instruction set).
- **Provenance:** written with Claude's help over many sessions, one human
  author. Treat that as a reason for more scrutiny, not less - and the same holds
  for the *tests* and the *models*, which came from the same process as the code
  they check.

## Ground rules

- **Modify nothing** outside `docs/reviews/`; write only the files named below.
  Do not commit.
- **Every finding needs:** location (file and line, design §, or spec §), a
  concrete failure scenario or counterexample, evidence, severity, and
  confidence. Severity: **blocks release**, **fix before release**, **after
  release**, **minor**.
- **Prefer executable evidence**, and **break things on purpose**: the project's
  own standard is that a test asserting an ordering, a wake or a refusal is
  shown to fail when the mechanism under it is removed. Apply it to the tests
  and the models you doubt.
- **Label every claim** *documented*, *observed in source*, *measured* or
  *recalled*. If web access fails, say so and mark the claim unverified.
- **Accepted trade-offs** - §3.6's publication window, §4.6's deferred
  sampling, §4.7, and whatever the README lists as a limit - may be challenged
  as unjustified, not reported as discoveries. **A departure from the
  specification that the README does not list is a finding.**
- **Calibrate.** Say what is sound, with the evidence that convinced you. The
  author must be able to tell from the review whether to release.

## The axes

The author's goals, which this review judges by (design §1 and README):

1. **An OpenSHMEM 1.6 library in C** that a parallel programmer who is not a
   SHMEM expert can build, run and trust on a small Linux cluster.
2. **Scale with nodes, not PEs**: work and memory logarithmic in the node count
   between nodes, linear or better in PEs within one; graded at hundreds of
   nodes of 256 PEs.
3. **Correct at every level**, from one atomic to the tree collectives.
4. **Reasonable performance on plain Ethernet**, judged against a local floor
   (a bare io_uring echo, a stream, a memcpy), not against another library.
5. **Loud, never hung**: failures name what failed; a hang can be diagnosed from
   outside; a deadlock is ended only once proved.
6. **A seam an RDMA (RoCE) backend could take**, at up to ~4,000 nodes of 256
   PEs - a constraint on the design, not a backend this project builds.

---

## Question 1: Is it OpenSHMEM 1.6?

Conformance, as distinct from internal correctness - and §10's still-blocking
"thorough 1.6 read". Read the specification's requirements for each routine
family end to end and check the code against the text, not against the design
record's summary of it. Weigh the SOS test suite's results (the runbook's §8,
"The SOS test suite": 159 of its programs passed at the last sweep, and the
four that do not are listed there with the reason for each) as evidence, not as a verdict:
say which specification requirements no SOS program exercises.

Look especially at: every entry of the README's "Limits, and where osoe departs
from the specification", checked for truth and for completeness; teams and
contexts (`split_strided`, `split_2d`, `destroy`, team contexts, `OSOE_MAX_TEAMS`
as a departure); `SHMEM_THREAD_MULTIPLE`; the collectives' argument checks and
where undefined behaviour is made loud; the scans; `shmem_ptr` and
`shmem_team_ptr`; `put_signal` atomicity; `pe_quiet`; the deprecated routines;
the environment variables of the specification's §8.

**Deliverable:** `docs/reviews/archive/phase1-release-conformance.md`.

## Question 2: Is it correct?

The concurrency is the part most likely to be wrong in a way no test shows.

- **The atomics rules (§3.16)**: every shared protocol word an
  `atomic_*_explicit` naming one of four orders. Find violations, and shared
  words that are not atomic at all.
- **The protocols and their models.** Re-run the 28 models; check the recorded
  evidence matches; **check that each model's checks can fail** - each has
  breakages that must, and the project has twice found a check no case reached.
  Where a protocol has no model, say whether it needs one.
- **The newest work, which has had the least time**: the deadlock detector
  (`deadlock.h`, §3.15, `deadlock_model.py`) - a false positive ends a healthy
  job, so try hardest to construct one: a wait the count misses, a post the
  sample's pass cannot see, a thread `/proc` misreports, a message the
  acknowledgement check does not cover (an adversarial read before this round
  found none a realistic program reaches, and four corner paths:
  `docs/decisions/2026-09-28-overnight.md`, item 2); the check that finds a
  vanished node with data in flight (§3.15, `osoe_progress_check_silent`),
  which must never take a stopped node for a vanished one; the pipelined scan
  (§5.8); the agent's scheduler slice and CPUs (§3.11); the stress tester's
  oracles (`docs/decisions/2026-09-27-evening.md` lists what each still
  accepts).
- **Run the suite under load**, and expect to find more: spinning threads past
  the VM's cores, one test in a loop. The records name what this found before.
- **The stress tester** (`osoe-pacer`, `tools/pacer/`): run `mix`, and ask
  whether its checks would catch the defects it was built for.

**Deliverable:** `docs/reviews/archive/phase1-release-correctness.md`.

## Question 3: Does it scale, and perform, as the README says?

- **The scaling table** in the README ("Scaling with nodes, not PEs"): check each
  row's cost claim against the code - memory and state per PE, per node and per
  connection - at hundreds of nodes of 256 PEs, and name anything that grows
  with the job's PE count where the table says it does not.
- **The measurements** (`docs/pi-cluster-results.md`, `docs/decisions/`):
  reproduce what you can in the VM or on the cluster; judge the methodology;
  flag any number the README quotes that no record supports.
- **Defaults**: `OSOE_COLLECTIVE_FANOUT`, `OSOE_COLLECTIVE_PIPELINE`,
  `OSOE_GET_LANDING_BYTES`, `OSOE_AGENT_SLICE_US`, `OSOE_AGENT_CPUS` - are they
  justified by measurement, and safe on hardware unlike the Pi?

**Deliverable:** `docs/reviews/archive/phase1-release-scale-and-performance.md`.

## Question 4: Will it run anywhere else?

Everything so far has run on aarch64 - the VM and the Pi - under one kernel
family. Judge how much of that is a gap and how much a risk.

- **x86-64**, never built or run: memory ordering under TSO is stronger, but
  what of the code's assumptions about cache lines, alignment, `_Alignas` on
  shared structures, the futex and io_uring paths? Build it if you can.
- **Kernels**: `tools/probe` checks what osoe needs; check the checks, and what
  happens on a kernel that lacks one - refusal by name, or misbehaviour?
- **Slurm**: task distributions, `--cpu-bind` settings, `sbatch` versus `srun`,
  heterogeneous jobs, the bootstrap's environment (§6.1-§6.2).
- **Build and install**: `cmake --install`, `oshcc`, the exported symbols, the
  profiling interface, static linking; could a stranger build this from the
  README alone?
- **Trust**: the node-to-node protocol accepts connections on a port; what is
  assumed about a peer, and is it stated (`OSOE_JOB_SECRET`, §6.2)?

**Deliverable:** `docs/reviews/archive/phase1-release-portability.md`.

## Question 5: Is it ready to be public?

- **The README** as its intended reader meets it: a parallel programmer who
  knows MPI or OpenMP and not SHMEM. Is anything wrong, missing or misleading?
  Every limitation stated? Every setting it names real, and its default right?
- **Diagnosability**: make a job fail each way you can - a dead PE, a stopped
  node, a deadlock, a mismatched collective, a bad setting, an exhausted
  resource - and judge what the user is told and whether `osoe-stat` finds it.
- **The repository**: licences (`LGPL-3.0-or-later`, every file's SPDX line),
  third-party material and its terms, anything private or host-specific that
  should not be published (paths, host names, keys, captured environments), and
  whether the `docs/` history reads as a record or as noise to a stranger.
- **The seam** (axis 6), briefly: read `transport.h` and `internode.h` as a
  contract an RDMA backend would implement; name anything above them a porter
  would have to rewrite that the design promises they would not.

**Deliverable:** `docs/reviews/archive/phase1-release-readiness.md`.

---

## Working method

- State the subagent plan, then proceed without waiting unless something is
  genuinely ambiguous. Launch independent subagents in parallel; each starts
  without this conversation, so give it the context and the finding fields
  above.
- **Verification pass**: give every finding rated *fix before release* or worse
  to a fresh subagent whose job is to **refute** it - reproduce it, find the code
  or record that already handles it, or find specification text that permits the
  behaviour. Keep what survives; list the refuted briefly, with the reason.
- Cross-check any fact that several conclusions rest on against a second source.
- **The synthesis**, `docs/reviews/archive/phase1-release-review.md`: a verdict -
  release now, release after named changes, or not yet - and why; the findings
  that block or must precede release, ranked; what is sound; the refuted
  findings; sources.
- End with a short chat summary: the verdict, and the three findings that matter
  most.
