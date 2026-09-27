# Task: the stress tester, phase 1 - `osoe-pacer` running

**Status: written on 2026-09-26 (night) from
`docs/design/osoe-pacer-proposal.md`, after the author decided its open
points (`docs/decisions/2026-09-26-night.md`).** This phase builds the tool's
frame and the first two exercises, so that something runs end to end - on
the VM's node maps and on the Pi - before the surface grows. Phases 2 to 4
are sketched in the proposal's §5 and get briefs of their own.

You are building `tools/pacer/osoe-pacer`: a program that puts the library
through its paces with whole-job patterns from real OpenSHMEM codes,
verifies every result against an oracle, and reports as it goes. It is not
`osoe-bench` (PE 0 alone, one measurement a line, against the floor) and
does not change it. Its output is for a person watching a run and for a
script afterwards, in that order.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1).
- **For the tool itself: nothing that grows faster than O(N) a PE**, N the
  job's size, and an oracle for everything it measures.

## Read first

1. `CLAUDE.md`, the naming rule especially: words, not contractions, in
   file names and identifiers alike.
2. The proposal, `docs/design/osoe-pacer-proposal.md`: §1 to §3 whole, §4.1
   and §4.2, §5's phase 1, §7.
3. `tools/bench/osoe-bench.c` and its `CMakeLists.txt`: the conventions to
   keep - names of measurements as arguments, `quick`, a name it does not
   know refused, `key=value` fields, a rate as every byte over the slowest
   PE's time, the smoke test over a node map with the launcher's failure
   strings as `FAIL_REGULAR_EXPRESSION`.
4. `shmem/tests/test-differential.c` (order-independent oracles; the FNV
   hash) and `shmem/tests/CMakeLists.txt` around `shmem.differential` and
   `osoe_tier2_map_test` (how a program is registered over the four maps of
   design §8.1, and why the multi-node maps stay out of ThreadSanitizer).
5. `tools/launch/launch.c`'s usage: `-n`, `-m`, `-t`, `-e`.
6. The spec, `docs/reference/OpenSHMEM-1.6.pdf`: §9.4 (`team_split_strided`,
   `team_translate_pe`), §9.6 (`iput`, `ibput`, `putmem_nbi`, `getmem_nbi`),
   §9.8 (`putmem_signal`), §9.10 (`fcollectmem`, `alltoall`, `alltoalls`,
   `alltoallmem`, `team_sync` - which does **not** complete RMA, unlike a
   barrier), §9.11 (`wait_until_all`), §9.12 (`quiet` completes "memory
   store" to symmetric objects too, which is what makes `shmem_ptr` stores
   safe before a sync), §9.3 (`malloc_with_hints`).
7. `corpus/isx/SHMEM/isx.c:209-258` and `:468`, with
   `docs/design/workloads/traces/U5-isx.trace`: the exchange the sort
   follows. `corpus/prk/SHMEM/Transpose/transpose.c` for the strided
   alltoall.
