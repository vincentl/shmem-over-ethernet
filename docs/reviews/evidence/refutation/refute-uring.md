# Refutation: io_uring / wakeup / ack findings (q1-transport B4, D1, D2, D4, D5, D6)

Design doc: `docs/design/shmem-ethernet-design.md` (line numbers refer to the file as it stands now).
Experiments: `scratchpad/exp/refute-uring/` (`r1_idle.c`, `r2_framing.c`, `r3_shared_ring.c`, `vm-output.txt`). The reviewer's `e1`/`e2`/`e3` were also rerun unchanged. All ran in the `shmem` VM (Debian 13, 6.12.95 aarch64, liburing 2.9).
Labels: **documented** = quoted from the design doc; **observed** = VM run or kernel source; **reasoning** = argued, not run.

## Verdict table

| ID | Verdict | Severity (reviewer → mine) |
|---|---|---|
| B4 | SURVIVES-DOWNGRADED | blocking → clarification (state a receive-parse rule plus a §9 invariant) |
| D1 | SURVIVES | design change (small, local fix) |
| D2 | SURVIVES-DOWNGRADED | design change → clarification; the shard half is refuted |
| D4 | SURVIVES | design change (a real hang; the fix is one rule) |
| D5 | SURVIVES-DOWNGRADED | design change → clarification (arithmetic right; §4.6 already has the knob) |
| D6 | SURVIVES-DOWNGRADED | design change → clarification (applies only when there is more than one shard) |

---

## B4. Receive-side framing across provided buffers is undesigned; holding pbufs stalls

**Verdict: SURVIVES-DOWNGRADED → clarification.**

**Reproduction (observed).**
- Reviewer's E2(b) reproduces exactly: `got 32768/65536 bytes in 8 pbufs, last terminal res=-105`.
- E2(a) is flaky on loopback. On my rerun, *no* CQE boundary fell inside a record (`CQE boundaries falling inside a record: 0`); the reviewer's run had 1. Arbitrary TCP cut points are still a certainty in general (reasoning), so this doesn't change anything.
- The E2(b) stall is a tautology of its setup: 8 × 4 KB = 32 KB is less than a 64 KB segment. My `r2_framing` shows the single-connection stall is purely a sizing artefact:
  - `(b) 32 pbufs, hold-until-complete, 1 conn, 64KB: completed`
  - `(c) 8 pbufs, stream-parse/recycle, 1 conn, 64KB: completed`
- The reviewer's stronger claim does reproduce: partial segments on several connections pin a *shared* ring jointly. In `r3_shared_ring`, two connections share a 128 KB ring and each sends half of a 128 KB segment before the second halves:
  - `hold-until-segment-complete : conn0 64997/131072 conn1 64997/131072 held=16+16 ENOBUFS=22 -> STALLED (both conns)`
  - `stream-parse, recycle per CQE: ... completed`
- So "hold pbufs until the segment completes" is a real permanent-stall design. Recycling on every CQE is not.

**Would a reader of §3.2/§3.3 get this wrong? Mostly no. The doc already points at stream parsing:**
- §3.7 L366–370 (documented): a scheme "that stops consuming receive buffers exhausts the provided buffer ring, stalls the peer, and reproduces the symmetric deadlock. **Any scheme where inbound processing can be halted by an outbound resource shortage is a cycle waiting to close.**" Pinning pbufs across reaps is the same "stops consuming receive buffers" shape, even though the shortage here is inbound.
- §3.15 L611 (documented): "A connection dying mid-message leaves part of a payload written into the symmetric heap". That is only true if payload is applied *as bytes arrive*. So the author's model is already the reviewer's option (ii), stream-parse with a per-connection cursor, not hold-until-complete.
- §3.3 L214–216, "bounded … by how many provided buffers the kernel can fill", and the CQ formula only hold if pbufs are returned promptly. That is an implicit, but unstated, recycle rule.
- §8.2 L1475–1478 flags partial reads as "the most common defect in hand-rolled TCP framing". The author knows the byte stream is cut arbitrarily.

**Why not refuted.**
- §3.2's "no reassembly, no fragment tracking" (L179–180) is literally false at the byte level. A per-connection parse state (partial header ≤ 32 B, in-progress message cursor) is required and unnamed.
- Nothing forbids the zero-copy "parse in place, recycle after dispatch" variant, and that variant deadlocks across connections (observed, r3).
- That makes this a missing rule, not a missing mechanism. Every piece needed for option (ii) already exists (§4.6 `bytes_landed` "progress thread adds as chunks land", L785).

