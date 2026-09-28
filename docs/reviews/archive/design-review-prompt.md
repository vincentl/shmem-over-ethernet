# Adversarial review: the osoe design, and a core API for OpenSHMEM at scale

*The first round's prompt, run against the design alone before there was code;
kept beside what it produced, `design-soundness.md` and `core-api.md`. Until
2026-09-28 it was `docs/reviews/adversarial-review-prompt.md`, which now holds
the next round's.*

You are leading a review that answers two independent questions. Use subagents
(the Agent tool) for parallel investigation as described below. You own the plan,
the cross-checking, and the final synthesis. Treat everything a subagent reports
as a claim to verify, not a conclusion.

## Context

- **Repository:** `/Users/vincent/Desktop/Maker Space/shmem-over-ethernet`, project
  **osoe** (OpenSHMEM Over Ethernet).
- **Plan under review:** `docs/design/shmem-ethernet-design.md`, about 1,700 lines.
  It is a decision record: each section states a choice, its rationale, and the
  alternatives rejected. §9 lists load-bearing invariants; §10 lists closed and
  open questions.
- **Specification:** `docs/reference/OpenSHMEM-1.6.pdf` (local, gitignored).
  `pdftotext -layout` is available on the host. Cite the spec as §9.x.y. Its
  change-history annex covers the differences between versions.
- **Implemented so far:** a CMake build skeleton, `libosoe` with two library
  information routines, and `tools/probe/probe.c`, which checks the kernel
  facilities the design relies on.
- **Linux environment:** the host is macOS. A Lima VM named `shmem` (Debian 13,
  kernel 6.12, aarch64, 4 cores) mounts the repository at the same path. From the
  repository root, run Linux commands with `limactl shell shmem <command>`. Use it
  to test kernel, io_uring, and futex claims by experiment rather than from
  memory. You may install packages in the VM.
- **Provenance:** the design was developed over several sessions with Claude's
  help. Treat that as a reason for more scrutiny, not less — the reasoning has
  had a single author's blind spots.

## Ground rules

- **Do not modify** the design doc, source files, or git history. Write output
  only to `docs/reviews/` as specified below. Do not commit.
- **Every finding needs:** location (design doc §, spec §, or file), a concrete
  failure scenario or counterexample, evidence (spec text, kernel source,
  experiment output, or library source/documentation with a URL), severity, and
  confidence.
- **Prefer executable evidence** for concurrency and protocol claims. A small
  exhaustive interleaving model (Python or C) or an experiment in the VM beats a
  prose argument.
- **Accepted trade-offs.** The doc marks some trade-offs as accepted (for
  example §3.6, the deferred-sampling hazard in §4.6, and §4.7). Challenge whether
  accepting them is justified, but do not report them as discoveries.
- **Sources.** If web access fails for a source, say so and mark the claim
  unverified. Never present recollection as fact: label each claim as
  *documented*, *observed in source*, or *recalled*.
- **Calibrate.** Report what is sound as well as what is not. A review that only
  lists problems cannot tell the reader whether to proceed.

## Question 1: Is the plan sound?

Is `docs/design/shmem-ethernet-design.md` a sound plan for producing a
**functional** OpenSHMEM 1.6 implementation over commodity Ethernet on its stated
targets — a Raspberry Pi cluster under Slurm, remaining usable at 256 PEs per
node and hundreds of nodes?

"Sound" means:

1. **Correct.** No deadlocks, races, lost wakeups, ordering violations, or spec
   nonconformance in the designs as written.
2. **Complete for phase 1** (§7). Everything a working implementation of the
   phase-1 routines needs is either designed or explicitly deferred with a
   reason. Look especially for what the doc never mentions: read the spec's
   requirements for each phase-1 routine end to end.
3. **Feasible.** Buildable within the §1 dependency budget, with effort
   proportionate to a small project.
4. **Plausible performance.** The load-bearing performance assumptions are stated
   and testable.
5. **Well sequenced.** The plan reaches something functional early, rather than
   only after all of the machinery exists.

