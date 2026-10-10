# A blocking get, a piece at a time: what question 4's exploration found

**For the author's decision.** On 2026-10-09 the author asked for a short
exploration of question 4 before deciding it (`docs/decisions/2026-10-09.md`,
item 1). This is that exploration: what a blocking get does today, what two
experiments measured, the options, and one recommendation. The question itself
is in `docs/decisions/2026-10-07-one-thread-profile.md`, "Question 4"; the
experiments' program, scripts and outputs are in
`docs/design/evidence/v0.2-step4/2026-10-09-question4/`.

## What a blocking get does

A blocking get from another node (`remote_get`, `transport-internode.c`)
asks for one piece at a time, and waits for each before it asks for the next:

- **The piece:** at most a chunk's payload, 64 KiB less a reply's header, so
  seventeen pieces to a mebibyte.
- **Each piece in turn:**
  - it claims a run of extents in the PE's landing zone and arms the calling
    thread's one slot with its token;
  - it sends the request and waits until the reply has landed;
  - the thread copies the bytes out and gives the extents and the credit back.

The other two ways move the same bytes differently:

- **A put** streams its parts behind one another, and its quiet waits once.
- **A nonblocking get** issues all its pieces at once, as far as two limits
  allow: the landing zone's extents (`OSOE_GET_LANDING_BYTES`, 256 KiB a PE by
  default) and the connection's credits. It copies each piece out at the next
  quiet.

So a blocking get of n pieces waits n round trips where the other two wait
about one.

## What was measured

`get-pieces.c` times three things between two PEs on two nodes, at sizes from
16 KiB to 4 MiB, and takes the median of 21 repetitions:

- a put and a quiet;
- a blocking get;
- a nonblocking get and a quiet - which is about what a blocking get that issued
  its pieces together would take.

It ran in two places:

- **On the Pi cluster:** two nodes, gigabit, a PE each, the tree of `7903b15`,
  three runs (`pi-get-pieces.txt`).
- **Over the development VM's loopback:** two logical nodes, a release build of
  `main` at `c897948`, on a quiet host, three runs at no progress helpers and
  one at two (`loopback-get-pieces-quiet.txt`). A first run on a host loaded
  far past its cores is kept beside it and not used
  (`loopback-get-pieces-loaded.txt`).

Each cell is the time against a put and a quiet's, low to high over the runs:

| Size | Pi, blocking get | Pi, nonblocking get and quiet | Loopback, blocking get | Loopback, nonblocking get and quiet |
|---|---|---|---|---|
| 16 KiB | 0.91 to 0.93 | 0.89 to 0.91 | 0.99 to 1.04 | 0.79 to 1.10 |
| 64 KiB | 1.23 to 1.25 | 1.01 to 1.03 | 1.39 to 1.76 | 0.72 to 0.91 |
| 128 KiB | 1.25 to 1.27 | 1.03 to 1.05 | 2.03 to 2.53 | 0.91 to 1.04 |
| 256 KiB | 1.26 | 1.02 | 3.15 to 5.52 | 1.50 to 2.50 |
| 1 MiB | 1.27 | 1.01 | 6.13 to 9.71 | 2.70 to 3.54 |
| 4 MiB | 1.28 | 1.00 | 9.35 to 9.70 | 3.15 to 3.26 |

In time, at 1 MiB:

- **On the Pi:** a put and quiet took 9.09 to 9.10 ms, a blocking get 11.55 to
  11.57, a nonblocking get and quiet 9.15.
- **Over loopback:** a put and quiet took 120 to 158 µs, a blocking get 744 to
  1,172, a nonblocking get and quiet 423 to 425.
- **At two helpers over loopback:** the same within the runs' spread.

## What it says

1. **On a gigabit link the whole gap is the round trips, and issuing the pieces
   together closes it.**
   - The Pi's blocking get is a quarter slower than a put from 64 KiB up: 2.5 ms
     at 1 MiB, about seventeen of its round trips.
   - A nonblocking get and a quiet is within 1 to 5% of the put at every size.
2. **On a fast path the gap is larger, and issuing the pieces together closes
   about two thirds of it.**
   - Over loopback a blocking get of 1 MiB or more takes 6 to 10 times a put's
     time; a nonblocking get and a quiet takes 2.7 to 3.5 times.
   - **What holds the nonblocking get there is the landing zone.** 256 KiB a PE
     holds four pieces, so at most a quarter of a mebibyte is in flight a round
     trip.
   - The step from 128 KiB, where it matches the put, to 256 KiB, where it does
     not, is where the pieces first outnumber the zone.
3. **How much a fast network link would show is not measured.**
   - At 25 Gbit/s and a round trip of tens of microseconds, the bytes a round
     trip carries are about the zone's size. So a get issued as the
     nonblocking one is may come close to the link there, or meet the zone as
     loopback does.
   - Part 4's link is the place to measure it.

## The options

- **(a) A blocking get issues its pieces as a nonblocking get does** - the
  2026-10-07 recommendation.
  - **How:** each piece gets extents and a landing record of its own, as many as
    the zone and the credits allow. The caller waits until every piece has
    landed, copying each out as it lands.
  - **What changes:** the calling thread's one-token slot becomes the
    nonblocking path's records per piece. `landing_extent_model.py` and
    `get_completion_model.py` are extended to a blocking get of several pieces
    in flight, models first. A blocking get then shares the zone with the PE's
    nonblocking gets, as those already share it with one another.
  - **Gain:** the Pi's large gets about a fifth faster, near a put's time;
    loopback's two to three times faster from 256 KiB.
- **(b) The same effect through the nonblocking path as it stands.** A blocking
  get becomes a nonblocking get on a context of the calling thread's own,
  followed by that context's quiet.
  - **For it:** no new protocol - the modelled nonblocking path entire.
  - **Against it:** a context per calling thread made by the library. Today
    those count against `OSOE_MAX_CONTEXTS` (64 a PE by default, against up to
    1,023 threads), so it wants a pool of its own. And each blocking get pays a
    quiet's fixed cost, which the measurements show is not visible at 16 KiB.
- **(c) Leave it,** and the README says so: between nodes a blocking get of more
  than a chunk takes a round trip a chunk. A program that wants them to overlap
  calls `shmem_get_nbi` and `shmem_quiet`, which close the gap today on a
  gigabit link.
- **Beside any of them, (d): the landing zone sized to the link,** as the unsent
  limit is - the bandwidth times a round trip, at least 256 KiB - rather than
  fixed. It is what holds the nonblocking get, and would hold (a) and (b),
  below a fast link's rate. It is Part 4's to measure, since only a fast link
  shows it.

## Recommendation

**(a), with its models first, built before Part 4's measurements, so that the
fast link times the gets 0.2 would ship; and (d) put to Part 4.**

- **Why (a) over (b):** it is the smaller change to what a program meets. (b)
  would add a pool of hidden contexts and a quiet to every large get, where (a)
  reuses the nonblocking path's records and nothing else.
- **Why at all:** a get is half of RMA, and a blocking get at a third of a
  nonblocking one's speed over loopback is the largest gap the ladder has met
  that is the library's own protocol.
- **If 0.2 ships without Part 4,** (a) still pays on gigabit: a fifth of a large
  get's time.
- **Until it is built,** (c)'s sentence is in the README's limits, made with this note.
- *Undo:* the blocking get one piece at a time again.
