# Protocols, by model and by reading (Question 2, second third)

Agent `protocols`, second adversarial review, against merge `d542c63` on `main`. Everything here was read from the tree at that commit. Experiments ran in private copies under `build/scratch/review-protocols/` in the shared VM, so every timing is **measured under contention**. The investigation was cut short by a usage limit: one of the four models the brief asked for was built, and the rest are listed under "Not modelled" with what each should check.

## Verdict per protocol

| Protocol | Verdict | Basis |
|---|---|---|
| §3.5 reservation (`connection.c`, `progress.c:450-539`) | **Sound**, with evidence | New model written from the C, 2-3 writers plus the progress thread (also as a writer, as `drain()` is), SC and TSO; seven broken variants all caught |
| §3.4/§3.9 acknowledgement request and `quiet` completion (`progress.c:269-294, 420-448, 467-468`; `transport-am.c:506-618`) | **Defect (P1) hiding a second defect (P2)** | Measured, and reproduced with injection |
| §3.8 credits and reply queues (`reply.c`, `progress.c:61-96, 320-409`) | Sound today; **gap** in pool sizing and the fallible path (P3); the DRAINING guard is not in the loop (P4) | Reading; `credit_model.py` covers the credit itself |
| §5.3 arrival handoff, §5.4 equality waits (`barrier.c`, `progress-host.c:163-208`, `base/epoch.c`) | **Sound** by reading; root counting not modelled | Argument below; one missing check (P6) |
| §4.9 watch (`watch.c`, `watch-range.h`, `wait.c`) | Sound by reading; unchanged in this slice, already modelled | Waiter order publish → seq → predicate → wait confirmed at `wait.c:118-156, 320-358`; rmw notify is exact, put notify is best effort as documented |
| Wire wake and close (`wire.c`) | **Sound** by reading | Store `produced`, then bump, then wake; `leaving` is add-then-wake against load-then-wait-on-the-loaded-value |
| §3.10 agent wake / stop (`agent.c`) | **Sound** by reading | `seen` is loaded before the pass and before `stop_requested`; the stopper stores, bumps, wakes |
| Teardown (`init.c:152-166`, `progress-host.c:280-320`) | **Sound for what exists**, by reading; not modelled; leans on P1's `forget_acks` | Argument below |
| Get completion through landing zones (`transport-am.c:335-417`, `progress.c:172-198`) | Sound under SERIALIZED; receive side unchecked against the slot (P5) | Reading |
| Exactly-once rmw, put-acked-before-applied, FENCED ordering | **Sound** | The wire is a reliable ordered stream with no retransmission; `ack = c->applied` is set only in `segment_applied`, after every message of the segment; FENCED rests on stream order, which the model checks as ORDER |
| Stream parser (`am.c`) | Sound for well-formed input at every split; two unchecked lengths (P5) | Reading only |
| Byte counters | Not a concern | `bytes_issued`/`bytes_landed` (`node.h:288-289`) are 64-bit and no `.c` file references them (*observed in source*: grep over `shmem/src/*.c shmem/tests/*.c` is empty, and the same pattern does find them in `node.h`). Get completion is `ready` + `token`. The wire's `produced`/`consumed` are 64-bit and monotone. |

## Findings

### P1. Every acknowledgement requests an acknowledgement: after the first `quiet`, a node pair exchanges header-only segments for ever

- **Location:**
  - `shmem/src/progress.c:283-284` sets `owe_ack = true` when the peer's segment carries `ACK_REQUESTED`.
  - `:442` sets `.flags = c->owe_ack ? ACK_REQUESTED : 0`.
  - `:467-468` lets a local writer's request set the same bool.
  - `progress.h:85` declares it.
- **Failure scenario:**
  - One bool means both "I owe the peer an acknowledgement" and "I want the peer to acknowledge me".
  - A asks, so A's segment carries the flag, so B owes.
  - B's header-only reply is finalized while `owe_ack` is true, so it carries the flag, so A owes. A's reply carries the flag, and so on.
  - Nothing ends it except `osoe_progress_forget_acks` at `shmem_finalize`. That call exists because the symptom was seen at teardown (`progress.h:154-158`), and it treats the symptom only there.
  - §3.4 (*documented*, design lines 376-387) describes the reply as "acknowledgement, zero messages" and does not ask for the flag on it.
