# Round three: synthesis - osoe before its first release

Review of `main` at `b0e0b53`, run 2026-09-28 against
`docs/reviews/adversarial-review-prompt.md`. Twenty investigations: two on
conformance, one each on the atomics rules, the deadlock detector, the
scaling table, the defaults, the backend seam, repository hygiene, the README's
settings, the install as a stranger, Slurm, the trust boundary and kernel
checks, and the lead's own runs (four presets, tier 3, load, repeats, the
eager detector) in a fresh VM made for the review; and, still running when the
author stopped the review, the models, the newest work (scan, agent slice,
silent-peer check), the stress tester's oracles, the diagnosability matrix,
x86-64 under Rosetta and the Pi cluster's reproduction and SOS sweep. Their
reports land in `build/scratch/review-r3/reports/` (git-ignored) when they
finish; the interim Pi numbers are below. The refutation pass by fresh
subagents did not run (the call failed when the review was stopped); in its
place the lead re-read the cited source for every finding rated fix before
release and re-ran what could be re-run, and each deliverable says
"lead-verified" where that happened. A usage limit interrupted fourteen
investigators an hour in; all were resumed from their transcripts.

The five deliverables: `phase1-release-conformance.md`,
`phase1-release-correctness.md` (partial), `phase1-release-scale-and-performance.md`,
`phase1-release-portability.md` (x86-64 pending), `phase1-release-readiness.md`
(diagnosability pending).

## Verdict

**Release after named changes - about two days of them, none to a protocol.**
osoe is what its README says it is in the ways that would be hardest to fix
and easiest to get wrong: no defined behaviour of OpenSHMEM 1.6 was found
implemented wrongly across every routine family; the atomics discipline holds
to the letter across 330 calls; the deadlock detector could not be made to end
a healthy job by any program inside the specification's model; no structure of
the job's PE count survives init; the four presets, tier 3 with every model,
three idle repeats and the suite at the detector's most eager setting pass in
a fresh VM; the licence is implemented completely; and the Pi cluster
reproduces the README's floor ratios, stream rate and smoke-test time on the
tree under review. What must change is what a stranger meets first and what
the README claims: one missing C11 interface, one operational contradiction
between the README's own advice and its promise, a start-up that hangs
silently when the network is not fully open, a rendezvous port that accepts
one disruptive connection before proving the secret, a performance pin that
fails the README's first command on other machines, and a dozen sentences in
the README and its tables that overstate or omit.

## What blocks or must precede release, ranked

Nothing was rated *blocks release*: every defect found is loud or
diagnostic, and every misleading claim is a sentence away from true. The
*fix before release* set, in the order the lead would do them:

1. **The C11 `shmem_signal_add(ctx, ...)` and `shmem_signal_set(ctx, ...)`
   generics do not compile** (conformance C1; `shmem.h:182-185`; *measured*;
   lead-verified). The one hole in "everything the specification requires".
   A few lines and a compile test.
2. **`shmem_global_exit` with a nonzero status under `srun -K1` kills the
   caller mid-`exit`** (conformance C2, readiness R5; *measured* 0 of 3 runs
   surviving a 200 ms handler, `srun` returning 143 for 7). The README
   recommends `-K1` and promises the caller's handlers run. Hold the other
   PEs' `_exit` until the caller's process has ended, within the abort grace,
   or state the limit beside `-K1`.
3. **A start-up failure on one PE or node leaves the rest waiting silently**
   (readiness R8; 138 measured cases): a PE exiting before `shmem_init`, the
   `/dev/shm` refusal, a bad setting on one PE or validated by the lead only,
   a heap size or `OSOE_JOB_SECRET` that disagrees - two of which the README
   and design §6.2 say end the job naming the variable. The refusal tests run
   without `-k`, so the suite cannot see it, and `srun -K1` masks it. Publish
   the failure where the others can see it before the control segment exists;
   also the newest-work gap (correctness K5): no test holds the gate that
   keeps a stopped node with a full receive buffer from being declared
   vanished - the experiment exists and should become a test.
4. **A node that cannot reach a peer waits forever and prints nothing**
   (portability N1; `listener.c:409-481`; *measured* on a firewalled mesh
   port, a wrong address and a name resolving elsewhere; lead-verified). One
   notice naming entry, address, port and error, repeated, without giving up.