**Tightest correct statement.** The doc never states that receive pbufs are recycled before the next reap. A pbuf-pinning parse strategy, which the text doesn't exclude, deadlocks every connection sharing the ring (reproduced with two connections).

**Minimal fix.**
- Add to §3.3 and §9: "Every provided buffer is returned to the ring before the next reap. Per-connection receive state is a stream cursor: a partial header of at most header-size bytes is copied, and payload bytes go straight to their destination."
- Amend §3.2's "no reassembly" to "no chunk reassembly".

---

## D1. §3.10 idle loop re-arms `IORING_OP_FUTEX_WAIT` every iteration → unbounded armed waits, CQ overflow

**Verdict: SURVIVES (design change; the fix is a few lines).**

**Reproduction (observed).**
- Reviewer's E1 reproduces: `futex SQEs submitted=1000, futex CQEs=0 … 1000 futex waits still armed`, then `CQEs from that single wake: 1000 (CQ size 64)`.
- E1 reports `overflow flag=clear`, but it checks *after* liburing's peek had flushed the overflow list, so it can't show overflow. My `r1_idle.c` fixes that. It uses a real multishot TCP recv with a 16-entry pbuf ring as the competing CQE source, runs the §3.10 loop literally, and checks the flag *before* reaping:
  - `mode=literal: idle iterations=1881 futex SQEs=1881 futex CQEs=0 armed now=1881`
  - `wake(1) woke 1; then wake(INT_MAX) woke 1880; CQ ring overflow flag before reaping: SET, cq_ready=64 (CQ size 64)`
  - `CQEs eventually reaped via peek (liburing flushes overflow list): 1881`
- With an armed flag the problem disappears: `mode=armed-flag: … futex SQEs=1 … armed now=1 (max 1)`, overflow `clear`.

**Doc text checked.**
- §3.10 L467–474 (documented) arms a new wait on every idle pass, with no guard and no cancel.
- §3.3 L218 CQ formula and §9 invariant 12 (L1625–1627) promise the CQ "can never fill". §3.3 L231–233 explicitly rejects relying on `IORING_FEAT_NODROP` overflow.
- The literal loop enters exactly that rejected path (observed flag SET).
- **Mitigation (reasoning).** No wakeup is lost. Any stale futex CQE still wakes the loop, and `wake(1)` completed one stale wait. Correctness holds; the damage is the CQ bound, kernel memory per armed wait, and bursts.

**Tightest correct statement.** The §3.10 snippet leaks one armed `FUTEX_WAIT` for every idle pass that is ended by some other CQE. The count is unbounded, and a wake-all floods the CQ past its size into the overflow list (observed on 6.12).

**Minimal fix.**
- Track `futex_armed`. Arm only when it is false, and clear it on the futex CQE (any result, including `-EAGAIN`).
- Add `+1` per ring to the §3.3 CQ formula, plus teardown cancel CQEs.

---

## D2. Futex words shared across resource classes + wake-one → lost wakeup

**Verdict: SURVIVES-DOWNGRADED → clarification. The cross-shard half is refuted.**

**What §3.11 actually says (documented).**
- L542–544: "Keep futex words few (one per connection, not one per PE per connection) and cache-line aligned. Futex waits hash into kernel buckets by address; unrelated words can collide on the same bucket spinlock."
- L515–519: "Buffer becomes `OPEN` (divisible …) → **wake many.** … Single descriptor frees (indivisible) → **wake one.**"
- §4.6 L862 (documented): the descriptor pool is "a per-connection descriptor pool sized in bytes; exhaustion blocks the PE".

**Assessment.**
- The doc does *not* say buffer-space and descriptor waiters share a word.
- The "one per connection" sentence contrasts granularity along the *PE* axis, not the resource-class axis.
- Read literally, though, "one per connection" with two waiter classes on the same connection (buffer space §3.5/§3.11, descriptors §4.6) does produce a shared word. The doc never says otherwise, so the reading is available to an implementer.
- **Reproduction (observed).** Reviewer's E3(b) reproduces: `buffer-space waiter woken (wake #1), no buffer, re-sleeps` / `descriptor waiter: TIMED OUT after 1.00s`. The model is correct *given* a shared word; it doesn't show the doc prescribes one.
- **Hang vs spike.** §3.10 L497–504 (documented) says PE-side `futex_wait` calls are wrapped once, with "only a deadline-computing helper" exposed. That strongly implies every PE-side wait has a deadline, so the realistic outcome is a latency spike with a counter, not a hang. §3.11 itself doesn't restate this.
- **Shard half refuted (documented + reasoning).**
  - §3.10's snippet uses `&ep->wake` and `&ep->ring` together, and §3.3 L187 says "One ring per progress thread". So `ep` is a per-progress-thread object, and `ep->wake` is per shard by construction.
  - §3.12 L556 shards by peer node, so a PE knows which shard owns the connection it sealed.
  - Nothing in the doc suggests a single node-wide `ep->wake` shared by several shards. The reviewer's scenario requires contradicting the snippet.
