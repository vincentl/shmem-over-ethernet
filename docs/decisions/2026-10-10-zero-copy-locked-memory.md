# 2026-10-10: the zero-copy send refused for want of locked memory

Portability P1 of the review of v0.2 step 4's merge point
(`docs/reviews/archive/v0.2-step4-portability.md`), rated to settle before Part
4 decides the zero-copy send's default, and the first item of the work Claude
offered the author, 2026-10-10, to do before the hardware comes - offered in
conversation, which no record holds.

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
  reference of this user's processes on this host lends, and on Linux 6.14 and
  later their rings, against each process's own limit (ulimit -l, K KiB here) -
  raise it for the job, or set OSOE_WIRE_ZERO_COPY=off. The connections that
  met it copy from here on". Where the process has no limit, the charge cannot
  be what refused the send, and the notice says the kernel refused it though
  locked memory is unlimited.
- **Where a ring cannot be made, the limit is named**: `ring.c`'s two messages
  and the start-up check's refusal add, on `-ENOMEM`, that Linux 6.14 and later
  count io_uring's rings against `ulimit -l`, and what it is here; `probe`
  prints the limit, or "unlimited" (`base/ring-check.[ch]`,
  `osoe_locked_memory_kib`). The README's limits on launching say it too.
- **The model first**: `reservation_model.py`'s `zc_refused` cases - the
  zero-copy send with any send by reference refusable, once a connection, the
  refused send's notification posted before or after those of the buffer's
  earlier parts - and `zc_refused_owes_none`, a refusal not counted owed, which
  must fail. As first built the `zc_refused` cases did not check BOTH LENT,
  which `zero_copy`'s do (the second read, finding 3); now they do. On the Mac,
  2026-10-10 ~20:00: every `zc_refused` case holds, with liveness - at one writer
  of two messages (13,511 states), two of one (48,216), two of two (6,236,896)
  and one of two beside a reply, by the opportunistic drain (142,638) - and two of
  two under x86-TSO (23,158,348 states, 544 s on the Mac, too many for the liveness check);
  `zc_refused_owes_none` at two writers of two fails, "SEQ: message 2 left in
  segment 1 but its writer recorded 0", the buffer released and written again
  while the kernel still had a part of it to read. At one writer it passed: the
  case cannot leave a part unread, and was dropped. The zero-copy cases may take
  1,500 s each, and their liveness is checked to 7 million states, which the two
  of two now needs. The eight `zero_copy` cases and the three earlier breakages
  are as they were (`docs/design/evidence/v0.2-step4/2026-10-10-zero-copy-locked-memory/`:
  `zero-copy-model.txt` as first built, `zc-refused-model.txt` after the read).
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
  which io_uring does not charge at all), which the test asks by locking a page
  through the system call: AddressSanitizer's and ThreadSanitizer's runtimes
  take `mlock()` and return 0 without locking, and as first built both tests
  skipped in both presets (the second read, finding 1).
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
one with zero copy on, as `init.c` does for open files, is for Part 4. N3 and
N6 are the release checklist's, beside Part 4's default.

By a second fresh agent at `73b30ef`, the rework, which the merge into the
integration branch left the same
(`docs/reviews/archive/v0.2-zero-copy-refused-second-adversarial-read.md`):
nothing fatal, the fallback right on every path traced, the Pi's 6.18 - which
refuses the send as the submission prepares it, not as 6.12 and 6.14 do, when
it is issued - among them. Its four should-fix findings were all taken:

1. both refused tests skipped under AddressSanitizer and ThreadSanitizer, whose
   runtimes take `mlock()` and lock nothing - now the probe locks by the system
   call, and both run there (below);
2. the record's Pi pass of the refused internode test was not in its evidence,
   whose directory held the failing first build, and 6.18's refusal at
   submission is a path through `progress.c` no saved run had taken - now run
   again and kept (below), the `ulimit -l 64` clause, which nothing ran, gone,
   and `ring.c`'s comment says what 6.18 does;
3. the model's refusal cases never checked BOTH LENT - now they do, and hold;
4. no test held the same-pass re-offer, the first read's finding 3 - now the
   wire's test checks, after a reap that counted a refusal, that the wire is
   free of its send, and the later-pass mutant fails it.

Of its notes: N1 - the charge's wording, the rings in the notice, "unlimited"
for no limit, and a notice that does not blame a limit there is none of -
taken; N2, the model's limits, taken (above); N3, Part 4's two questions,
given a checklist item again; N4, the model's comments on the pre-open drain,
which the zero-copy designs leave out, corrected; N5, the README's counts, are
the integration's; N6, this record's citations, corrected; N7, the README's
limits on launching, given the sentence; N8, `publish_ring`'s comment order,
taken.

## The verification

