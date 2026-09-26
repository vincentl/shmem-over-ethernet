# Adversarially review the proposed boundary changes, then implement what survives

You are continuing the `phase1-slice2` branch. Stop before your next planned step
and deal with this first — one of the proposals reorders your work.

## What happened

A side conversation planned how `shmem/src/transport.h` would be implemented on a
RoCE-based cluster, to test whether the §4.10 boundary can carry an RDMA backend.
It produced two **uncommitted, unreviewed** documents:

| File | What it is |
|---|---|
| `docs/reviews/archive/roce-transport.md` | The analysis. A RoCE backend plan, a per-operation cost model, what RoCE features the boundary can and cannot reach, and an estimate of what is left on the table. |
| `docs/design/transport-boundary-changes.md` | The proposal. Ten amendments (A1–A10) to `shmem-ethernet-design.md`, then Part B: six recommendations (B1–B6) that change what this slice does. |

Read the proposal first; read the analysis when you need the reasoning behind an
amendment. Both are cross-referenced.

**Their status matters.** They are one session's output, written without hardware,
and they are not decisions. Three specific cautions:

1. **Every hardware claim in the analysis is labelled *recalled*.** Nothing was
   measured and no driver source was read. `roce-transport.md` §10 lists eight
   things that must be verified before any number is quoted as a result. Do not
   implement anything whose only justification is an unverified hardware claim.
2. **The analysis already had one load-bearing argument withdrawn under its own
   review.** A1 originally claimed §4.10 contradicts itself about collectives. It
   does not — §2.2 scopes the boundary claim to §4 and puts collectives on AM
   deliberately. Both documents were corrected, and A1 now rests on a performance
   estimate alone. Treat that as evidence about the documents' reliability, not
   as the last error in them.
3. **`docs/reviews/archive/roce-transport.md` is new, not the committed record** your
   rules tell you to leave alone. You may correct it. Prefer correcting it over
   letting it diverge from what you conclude.

## What to do, in order

### 1. Adversarially review

Attack the proposal the way `docs/reviews/adversarial-review-prompt.md` attacks a
design: look for the claim that is load-bearing and unsupported, the case that was
not considered, and the cost that was not counted. Rate what you find.

The list below is where the proposing session was weakest. It is a starting
point, not a scope limit — an attack it did not anticipate is worth more than a
confirmation of one it did.

**A6 — the watch range encoding. Highest priority, most likely to be built.**
- Is the hazard reachable? The claim is that `alloc.c`'s first fit with only
  16-byte alignment can place a small array straddling a high power-of-two
  boundary, making `block_of` publish a block of hundreds of megabytes. Verify it
  against the allocator rather than accepting the arithmetic.
- Is it a hazard at all? Spurious wakes are legal and the §4.9 loop re-checks, so
  the claim is about throughput on a busy node, not correctness. Is that
  throughput cost real, and large enough to reorder your work for?
- The proposed encoding spends 23 bits on a grain count and 40 on a grain index:
  512 MB of watched range, 64 TB of region, saturating to whole-region beyond
  either. Are those the right budgets, and is a fixed 64-byte grain right when
  `OSOE_CACHELINE` is 128 on aarch64?
- The proposal asserts `OSOE_NODE_LAYOUT` need not bump because the word's width
  and its meaning as an opaque filter are unchanged. **That was not verified.**
  Check it against the layout-version rule.
- It asserts the §4.9 model's proof carries because only the predicate changes,
  not the protocol shape. Test that claim by breaking it.
- It rejects a simple cap on the block size, on the grounds that capping while
  keeping the start loses wakes and so breaks notify's exactness for rmw. Attack
  that: is there a capping scheme that does not lose wakes?

**A1 — `osoe_tp_sync`, and B6, its seam.** The strong argument was withdrawn; what
is left is ~4–6× on barrier, estimated, at a job size nobody will run soon.
Separately: the hook assumes one calling PE per node, "the local lead". Does that
hold for an arbitrary team, whose members may not cover every node and whose lead
may not be the node lead? Check §5.3. **Rejecting B6 is a legitimate outcome.**

**A2 — `osoe_tp_set_range`.** It is called collective, but the allocator is a
deterministic function of its call sequence, so every PE calls it identically
without coordination. Is "collective" the right word, and does it imply
synchronization that is not needed? Separately: does `realloc`'s hint preservation
actually work against opposite-end growth once the heap is fragmented?

**A3 — `OSOE_TP_MORE`.** Deferring an operation is claimed to be safe because the
backend owns the source bytes once a put is inlined or bounced. That is false for
a large zero-copy put. Is the "a backend that ignores it is correct" escape
sufficient, or does the flag need a stated size rule?

**A4 — dispatch per `(pe, operation class)`.** The conclusion that on-node atomics
must go through NIC loopback rests on a *recalled* claim that mlx5 reports
device-scope rather than global-scope atomicity. Is there a cheaper scheme that
does not need it?

**A7 — abort cadence.** "A bounded cadence" is not a bound. What is it, and where
does it come from?

**A9 — boundary metrics.** §3.14's cost rules require zero cost when nothing is
wrong. A counter for time spent spinning may violate that. Check.

**A10 — one registrable heap range per node.** Rests on the claim that a node
lead can register memory that other processes also map, and that remote traffic
can reach it. That is `roce-transport.md` V1, explicitly unverified and named
there as the load-bearing assumption of the RoCE plan.

### 2. Challenge, do not work around

Your existing rule stands: **blockers are raised, not worked around.** It applies
here with more force than usual, because these proposals were written by a session
that could not build or run anything.

Bring back anything ambiguous, wrong, or underspecified before implementing it,
with the options you see. A proposal you reject is a good outcome — record the
rejection and its reason in §10 or as a rejected alternative, in the design doc's
own style, so the question is not reopened from scratch later.

### 3. Implement what survives

Part B of the proposal says what it thinks this slice should do. In its own
ordering:

- **B1 — A6, before the multiple-element waits.** This is the one that reorders
  your work. The multiple-element waits in your scope are what arm the hazard.
- **B2 — record hints in `malloc_with_hints`/`realloc`,** rather than parsing and
  discarding them, and place hinted allocations at the opposite end per §4.4.
- **B3 — A7's abort paragraph,** while you are building the watchdog.
- **B4 — A9's counters,** in the catalogue you are already checkpointing with the
  user before writing `osoe-stat`.
- **B5 — A4 and A8 as header comments.** No code.
- **B6 — A1's seam.** Optional; see above.

Everything else in Part A is deferred: it needs a network, a device, or team
creation. Your slice's scope is otherwise unchanged — nothing here adds
networking, collectives beyond `barrier_all` and `sync`, or nonblocking RMA.

All your existing rules apply unchanged: C11 atomics only with named orders,
model-check before implementing any new concurrent protocol and break every rule
once to show the failure, every commit builds clean and passes every test in all
four presets in the VM, and the design doc is updated with rationale in the same
commit as the code.

## When you are done

- **Fold what you accept into `docs/design/shmem-ethernet-design.md`** with its
  rationale, in the sections each amendment names. That is where these decisions
  live.
- **Then delete `docs/design/transport-boundary-changes.md`.** It says so itself
  (§14). Leaving it is a second, diverging record of decisions the design doc now
  owns.
- **Decide with the user what happens to `docs/reviews/archive/roce-transport.md`.** It is
  analysis, not a decision, and it is the only place the RoCE cost model and the
  eight verification items are written down. It probably belongs in the committed
  record — but it is the user's call, not yours.
- **Report what you rejected and why**, not only what you built.