- The reviewer's E3(a) also printed a SCHED_FIFO-first wake order on its run; my rerun couldn't set SCHED_FIFO (`98`). That isn't part of D2 and doesn't matter for it.

**Tightest correct statement.** §3.11 doesn't say whether the divisible (buffer-space) and indivisible (descriptor) waiter classes on one connection use separate futex words. If they share one, the prescribed wake-one for descriptors can be consumed by a buffer-space waiter, and the descriptor waiter then waits until its deadline.

**Minimal fix.**
- State "one futex word per (connection, resource class)". That is still "few": two per connection.
- Alternatively, require a waiter that can't use a wake to pass it on with `wake(1)`.
- State explicitly that §3.11 waits carry a deadline, as §4.9 does.

---

## D4. No ack-only segment path for one-way traffic → `quiet` / `barrier_all` hang

**Verdict: SURVIVES (design change, small).**

**Search of §3.4, §3.8, §3.9, §3.10, §3.14 (and §3.3, §4.6, §5.6) for anything that sends an ack without reverse data (documented):**
- §3.4 L265: "**Piggybacked acknowledgement** of the reverse direction". This is header-only, and there is no rule for emitting a header without messages.
- §3.8 L438–440: "**Quiet acknowledgements:** coalescable to one outstanding per connection (a high-water mark subsumes older ones); piggybacked, so not really a queue entry." This is the closest text. It acknowledges an "ack owed" state, but explicitly says the ack rides on other traffic rather than being a drained item.
- §3.9 L460–461: "Explicit flush on `fence` and `quiet` covers the case where there is no reverse traffic to clock it." This is the *sender's* flush of its own data (§4.6 L805 `seal_and_submit_dirty_buffers`). It doesn't make the receiver emit anything.
- §3.10 L492–495: the safety sweep "submit[s] anything sealed". An idle receiver has nothing sealed.
- §3.14 L589–590: slow mode has "synchronous acknowledgement". That's a policy name with no mechanism, and only in slow mode.
- §3.3 L243–247: the author notes that piggybacked acks mean "reverse traffic usually clocks it out — so this looks perfect under ping-pong benchmarks and stalls on one-way patterns". This concerns Nagle, but it shows the one-way hazard is recognised and the AM-level instance isn't closed.
- §4.6 L807: `wait_until(acked[...] >= pe->high_seg[...])`. §5.6 L1233: `barrier_all` "must seal the dirty set and wait for acknowledgements".

**Partial refutation of one supporting claim.** The reviewer says the progress thread "may reserve only in DRAINING buffers". That's wrong:
- §3.8 L422 restricts DRAINING buffers to the progress thread, not the progress thread to DRAINING buffers.
- §3.8 L381 ("reservation may now *fail* — the progress thread leaves the descriptor in place") implies the progress thread may try any buffer.
- §3.9 L456 ("falls to one when idle") implies an idle connection submits promptly.

A queued *message* on an idle B→A connection would therefore be sent. The gap is only that an ack isn't a message and nothing triggers a header-only segment.

**Scenario check (reasoning).** A→B puts only, B has no traffic to A.
1. A's `quiet` flushes, and B applies.
2. No B→A segment is ever sealed, so `acked[B]` at A never advances.
3. A's `quiet`, and therefore `barrier_all` and finalize, waits.
4. A deadline on the PE-side wait would only turn this into a loop, since nothing ever sends the ack.

This is a deterministic hang on a common pattern (e.g. a scatter from rank 0 followed by `barrier_all`). Survives.

**Minimal fix.**
- Rule: "After a reap batch that applied data on a connection whose ack high-water exceeds the last ack sent, and with no send in flight or sealed on that connection, the progress thread seals and submits a header-only segment."
- Progress-thread-only header writes don't involve reservation, so §3.7 is unaffected.
- A sender-set "ack requested" header flag (set by fence/quiet flushes) can limit this to when it's needed.

---

## D5. Deferred-queue preallocation is per connection → ~6 GB/node at 500 nodes

**Verdict: SURVIVES-DOWNGRADED → clarification.** The arithmetic is correct against §3.8's own numbers, but §4.6 already supplies the sizing knob that removes the problem.