8. `docs/pi-cluster.md` §6 and §8 (how `osoe-bench` and ISx are run there,
   and ISx's heap size), `docs/decisions/README.md` "How work is done".

## Decisions, made concrete

1. **Place and names.** `tools/pacer/`, target `osoe-pacer`, not installed.
   Sources: `osoe-pacer.c` (the driver), `parallel-print.c` and
   `parallel-print.h`, `exercise.h` (the interface every exercise
   implements), `sort.c`, `alltoall.c`, `print.c`, `random.h` (splitmix64
   and the mixing function). `CMakeLists.txt` as `tools/bench`'s, linking
   `osoe` and `Threads::Threads`, with `osoe_warnings`. Identifiers spell
   words: `pacer_exercise`, `receive_capacity`, not `exc`, `rcap`.
2. **The command line.** `osoe-pacer [OPTION]... [EXERCISE]...` with the
   exercises `sort`, `alltoall` and `print`; nothing named runs `sort` and
   `alltoall`; an unknown name is refused with the list. Options:
   `--seconds S` (each exercise about S seconds, default 60), `--rounds R`
   (a fixed count instead, for a reproducible run), `--report S`
   (checkpoint interval, default 5), `--memory SIZE|P%` (decision 10),
   `--seed N` (default 1), `--quick` (the smoke test's sizes: three rounds
   of 64 K keys a PE; three block sizes and four exchanges a variant),
   `--sort rounds|accumulate` (default `rounds`), `--exchange
   fetch-add|alltoall` (default `fetch-add`), `--no-ptr`,
   `--stop-on-failure`, `--plant DEFECT` (decision 9). Long options only;
   `--help` prints them and exits 0 before `shmem_init`.
3. **The header**, printed once by PE 0 after `init`, one line of fields:
   `osoe-pacer` and the library's name and version (`shmem_info_get_name`,
   `shmem_info_get_version`), the thread level (`shmem_query_thread`),
   `pes`, `nodes` and `layout` (PEs a node in node order, from
   `SHMEM_TEAM_SHARED`: a PE's node is its shared team's rank 0 translated
   to the world), `hosts` (hostnames gathered by fcollect, folded as Slurm
   folds them: `pi-[001-004]`), `heap`, `memory` (the budget), `seed`,
   `exercises`, and `plant` when one is set. Then one line per exercise
   naming its parameters and the symmetric bytes it will use.
4. **The exercise interface** (`exercise.h`): a struct of a name and five
   functions - `memory(npes, options)` returns the symmetric bytes the
   exercise would use at its parameters, so the driver can scale it before
   `setup`; `setup(team, options)` allocates; `step(units)` does that many
   units of work (a round of the sort; one exchange of every variant at one
   size for the alltoall) and records its own failures; `checkpoint(out)`
   fills this PE's counters since the last call - work units, bytes moved,
   the step's seconds, failures; `teardown()`. The driver owns the loop: it
   runs `step`, times it, reduces the counters over the team (`sum` of
   work, bytes and failures, `max` and `min` of seconds), prints the
   checkpoint line from the team's rank 0, and adapts `units` so a step
   takes about `--report` seconds; under `--rounds` a checkpoint is a round.
   Under `--seconds` an exercise ends at the first checkpoint past its
   time. Nothing here needs a thread.
5. **Checkpoint and summary lines**, `key=value` after the exercise's name:
   `sort t=35.2 round=7 keys=117440512 keys_per_s=41200000 mib_per_s=314.3
   imbalance=1.03 sort_s=1.9 failures=0`; a rate is the job's bytes over the
   slowest PE's seconds; `imbalance` is the largest PE's batch over the
   mean. At the end, one `summary <exercise> ...` line per exercise with
   its totals, and last `osoe-pacer failures=N seconds=S`. The exit status
   is 1 when N is not 0, else 0. Every line is one `printf` ending in a
   newline, flushed, so a launcher's interleaving cannot split it.
6. **`parallel_print`**, in `parallel-print.c`, exactly the proposal's §2:
   `parallel_print_init(capacity)` (collective on the world, allocates the
   16-byte-a-PE record array sized for the world, a 64 KiB arena, the format
   buffer, and gathers hostnames), `parallel_print_name_team(team, name)`
   (local, a table of at most 64 names), `parallel_print(team, format,
   ...)` and `parallel_print_finalize()`. One `shmem_fcollectmem` of
   `{hash, length}`; the owner of a hash is its lowest team rank; texts
   packed in rank order into arena windows, never straddling one; owners
   `shmem_putmem` NUL-terminated text to the team's rank 0's arena,
   `shmem_quiet`, everyone `shmem_team_sync`; rank 0 prints the window; a
   further window is preceded by another sync; no sync after the last. The
   hash is FNV-1a 64 over the bytes, then `fmix64` (`h ^= h >> 33; h *=
   0xff51afd7ed558ccd; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53; h ^= h >> 33`);
   a result of 0 becomes 1; 0 means silent. Labels: the group's world PE
   numbers (`shmem_team_translate_pe` to `SHMEM_TEAM_WORLD`, sorted) in
   Slurm's bracket syntax, `[0-3,8-11]`, `[5]`; no prefix on the world
   team; `shared` on `SHMEM_TEAM_SHARED`; the registered name on a named
   team; otherwise `team(start:stride:size)` in world ranks, which is the
   same on every member. Text longer than the capacity is cut to end in
   `...`. Returns the number of distinct lines on every PE. The source's
   header comment states the collision odds (about D² / 2⁶⁵ a call) and
   the fix if ever needed (a 128-bit record). Written against the OpenSHMEM
   API alone - no osoe header, nothing from `tools/stat` - so it compiles
   against SOS too.
