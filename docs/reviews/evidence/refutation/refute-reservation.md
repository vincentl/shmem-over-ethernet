# Refutation: reservation protocol findings B2, B3, C1, D3 (from q1-transport.md)

Refuter scope: design §3.4, §3.5, §3.8 (DRAINING), §3.9, §4.6, §9.
Design doc: `docs/design/shmem-ethernet-design.md` (§3.4 is at L253, §3.5 at L279, §3.8 starvation guard at L416, §3.9 at L452, §4.6 at L756, §9 at L1599).

Artifacts I used (scratchpad, not in the repo):
- The reviewer's model `exp/q1-transport/resv_model.py`. I read all of it and reran `one_open`. It reproduces exactly: 6,215 and 76,513 states, same violations.
- `refute_model.py`, my own independent model. It is exhaustive BFS with every step atomic. Its switches:
  - seal mode: `state` = plain state store; `recheck` = PT re-reads head after the snapshot and before sending; `headcas` = seal bit lives in the head word.
  - open rule: `any`; `one` = at most one reservable buffer; `cqe` = at send completion, seal the other buffer if it is OPEN and non-empty (the §3.9 reading).
  - DRAINING: on or off.
  - When the seq is assigned: at open or at seal.
  - Also: a liveness check (NOLIVE: some reachable state can no longer finish), bounded reply descriptors, and the PT may reserve replies in OPEN as well as DRAINING.
- An inline re-run of the D3 simulation with non-zero round-trip delay.

Labels: **documented** = quoted from the design doc; **observed** = produced by running a model or simulation; **reasoning** = my own argument, not checked by a run.

---

## B2. §3.5 state check followed by a CAS on `head` races with seal/snapshot

**Finding restated:** a writer reads `buf->state == OPEN`, then CASes `head`. Seal changes only `state`, so a stale writer's CAS can land after the PT has checked `filled == head` and taken its snapshot. The reservation is then lost or torn, and `filled > head` in the next generation.

**Verdict: SURVIVES-DOWNGRADED.** Blocking → design change (local, small). The defect is real, but the architecture is intact: the fix keeps "two atomics, no locks".

### Is the reviewer's reading forced by the doc?
Essentially yes (documented):
- The snippet (§3.5 L285–291) loads `head`, then reads `buf->state` as a plain non-atomic field, then CASes `head` against the loaded value. Nothing in the CAS encodes seal.
- "Two atomics" (L283) are `head` and `filled`, so `state` is not one of them.
- L317–320 says "the buffer cannot recycle until `filled == head` and this writer has not completed". That argument assumes that once a writer passes the check, its reservation is visible in `head` before the PT compares. The snippet does not guarantee this.
- The doc never says seal touches `head`. An implementer who follows the text writes `buf->state = SEALED`, and the race follows.

### Can "PT sets state, then waits for (or re-checks) `filled == head`" close it?
No (observed + reasoning). The window is between the writer's state read and its CAS. A writer can be descheduled there for any length of time, and §3.5 L307–309 itself warns about descheduled writers. No finite wait or re-check by the PT can see a CAS that has not happened yet. And "never spin" (§9 inv. 4) rules out waiting anyway.

I modelled the most charitable variant (`recheck`): after `filled == head`, the PT re-reads `head` and `filled` and aborts the submit on a mismatch. It still fails:

```
recheck/one/drain=0  writers=[[1,1],[1]] cap=2  states=7532  viol=[ACCT, LOST, NOLIVE, STALESEQ, TORN]
  W0 load b0 head=1; W0 check ok b0; PT seal b0; PT submit-check b0 snap=1;
  PT recheck ok b0 submit len=1; W0 CAS ok off=1 b0 (state SENDING); PT complete b0  -> LOST
recheck/one/drain=1  (with DRAINING)  adds DRAINRES
```

Re-checking after the send cannot help either. The late bytes sit at an offset after the header of a segment that has already gone. Recovering them would need a second header plus an iovec re-send, which the doc does not describe.

The same race also breaks §3.8 (observed, `state/one/drain=1`). A writer that read OPEN in generation N can CAS into generation N+1 while that generation is DRAINING, at the same `head` value (ABA on 0). That is exactly the "only the progress thread may reserve" rule failing (DRAINRES).

### Model faithfulness (reviewer's `resv_model.py`)
- The writer's steps (load head → read state → CAS → read seq → memcpy → `filled += n`) match the snippet line for line.
- Seal happens at any time. That is a superset of the doc's triggers (§3.9 CQE, fence/quiet §4.6, safety sweep §3.10), and since fence can seal at any moment, the superset is legitimate.
- Submit requires `filled == head` and snapshots `head`, as §3.5 L302 says.
- The `literal` variant (writer blind-stores SEALED) is an invented reading, and the reviewer flags it as such. B2 does not depend on it: `one_open` and my `state`/`recheck` variants reproduce the defect without it.
- One real modelling gap: the reviewer's `draining` variant is unbounded, because reply descriptors are created at every recycle. Its counterexamples are real but its coverage is not exhaustive. My variant bounds replies and is exhaustive (see B3).

