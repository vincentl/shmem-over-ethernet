# 2026-10-10: a blocking get's pieces go out together

The author's second answer of the afternoon ("accept 1 & 2";
`docs/decisions/2026-10-10.md`, item 2): a blocking get of more than one piece
issues its pieces as a nonblocking get does, before Part 4's measurements, the
models first - question 4, explored in `docs/design/blocking-get-proposal.md`.

## What it was

- **A piece at a time.** A blocking get from another node moved at most one
  piece - up to 64 KiB, the chunk's payload - at a time: each piece armed the
  calling thread's slot, went out, and waited its round trip before the next
  was asked for (§4.11 item 5). On the Pi cluster a `shmem_get` of 64 KiB or
  more took 1.23 to 1.28 times a put and quiet of the same size; over a fast
  loopback path, six to ten times from a mebibyte.
- **The nonblocking path already did better.** `shmem_get_nbi` and
  `shmem_quiet` issue the pieces together, as far as the landing zone and the
  credits allow: on the Pi 1.00 to 1.05 times the put; on loopback 2.6 to 3.5
  times, held by the zone's four pieces (`OSOE_GET_LANDING_BYTES`, 256 KiB).

## What was decided, and what was built

- **The recommendation the author accepted was (a)**: a blocking get's pieces
  issued together through the thread's own slot. The build began with a closer
  look at the lead's side, and found (b) the smaller change: the lead accepts a
  blocking piece only into the slot its thread armed for that one token, so (a)
  needs a new kind of token and a new landing path on the lead; (b) leaves the
  lead's landing as it is. The author was told (b) was being built, and why.
  No answer on (b) itself has come: his next word, that evening, was to finish
  and merge the work in hand, this among it. So (b) is Claude's choice inside
  his acceptance of the question's direction, not his - which is why the code
  and design §4.11 cite this record for it, and not item 2.
- **The proposal's reasons against (b)**
  (`docs/design/blocking-get-proposal.md`) do not hold as it was built. A pool
  of hidden contexts counted against `OSOE_MAX_CONTEXTS`: the blocks are past
  the pool, one a thread place, and a program has every context it had. A
  quiet's fixed cost on every large get: the get's wait is for its own pieces
  alone, with no dirty set to walk and no fence.
- **(b), as built** (`transport-internode.c`, `remote_get`): a blocking get of
  more than a piece goes as a nonblocking get on a context of the calling
  thread's own, and the thread completes it as that context's quiet would,
  copying each piece out as it lands. The context is the thread place's block
  past the pool's - each PE's pool of context blocks in the control segment
  holds `OSOE_MAX_CONTEXTS` blocks and then one for each thread place (node
  segment layout 37, `osoe_node_thread_context`) - which no context a program
  makes can take. Its pending list lives for the call alone, so nothing outlives
  a thread that returns from the call. A thread cancelled inside its wait -
  which the library does not support (`base/sweep.h`) - would leave its pieces
  in flight, to be copied into its buffer by another thread, and its place's
  block owing their bytes to the next thread there; on `main` such a thread left
  one piece's extents held and wrote nowhere. These pieces are not counted among the nonblocking gets
  (`am.gets_nonblocking`).
- **Where (b) is not taken.** A get of one piece keeps the thread's slot - one
  round trip either way. And a thread place whose block's index a token cannot
  carry - past `OSOE_LANDING_OWNER_MASK`, 4,095, which the last places' blocks
  pass from `OSOE_MAX_CONTEXTS` 4,080 at the default `OSOE_MAX_THREADS`, and
  from 3,073 at `OSOE_MAX_THREADS` 1,023 - keeps the piece-at-a-time path, as
  the README says.
