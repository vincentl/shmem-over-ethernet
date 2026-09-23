# Prompts

The brief each working session was given, kept so that a decision can be traced
back to what was asked for as well as to what was done.

**Naming.** Three numberings appear in the design notes and only one of them is
the unit of work, so the filenames use that one:

- **Phase** — §7.1's heading. Only phase 1 is enumerated, so `phase1` carries no
  information on its own and never stands alone in a name.
- **Step** — §7.1's numbered items 0–4. **This is the unit of work.**
- **Slice** — a session-sized chunk of a step. A prompts-only idea; the design
  notes do not use the word.

So `phase1-step1-slice2-prompt.md` is the second slice of §7.1 step 1. An
earlier round of files omitted the step and were named `phase1-sliceN`, which
read as "the Nth part of phase 1" when both slices were in fact parts of step 1;
they were renamed rather than left to mislead.

A fourth numbering shares the word and is unrelated: **§4.3's** phases 1, 1.5 and
2 stage *symmetric-statics support* only, and §4.3 phase 1.5 is scheduled after
§7.1 step 4.

`task-*.md` files are one-off jobs — a merge, a review — rather than a slice of a
step.

**These files are not updated when the code they describe is renamed.** They are
the brief as it was given, and rewriting one would make it describe something
that was not asked for. Two driftings are known and harmless: the earlier
prompts say the development VM is called `shmem` where §8.5 now says `osoe`, and
every prompt before the naming standard (CLAUDE.md) spells the transport
boundary `osoe_tp_*`, which is `osoe_transport_*` in the code. The design notes
and `docs/reviews/` do track the current names.

| File | Covers | Outcome |
|---|---|---|
| `phase1-step1-slice1-prompt.md` | §7.1 step 1, first slice: init, heaps, barrier, RMA, first AMOs and waits | merged, `5ada021` |
| `task-merge-thread-multiple.md` | Merge the §4.11 `SHMEM_THREAD_MULTIPLE` design branch | merged, `fcef106` |
| `phase1-step1-slice2-prompt.md` | §7.1 step 1, second slice: signals, watchdog, introspection, conformance breadth | see below |
| `task-boundary-changes-review.md` | Review the RoCE-driven §4.10 proposals mid-slice, implement what survives | `2e44e7f`, `4cf2a93` |
| `phase1-step1-slice2-continuation-prompt.md` | Resumption of that second slice | merged, `5955d20` and `4e96700` |
| `phase1-step2-slice1-prompt.md` | §7.1 step 2, first slice: the in-process AM backend | see below |
| `phase1-step2-slice1-continuation-prompt.md` | Resumption of that first slice | in progress |
| `phase1-step2-slice2-prompt.md` | §7.1 step 2, second slice: what the second review (`docs/reviews/phase1-step2-slice1-review.md`) found in the protocols and failure paths — the acknowledgement protocol, the pre-open drain, the reply pool, sweeps and the no-progress rule, receive-side validation, how a job dies and what it leaves behind | merged, `e03fbd5`. All four parts. B2's starvation did not reproduce and the pre-open drain was built on the model and the connection test (the user's choice). Two questions left open in §10: fence on aarch64, and whether the `/dev/shm` capacity check should warn rather than fail |
| `phase1-step2-slice3-prompt.md` | §7.1 step 2, third slice: the internode collective interface beside §4.10, the k-ary tree, send-side work discovery, the O(P) arrays, the chained wake, per-connection metrics, force-remote. Requires slice 2 merged | branch `phase1-step2-slice3`, six commits, all seven parts. Three decisions taken with the author: the tree's fan-out default belongs to the backend (AM 4, RoCE would be 8-16) and the step-2 barrier regression is the poll interval, not the tree; the chained wake is the progress thread's release only, since on four cores a chain loses to one wake-all; part D's +0.4 ns on the put floor is accepted. Two briefs' premises were found false by model or test: a chained wake needs futex bitsets (part E), and the pre-open drain leaves work no writer marks (part C). Merged, `a50c829` |
| `phase1-step3-slice1-prompt.md` | §7.1 step 3, first slice: the `echo-uring` floor harness, io_uring and a real socket behind the wire's contract, §3.10's armed-futex idle loop, the socket rendezvous with its compressed map | branch `phase1-step3-slice1`, fifteen commits, all five parts. Two decisions taken with the author: the agent seam grows `osoe_transport_agent_wait` and `_exit` in place of `idle_ns`, since a single-issuer ring refuses every thread but its submitter; and the rendezvous is a tree, not §6.2's star. Decided here: `OSOE_JOB_SECRET`, optional and proved with SipHash, never sent; `set_range` not acted on, since TCP has nothing to prepare; the mesh kept eager, with on-demand described in §6.2. Found: the rendezvous was written before its model, and the model found it hung on a miscounted launch until a lead called its parent first and the hello carried the count; step 2's `poll_lead` sleep holds a put and quiet at 4× the floor and a stream at 3% of it, and belongs to the next slice's per-class wake words; io_uring's median wake measured 3–5× a blocking socket's beside one load and not beside another. Not merged |