### Does the fix work?
Observed. Put the seal in the head word: the PT seals with a CAS or exchange that sets a bit, a stale writer's CAS then fails, FREE and DRAINING buffers carry the bit, and at most one buffer is reservable. With that, all configurations show 0 violations, including DRAINING, PT reply reservations and the liveness check:

```
headcas/one/drain=1/seq@open  [[1,2]] 1330 st, [[1,1],[1]] 58088 st, [[2,1],[1,1]] cap3 623661 st: viol=[]
headcas/one/drain=0/seq@open  ... 468559 st: viol=[]
```

The reviewer's extra requirement that "a writer that fails for space waits rather than moving on" is **not** needed. My `one` variant lets the writer try the other buffer and passes: the other buffer is never reservable, so the attempt just fails and the writer retries.

**Tightest statement:** §3.5 seals by changing a field that the writer's CAS does not compare, so a writer between its state check and its CAS can reserve past the submit snapshot or into a recycled or DRAINING generation. The "self-stabilizing generation" argument (L317–320) is therefore false as written.

**Fix direction:** seal (and FREE/DRAINING) must change the value the writer's CAS compares, e.g. a SEALED bit in `head` set by the PT's CAS or exchange. `state` becomes a hint at most. Add this to §9.

---

## B3. Two reservable buffers → same-PE reordering and non-monotone segment sequence numbers

**Finding restated:** "try other buffer" together with "freed → DRAINING → OPEN" lets both buffers accept reservations at once. A PE's large op then goes into buffer 1 while its next small op still fits in buffer 0, so ORDER and SEQ break and `quiet` can return early.

**Verdict: SURVIVES-DOWNGRADED.** Blocking → design change (one stated invariant). It does not depend on B2.

### Refutation attempts
1. **§3.4 L255 "one filling, one in flight" already excludes two OPEN buffers.** If you read this as a rule, the finding disappears. Observed:
   - The reviewer's `one_open` shows no ORDER or SEQ.
   - My `headcas/one` shows 0 violations with DRAINING on.

   But the doc states it as a sizing remark ("A third smooths nothing"), not as an invariant. §3.8 L421–423 then says a freed buffer "drains, then transitions to `OPEN`" with no condition on the other buffer. §9 does not list it.
2. **§3.9 L454–457 (the CQE swap) prevents it.** "On the CQE, submit everything queued as one segment" suggests the filling buffer is sealed exactly when the in-flight one frees. The reviewer's `pt_seals` trace relies on the initial FREE buffer opening next to a non-empty OPEN one, which this reading would forbid.

   So I modelled the CQE swap directly (`cqe`): at completion, seal the other buffer if it is OPEN and non-empty, starting from b1 in flight. The defect still reproduces (observed). The CQE finds b0 OPEN but **empty**, so b0 is not sealed, and b1 goes DRAINING → OPEN. Now two buffers are OPEN, and a fence/quiet flush (§4.6 `seal_and_submit`) seals b1 first:

   ```
   cqe (b1 initially in flight)  [[1,2,1]] cap=2: viol=[ORDER, SEQ]
     PT complete b1; W0 op0 (size1) -> b0; W0 op1 (size2) fails b0 -> b1;
     PT recycle b1 -> DRAINING; PT drained b1 -> OPEN; W0 op1 CAS b1 ...;
     PT seal b1; PT submit b1; PT complete b1 seq=1 (seals b0); PT submit b0; PT complete b0 seq=0
   same with one-open rule: [[1,2,1]], [[1,1],[1]], [[1,2],[1]]: viol=[]
   ```

   An idle connection that only carries replies naturally has an empty OPEN buffer at CQE time, so this is not contrived.
3. **Severity.** Per-PE order within the stream is what §3.1 "fence requires no per-operation tracking" and §9 inv. 5 (signals after payload) rely on. A seq-1 segment ahead of seq-0 lets `acked >= high_seg` (§4.6) pass before op 0 is applied. That is a correctness bug, not only a performance one.

**Tightest statement:** the doc never states that at most one buffer per connection is reservable. The §3.8 DRAINING→OPEN transition and the "try other buffer" comment allow two, and then seal order (e.g. from fence) can put a PE's later op, and a higher seq, on the wire first.

**Fix direction:** add to §3.5/§9:
- at most one reservable buffer per connection;
- a freed buffer leaves FREE/DRAINING for OPEN only when the other buffer is not reservable (sealed, in flight, or retired if empty);
- buffers are submitted in seal order and seqs are assigned in that same order.

No writer "wait instead of try other" rule is needed (observed).

---

