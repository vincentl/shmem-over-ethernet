# What changed since the first review

The first round (`design-soundness.md`, `core-api.md`, `roce-transport.md`,
`watch-notification-options.md`) reviewed **a design and almost no code**: about
1,700 lines of `docs/design/shmem-ethernet-design.md`, a CMake skeleton,
`libosoe` with two library-information routines, and `tools/probe`. Its prompt
still says so, and that prompt is a committed record which is not rewritten
(`docs/prompts/README.md`).

This note is what a second round needs to know before reading either the design
or the tree.

## The scale of the difference

| | First review | Now |
|---|---|---|
| Design doc | ~1,700 lines | **4,420 lines** |
| C source (`shmem/`, `base/`, `tools/`) | a skeleton | **~13,750 lines** |
| Tests | none to speak of | **131**, four presets |
| Commits | — | 91 total, 27 on `phase1-step2-slice1` |
| Implementation status | §7.1 step 0 | **§7.1 step 1 merged, step 2 slice 1 complete** |

The design doc is no longer mostly a plan. Roughly half of it is now a record of
decisions *made against code*, several of which reversed what the first review
read.

## What now exists that did not

**Step 1, merged to `main`:** the symmetric heap and a deterministic allocator
(§4.4); the §4.10 transport boundary with an intranode backend; the control
segment with §3.14's self-describing metric table, per-thread counter blocks and
the "blocked in" record; `osoe-stat`, which maps that segment read-only and
shares no headers with the library; agent threads and the §6.4 lifecycle; the
§4.9 watch mechanism; **526 routines**, each a strong `pshmem_` definition with a
weak `shmem_` alias, with teams and contexts plumbed on every entry point.

**Step 2, slice 1, on the branch:** everything below the §4.10 boundary and then
the join to the SHMEM layer — the §8.1 logical node map and launcher; the §6.2
job-wide rendezvous; the node-pair wire (a byte stream, not a message queue);
§3.4's segments and §3.5's reservation protocol; the connection area; §3.8's
deferred reply queues and per-connection credit; §3.12's landing zones; §3.13's
handler table and §3.3's stream parser; the progress loop; the internode phase
of a barrier; multi-node `shmem_init`; and the §4.10 data path — put, get, the
atomics, `fence`, `quiet`, `pe_quiet`, `put_signal` — dispatching on locality.

## Decisions taken against the code, which the first review could not have seen

These are the ones a reviewer should treat as the most interesting, because each
one is a place where writing the thing changed the answer.

- **`osoe_transport_sync` is closed, and the answer is no.** §10 scheduled the
  question for the step that writes its only caller. Two obstacles turned out to
  be concrete rather than estimated: §4.10 addresses a *PE* while a collective's
  internode phase addresses a *node*, and §5.4 makes each epoch counter its own
  futex word, which `osoe_transport_notify` does not mean. The boundary stays
  point-to-point and PE-addressed.
- **§8.1's force-remote flag lost most of its purpose.** It was to route
  same-logical-node traffic over the network, described as "the strongest
  differential test available". Connections exist per node *pair*, so there is
  none from a node to itself and such traffic has nowhere to go. The node map
  already expresses every other arrangement, so the differential test is now two
  node maps, and the flag keeps only its locality half.
- **The connection area is a separate object from the control segment**, which
  keeps §10's "does the AM data path stay hot in an RDMA build" open rather than
  settling it by accident.
- **`OSOE_TRANSPORT_MORE` has a caller**, `put_signal`, and defers the wake
  rather than the seal.
- **The control segment reached layout 7**, and `tools/stat` grew
  `OSOE_SEGMENT_HAS`, because its length check had made §3.14's append-only rule
  one-directional.

## What is still not built, and is therefore not reviewable as code

Real sockets and io_uring submission (step 3); pidfd liveness, abort propagation
and `shmem_global_exit` (step 4); symmetric statics and the compiler wrapper
(§4.3 phase 1.5); collectives beyond `barrier_all` and `sync`; nonblocking RMA
and AMOs; locks; `SHMEM_THREAD_MULTIPLE` (§4.11); teams beyond the two
predefined; the §7.5 typed RMA cross product; the §3.14 flight recorder.

The wire between logical nodes is **shared memory standing in for a socket**.
Nothing has yet run over TCP, and §8.2 is explicit that loopback would hide the
worst of it anyway.

## Known limits of the evidence

A reviewer should not take these as verified, because they are not:

- **ThreadSanitizer does not cover the multi-node data path.** The ordering that
  makes an applied put safe runs through another process, and tsan sees one. The
  multi-node maps of `shmem.internode` and all of `shmem.differential` are not
  registered under it.
- **Nothing has been run on more than one physical machine**, or on a Raspberry
  Pi, or under Slurm. The launcher is `osoe-launch`, and the node map is a
  simulation of a cluster on one host.
- **Performance is unmeasured.** There is no benchmark in the tree. Every
  performance claim in the design is an argument.
- **Two intermittent failures were chased down and fixed** rather than left for
  this review; both needed artificial CPU load to reproduce, which is why they
  had only ever appeared in the full suite. They are noted here because the
  *class* is worth a reviewer's attention: in both, a test asserted something
  the code did not promise, and the promise turned out to be the thing worth
  fixing. See the commit for the measurements.

- **The models prove what they model.** Five exist
  (`docs/design/evidence/`), each with deliberately broken variants, and they
  cover the wire, the reply credits, the §5.3 arrival handoff, the blocked
  record and §4.11. They do not cover the reservation protocol, which has two
  independent models from the first review instead.