- **A stalled get names its target.** A blocking get's pieces are toward one
  node and PE, and both its waits - for its pieces' bytes, and, past the pieces
  the zone holds, for room - publish a reply's wait toward that node and end,
  under `OSOE_NO_PROGRESS_TIMEOUT`, naming node and PE, as a piece's reply did
  on `main` (the phase-1 diagnosability review's D-6). As first built they
  named neither and osoe-stat called them nonblocking gets; the adversarial read
  found it (finding 2). A nonblocking get's wait for room now records the bytes
  the zone holds, where it recorded extents under a kind that counts bytes.
- **What it costs:** a context block, 128 bytes of the control segment, for each
  thread place: 2,176 bytes a PE at the default `OSOE_MAX_THREADS` of 16 and the
  agent's place.
- **What stays as it was:** the landing zone's size, which now holds a large
  blocking get as it holds a nonblocking one, is Part 4's (the proposal's (d)).
- **The models first, by building no new protocol.** (b) is the nonblocking
  get's own path, on a block no other caller uses: its counters and their wake
  (`get_completion_model.py`, every design §4.11 names, with threads issuing
  and quieting on one context), its extents (`landing_extent_model.py`) and
  the claim of a landed piece by whoever copies it out
  (`copy_out_claim_model.py`) are what the models already hold, at every
  thread count they check; the thread here is the context's only issuer and its
  only waiter, a case inside theirs. (a) would have needed a new landing path,
  and a model of it, first.

## Tests

- `shmem.get-pieces.0-1` (`test-get-pieces.c`): a get of 1,000 bytes leaves
  `am.gets_completed` where it was; a get of 1 MiB + 3 bytes into an offset of 5
  raises it by at least two - its pieces landed as a nonblocking get's do -
  while `am.gets_nonblocking` does not move, and `am.landing_full` moves, since
  its first four pieces fill the zone before any is copied out, which a get
  completing each piece before the next never does. A mutant with the
  pipelined path taken out fails it. Its failure expression catches a failed
  check, the invariants' and the launcher's, since the banner prints first.
- `shmem.get-pieces.fallback.0-1`: at `OSOE_MAX_CONTEXTS` 4,096, the same get
  goes a piece at a time - none copied out as a nonblocking get's, the zone
  never full - with every byte right.
- `shmem.get-no-progress.pieces-3.0-1` and `.pieces-20.0-1`: a get of three
  pieces and one of twenty toward a stopped node end naming node 1 and PE 1,
  the first in the wait for its pieces' bytes and the second in the wait for
  room.
- `shmem.multiple.*` (`test-multiple.c`): each worker's round adds a blocking
  get of 20 KiB, three pieces at the tests' 8 KiB zone, from a start of its
  own, every word checked - several threads' pieces in flight at once, each
  copied out by whichever thread finds it landed, under ThreadSanitizer at
  `0,1`, `OSOE_CHECK_QUIET` and the interleaved maps too.