- **Evidence (*measured*, `ack-echo.out`):** Two nodes, one PE each. Optionally one 8-byte put and one `shmem_quiet`. Then both PEs sleep in the application with no SHMEM calls. Counters are printed at finalize by a four-line instrumentation patch. One run per configuration:

  | put + quiet | application sleep | segments sent, node 0 / 1 | header-only, node 0 / 1 |
  |---|---|---|---|
  | no | 500 ms | 4 / 4 | 0 / 0 |
  | no | 2000 ms | 4 / 4 | 0 / 0 |
  | yes | 500 ms | 1302 / 1301 | 1297 / 1297 |
  | yes | 2000 ms | 4973 / 4974 | 4969 / 4970 |

  That is about 2,500 header-only segments per second per direction, growing linearly with idle time. It is also visible in all three unmodified `quiet-stress` runs: 20,000 quiets produced 20,482-24,550 header-only segments from the origin, which has no reason to send any.
- **Consequences:**
  - The lead's agent never reaches §3.10's idle.
  - Every pair that has ever been quiesced keeps a permanent exchange with a period of about 250 µs. At 500 nodes, a lead that has quiesced toward every peer services about 499 echo streams.
  - Over real TCP (step 3) it is about 5,000 tiny packets per second per pair on an idle job.
  - `acks_only` and `segments_sent` stop meaning anything for diagnosis.
- **Severity:** design change needed. **Confidence:** high; it is deterministic, and the measured growth matches the source reading.
- **Do not fix it alone — see P2.**

### P2. The acknowledgement request is attached to whichever segment is finalized next, which can be earlier than the write `quiet` is waiting for; only P1's echo keeps `quiet` live

- **Location:**
  - `progress.c:467-468` takes the request at the top of `flush`.
  - `:492-502` promotes the previously sealed buffer and calls `finalize`, which consumes `owe_ack`.
  - `:512-523` seals the buffer holding the new write after that.
  - `transport-am.c:592-613`: `flush_dirty` runs once, and the wait loop only re-wakes the lead; it never re-requests.
- **Failure scenario:**
  - put #1: the lead seals segment S-1 in pass N.
  - put #2 goes into S, and `quiet` stores `ack_requested`.
  - Pass N+1: the request is taken; S-1 is promoted and finalized with the flag, and the flag state is cleared; S is sealed.
  - Pass N+2: S is finalized without the flag.
  - If the peer's reply is finalized before it applies S, the reply carries `ack = S-1`. Nothing ever asks for S.
  - `quiet` waits on `acked >= S` until `OSOE_BOOTSTRAP_TIMEOUT` and aborts.
  - Today the peer's reply itself carries the flag (P1). So A answers with a fresh header-only S+1 carrying the flag, and the peer's answer to that covers S. The echo is load-bearing by accident.
- **Evidence (*measured*, `quiet-stress.out`, `quiet-large.out`, `naive-echo-fix.patch`):** a private tree with the obvious P1 fix, separating `want_ack` from `owe_ack` and changing nothing else.
  - Without injection, `quiet-stress` (20,000 × put, put, quiet) completed 3 of 3 runs. `quiet-large` (200 × 1 MiB put, quiet, default sizes) completed 3 of 3. The window is narrow on shared memory.
  - With a 3 ms sleep injected in the lead only before sending an unflagged segment, 3 of 3 completed.
  - With that, plus a 3 ms sleep at the end of a pass that leaves one buffer sealed and one open, **4 of 5 runs aborted**.
    - Runs 4 and 5 show `osoe: PE 0: node 1 has not acknowledged this PE's writes after 10 s`.
    - Runs 1 and 3 were captured with `tail` only and show the launcher's failure cleanup. That is consistent with the same abort, but the message itself was not captured.
    - Run 2 completed.
  - The injections are sleeps in the lead's thread; they change timing, not logic.
  - The unmodified tree never failed (3 of 3 and 3 of 3).
- **Severity:** design change needed, jointly with P1. As the tree stands `quiet` is live; the hazard is that P1 gets fixed the obvious way. **Confidence:** medium-high; the mechanism was reproduced under injection. The ack model listed below, and full output for the two truncated runs, would raise it.
- **Shape of a fix:**
  - Make the request carry a sequence number rather than a bool. The lead remembers "an ack is wanted through segment S" and flags every segment up to and including S.
  - Or have the wait loop in `osoe_transport_wait` re-issue `osoe_connection_request_ack` each round. That is cruder but sufficient.

### P3. The reply pool has no room for collectives, and the "fallible" path drops the request rather than leaving it

