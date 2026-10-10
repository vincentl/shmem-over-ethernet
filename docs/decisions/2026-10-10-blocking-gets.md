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
- **(b), as built** (`transport-internode.c`, `remote_get`): a blocking get of
  more than a piece goes as a nonblocking get on a context of the calling
  thread's own, and the thread completes it as that context's quiet would,
  copying each piece out as it lands. The context is the thread place's block
  past the pool's - each PE's pool of context blocks in the control segment
  holds `OSOE_MAX_CONTEXTS` blocks and then one for each thread place (node
  segment layout 37, `osoe_node_thread_context`) - which no context a program
  makes can take. Its pending list lives for the call alone, so nothing outlives
  a thread that exits. These pieces are not counted among the nonblocking gets
  (`am.gets_nonblocking`).
- **Where (b) is not taken.** A get of one piece keeps the thread's slot - one
  round trip either way. And a thread place whose block's index a token cannot
  carry - past `OSOE_LANDING_OWNER_MASK`, 4,095, which only `OSOE_MAX_CONTEXTS`
  near its most reaches - keeps the piece-at-a-time path.
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
  while `am.gets_nonblocking` does not move. A mutant with the pipelined path
  taken out fails it.
- Every test of gets, contexts, the landing zone and threads, unchanged.

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