- **On the Pi cluster** (Linux 6.18, 16 KiB pages, where rings are charged; one
  node, its logical nodes; 2026-10-10 ~14:55, `pi-zero-copy-refused/` in the
  evidence directory): `shmem.wire.zero-copy-refused` passed, "2 sends by
  reference, 2 refused, 2 notifications, 2 copied", and the four
  `shmem.internode.zero-copy.*` maps and `shmem.wire.zero-copy` passed as
  before; `shmem.internode.zero-copy-refused.0-1-2` failed, its first build
  having reduced two static flags, which in a program not linked by `oshcc` are
  not symmetric, so every PE ended there. The flags are on the heap now. A job
  of two logical nodes under `ulimit -l 0` was refused at `shmem_init` with "it
  fails with \"Cannot allocate memory\", which on Linux 6.14 and later can be
  the locked-memory limit, ulimit -l, 0 KiB here". This record said the fixed
  test had passed there, which no saved run showed (the second read, finding
  2); its run after the second read is below.
- **In the development VM** (Linux 6.12, at `8eea960`; 2026-10-10 ~15:45;
  `zero-copy-refused-check/` in the evidence directory): the static checks 12
  of 12; the wire's and the internode tests 20 of 20, the
  two refused ones as on the Pi; three mutants, each built and run against the
  two refused tests -
  - **without the fallback** (the refusal fatal, as before): both fail,
    "Cannot allocate memory";
  - **lending on after a refusal**: the wire's test fails - 17,815 sends by
    reference, every one refused and its bytes copied, both streams finishing
    in 26 s, and its count of refusals no longer two; the internode test
    passes, a region owing the refused send's notification making the next
    send copy;
  - **the refused bytes offered again only at a later pass**, as first built:
    both pass, and the internode test took 0.18 to 0.30 s in five runs where
    the branch took 0.07 to 0.08 - finding 3's wait, measured.

  `probe` printed "locked-memory limit (ulimit -l): 8192 KiB", and under
  `ulimit -l 0` passed whole: 6.12 does not charge rings, where the Pi's 6.18
  failed the same run.
- **After the second read, in the development VM** (2026-10-10 ~20:04;
  `second-read-check/` in the evidence directory): the static checks 13 of 13;
  in the debug preset the wire's tests and the internode zero-copy tests 11 of
  11, the refused ones as before; under AddressSanitizer both refused tests, and
  under ThreadSanitizer the wire's, passed rather than skipped, "2 sends by
  reference, 2 refused, 2 notifications, 2 copied" and each node's notice once;
  the mutant offering the refused bytes again only at a later pass now fails
  the wire's test, "CHECK(!tx->sending && tx->sent == -1) failed", where it
  passed it before, and the internode test still passes it, in 0.30 s.
- **After the second read, on the Pi cluster** (at `e53c9e2`, 2026-10-10
  ~20:09; `pi-zero-copy-refused-e53c9e2/` in the evidence directory): the seven
  zero-copy tests passed, `shmem.internode.zero-copy-refused.0-1-2` among them,
  each node's notice once, "ulimit -l, 0 KiB here" - a refusal 6.18 gives at
  submission, taken through `progress.c`, which no saved run had shown; and a
  job of two logical nodes under `ulimit -l 0` was refused at start-up, naming
  the limit, as before.

- **The integration branch's full verification** (the development VM, the
  branch at `4df0047` - the blocking gets, the zero-copy refusal and
  `shmem.helper-wake`, each with its read's fixes - 2026-10-10 20:26 to 22:42;
  `docs/design/evidence/v0.2-step4/2026-10-10-integration/`): the static checks
  13 of 13; debug and release 867 of 867 at no helpers, two and three, the
  release build with the timing pins on; AddressSanitizer 852 of 852 at each,
  `shmem.helper-wake` and the two refused zero-copy tests among them, none
  skipped in any preset; ThreadSanitizer 606 of 606 at no helpers -
  `shmem.multiple.0-1`'s gets of three pieces from four threads at once among
  them - and at two and three helpers 15 and 13 failures, every one the tool's
  reports of landings through another process (`tsan-gate.py`, run by hand
  after the script's path to it failed): `shmem.multiple.0-1` new among them,
  its 20 KiB gets giving the helpers heap to read that the main thread zeroed,
  ordered by a barrier through another process; tier 3 114 of 114; the models
  40 of 40, `model.reservation` in 902 s; `osoe-pacer`'s mix, mailbox and
  collectives a minute each and ten minutes of mix at no helpers and at two,
  `failures=0` and no deadlock line; the single-node build, without warnings,
  404 of 404.

*Undo:* the refusal's branch in `ring.c`, the notice, the limit in the ring's
and the start-up check's messages and in the probe, the model's cases and its
limits, the two tests and `locked-memory.h`, and the README's, design's and
`wire.h`'s sentences back; the release checklist's item back as it was.