- **Location:**
  - `progress-host.c:233`: `descriptors = peers * credits`.
  - Design §3.8 (*documented*, lines 654-655) sizes the pool as `C + degree×max_teams + slack` per connection.
  - `progress.c:61-72`: on an empty pool `enqueue` counts and returns, after the parser has already consumed the message.
  - `reply.h` and `progress.c:56-59` both say the request is "left in place".
- **Failure scenario:**
  - Every credit toward node 0 is in use when a collective completes at the root. `enqueue_collective` aborts blaming "§3.8's per-connection credit is not holding". That is false: the credit held, and the pool was one short.
  - Or releases occupy descriptors and a get request finds the pool empty. It is silently dropped, and its origin aborts later with "no get reply", on the wrong node.
- **Reachability:** not reachable today.
  - The only internode collective is the world barrier, and it completes only when every PE is inside it.
  - Under SERIALIZED a PE inside a barrier has no get outstanding, and descriptors return at drain, before credits.
  - It becomes reachable with team barriers on a subset, `SHMEM_THREAD_MULTIPLE`, or nonblocking gets.
  - With `OSOE_AM_CREDITS=1` and two nodes the pool is a single descriptor.
- **Severity:** clarification needed; it is a deferral hazard, and the comment claims more than the code delivers. **Confidence:** high on the reading, medium on unreachability, which is argued and not modelled.

### P4. §3.8's DRAINING starvation guard is not in the progress loop, and `osoe_connection_drain_reserve` could not be used as written

- **Location:**
  - `connection.c:149-174`; `connection.h:43-45, 196, 308-315`.
  - The only caller is `shmem/tests/test-connection.c:229` (*observed in source*).
  - `progress.c:320-409` drains through the ordinary `osoe_connection_reserve`, in competition with every local writer.
- **Starvation scenario:**
  - `flush` opens a buffer at the end of pass N. `drain` first runs in pass N+1, after `receive`.
  - With many local PEs putting to the same peer, the buffer can be full by then on every pass.
  - Get replies (and, with team collectives, releases) then wait indefinitely behind local puts.
  - A world barrier is immune, since no writer is active when its release drains.
- **Why the function itself fails:**
  - After `osoe_connection_release`, `head = SEALED|0` and `filled = 0`.
  - `drain_reserve` stores `head = (32+len)|SEALED` and adds `len` to `filled`. So `filled = len` while used is `32+len`, which is never sendable under the equality test.
  - `osoe_connection_open` then stores `filled = 32, head = 32`, discarding what was drained.
  - The header of `connection.h` presents DRAINING as part of the repaired protocol; the loop does not implement it.
- **Severity:** clarification needed. **Confidence:** high on the reading; the starvation is argued, not measured.

### P5. The receive side trusts three lengths

- **Get reply offset.** At `progress.c:50` a get reply lands at `slot->data + m.chunk_offset` with no check against the landing slot's capacity. A put is bounds-checked (`progress-host.c:62`); a get reply is not, and it writes into the node's shared control segment.
- **Message length.** At `am.c:161-167` a message's `len` is not checked against the bytes left in its segment. An over-long message is applied before `end_segment` notices.
- **Segment count.** At `am.c:138` a segment with `message_count > 0` and `total_len == 32` parses the next segment's header as a message.
- **Arithmetic that does hold:**
  - `resolve` checks `length > heap_size - offset` without overflow.
  - A get's `offset + sent` cannot wrap, because the first chunk is checked with `sent = 0`.
  - `head + len` stays below 2^31, because a buffer is at most 1 GiB.
- **Severity:** minor today, since the peer is the same binary over shared memory; it becomes a step-3 item. **Confidence:** high.

### P6. The root's own arrival skips the epoch check its handler makes

- `progress.c:662-664` sets `arrival_epoch` when the count is zero and otherwise does not compare. At `:213-217` the handler aborts on a mismatch.
- It is unreachable while §5.4's bounded skew holds. It is the check that would say so if the skew ever did not hold.
- **Severity:** minor. **Confidence:** high.

## Model versus C: divergences

**Reservation.** The first-round models predate the C. The differences found are all now covered by `reservation_model.py`, unless noted.

- **Open-buffer word.** The C has no word naming the open buffer; writers scan both heads.
- **Sequence number.** It is loaded after the CAS, from a word published before `head`. The `head_before_seq` and `seq_at_seal` variants fail with SEQ.
- **ABA on `head`.** It is reachable and benign: the offset is the same and `seq` is read afterwards. The `cap=1` configurations recycle a buffer per message and pass.
- **Progress thread as writer.** `drain` reserves through the same function, before `flush`, in the same pass. Modelled with `replies=1`.
- **`release`.** It is two plain stores. Modelled, including under TSO.
- **`finalize`.** It reads `owe_ack` at promotion, not at seal. This is not in the model; it is P2.
- **Left out of the model:** the header-only seal, and three writers with more than one message each.

