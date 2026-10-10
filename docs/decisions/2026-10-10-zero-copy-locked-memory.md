# 2026-10-10: the zero-copy send refused for want of locked memory

Portability P1 of the review of v0.2 step 4's merge point
(`docs/reviews/archive/v0.2-step4-portability.md`), rated to settle before Part
4 decides the zero-copy send's default, and the first item of what the author
was offered to do before the hardware comes (`docs/decisions/2026-10-10.md`).

## What it was

- **The charge:** with `OSOE_WIRE_ZERO_COPY=on`, io_uring charges each send by
  reference the length it offers, and two pages, until the send's notification
  comes. It adds them to one count of locked memory for every process of the
  user on the host, and holds that count to the charging process's own
  `RLIMIT_MEMLOCK` (`ulimit -l`, 8 MiB by default), saying `-ENOMEM` past it.
- **A job past it ended** "the connection to node N failed: Cannot allocate
  memory", naming neither the limit nor the setting. Two cases were measured:
  sixteen logical nodes on an emulated gigabit link at the default limit, and
  `shmem.internode.zero-copy.0-1-2` under `ulimit -l 64`.
- **And the rings themselves**, on Linux 6.14 and later: io_uring charges a
  ring's memory, and a provided-buffer ring's, to the same count, zero copy or
  not. *Measured* on the Pi cluster's 6.18 (2026-10-10, `probe` under `ulimit -l
  0` on a node): the ring the start-up check makes fails "Cannot allocate
  memory", and so does the provided-buffer ring; at the node's 8 MiB the probe
  passes. Read in the kernel's source at 6.13, 6.14 and 6.18 by the fresh read
  below; the development VM's 6.12 does not charge rings.

## What was built

- **A send by reference that completes `-ENOMEM` is not the connection's
  failure** (`ring.c`). The charge comes before the socket is reached, so
  nothing was sent. The wire stops sending by reference for the rest of the job,
  and is marked free of its send, so that the visit the mark brings, in the same
  pass, offers the same bytes again by copy. A notification the completion says
  will follow (`IORING_CQE_F_MORE`) is counted owed, as for any send. `-ENOBUFS`,
  `MSG_ZEROCOPY`'s own refusal, which io_uring does not give here, is taken the
  same way, since a copy is always right.
- **Once a process, a notice** (`progress.c`): "node N: a zero-copy send was
  refused for want of locked memory: io_uring counts what every send by
  reference of this user's processes on this host lends, against each process's
  own limit (ulimit -l, K KiB here) - raise it for the job, or set
  OSOE_WIRE_ZERO_COPY=off. The connections that met it copy from here on".
- **Where a ring cannot be made, the limit is named**: `ring.c`'s two messages
  and the start-up check's refusal add, on `-ENOMEM`, that Linux 6.14 and later
  count io_uring's rings against `ulimit -l`, and what it is here; `probe`
  prints the limit (`base/ring-check.[ch]`, `osoe_locked_memory_kib`).
- **The model first**: `reservation_model.py`'s `zc_refused` cases - the
  zero-copy send with any send by reference refusable, once a connection, the
  refused send's notification posted before or after those of the buffer's
  earlier parts - and `zc_refused_owes_none`, a refusal not counted owed, which
  must fail. On the Mac, 2026-10-10: every `zc_refused` case holds, with
  liveness - at one writer of two messages (12,384 states), two of one (43,799),
  two of two (5,974,040) and one of two beside a reply (133,997) - and two of two
  under x86-TSO (22,533,987 states, too many for the liveness check);
  `zc_refused_owes_none` at two writers of two fails, "SEQ: message 2 left in
  segment 1 but its writer recorded 0", the buffer released and written again
  while the kernel still had a part of it to read. At one writer it passed: the
  case cannot leave a part unread, and was dropped. The eight `zero_copy` cases
  and the three earlier breakages are as they were
  (`build/scratch/ladder/merge-point/zero-copy-model.txt`).
- **The tests**: a limit set before a job starts would have to leave room for
  the rings, which 6.14 and later charge, and none for the sends, which no one
  number does on every kernel, page size and helper count, with the tests beside
  it spending the same count. So each test lowers its own process's limit to
  nothing once its rings are made (`shmem/tests/locked-memory.h`), and every send
  by reference after that is refused, on any kernel:
  - `shmem.wire.zero-copy-refused` (tier 1): the wire's streams with every send
    by reference refused - each of the two sending wires refused once and lending
    nothing after, every byte arriving by copy;
  - `shmem.internode.zero-copy-refused.0-1-2`: the internode test with zero copy
    on, every PE's limit at nothing after `shmem_init`
    (`OSOE_TEST_LOCKED_MEMORY=0`). It passes on the notice and fails on "Cannot
    allocate memory", a rank's failure, the launcher's timeout or a start's
    leftovers. A wire that went on lending after a refusal passes it - a region
    owing the refused send's notification makes the next send copy, so the job
    goes on - and fails the wire's test, which counts the refusals. Not under
    ThreadSanitizer, as the program's other multi-node maps are not.

  Both skip where a process may lock memory whatever its limit (`CAP_IPC_LOCK`,
  which io_uring does not charge at all).
- **No new counter.** `osoe-stat` counts a refused send among the sends by
  reference, and its notification among those not sent by reference after all;
  the README says so. A counter of its own would have changed the node
  segment's layout.
- **The documents**: the README's row, design §3.3 (a refused send is no
  failure; the zero-copy send as built) and §3.14's counters, and `wire.h`'s
  comments.

## Read adversarially

By a fresh agent at `7176a0f`
(`docs/reviews/archive/v0.2-zero-copy-memlock-adversarial-read.md`): nothing
fatal in what the library does, the fallback correct on every path traced. Its
six should-fix findings were all taken:

1. the test registered under ThreadSanitizer, where this program's multi-node
   maps report a race the suite excludes elsewhere - now not;
2. the test failing a correct library on 6.14 and later, at two or three helpers,
   as root, and possibly by timing, under `ulimit -l 64` - now each test lowers
   its own limit to nothing once its rings are made, and skips under
   `CAP_IPC_LOCK`;
3. the refused bytes offered again only at a later pass, which an idle owner
   sleeps up to 100 ms before - now in the same pass;
4. the test passing a mutant that kept lending, and the launcher's "left
   behind" - now the white-box test counts the refusals, which fails that
   mutant (the internode test still passes it: see the verification), and
   "left behind" fails the internode test;
5. the notice, the README and the comments calling the limit shared, the charge
   "its pages", and `-ENOBUFS` the limit's refusal - now as above;
6. design §3.3 and `wire.h` making every send failure but a reset fatal - now
   each says what a refusal is.

Of its notes: N1, the rings' charge on 6.14 and later, is measured above and its
messages built; N4, the model, built; N10, the once-flag's style, and N11, the
names, taken. N2 (`osoe-stat`'s counts) is stated in the README; N3 - one
refusal stops lending for the rest of the job - is the choice, and Part 4 may
want lending back after a while; N5, an `-ENOMEM` that is not the limit, is rare
and harmless (the copy is right), left; N6, raising the soft limit to the hard
one with zero copy on, as `init.c` does for open files, is for Part 4.

## The verification

- **On the Pi cluster** (Linux 6.18, 16 KiB pages, where rings are charged and a
  `ulimit -l 64` set before the job fails at ring creation; one node, its
  logical nodes; 2026-10-10 ~14:55,
  `build/scratch/ladder/merge-point/pi-zero-copy-refused/`):
  `shmem.wire.zero-copy-refused` passed, "2 sends by reference, 2 refused, 2
  notifications, 2 copied"; `shmem.internode.zero-copy-refused.0-1-2` passed,
  each node's notice once, "ulimit -l, 0 KiB here" - after one fix: its first
  build reduced two static flags, which in a program not linked by `oshcc` are
  not symmetric, and every PE ended there; the flags are on the heap now; the
  four `shmem.internode.zero-copy.*` maps and `shmem.wire.zero-copy` passed as
  before. A job of two logical nodes under `ulimit -l 0` was refused at
  `shmem_init` with "it fails with \"Cannot allocate memory\", which on Linux
  6.14 and later can be the locked-memory limit, ulimit -l, 0 KiB here".
- **In the development VM** (Linux 6.12, at `8eea960`; 2026-10-10 ~15:45;
  `docs/design/evidence/v0.2-step4/2026-10-10-zero-copy-locked-memory/zero-copy-refused-check/`):
  the static checks 12 of 12; the wire's and the internode tests 20 of 20, the
  two refused ones as on the Pi; three mutants, each built and run against the
  two refused tests -
  - **without the fallback** (the refusal fatal, as before): both fail,
    "Cannot allocate memory";
  - **lending on after a refusal**: the wire's test fails, 17,815 sends by
    reference all refused before it gave up after 26 s; the internode test
    passes, a region owing the refused send's notification making the next
    send copy;
  - **the refused bytes offered again only at a later pass**, as first built:
    both pass, and the internode test took 0.18 to 0.30 s in five runs where
    the branch took 0.07 to 0.08 - finding 3's wait, measured.

  `probe` printed "locked-memory limit (ulimit -l): 8192 KiB", and under
  `ulimit -l 0` passed whole: 6.12 does not charge rings, where the Pi's 6.18
  failed the same run.

*Undo:* the refusal's branch in `ring.c`, the notice, the limit in the ring's
and the start-up check's messages and in the probe, the model's cases, the two
tests and `locked-memory.h`, and the README's, design's and `wire.h`'s sentences
back; the release checklist's item back.