**§3.8 text (documented).**
- L374: "Per connection."
- L376–378: "~4 KB per outstanding get … a *descriptor* commits ~48 bytes. At `P=256`, that is the difference between roughly a gigabyte **per node** and roughly 12 MB."
- L442: "Size as `P×G + degree×max_teams + slack`. Pre-allocate at maximum."

**Arithmetic check.**
- 1 GiB / 4 KiB = 262,144 = P·G, so G = 1,024 at P = 256.
- 262,144 × 48 B = 12.58 MiB. That matches the doc's "roughly 12 MB".
- P is a peer node's PE count and G a per-PE outstanding-get bound, so `P×G` is the worst case *one peer* can direct at this node. Each connection needs its own copy under "Pre-allocate at maximum", so a node holds (N−1) × 12.6 MB:
  - ≈ 189 MB at 16 nodes
  - ≈ 1.26 GB at 101 nodes
  - ≈ 6.3 GB at 500 nodes
- §1 L23–24 sets "256 PEs per node and hundreds of nodes" as a target. The reviewer's figures are right, and "per node" in L378 is really a per-connection figure. **Arithmetic confirmed.**

**What G is.**
- §4.6 L857: "`G` is **not** a spec concept."
- §4.6 L861–865: "Express it as a **resource** … a per-connection descriptor pool sized in bytes; exhaustion blocks the PE … 16-byte descriptors make a pool of thousands cost megabytes."
- That origin-side pool is per *connection*, shared by all of the origin node's PEs, not per PE. Using it as the bound makes the target-side queue per connection = pool size (thousands of entries), not P×G:
  - e.g. 4,096 × 48 B ≈ 200 KB per connection
  - ≈ 100 MB per node at 500 nodes (reasoning)
  - smaller still with lazy growth
- The doc is also internally inconsistent: 48-byte descriptors in §3.8, 16-byte in §4.6.

**Tightest correct statement.** §3.8 sizes each per-connection queue as P×G and calls the result per-node memory. Summed over connections, that is O(nodes × P × G) pinned memory (≈ 6 GB/node at the doc's implied G = 1,024 and 500 nodes). §4.6's per-connection byte-sized descriptor pool is the intended bound, but §3.8 doesn't use it.

**Minimal fix.**
- Size target queues from §4.6's per-connection origin pool, not P×G.
- Correct "per node" to "per connection" in L378.
- Reconcile 48 B vs 16 B.
- Keep assert-on-overflow; it stays meaningful because the pool is enforced at the origin.

---

## D6. Handlers enqueue collective forwards onto connections owned by other shards

**Verdict: SURVIVES-DOWNGRADED → clarification.**

**Doc text (documented).**
- §3.7 L340–341: "collective forwarding (§3.8) is a handler issuing a *request*".
- §3.8 L374: queues "Per connection"; control queue holds "collective forwards" (L390).
- §3.8 L432–434: "if X holds an unforwarded epoch-N arrival", so intermediate nodes do forward in handlers.
- §3.12 L556–557: "Shard by peer node so each connection has exactly one owner, preserving single-threaded ordering per stream."
- §3.12 L555: "On a Pi it is one. At 256 PEs perhaps four."

**Assessment (reasoning).**
- With more than one shard, an arrival from child C is handled on shard(C), and its forward targets parent Q's connection, which may belong to another shard. Release broadcasts to children fan out the same way. Get replies and AMO results return on the arriving connection and are unaffected.
- The doc says nothing about cross-shard enqueue, so the finding survives.

**Why downgraded.**
1. It can't arise on the primary target, where there is one progress thread per node.
2. "Exactly one owner" stays true for ring submission, reaping, and stream ordering. Only the control-queue *enqueue* needs to be multi-producer. Local PEs already produce into connection buffers from arbitrary threads through the lock-free §3.5 CAS, so a lock-free enqueue is in keeping with the design.
3. §3.7's no-blocking proof holds if the enqueue is a bounded lock-free push.
4. §3.8's bound (degree × teams per connection) is unchanged by who enqueues.
5. The waking shard bumps the owning shard's `ep->wake`, which is per shard (see D2).

**Tightest correct statement.** With more than one progress thread, handler-issued collective forwards and release broadcasts are enqueued from a non-owning shard. §3.8's control queues must be multi-producer (bounded, lock-free), and the enqueuer must wake the owning shard's word. "Exactly one owner" should be scoped to submission/reaping.

**Minimal fix.** In §3.8/§3.12, declare control queues MPSC (a fixed-capacity lock-free ring sized by the existing bound), and state that a cross-shard enqueue is followed by bumping and waking the owner shard's `ep->wake`.
