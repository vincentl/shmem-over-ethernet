# Adversarial review, round two: osoe as an implementation

You are leading the second adversarial review of **osoe** (OpenSHMEM Over
Ethernet). Use subagents (the Agent tool) for parallel investigation. You own
the plan, the cross-checking, and the final synthesis. **Treat everything a
subagent reports as a claim to verify, not a conclusion.**

The first round reviewed a design. This round reviews **an implementation** —
about 13,750 lines of C, 131 tests, and a design document that has grown to
4,420 lines and now records decisions made *against* code. Read
`docs/reviews/phase1-step2-slice1-what-changed.md` first: it says what exists,
what does not, which first-round conclusions have been reversed, and where the
evidence is known to be thin.

## Context

- **Repository:** `/Users/vincent/shmem-over-ethernet`, project **osoe**.
- **Design doc:** `docs/design/shmem-ethernet-design.md`, a decision record —
  each section states a choice, its rationale, and what was rejected. §9 lists
  load-bearing invariants; §10 lists closed and open questions.
- **Specification:** `docs/reference/OpenSHMEM-1.6.pdf` (local, gitignored; if
  absent, say so and mark every conformance finding unverified). `pdftotext
  -layout` is available on the host. Cite the spec as §9.x.y; its change-history
  annex covers version deltas.
- **Linux environment:** the host is macOS. A Lima VM named **`osoe`** (Debian
  13, kernel 6.12, aarch64, 4 cores) mounts the repository at the same absolute
  path. Run Linux commands as `limactl shell osoe <command>`; build and test with
  `limactl shell osoe cmake --workflow --preset debug|release|asan|tsan`. **Use
  it to test claims by experiment rather than from memory.** You may install
  packages. Run `tools/probe` (the `env.probe` test) before debugging anything
  else.
- **Where the work is:** merged to `main` as `d542c63`, a `--no-ff` merge of the
  31 commits of `phase1-step2-slice1`; that branch has been deleted, so read the
  slice with `git log --first-parent main` for the shape and
  `git log d542c63^..d542c63^2` for the commits themselves.

  **The commit messages are a primary source, not commentary.** Most record a
  defect a green test suite did not catch, several record a rule broken on
  purpose to prove a test could see it, and a few record a measurement - failure
  rates under load, mutation catch rates - that exists nowhere else. A finding
  that contradicts one of them is worth having; so is one that shows a commit
  message claims more than the code delivers.
- **Provenance:** developed over several sessions with Claude's help, one human
  author. Treat that as a reason for more scrutiny, not less — and note that the
  same holds for the *tests*, which were written by the same process as the code
  they check.

## Ground rules

- **Do not modify** the design doc, the source, or git history. Write output only
  to `docs/reviews/`, in the files named below. Do not commit.
- **Every finding needs:** location (file and line, design §, or spec §), a
  concrete failure scenario or counterexample, evidence, severity, and
  confidence.
- **Prefer executable evidence.** This round has a tree you can build, run,
  instrument and break. A failing test, a model counterexample, or a VM
  experiment beats a prose argument. `tools/modelcheck/` holds the model checker
  and `docs/design/evidence/` the recorded runs.
- **Breaking something on purpose is the strongest evidence available.** The
  project's own standard is that a test asserting an ordering or a wake must be
  shown to fail when the mechanism is broken. Apply it to the tests you doubt.