- `shmem.stat.*`: osoe-stat decodes layout 37's watch word, which is layout
  36's - the thread records it lives in are found by their offset - and its
  newest layout is 37; as first built it was 36, and `shmem.stat` failed in
  every preset (the read's finding 1).
- Every other test of gets, contexts, the landing zone and threads,
  unchanged.

## The measurements

- **On the Pi cluster** (two nodes, one PE each, gigabit; question 4's probe,
  `get-pieces.c`, 21 repetitions a size, medians; main at `7a42c92` and the
  branch alternating three times, 2026-10-10 ~14:10;
  `docs/design/evidence/v0.2-step4/2026-10-10-blocking-gets/pi-blocking-gets/`;
  the probe is `docs/design/evidence/v0.2-step4/2026-10-09-question4/get-pieces.c`):

  | Bytes | Put and quiet | Main's blocking get | The branch's | A nonblocking get and quiet |
  |---|---|---|---|---|
  | 16 KiB | 288-293 µs | 0.91-0.92 × | 0.91-0.92 × | 0.90-0.92 × |
  | 64 KiB | 724-732 µs | 1.22-1.24 × | 1.01-1.03 × | 0.98-1.01 × |
  | 128 KiB | 1,262-1,275 µs | 1.26-1.27 × | 1.05-1.06 × | 1.01-1.06 × |
  | 256 KiB | 2,392-2,398 µs | 1.26 × | 1.02 × | 1.02-1.03 × |
  | 1 MiB | 9,081-9,090 µs | 1.27 × | 1.01 × | 1.01 × |
  | 4 MiB | 35,831-35,837 µs | 1.28 × | 1.00 × | 1.00 × |

  A blocking get of a mebibyte took 11,550 to 11,559 µs on main and 9,148 to
  9,154 on the branch: a fifth less, the nonblocking get's time. A get of one
  piece, 16 KiB, is as it was.
- **On the Pi, the branch's tests of gets** on one node of four logical nodes'
  maps (`srun -N1 -n1 -c4`): 57 of 57 once the internode test's count was
  corrected (below); before it, 48 of 57.
- **Over the development VM's loopback** (the same probe, main at `7a42c92` and
  the branch at `8f56d03` alternating three times, 2026-10-10 ~15:28;
  `docs/design/evidence/v0.2-step4/2026-10-10-blocking-gets/blocking-gets-check/`):

  | Bytes | Main's blocking get | The branch's | A nonblocking get and quiet |
  |---|---|---|---|
  | 1 MiB | 1,147-1,170 µs, 7.16-7.27 × a put | 422-425 µs, 2.63-2.68 × | 418-427 µs, 2.59-2.70 × |
  | 4 MiB | 4,438-4,462 µs, 9.42-9.51 × | 1,522-1,526 µs, 3.26-3.36 × | 1,511-1,534 µs, 3.23-3.39 × |

  From a mebibyte the branch's blocking get is the nonblocking get's time,
  which the landing zone's four pieces hold to about three times a put there;
  below 256 KiB the VM's puts varied from 35 to 75 µs between runs, too much to
  read a ratio from.
- **The control segment** (the probe's reading of its own mapping, one node of
  P PEs, main at `7a42c92` and the branch at `f776858`, 2026-10-10 ~19:23;
  `docs/design/evidence/v0.2-step4/2026-10-10-blocking-gets/segment-size-check/`):
  2,232,320 bytes and 2,240,512 at 4 PEs - the 8,704 the thread places add,
  rounded to whole pages - 19,730,432 and 19,869,696 at 64, and 38,391,808 and
  38,670,336 at 128: exactly 2,176 bytes a PE, seventeen thread places of 128
  bytes at the default `OSOE_MAX_THREADS`. A first probe looked for the
  segment's name in `/dev/shm`, which `shmem_init` removes, and found nothing.

- **The read's fixes** (the debug preset in the development VM, 2026-10-10
  ~19:58; `docs/design/evidence/v0.2-step4/2026-10-10-blocking-gets/read-fixes/`):
  the static checks 13 of 13, and the 86 tests of gets, contexts, threads, the
  landing zone and osoe-stat, the new ones among them. A get of 1 MiB + 3 bytes
  copied out 17 pieces, and 7 of its issues found no room; at
  `OSOE_MAX_CONTEXTS` 4,096 none and none; a get of three pieces toward a
  stopped node ended "no progress toward node 1 (PE 1) ... the bytes of its
  get's pieces", and one of twenty "... the landing zone's released extents".
  The mutant completing each piece before it issues the next failed
  `shmem.get-pieces.0-1` - 17 pieces copied out, no issue without room - where
  the test as first written passed it.

## A test's premise the change made false

`shmem.internode.*` checked that a PE whose put-with-signal partner is on its
own node defers no wakes (`am.wakes_deferred`), counted from the end of
`shmem_init`, on the premise that put-with-signal is the only caller that
defers. A blocking get of more than a piece now holds its pieces' wakes as a
nonblocking get does, and the test's `CHUNKED` gets come before that check: on
the Pi, nine of the internode test's maps failed on each node's lead,
"CHECK(deferred == 0) failed". The library is right, and the test now counts
over the rounds of put-with-signal alone. The other tests that read
`am.wakes_deferred` count across one operation.

## The verification

*To be filled in.*

*Undo:* `remote_get`'s pipelined path, the thread places' blocks (layout 36),
`landed()`'s bound, the test, and the README's, design §4.11's and the
proposal's sentences back.