Each broken variant and the check it reaches (`reservation_model.out`):

| Variant | First check to fail |
|---|---|
| `release_unsealed` | QUIET |
| `sendable_ge` | QUIET |
| `seq_at_seal` | SEQ |
| `head_before_seq` | SEQ (also with one writer) |
| `two_open` | SEGSEQ |
| `seal_store` | QUIET |
| `promote_unchecked` | TORN |

**ORDER and ONCE were never the first check to fail in any variant.** The engine stops at the first violation, and SEGSEQ or QUIET is reached sooner. So those two checks are not shown to be able to fail, and should be treated as unproven rather than as passing.

**Arrival handoff (`arrival_model.py`).** The model matches the C on three points: the load order, consuming from the loaded value, and the store followed by a locked add. It abstracts the whole internode round trip into one thread. So it says nothing about:
- the root's counting;
- `participants[]` being set only by the root's own arrival (`progress.c:658`). This is harmless, since a count of at least 1 never equals an unset 0, and the root's arrival is always one of the required ones;
- P6.

**Credit (`credit_model.py`).** The model matches the C for take and give. It has no collective descriptors in the pool; that is P3.

## Why the unmodelled protocols look sound

- **Equality waits, §5.4.**
  - `arrived` reaches `(k+1)·L` exactly once. The arrivals of epoch k+1 are the counter values `kL+1 … (k+1)L`, which are distinct modulo 2^32 for any L < 2^32. The target is computed in the same modular arithmetic.
  - `released` moves only by a checked `bump_eq`.
  - A member cannot post epoch k+2 before the agent has forwarded k+1, because it needs the release of k+1 to get there.
  - A non-root cannot arrive for k+2 at the root while k+1 is open, for the same reason one hop out.
- **Teardown.**
  - The lead's agent cannot be stopped before the final release is applied on its node, because the lead's own application thread is waiting on that release.
  - After the release, every PE of the job is past `quiet` and inside or past the barrier. So there are no writers, no gets, and nothing a peer needs this node to apply.
  - What remains is what this node must send, namely the root's releases. `flush_before_close` polls those out before `wire_shutdown`.
  - A wire is unmapped only when `leaving == 2`, so a late peer write lands in a ring that is still mapped.
  - `osoe_progress_idle` does not look at `owe_ack`. That is right only because `forget_acks` ran first (P1).
- **Agent stop, wire wake, landing wake.** Each follows the same pattern, read line by line with no deviation found:
  - one side stores the predicate, bumps with a locked add, then wakes;
  - the other loads the word, checks the predicate, then waits on the loaded value.

## Not modelled (usage limit), and what each model should check

1. **Acknowledgement request plus quiet (highest value).**
   - Setup: two leads, one writer, two or three segments, with the flag consumed at promotion as the C does it.
   - Properties: quiet terminates; and quiescence, meaning that with no pending request no thread has a segment left to send.
   - Expected results: the tree as it stands should fail quiescence (P1); the naive fix should fail termination (P2); a sequence-carrying request should pass both.
2. **Teardown.**
   - Setup: a root plus two non-roots. The root releases itself at once, enqueues releases, stops polling when "idle", then runs shutdown and close.
   - Properties: every node applies its release; no node unmaps a wire a peer can still store into.
   - Broken variant: idle tested as "a pass moved nothing", which is the recorded defect.
3. **Multi-node barrier.**
   - Setup: at least three nodes, two epochs, a member re-entering early.
   - Properties: the root never sees two epochs at once; each node is released exactly once per epoch.
   - Broken variant: the release applied locally before `arrivals` is reset.
4. **Dirty set across two nodes**, with `pe_quiet`'s compaction of `order[]`.

## Lessons for a successor API

- **`shmem_quiet` has no handle.** It must cover everything a context ever sent. That forces the dirty set, the cumulative ack, and the request-an-ack side channel where P1 and P2 live. A completion handle returned by a put or a batch would make the ack request carry its own sequence number by construction.
- **`shmem_finalize` is both an implicit barrier and the teardown.** That is what makes "the root leaves first while owing the release" possible. An explicit quiesce-then-close pair would separate the collective from the resource lifetime.