### Subagents

Run these in parallel. Adjust the split if you see a better one.

- **Transport (§3).** Stream ordering, the reservation protocol and buffer
  states, the handler rule and deferred reply queues, submission- and
  completion-queue bounds, wakeup and waiter fairness, progress threads. Try to
  construct deadlocks and ordering violations. Test io_uring claims in the VM.
- **SHMEM semantics (§4, §7).** Symmetric heap and addressing, allocator
  determinism, symmetric statics, the completion model (`fence`, `quiet`,
  `pe_quiet`), get handling, `shmem_ptr` and `shmem_team_ptr`, and the
  `wait_until` watch table. Check each against the spec text.
- **Collectives and teams (§5).** The epoch counters and the argument that
  equality waits are sound, the single-writer lead, copy-out, team accounting,
  team split and destroy, internode trees. Build an exhaustive interleaving model
  of §5.4–§5.5 for small local member counts and several epochs, including mixed
  sequences of barriers and reductions.
- **Bootstrap, lifecycle, and testing (§6, §3.15, §8).** Slurm environment
  variable semantics — verify against Slurm documentation, including
  `SLURM_LOCALID` and `SLURM_NODEID` under non-default task distributions,
  heterogeneous jobs, and `srun` versus `sbatch` — plus the rendezvous, repeated
  `init`/`finalize`, the error model, and whether the §8 test tiers would
  actually catch violations of the §9 invariants.
- **Omissions red team.** Ignore the doc's structure. Read the phase-1 routines
  and the spec's general sections — threading levels and `shmem_init_thread`,
  contexts, error handling, environment variables, the interoperability annex —
  and list requirements the doc does not address.

### Verification pass

Give each finding rated *design change needed* or worse to a fresh subagent whose
job is to **refute** it: reproduce it, find the doc text that already handles it,
or find spec text that permits the behaviour. Keep only findings that survive,
and list refuted ones briefly with the reason.

### Deliverable: `docs/reviews/archive/design-soundness.md`

- **Verdict**, one paragraph: proceed, proceed after specific changes, or rethink —
  and why.
- **Findings**, ranked:
  - **Blocking** — the plan cannot produce a correct, functional implementation
    as written.
  - **Design change needed.**
  - **Clarification needed.**
  - **Minor.**

  Each with the fields from the ground rules.
- **What is sound**, with the evidence that convinced you.
- **Sequencing:** a suggested reordering of phase 1, if sequencing is a problem.
- **Refuted findings**, briefly.
- **Sources.**

## Question 2: A minimum core API for OpenSHMEM at very large scale

Set commodity Ethernet aside. For existing and future high-performance networks,
determine a **minimal core API** that, implemented well, gives a fast path to
OpenSHMEM 1.4, 1.5, and 1.6 on very large systems — assume 10^5 PEs and up —
with everything else layered in portable software above it.

The target is a small set of primitives with precise semantics, in the spirit of
GASNet-EX's core/extended split, such that:

- the performance-critical OpenSHMEM operations map onto core primitives with no
  avoidable overhead;
- the rest of OpenSHMEM can be implemented above the core, some of it
  deliberately more slowly;
- each primitive can be implemented efficiently on the networks surveyed, or its
  cost where it cannot is known.

### Survey subagents

One per family, run in parallel, each using the template below.

- **libfabric/OFI**, including the `cxi` (HPE Slingshot), `verbs`, `efa`, and
  `opx` providers, and Sandia OpenSHMEM's OFI transport.
- **UCX** (UCP and UCT), including OSSS-UCX and Open MPI's `oshmem`.
- **Portals 4**, including Sandia OpenSHMEM's Portals transport and BXI.
- **GASNet-EX** (core versus extended API), **GPI-2/GASPI**, and **LCI**.
- **MPI-3 RMA** as a substrate, including OSHMPI.
- **NVSHMEM**: GPU-initiated communication, IBGDA, and the proxy-thread model.
- **Vendor and historical:** Cray DMAPP and uGNI (Cray SHMEM), Fujitsu uTofu, AWS
  EFA/SRD.