## C1. Sequence number "filled at seal time" (§3.4) vs "writer reads it after reserving" (§3.5)

**Verdict: SURVIVES as a clarification (low).** It is not a design defect.

- The two passages can be reconciled (documented). §3.4 L259–260 says the *header* is "filled by the progress thread at seal time", which is about writing the header bytes. §3.5 L317 calls the value a "generation number" that the writer reads after reserving, which implies it is *assigned* when the generation starts (recycle/open) and only written into the header at seal. So a consistent reading exists.
- The other reading is plausible and wrong (observed). Assign seq at seal and even the fully fixed protocol fails:
  ```
  headcas/one/drain=0/seq@seal: viol=[STALESEQ]  (W0 read seq=0; PT seal b0 seq=1; sent in seq 1)
  headcas/one/drain=1/seq@seal: viol=[STALESEQ]
  ```
  `quiet` then waits for the previous generation's ack and returns early.
- Seq-at-open is monotone on the wire only under the B3 one-reservable invariant. That ties C1 to B3.

**Fix:** state explicitly that the seq is assigned when the buffer becomes reservable (in submission order), and that only ack, count and length are filled at seal.

---

## D3. The DRAINING starvation guard starves PE writers under sustained reverse get load

**Verdict: SURVIVES** (design change, medium). The reviewer's zero-think-time assumption overstates it at low PE counts, but not in general.

### Sim assumptions checked (`draining_sim.py`)
- One DRAINING pass per generation, one chunk per descriptor, limited by buffer capacity. This matches §3.8's round-robin rule, and "drains" meaning "to empty" or "one pass" makes no difference once one pass exceeds 256 KB.
- A's PEs always want the rest of the buffer. This is conservative: it maximises the PE share.
- **Zero think time: optimistic toward starvation.** A reply has to cross the wire, wake a B PE, and send a new request back over B→A, where B's own aggregation clock applies.

### Re-run with a delay of d generations before the next get is enqueued (observed)
One A generation is about one 256 KB send, roughly 2 ms at 1 GbE, so d = 1–2 is a realistic LAN round trip.

| B PEs × get size | d=0 | d=1 | d=2 | d=5 |
|---|---|---|---|---|
| 4 × 64 KB | 0.00 | 0.50 | 0.67 | 0.83 |
| 8 × 64 KB | 0.00 | **0.00** | 0.33 | 0.67 |
| 16 × 64 KB | 0.00 | **0.00** | **0.00** | 0.33 |
| 256 × 4 KB | 0.00 | **0.00** | **0.00** | 0.33 |
| 16 × 1 MB | 0.00 | **0.00** | **0.00** | **0.00** (longest starved run 3934/4000) |

Cells show A-PE share of segment bytes. With 4 PEs and a delay of at least 1, the guard does not starve A. It does starve A completely whenever B's outstanding reply bytes exceed about (d+1) buffers. Sixteen PEs doing 64 KB gets (1 MB outstanding) is ordinary for a get-heavy job.

### Doc or spec text that bounds this
None found:
- §3.8 L416–424 offers no cap.
- §3.11's handoff/wake rules only order waiters for a buffer that never becomes OPEN with free space.

The spec gives no latency guarantee, so this is not a conformance violation. But the observable effect is a hang, which is exactly what the guard itself was introduced to prevent (L418–420). An application where B polls A with gets until A's put to B lands would livelock (reasoning).

**Tightest statement:** DRAINING lets the PT fill a whole buffer, so whenever the bulk queue holds at least about one buffer of reply chunks at each recycle, local PE writers on that connection get zero bytes, indefinitely.

**Fix direction:** cap DRAINING. For example: drain the control queue fully, then bulk up to a fixed fraction of the buffer (the sim shows half guarantees each side 0.5), or run one round-robin pass bounded by that fraction. This still needs no counters or fairness arithmetic.

---

## Verdict table

| ID | Verdict | New severity | Key evidence |
|---|---|---|---|
| B2 | SURVIVES-DOWNGRADED | design change (local) | Snippet compares `head` only while seal changes `state`; a PT re-check after the snapshot still gives LOST/TORN/ACCT/DRAINRES (`recheck`); head-bit seal gives 0 violations incl. DRAINING and liveness |
| B3 | SURVIVES-DOWNGRADED | design change (state invariant) | Reproduced under the §3.9 CQE-swap reading via an empty OPEN buffer plus a fence seal; the one-reservable rule gives 0 violations; the reviewer's "writer must wait" part is unnecessary |
| C1 | SURVIVES | clarification (low) | The doc can be read consistently (header written at seal, generation assigned at open); seq assigned at seal gives STALESEQ even in the fixed protocol |
| D3 | SURVIVES | design change (medium) | Zero-think-time assumption is optimistic, but with 1–2 generation round trips, 8+ PEs × 64 KB or 256 × 4 KB still gives a 0.00 PE share |
