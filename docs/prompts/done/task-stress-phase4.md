# Task: the stress tester, phase 4 - stress proper

**Status: written on 2026-09-27, at the end of phase 3, from
`docs/design/osoe-pacer-proposal.md` §2.5, §3, §4.9, §4.10 and §5, and what
phases 1 to 3 built and found (`docs/decisions/2026-09-27.md`,
`docs/decisions/2026-09-27-afternoon.md`). Its first draft preceded phase 3's
last measurements, and item 1's design question was taken up while they
ran; the answer is phase 4's record's.** Phases 1 to 3 left `osoe-pacer`
running eleven exercises, each alone on the whole job, each with an oracle
and a plant for every check, and a counted build that says which routines a
run calls. Phase 4 makes it a stress: exercises at once on teams out of step,
for hours, with the library's own counters in the stream and a second route
for its output to check the first.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1).
- **For the tool itself: nothing that grows faster than O(N) a PE**, N the
  job's size, and an oracle - with a plant that fires it - for everything it
  measures.

## Read first

1. `CLAUDE.md`.
2. The proposal: §2.5 (the second route), §3 (the tool's shape, "The
   library's own counters"), §4.9 (`mix`), §4.10 (the deprecated forms),
   §5's phase 4, §7.
3. The records of phases 1 to 3, for the interface and every choice made
   without the author - phase 3's shim, its report and its item 8 above all:
   a check can only fail on what runs, and whether something runs is a
   count.
4. `tools/pacer/exercise.h` and `osoe-pacer.c` whole; one exercise whole.
5. `tools/stat/segment.c` and design §3.14 (the control segment's counters,
   layout-versioned); how `shmem/tests` link the reader.
6. The spec: §9.3 (symmetric allocation is collective over the world), Annex F
   (the deprecated routines).

## What to build

1. **`mix`** (proposal §4.9): the world split into K teams by the seed; each
   team runs one exercise for a slice, the teams out of step; slices rotate
   the exercises through the teams; some slices put `sort` or `alltoall` on
   the world while the others pause. Each slice verifies. `--seconds` makes
   it a soak; the schedule prints in the header and repeats from the seed.

   **The design question to settle first**: symmetric allocation is
   collective over the world (§9.3), and an exercise's `setup` allocates, so
   an exercise cannot set up on one team while another team runs. The
   likely answer is an interface change - each exercise's `setup` split into
   a world-collective allocation, sized for the largest team it may run on,
   and a team-collective start, and `teardown` the same way - with every
   exercise allocated before `mix` begins. Every exercise's state is one
   static structure, so no exercise may run on two teams at once: a slice's
   exercises are distinct. Settle it, record it, then build.
2. **The library's counters in the checkpoint line** (proposal §3): with
   `tools/stat/segment.c` linked as the tests link it, rank 0's checkpoint
   adds the deltas of a few node-scope counters since the last one -
   `connection.bytes_sent`, `am.gets_nonblocking`, `am.landing_full`,
   `am.reserve_stalls`, `am.wakes_sent`, `shmem.barrier.ns`. The reader is
   layout-versioned: when the table is unusable, or the library is not osoe,
   the fields are omitted, not guessed.
3. **Node-folded labels** (proposal §1, decision 2): `--fold-nodes`, a label
   by node names (`pi-[001-004]`) where every PE of each node agrees.
4. **The second route of `parallel_print`** (proposal §2.5): log₂ N rounds of
   recursive doubling exchanging sorted `(hash, lowest rank, count)` lists by
   `putmem_signal` and `wait_until`; the same labels and order, so a run can
   call both routes at each checkpoint and diff the lines - a differential
   test of Bruck's fcollect against put-with-signal and the watch machinery.
5. **The deprecated forms** (proposal §4.10), off by default, for coverage:
   the active-set collectives with `pSync` and `pWrk` on partial sets and on
   the whole job, `shmem_barrier` and `shmem_sync` with a stride, the
   `_to_all` reductions, `broadcast32/64`, `collect32/64`, `fcollect32/64`,
   `alltoall32/64`, `alltoalls32/64`, the deprecated atomic names,
   `shmem_wait`, the cache routines, and `shmalloc` and `start_pes` in a
   program of their own. **And what phase 3's shim found uncalled beside
   them** (its record, "For the author" 1): the 117 `put_signal` forms beyond
   `putmem` and `uint64_t` - a transfer class of `rma`'s, every kind and
   context - and the 160 typed wait and test forms beyond `uint64_t` -
   `mailbox`'s signal words taking another type each round - and the singles
   an exercise has a place for: `shmem_pe_accessible` beside
   `addr_accessible`, `shmem_sync_all` in the sweep beside `barrier_all`,
   `shmem_getmem`, `shmem_alltoallsmem`, `shmem_query_initialized`. Each
   typed wrapper could be wired to the wrong width, and only a call finds
   that. The shim's report says what all of it adds to the count.
6. **Plants**, one for every new check, asserted line by line.

Not in this phase: installation (the author decides after), anything that
needs RoCE.

## Order of building, each part running before the next

1. The interface change for `mix`, the other exercises unchanged in
   behaviour: their tests pass as before.
2. `mix`, then a soak on the VM.
3. The counters in the line; `--fold-nodes`.
4. The second route and its differential.
5. The deprecated forms; the shim's report again.
6. The tests, the runbook's §6, the soak on the Pi, the notes.

## The measurement

A soak of `mix` on the Pi at 4 × 4 for several hours, in release, with each
node's retransmission timeouts before and after; an hour of it on the VM on
the four maps' largest; the shim's report of a default run and of `mix`.

## Stop and ask

- The tool finds a failure in the library. Record a reproduction.
- A check no plant can fire, or a plant caught by a check it was not meant for.
- Anything in the tool that grows faster than O(N) a PE.
- The interface change for `mix` cannot keep an exercise's behaviour alone.
- The quick runs cannot be kept to about a minute a preset in release.

*Author decides after*: whether `osoe-pacer` is installed with `oshcc` and
`osoe-stat` as a cluster check for a stranger (the release point's item 5),
and whether a soak joins the release checklist.