- **Future:** the Ultra Ethernet Consortium specification (transport semantics,
  packet spraying and out-of-order delivery, its libfabric-based API), and
  fabric-attached or CXL shared memory (for example OpenFAM).

Template:

1. Primitives relevant to one-sided PGAS: RMA read and write, atomic or triggered
   operations, counters, tag matching, active messages, collective offload.
2. Memory registration model; how remote keys scale with PE count; whether one
   registration or key can cover a symmetric heap.
3. Per-peer state: connections, address vectors, and what grows with PE count at
   10^5–10^6 PEs.
4. Ordering and delivery guarantees, especially under multipath or adaptive
   routing.
5. Completion and progress model: event queues, counters, whether a progress
   thread is needed.
6. Thread and context model, and how many hardware resources a context costs.
7. GPU memory support.
8. **What OpenSHMEM implementations built on it had to emulate in software.** This
   is the most important evidence. Cite source files where possible.
9. Known performance pathologies and scaling limits, with sources.
10. Build, portability, and maintenance burden.

### Analysis subagents

- **Usage.** Which OpenSHMEM routines real applications and benchmarks use, and
  how hot they are — for example OpenSHMEM benchmark suites, ISx, Graph500 SHMEM
  ports, SSCA, NAS ports, and runtimes that target SHMEM. Produce a table of
  routine family against usage evidence. Where the evidence is thin, say so: a
  routine that "seems less useful" must be backed by evidence.
- **Semantics.** For every routine family in 1.4, 1.5, and 1.6 (use the spec's
  change-history annex for version deltas), identify what makes it hard or cheap
  on RDMA-class networks: remote atomics and compare-and-swap, `shmem_wait_until`
  on remote variables, put-with-signal atomicity, fence ordering on multipath
  networks, contexts and thread levels, teams and team-relative addressing,
  `shmem_ptr`, locks, strided and interleaved transfers, `realloc`/`align`
  collectives, the deprecated active-set collectives, and sessions. Classify each
  as **core fast path**, **derivable efficiently from core**, or **slow path
  acceptable**, with justification.
- **Architect** (after the surveys and analyses). Propose the core API: a
  signature sketch; precise semantics for ordering, completion, atomicity, memory
  registration, and errors; a resource model (what a context or endpoint costs);
  and a mapping from core primitives to OpenSHMEM routines. State explicitly which
  OpenSHMEM features are relegated to slow paths and their expected cost.
- **Adversary.** Attack the proposal: find an OpenSHMEM feature it cannot express
  efficiently, a surveyed network on which a primitive cannot be implemented
  efficiently, or a scale at which per-peer or per-context state explodes. The
  architect revises once. Record what changed and what remains contested.

### Deliverable: `docs/reviews/archive/core-api.md`

- **Summary:** the proposed core API on one page.
- **Primitives:** semantics and rationale for each.
- **Mapping tables:** core primitive → OpenSHMEM 1.4/1.5/1.6 routines; and routine
  → fast / derived / slow classification, with usage evidence.
- **Network feasibility matrix:** primitive × network, each cell one of native,
  cheaply emulated, expensively emulated, or unsupported, with sources.
- **Substrates:** strengths and weaknesses of each surveyed library as a basis for
  OpenSHMEM.
- **Contested points and open questions.**
- **Relation to osoe:** a short section on where osoe's AM layer (§2 of the design
  doc) is or is not a special case of the proposed core, and whether that suggests
  any change. Keep it brief; the main target is high-performance networks.
- **Sources.**

## Working method

- State the subagent plan briefly, then proceed without waiting unless something
  is genuinely ambiguous.
- Launch independent subagents in parallel. Subagents start without this
  conversation, so give each one the context it needs and the exact output
  fields above.
- Cross-check any fact that several conclusions depend on against a second
  source or a second agent.
- End with a short chat summary: the Question 1 verdict, the three most important
  Question 1 findings, and the proposed core API in a few lines.