5. **The rendezvous port acts on a hello's counts and node claim before the
   secret is proved, and reads the confirmation with no time limit**
   (portability N2; `listener.c:334-404`; *measured*: one connection ends or
   stalls a job's start, with the secret set; lead-verified). Prove first,
   bound the read, and add a "Trust" paragraph to the README.
6. **`shmem.floor`'s stream pin fails the README's first command on other
   machines** (readiness R2; *measured*: 0.0999 against 0.10 in the parallel
   suite run of a fresh VM, 0.10-0.32 idle). Take the pin out of the default
   suite, widen it, or say what it is.
7. **The README's scaling table and Limits omit three things the code does**
   (scale P1-P3, readiness R3; all *measured*, lead-verified): a split whose
   parent is per node spends one job-wide team number per node and fails past
   61 nodes at the default; one progress thread carries a node's whole
   internode traffic and "line rate" was measured at 1 Gbit/s; the
   connection-buffer budget cannot shrink at the defaults and refuses more
   than 1,025 nodes. Three sentences now; per-node team numbers and the
   sharded engine after release.
8. **`OSOE_MAX_THREADS` is described wrongly twice** (conformance C3; *measured*:
   a pool of 16 serialized workers ends the job; only the node's first PE
   reads the variable; lead-verified). Wording in four places, and a thought
   about the default on many-core nodes.
9. **Eight departures the Limits section does not list** (conformance C4):
   the per-node team cost, silent collective mismatches within a node,
   `num_contexts` not enforced, `SHMEM_SYMMETRIC_SIZE=0` refused, exit without
   finalize ending the job with status 1, `shmem_init_thread` never returning
   nonzero, waits woken by `shmem_ptr` stores at the 100 ms sweep, the cache
   routines removed rather than deprecated. All documentation.
10. **The README's requirements, first example and Slurm advice** (readiness
   R5a, R4): Python 3 required and unlisted; no version string anywhere; the
   first `srun` example without the `-K1 --cpu-bind=none` the README requires;
   start-up failures under `srun` leaving `/dev/shm` objects that
   `osoe-stat --gc` does not know while the README's `RemoveIPC=no` advice
   disables the cleanup that would; three settings rows without their ranges;
   the build command running 620 tests serially with no time given.
11. **The repository** (readiness R1): the author's paths in 61 evidence
    files, a docs index pointing at a private page, five fixtures with the
    VM's address, four specification examples without the required notice,
    two `dev/lima` files without SPDX lines.
12. **Two README passages on RDMA** (readiness R3, seam): XRC where the record
    concluded DC, and "in-network reduction is out of reach" where
    `internode.h` offers it.
13. **The README's model-checking sentence** (correctness K6): "every
    lock-free and futex protocol is checked exhaustively" - the active-set
    protocol has no model in the tree, and every model runs under SC or x86-TSO,
    not the aarch64 the targets are; one sentence, and a model for
    `active-set.c` after release.
14. **x86-64 in the README** (portability X1, X2): name the architecture
    tested and that the network path has never run on x86-64 (it builds
    clean, passes its single-node tests and matches §3.16's disassembly under
    Rosetta, but Rosetta refuses io_uring); and say that nodes must share an
    architecture, since a mixed job passes the rendezvous and reduces
    `long double` to garbage silently (the check after release).
15. **Two Pi numbers** (scale P9, P10; *measured* on this tree): the README's
    "16-PE barrier about 180 µs" holds under Slurm's default binding and is
    196-208 µs under the `--cpu-bind=none` the README recommends, where small
    collectives cost 4-17% more; and the record's "no difference in small
    operations" for the agent's 100 µs slice is wrong (barrier 8%, small
    reduce and broadcast 6-16% slower) while its gains reproduce. Documentation,
    with the slice's default the author's call. Everything else on the Pi
    reproduced: floor ratios 9.0-10.5%, streams at line rate, the smoke test in
    7 s, the bound-against-unbound 6.9× and 4.0×, `mix` clean, and the SOS
    suite at 4 × 2 with 161 passing and the same four documented exceptions.
16. **The torn "blocked in" record** (correctness K1; *measured* by litmus,
    not independently reproduced): one acquire fence or four atomic words;
    diagnostic impact only.

## After release, the ones worth planning