7. **`sort`** (`sort.c`), the proposal's §4.1. Ownership: PEs ordered
   node-major (node in order of its lead's world rank, then local rank),
   each owning an equal share of the 64-bit key space; the owner of a key
   is the PE at index `(key * N) >> 64` in that order (a 128-bit product),
   so a node's range is contiguous whatever Slurm's distribution was, and
   the `0,1,0,1` map is where that shows. Keys: splitmix64 seeded from a
   mix of (seed, "sort", PE, round); the generating PE accumulates count,
   sum and xor as it goes. Binning: private staging, one bin a destination,
   each bin's capacity R/N plus slack; a bin that fills is sent early and
   refilled, so no key is dropped and the slack stays small. The push
   exchange (`--exchange fetch-add`): per destination,
   `shmem_uint64_atomic_fetch_add` on the destination's cursor (cursors from
   one `shmem_malloc_with_hints(..., SHMEM_MALLOC_ATOMICS_REMOTE)`), then
   `shmem_putmem_nbi` of the bin at the reserved offset, or plain stores
   through `shmem_ptr` when the destination is a node-mate (`--no-ptr`
   forces the put); after every bin, `shmem_quiet`, then `shmem_team_sync`.
   A reserved offset past the receive capacity is a failure line, never a
   write; the capacity is the expectation plus one eighth plus 4,096 keys.
   `--sort rounds`: two receive buffers and two cursors, alternated by round
   parity, so one sync a round suffices - the receiver checks and resets
   buffer `r % 2` (`shmem_uint64_atomic_set` of the cursor to itself, never a
   plain store to a word others AMO) while round `r + 1` fills the other,
   and no sender reaches round `r + 2`'s reservation before round `r + 1`'s
   sync, which follows the reset. `--sort accumulate`: one buffer, cursors
   never reset, rounds until the budget's capacity is reached, then one
   sort and check of everything. The check, per batch: every key in this
   PE's range; the batch sorted (LSD radix with 16-bit digits into a
   scratch of the batch's size in `rounds` mode; in `accumulate` mode
   either an in-place sort or the scratch counted in the budget - choose,
   and say so in the header); non-decreasing afterwards; count, sum and
   xor accumulated. The pull exchange (`--exchange alltoall`): staging is
   symmetric and bins are contiguous in destination order; a
   `shmem_alltoall` of one `{count, offset}` record a pair tells every
   receiver where each sender's bin for it lies; the receiver
   `shmem_getmem_nbi`s every bin into its buffer at the prefix sum of the
   counts, one `shmem_quiet`, checks, then `shmem_team_sync` before any
   sender refills its staging. Oracle at the end: the `shmem_uint64_sum_reduce`
   of the generated triple equals that of the received triple, on every
   PE, or a failure line says which component differed. Reports the fields
   of decision 5, and the seconds in generate, bin, exchange, sort and
   check. Memory: `memory()` returns the receive buffers (two in `rounds`
   mode), the scratch, staging and the cursors, for the R the options ask;
   the driver lowers R to fit the budget and the header says the R it ran.
   Defaults: R = 4 M keys a PE (32 MiB) in `rounds` mode; in `accumulate`
   mode R = 1 M a round until the budget's capacity.