- **Accepted trade-offs.** The doc marks some as accepted (§3.6's publication
  window, §4.6's deferred-sampling hazard, §4.7). Challenge whether accepting
  them is justified; do not report them as discoveries.
- **Label every claim** *documented*, *observed in source*, *measured*, or
  *recalled*. Never present recollection as fact. If web access fails, say so
  and mark the claim unverified.
- **Calibrate.** Report what is sound as well as what is not, with the evidence
  that convinced you. A review that only lists problems cannot tell the reader
  whether to proceed.

## What this project is trying to be

The six questions below come from the author's own statement of intent. They are
the axes on which this review should judge the work, and they are not equally
well served by the current tree — saying which are weakest is part of the job.

1. An **OpenSHMEM library in C** that works on a wide variety of platforms.
2. A **path to high performance** by porting only a lower-level part of the
   package to RoCE.
3. **Correctness at every level**, from local atomics through the parallel
   algorithms that give barriers and reductions.
4. **Reasonable performance on plain Ethernet.**
5. Three things the author's list did not name and this review should also
   cover: **specification conformance** as distinct from internal correctness,
   **diagnosability** when a job misbehaves, and **scale** in memory and state.
6. Whether what has been **deferred** is safely deferred.

---

## Question 1: Is it OpenSHMEM?

Conformance is a separate axis from correctness, and it is the one a design
review could not reach. There are **526 routines**, each a strong `pshmem_`
definition with a weak `shmem_` alias. Read the specification's requirements for
the implemented routines end to end and check the code against the text, not
against the design doc's summary of the text.

Look especially at: the boundaries of undefined behaviour and whether the
library's choice to be *loud* where the spec says *undefined* is defensible in
each case; `shmem_ptr`'s exact contract and the claim that returning `NULL`
unconditionally is conforming; thread levels and `shmem_init_thread`'s
negotiation; repeated `init`/`finalize` and `shmem_query_initialized`;
`shmem_pe_quiet`'s completion set; `put_signal` atomicity; the deprecated
routines and Annex F's header location; and the environment variables of §8.

**Deliverable:** `docs/reviews/phase1-step2-slice1-conformance.md`.

## Question 2: Is it correct?

From local atomics up to the parallel algorithms. The concurrency here is the
part most likely to be wrong in a way no test shows.

- **§3.16's atomics rules.** Every shared protocol word should be an
  `atomic_*_explicit` naming one of four orders. Find violations, and find words
  that are shared but not atomic at all.
- **The protocols.** §3.5's reservation, §3.8's credit and queues, §5.3's
  arrival handoff, §5.4's epoch counters and the argument that equality waits are
  sound, §4.9's watch, the wire's wake protocol, the teardown handshakes added in
  this slice. Build models where one is missing; the engine is in
  `tools/modelcheck/` and its README explains the instruction set.
- **Audit the evidence.** Five models exist with deliberately broken variants.
  Re-run them. Check that the recorded `.out` files match. **Check that each
  model's checks can actually fail** — the project has twice found a check that
  no case reached, and has recorded both times.
- **Audit the tests the same way.** Pick the assertions the tree most depends on
  and break the mechanism under them. `shmem/tests/arrivals.h` has had two flaws
  found in it already, each of which made it silently vacuous.

**Deliverable:** `docs/reviews/phase1-step2-slice1-correctness.md`.

## Question 3: Will it run anywhere else?

Nothing here has run on a second physical machine, on a Raspberry Pi, under
Slurm, or over a socket. Judge how much of that is a gap and how much is a risk.

- **Kernel and toolchain assumptions.** `tools/probe` checks them; check the
  checks. futex2, `IORING_OP_FUTEX_WAIT`, multishot recv, cross-process futexes,
  huge pages, `MAP_FIXED_NOREPLACE`. What happens on a kernel that lacks one —
  refusal, or misbehaviour?
- **Bootstrap against a real launcher.** §6.1 and §6.2 claim the `OSOE_`
  variables mirror Slurm's. Verify against Slurm's documentation, under
  non-default task distributions, heterogeneous jobs, and `srun` versus
  `sbatch`. PMIx is not used; should it be?
- **Portability of the C.** ISO C11 plus C23 `<stdbit.h>`; libc and liburing
  only. Endianness, alignment, `_Alignas` on shared structures mapped at
  different addresses in different processes, 32-bit hosts, aarch64 versus x86
  (the design notes an unaligned 64-bit atomic faults on aarch64).
- **Build and install.** `shmem.exports`, the profiling aliases, symbol
  visibility, the historical `<mpp/shmem.h>` location, and whether a third party
  could build this.
- **The trust model.** Step 3 puts a parser on a socket. §3.4's redundant fields
  and the malformed-segment tests exist — but is the trust boundary *stated*
  anywhere, and what is assumed about a buggy or hostile peer?

**Deliverable:** `docs/reviews/phase1-step2-slice1-portability.md`.

## Question 4: Is §4.10 the seam an RoCE port needs?

The author's second goal is that porting only a lower-level part of this package
to RoCE should give high performance. The first review had to take that on
argument, because one backend existed. **Two paths now exist in the tree**, and
this slice closed §10's `osoe_transport_sync` question against the code.

- Read `shmem/src/transport.h` as a contract and ask what an RoCE implementation
  of it would have to do. Which operations map to RDMA verbs, and which would
  have to be emulated?
- §10's remaining open question — whether §3's AM data path stays hot in an RDMA
  build — is it answered, deferrable, or quietly settled by something already
  built?
- Is the **cut** in the right place? `osoe_transport_direct`'s contract is about
  *store ordering*, not locality, which was deliberate. Does that survive
  contact with a NIC that applies atomics out of the CPU's sight?
- What about the things §4.10 keeps *outside* the boundary — collectives, the
  watch mechanism, the landing zones? For each, would an RoCE port want it
  inside?
- Consider the counterfactual: name the parts a porter would have to rewrite
  that this design promises they would not.

**Deliverable:** `docs/reviews/phase1-step2-slice1-roce-seam.md`.

## Question 5: Will it perform, and will it hold at scale?

Two questions that share a deliverable because the same structures answer both.

**Performance on plain Ethernet.** There is no benchmark in the tree, so every
performance claim is currently an argument. Identify the load-bearing ones —
§3.9's self-clocking aggregation, §3.2's chunking and head-of-line bound, the
intranode floor of §8.4, the cost of the progress thread's polling in step 2 —
and say which are testable now, which need step 3, and which are unfalsifiable as
stated. Where you can measure something in the VM, measure it.

**Scale.** §1 targets hundreds of nodes and 256 PEs per node. The design
repeatedly claims state "scales with node count, never PE count". Check it end to
end with arithmetic: connection buffers, reply descriptors and the credit,
landing zones, the dirty set, the control segment, the node map, per-team state.
Find what grows with PE count, with job size, or with both. Give the numbers at
500 nodes × 256 PEs.

Also: what happens *at the limits* — when a credit runs out, a buffer stays
full, a reply pool empties, or a peer is slow? Which of those degrade and which
abort?

**Deliverable:** `docs/reviews/phase1-step2-slice1-performance-and-scale.md`.

## Question 6: When it breaks, can you tell why? And what is deferred?

**Diagnosability** is a large investment nobody has yet audited: §3.14's
self-describing metric table, the "blocked in" record, per-thread counter blocks,
`osoe-stat` reading a segment it shares no headers with, the §3.15 error model,
the development watchdog. The claim is that a hung job on many nodes can be
diagnosed from outside without cooperation from the job. **Test it.** Hang a job
deliberately — a PE that never reaches a barrier, a lost wake, a peer that stops
reading — and find out from `osoe-stat` alone what went wrong. Report what you
could not determine.

**Deferral safety.** For each item in the "not built" list of the what-changed
note, ask whether the current structure forecloses it. §5.4 is explicit that
equality-compared epoch counters are what nonblocking collectives break; hunt for
the equivalent elsewhere. `SHMEM_THREAD_MULTIPLE` (§4.11), symmetric statics
(§4.3), teams beyond the predefined two, locks, and the typed RMA cross product
are the ones to press on.

**Deliverable:** `docs/reviews/phase1-step2-slice1-diagnosability-and-deferrals.md`.

---

## Subagent plan

Run the six questions in parallel, splitting further where a question is large —
Question 2 in particular wants at least three: the atomics sweep, the protocol
models, and the evidence-and-test audit. Give each subagent the context it needs;
it starts without this conversation. Require each to name files and lines.

Two agents should work **across** the questions rather than within one:

- **Omissions red team.** Ignore the structure above and the design doc's.
  Read the spec's general sections and the implemented routines, and list
  requirements nothing addresses. The first round found real things this way.
- **Premise auditor.** The design doc is a decision record, and a decision is
  only as good as the claim under it. Pick the load-bearing claims — especially
  ones that reversed during implementation — and check whether the reasoning
  still holds against the code as it now is. The commit messages are where the
  reasoning lives.

## Verification pass

Give every finding rated *design change needed* or worse to a **fresh** subagent
whose job is to refute it: reproduce it, find the code or doc text that already
handles it, or find spec text that permits the behaviour. Keep only findings that
survive. List refuted ones briefly with the reason — those are as useful to the
author as the survivors.

## Synthesis: `docs/reviews/phase1-step2-slice1-review.md`

- **Verdict**, one paragraph: merge this branch as it stands, merge after
  specific changes, or rethink — and why.
- **The six goals, scored.** For each of the author's goals, say how well the
  current tree serves it and what the strongest evidence is either way. Be
  explicit about which goal is currently least well served.
- **Findings**, ranked *blocking* / *design change needed* / *clarification
  needed* / *minor*, each with its fields, and each naming which deliverable it
  came from.
- **What is sound**, with evidence.
- **Where the evidence is weakest**, and what would strengthen it most cheaply.
- **A recommendation on sequencing steps 3 and 4** in light of what this found.
- **Refuted findings.**
- **Sources.**

## Working method

- State the subagent plan briefly, then proceed without waiting unless something
  is genuinely ambiguous.
- Launch independent subagents in parallel.
- Cross-check any fact several conclusions depend on against a second source or a
  second agent.
- End with a short chat summary: the verdict, the three most important findings,
  and the goal currently least well served.