Per-node team numbers (§10's colouring); the sharded progress engine §3.12
describes; `collect`'s O(P) length reduction; the TCP send-buffer question the
second review's F7 left open; hugetlbfs heaps as §6.2 promises; the mesh's
ports bounded for firewalled sites; `SO_REUSEADDR` on the rendezvous listener
(the 33rd session in a minute fails) and `INADDR_ANY` for multi-homed nodes;
`iput`/`iget` stride checks and a debug compare of collective allocation sizes;
the deadlock detector's uncounted mutex sleep, its collection without a
timeout, and a test that sees the second collection; the seam's two AM symbols
and the io_uring preflight in `init.c`; `osoe-stat --gc` learning the `.conn`
and `.job` names; the `shmem.reply` test spinning where the library sleeps;
a three-node relay exercise in the pacer, whose oracles caught seven of eight
planted library defects but cannot see a quiet that returns early, and a
`mix` whose team periods move data between node-mates; a model for
`active-set.c`; a test that the suite could see an x86-only lost wake.

## What is sound, with the evidence that convinced the lead

- **Conformance**: two investigators, every routine family against the
  specification's text, forty probe programs on one and several logical
  nodes; the names, the generics, mixed-path atomicity (16,000-operation
  contention runs exact with unique fetched values), ordering across nodes as
  a third PE sees it, teams' argument rules including negative strides,
  collectives' type tables equal to the specification's, both broadcast root
  rules, cross-team collectives from several threads. The SOS suite's 159
  agree.
- **Correctness**: 330 atomic calls audited with no forbidden form, every
  two-sided protocol `seq_cst` on both sides, the shipped library free of
  `ldapr`, the lost-wake litmus reproduced (297,901 of 2 million lost with the
  weaker order the project rejected, 0 with its own); the deadlock detector
  proof against a healthy job by every program inside the model, its counted
  loops lead-verified, and the suite at one second reproducing the record.
- **Runs**: 598, 597, 387 and 597 of 598 in the four presets; 725 of 725 in
  tier 3 with every model; 1,794 idle repeats without a failure; 1,330 runs of
  the newest tests under load without a failure; the two load timeouts a test
  and a nested build, not the library.
- **Scale**: no array of the job's PE count after init (every allocation
  sized by the job checked; the segment formula against a measured segment);
  the logarithmic trees the table names; kernel resources sized and checked.
- **The Pi cluster, on this tree** (interim, `reports/pi.md`): the floor
  ratios 1.09-1.10× across three runs (the README's 8-13%); the stream 0.999×
  the echo's (line rate); the 16-PE barrier 181 µs bound; the smoke test 6.8 s
  unbound and 7.2 s bound (the README's 7 s); eighteen minutes of `mix` with
  `failures=0` and nothing declared deadlocked or vanished; `/dev/shm` empty
  after every step. The SOS sweep at 4 × 2 was running when the review
  stopped.
- **The seam**: a stub backend defining only the 31 functions the two headers
  ask for links against every non-backend object; no CPU atomic on a
  symmetric word above the seam; every ordering above it expressed by
  `FENCED` or a wait.
- **Slurm**: nine variables, never mixed; the host-list grammar equal to
  `scontrol`'s on 32 forms; every distribution and binding that could be run
  or emulated; batch and `salloc` shells refused by name.
- **The repository**: the licence implemented completely and consistently;
  no third-party code without its origin; no secrets; nothing private in the
  code or the public documents.
- **The records**: unusually candid about what is *recalled* and unmeasured,
  which is what made this review possible from them.

## Refuted or lowered

- "Intranode scaling is unmeasured past 4 PEs a node, contradicting 'linear
  or better'" (`defaults`, rated fix before release): lowered to after
  release with a README note. An L-way add on one cache line is linear; the
  design names only a risk that it grows worse (§5.3, lead-verified).
- "The team-number cost blocks release" (`scaling`): lowered to fix before
  release for the README sentence; the failure is loud and named, and the
  mechanism's fix is after release.
- The deadlock detector's signal-handler false positive (`deadlock-adversary`):
  not a discovery; the README lists it. The design's sentence is what is wrong.
- The Slurm fixtures' private address and the evidence files' SPDX lines:
  kept as fix before release for cheapness, not for risk.

## Pending, and where to find it

`build/scratch/review-r3/reports/models.md`, `newest-work.md`, `pacer.md`,
`diagnosability.md`, `x86.md` and the final `pi.md` (with the SOS sweep and
the captured 4 × 2 Slurm environments), each written by its investigator when
it finishes; `CONTEXT.md` there is the brief they worked from. Two Lima VMs
were made for the review and can be deleted when done: `osoe-x86` (Rosetta,
for the x86-64 build) and `osoe-load` (the baseline and load runs;
`load/*.log`). The review worktree is `build/scratch/wt/review-r3`.

## Sources

The five deliverables and the investigators' reports named in each;
`build/scratch/review-r3/` for every script, program, log and raw output;
`README.md`, `docs/design/shmem-ethernet-design.md`, `docs/decisions/`,
`docs/pi-cluster-results.md`, the 1.6 specification as text.