8. **`alltoall`** (`alltoall.c`), the proposal's §4.2: block sizes 8 B,
   64 B, 512 B, 4 KiB and 64 KiB a pair, each skipped alike by every PE when
   N × block × the buffers does not fit the budget (`osoe-bench`'s rule);
   every element `mix(source PE, destination PE, index, round)` as 64-bit
   words, so a wrong word names its origin; the gaps of the strided forms
   filled with a canary and checked untouched. Seven variants, each timed
   from just before its first call to just after the data is known
   complete - the collective's return; the put form's sync; the get form's
   quiet; the signal form's `wait_until_all`; the `iput` and `ibput` forms'
   quiet and sync - and verified after the timing: `shmem_uint64_alltoall`
   through the C11 generic, `shmem_alltoallmem`, `shmem_uint64_alltoalls`
   with `dst` 3 and `sst` 2, puts to every peer then `quiet` and
   `team_sync`, `team_sync` then gets from every peer, `shmem_putmem_signal`
   to every peer (`SHMEM_SIGNAL_SET` of the round number into one word a
   sender) then `shmem_uint64_wait_until_all` over the N words with
   `SHMEM_CMP_EQ`, and the transposing forms - `shmem_uint64_iput` from a
   peer-minor source layout, and `shmem_uint64_ibput` of several blocks in
   one call from a layout of `[chunk][peer][bsize]`. On the world team and
   on `even` (`shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, (N + 1) / 2,
   NULL, 0, &even)`, named `even` for `parallel_print`; on the odd PEs the
   split gives `SHMEM_TEAM_INVALID` and they wait at the world sync that
   follows). Reports one line a size with every variant's median
   microseconds side by side, and the failures.
9. **Planted defects, `--plant`**: `sort-key` (PE 1 changes one key after
   counting it and before sending), `sort-order` (PE 1 skips one round's
   local sort), `alltoall-byte` (PE 1 flips one byte of one block before
   the exchange, in each variant), `print-hash` (PE 1 hashes a different
   string than it prints, so its line separates from its group's). Each
   must produce a failure line and a nonzero count; the header names the
   plant. The plants live in the tool, guarded by the option, so they can
   never be left on by mistake: the header line is the check.
10. **Memory.** `--memory` takes a size as `SHMEM_SYMMETRIC_SIZE` spells one
    or a share of the heap as `P%`; the default is `50%`; the heap's size is
    read as the library reads the variable (its parser is not exported, so
    the tool has its own for the same spellings, default 256 MiB). A budget
    the heap cannot hold, after the tool's own structures, is refused before
    any exercise, naming the `SHMEM_SYMMETRIC_SIZE` that would allow it. The
    `print` exercise and `parallel_print` are outside the budget.
11. **Failures.** A failure is recorded by the PE that finds it: a count and
    the first few lines of text, each `exercise round=r what: value`, with
    what differs per PE last so that lines coalesce. At each checkpoint the
    counts join the reduction and the lines go through `parallel_print` on
    the exercise's team, then are cleared; `--stop-on-failure` makes a
    nonzero count at a checkpoint end the job with
    `shmem_global_exit(1)` after the lines. A failure never stops an
    exercise on its own: a run that continues past one shows whether it
    was one or a thousand.
12. **`print`** (`print.c`), a self-checking exercise of `parallel_print`,
    not run unless named: every PE the same text (one line, `[0-N-1]`);
    by parity (two); the odd PEs silent (one); every PE distinct (N lines
    in rank order); on `even` (`even[0,2,...]`); on `SHMEM_TEAM_SHARED`
    (`shared[...]`, one line a node); an unnamed strided team of three;
    several windows (an arena of 64 bytes through a hidden option, 1 KiB
    texts); a text past the capacity (ends in `...`). The team's rank 0
    compares the lines it assembled with the ones the map predicts - it
    knows N and every PE's node - and every PE checks the returned count.
13. **Not in this phase**: threads and contexts, teams beyond `even`,
    mailbox, atomics, rma, locks, memory churn, the collectives sweep, the
    library's counters in the stream, node-folded labels, the second route,
    the shim, the mixer, installation. Leave the interface able to take
    them: the exercise struct, the option parser and the team argument are
    what the later phases extend.

## Order of building, each part running before the next

1. The skeleton: options, `init`, the header, `parallel_print` on the world,
   the driver's loop with `print` as its first exercise; run on the four
   maps by hand.
2. `alltoall`: the three collective forms first, verified; then the four
   by hand; then on `even`; its plant.
3. `sort`: `rounds` with the push exchange and the `shmem_ptr` path; then
   `accumulate`; then the pull exchange; its two plants; the budget and
   the header's R.
4. Failure lines, `--stop-on-failure`, the summary and the exit status.
5. The tests, the runbook's §6, the Pi run, the notes.

## Tests

- `tools.pacer.<map>`: `osoe-pacer --quick` over the four poles, `0,0,0,0`,
  `0,1,2,3`, `0,0,0,1,1,2` and `0,1,0,1`, each `osoe-launch -t 240`, label
  `tier2`, `TIMEOUT 300`, `PASS_REGULAR_EXPRESSION "osoe-pacer failures=0 "`
  and `osoe-bench`'s `FAIL_REGULAR_EXPRESSION`. Under ThreadSanitizer only
  `0,0,0,0`, as every multi-node test. Also `--quick --sort accumulate
  --exchange alltoall --no-ptr` on `0,0,1,1`, so every path of the sort runs
  on every merge.
- `tools.pacer.plant.<defect>`, four of them, on `0,0,1,1`:
  `PASS_REGULAR_EXPRESSION "osoe-pacer failures=[1-9]"` and the failure
  line's text.
- `tools.pacer.print.<map>` on `0,0,1,1` and `0,1,2,3`: `osoe-pacer print`,
  passing on `failures=0`.
- The quick runs stay under about a minute a preset in release; if they
  cannot, shrink `--quick`, not the coverage.
- Whether these stay in every preset's suite is the author's decision after
  he has seen their cost (proposal §3); register them in one block with a
  comment saying so.
- Before merge, the standard verification less the SOS sweep: the four
  presets, the plants (which are the mutations here, and permanent), and a
  review of the oracles - for each, the question "what wrong output would
  this still accept?" - written into the decisions record.

## The measurement (numbers to bring back)

On the Pi at 4 × 4, `release`, ten minutes each: the default run; `--sort
accumulate` with `SHMEM_SYMMETRIC_SIZE=1500m --memory 1g`; `--exchange
alltoall`; `--no-ptr`. Keep the outputs under
`docs/design/evidence/pi-cluster/<date>/pacer/` and put the sort's keys/s
under each exchange, and the alltoall variants' microseconds at 8 B, 4 KiB
and 64 KiB, in `docs/pi-cluster-results.md`. `osoe-bench`'s alltoall line at
the same block sizes is the number to compare the collective form with; the
put-by-hand form against the collective is the number the author most
wants: how much the library's alltoall buys over what an application would
write.

## Stop and ask

- The tool finds a failure in the library. Record a reproduction (the
  command line, the seed, the map, the output) in the decisions record and
  stop; whether to fix it on this branch is the author's call.
- An oracle cannot be made to fail by a plant, or a plant is caught by a
  check other than the one it was meant for.
- Anything in the tool that would grow faster than O(N) a PE, or per
  team member beyond that.
- The quick runs cannot be brought under about a minute a preset in
  release without losing a path.
- `parallel_print` needs more than one collective in the common case, or
  a record of more than 16 bytes a PE.
- The header, the labels or the checkpoint fields seen on a real run read
  worse than the proposal promised; show the author a screenful before
  changing the format.
