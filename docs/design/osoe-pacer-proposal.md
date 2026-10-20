# osoe-pacer: a stress tester and benchmark for the whole API

*Proposed 2026-09-26 and decided with the author the same night
(`docs/decisions/2026-09-26-night.md`): the name, the labels, `parallel_print`
kept in the tool, the phase order, the memory default and its option, the
briefs' names. All four phases are built
(`docs/prompts/done/task-stress-phase1.md` to `-phase4.md`; the records, with
where the tool differs from this proposal and why, are
`docs/decisions/2026-09-27.md`, `2026-09-27-afternoon.md` and
`2026-09-27-evening.md`). The three points left to the author were settled
the same night (`docs/decisions/2026-09-27-night.md`): the smoke tests run in
every preset, `osoe-pacer` is installed, and an hour of `mix` joins the
release checklist.*

## 0. What it is, and what it is not

`osoe-pacer` is one program, built in the tree beside `osoe-bench`, that puts
the library through its paces: whole-job patterns drawn from real OpenSHMEM
codes, run for as long as it is told to, **every result verified against an
oracle**, and reported as it goes - one line a checkpoint from the job's
first PE, and, where PEs differ, one line per group of PEs that said the same
thing (`parallel_print`, §2). It ends nonzero if any PE's verification
failed.

It is not `osoe-bench`, and does not replace it. `osoe-bench` measures what
the library costs with PE 0 alone in motion and everyone else idle: one
number a line, against the floor of §8.4. `osoe-pacer` puts every PE in
motion at once, mixes idioms, and runs for hours; its numbers are throughput
under load, and its main product is confidence. Nor is it the test suite:
ctest's programs each pin one behaviour in seconds; this one looks for what
appears only after the ten-thousandth round, on a real switch, with teams and
threads and atomics all live together. The SOS sweep and the corpus programs
(ISx, PRK, NPB IS) already give conformance breadth; this gives duration,
concurrency and scale.

Two existing pieces are reused rather than rebuilt: the control segment's
counters, read in-process through `tools/stat/segment.c` as the tests already
do (design §3.14), so the progress stream can show what the library is doing
underneath; and the `pshmem_` profiling names (§7.4), which make a generated
interposition shim a cheap, exact answer to "which routines did this run
call" (§5, phase 3).

**The name.** The author offered `osoe-mark` and `osoe-pacer`; `pacer` is the
one that is a word on its own - the tool paces the library - where `mark` is
a piece of "benchmark", which CLAUDE.md's naming rule refuses. So the
directory is `tools/pacer/`, the target `osoe-pacer`, and the briefs are
`task-stress-phaseN.md`, the author's spelling for the task.

## 1. What the plan became

1. **Every exercise verifies.** A benchmark that only generates load finds
   nothing; each exercise below has an oracle it can compute without a serial
   reference, and a failure is a first-class result carried to the end of the
   run. The tool is a correctness test that happens to print rates.
2. **`parallel_print` is one collective, not rounds of them.** An `fcollect`
   of a 16-byte record per PE (hash and length) lets every PE compute the
   groups, the owners and the byte offsets locally; owners then put their
   text to the team's rank 0, which prints in rank order. Rounds of
   reduction happen inside the library's fcollect already (Bruck's steps at
   the seam, §5.8); the tool need not write them again. The rounds design is
   kept as a *second route* to the same output, added later as a
   differential check of the first (§2.5).
3. **Labels are world PE numbers in Slurm's bracket syntax**, `[0-3,8-11]`,
   whatever team the call is on, and **a call on any team but the world
   carries the team's name before the bracket**: `even[0,2,4,6]`,
   `shared[4-7]`, `row3[8-11]`. A reader following a PE through a run wants
   one number for it, and a line from a team collective should say which
   team. Folding to node names (`pi-[001-004]`) is a later option for when
   every PE of each node agrees.
4. **The sort is two-level and ISx-shaped.** Ownership is node-major, so a
   node owns a contiguous key range and its PEs subranges; the exchange is
   fetch-add reservation then a nonblocking put (the U5 idiom, ISx's own),
   with `shmem_ptr` stores for node-mates (U8); verification is exact
   without a reference sort: counts, sums and xors of every key generated
   against every key received, each PE's range, and monotonic order after
   the local sort.
5. **Threads early.** The lock-free shared-context path is the newest code
   and holds the one open item (the unreproduced `OSOE_CHECK_QUIET` firing,
   `docs/decisions/2026-09-26-evening.md` item 13). A threaded exercise that
   runs for an hour under `OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1` is the
   best chance of seeing it again with its message. So threads are phase 2,
   not last.
6. **Seeded determinism.** Every random choice derives from
   (seed, exercise, PE, round), and the header prints the seed and the
   schedule, so a failure reproduces from its own log line.
7. **The mixer is the stress.** Exercises one at a time are tests; several
   at once on disjoint teams, rotating, is what finds the interaction bugs
   (a team collective while its members are targets of an AMO storm; a team
   destroyed while a neighbouring team's fcollect is in flight). It is the
   last phase because it needs the exercises first, but it is the point.
8. **Team creation is a per-job budget.** A destroyed team's number is never
   reused (`team.c`, design §5.7; default 64 with three predefined). So the
   teams exercise cannot churn forever: it creates its teams once, keeps
   them, and exhausts the limit only in a dedicated short mode that checks
   the split's nonzero return. This is worth the author's attention beyond
   the tool: an application that makes a sub-team per iteration runs out of
   numbers after 61 iterations at the default. *Since the team redesign's
   step D (2026-10-03) a number is each PE's own and a destroyed team gives
   it back: the exercise renews a strided team every third round, and the
   exhaust mode splits once more after destroying what it made
   (`tools/pacer/teams.c`); a program that keeps every team it makes still
   meets the limit, at 61 teams a PE.*
9. **Scale rules for the tool itself.** No structure larger than O(N) per PE
   (N the job size), the same bar the library is held to, so the tool still
   runs at the hundreds-of-nodes target and a 1M-PE run would cost 16 MB a
   PE for `parallel_print`'s array, not more. Every exercise states its
   memory in the header and scales itself to the budget (§3, "Memory").

## 2. `parallel_print`

### 2.1 Interface

```c
int  parallel_print_init(size_t message_capacity);        /* collective on WORLD, once */
int  parallel_print_name_team(shmem_team_t team, const char *name);
int  parallel_print(shmem_team_t team, const char *format, ...);
void parallel_print_finalize(void);
```

A team collective: every member calls it with its own arguments. Each PE
formats with `vsnprintf` into a private buffer of `message_capacity` bytes
(default 1 KiB; longer text is cut and ends with `...`). An empty string
means *nothing to say*: the PE is silent, and a group of silent PEs prints no
line, so a subset can report. Returns, on every PE, the number of distinct
lines printed; negative before `init`.

Output, from the team's rank 0, one line per distinct message, in the order
of each group's lowest team rank:

```
[0-15] round 7: 16.8 M keys exchanged, 41.2 M keys/s
[0-3,8-11] 112 MiB/s
[4-7,12-15] 94 MiB/s
even[0,2,4,6] alltoall block=4096 collective_us=612
```

The bracket holds the group's world PE numbers, compressed as Slurm
compresses a hostlist: consecutive runs as `a-b`, runs joined by commas, one
PE as `[5]`. A call on the world team has no prefix. A call on any other
team prefixes the team's name: `shared` for `SHMEM_TEAM_SHARED`, the name
given by `parallel_print_name_team` for a team the program made, and for a
team never named a spelling derived from its world ranks, the same on every
member - `team(start:stride:size)`, `team(4:2:8)` - since a name assigned by
order of first use would differ between PEs that meet the teams in
different orders. `pdsh`'s `dshbak -c` is the model for the whole: identical
output from many hosts shown once, under the list of hosts that produced it.

Written purely against the OpenSHMEM API, it also compiles against SOS, which
is useful twice: a tool failure can be told from a library failure by running
the same binary there, and the function is worth having in any OpenSHMEM
program. It stays in the tool, not in `shmemx.h`, which is empty by design
(§7.4); promoting it later needs nothing from the library.

### 2.2 The algorithm, one collective

1. Format; `L` = length, 0 if silent.
2. `h` = hash of the bytes (2.3); `h = 0` is reserved for silent, so a hash
   of 0 becomes 1.
3. `shmem_fcollectmem(team, records, my_record, 16)`: every member gets
   every member's `{h, L}` in team-rank order. The array is sized for the
   world at `init`, so any team fits.
4. Local analysis, identical on every PE since the input is: the *owner* of
   a hash is the lowest rank with it; `D` = number of owners; the owners'
   texts are packed in rank order into windows of `ARENA` bytes (64 KiB by
   default), a message never straddling a window, so every PE knows its own
   offset and the number of windows `W`, usually one.
5. For each window: owners whose text falls in it `shmem_putmem` it, NUL
   included, into rank 0's arena, then `shmem_quiet`; everyone
   `shmem_team_sync`; rank 0 prints the window's lines and, if another
   window follows, everyone syncs again. The last window needs no trailing
   sync: rank 0 prints before it enters the next collective, and no owner's
   next put can begin before that collective has released it.
6. Rank 0's labels: for each owner, the ranks with its hash, translated with
   `shmem_team_translate_pe(team, r, SHMEM_TEAM_WORLD)`, sorted (a negative
   stride reverses them), compressed, prefixed by the team's name. Each line
   is assembled privately and written with one `fputs`; `fflush` once per
   call.

Cost per call: one fcollect (16N bytes in, log N steps across nodes), D puts,
one sync. Memory per PE: 16N bytes plus the arena plus the format buffer.
At the Pi's 16 PEs it is a few hundred bytes; at 1,024 PEs, 16 KiB.

Silent PEs still take part in the collectives, which is what makes the call
safe to put inside a loop every PE runs. A member that skips the call hangs
its team, as with any collective; the tool's checkpoints are the only
callers.

### 2.3 The hash

FNV-1a over the bytes, 64 bits, as `test-differential.c` and the segment
reader already use, followed by MurmurHash3's `fmix64` finalizer (two
xor-shift-multiply steps) so that every bit of the result depends on every
byte, which FNV alone does not give for short strings differing at the end.
About three instructions a byte, no table, no state.

Two different messages with one hash would print once, under the union of
both groups' labels, with the lower rank's text. With D distinct messages a
call the odds are about D² / 2⁶⁵: at ten thousand distinct lines, one in
10¹¹. Accepted for a benchmark's console, and written into the header of the
source; a 128-bit record (two independent finalizers) is the fix if it is
ever needed, at 24 bytes a PE.

### 2.4 What it exercises, as a side effect

`fcollectmem` on every team the tool makes, including strided and reversed
ones; `putmem` and `quiet` from many PEs to one; `team_sync`;
`team_translate_pe`. Called at every checkpoint, it is itself a long-running
collectives test with a checkable output.

### 2.5 The second route (phase 4)

The same output by the design first sketched: log₂ N rounds of recursive
doubling in which partners exchange sorted `(hash, lowest rank, count)` lists
by `putmem_signal` into a landing buffer sized for N records and
`wait_until` on the signal, merging as sets; then the owners' texts travel
to rank 0 as before. Every PE ends with the full set. Same labels, same
order, so a run can call both routes at each checkpoint and diff the lines:
a differential test of Bruck's fcollect against put-with-signal and the
watch machinery, with the tool's own output as the transcript.

## 3. The tool's shape

**Invocation.** As `osoe-bench`: `osoe-pacer [options] [exercise...]`, all
exercises but `threads` and `print` when none is named, `threads` only when
named since it initializes with `SHMEM_THREAD_MULTIPLE`; a name it does not
know is refused. Options: `--seconds S` or `--rounds R` per exercise,
`--memory SIZE` or `--memory P%` (below), `--report S` (checkpoint interval,
default 5 s), `--seed N`, `--quick` (the smoke test's sizes),
`--stop-on-failure` (a `shmem_global_exit(1)` after the failure lines),
`--plant DEFECT` (below), `--threads T`, `--sort MODE`, `--exchange HOW`,
`--no-ptr`, `--fold-nodes`.

**The header**, once: library name and version, vendor string, thread
level from `shmem_query_thread`, job shape (nodes and PEs a node from
`SHMEM_TEAM_SHARED`, hostnames folded), heap size, `OSOE_MAX_TEAMS`,
`OSOE_MAX_CONTEXTS`, `OSOE_MAX_THREADS` as the environment gives them, the
seed, the schedule, each exercise's memory, and any planted defect.

**Exercises** share one interface: `setup(team, budget)`, `step(n)` runs
`n` units of work (a round of the sort, a batch of updates), `checkpoint()`
returns the counters the driver reduces, `verify()` at the end of a slice,
`teardown()`. The driver adapts `n` so that a step takes about `--report`
seconds, then does one `team_sync`, one reduction of the counters (ops,
bytes, failures, min and max per-PE time for imbalance), prints the
checkpoint line from rank 0, and calls `parallel_print` for anything PEs
report individually. So the cadence is in work, converted to time, and no
reporter thread is needed; the exercises run under `SHMEM_THREAD_SERIALIZED`
except `threads`.

**Checkpoint lines** are `key=value` fields after the exercise's name, the
convention `osoe-bench` and the runbook's greps already rely on:

```
sort t=35.2 round=7 keys=117440512 keys_per_s=41200000 mib_per_s=314.3 imbalance=1.03 sort_s=1.9 failures=0
```

**Failures.** A PE that finds a wrong value records a short line (exercise,
round, what and where, with the first offending value) and counts it. At the
next checkpoint the counts are reduced and the lines go through
`parallel_print`, so identical failures on many PEs are one line under their
label - the case where it is most needed. The run continues unless
`--stop-on-failure`. The summary at the end gives, per exercise, work done,
rates and the failure count; the last line is `osoe-pacer failures=N`, and
the exit status is nonzero if N is.

**Planted defects, `--plant`.** An oracle is trusted only once it has been
seen to fire, and it must keep firing as the tree changes. So the tool can
plant one defect in itself by name - a key altered after it was counted, a
round's local sort skipped, a byte of a block changed before the exchange, a
PE hashing a different string than it prints - and a ctest run per defect
asserts the failure line and the nonzero count. The header names the plant,
so a planted run is never mistaken for a finding.

**The library's own counters** (phase 4). With `tools/stat/segment.c` linked
as the tests link it, rank 0's checkpoint can add the deltas of a few
node-scope counters since the last one: `connection.bytes_sent`,
`am.gets_nonblocking`, `am.landing_full`, `am.reserve_stalls`,
`am.wakes_sent`, `shmem.barrier.ns`. A reader then sees stalls appear as
the load grows without a second terminal for `osoe-stat`. The reader is
layout-versioned; when the table is unusable the fields are omitted, not
guessed.

**Memory.** The budget is symmetric bytes a PE the exercises may use.
`--memory` takes a size spelled as `SHMEM_SYMMETRIC_SIZE` spells one
(`512m`, `1g`) or a share of the heap (`90%`); the default is `50%`. The
heap's size is read as the library reads it (`SHMEM_SYMMETRIC_SIZE`, default
256 MiB). A budget the heap cannot hold, after the tool's own small
structures, is refused in the header with the `SHMEM_SYMMETRIC_SIZE` that
would allow it, so pushing higher is one variable and one option: on the Pi,
`SHMEM_SYMMETRIC_SIZE=1500m --memory 1g` puts four PEs' 4 GiB in a node's
8 GB `/dev/shm`. Every exercise computes what it needs from N and its
parameters, scales its main parameter down to fit, and says so in the
header. Private memory is bounded by the same number. Nothing is sized per
team member beyond O(N).

**Tests - what "smoke tests in every preset" means, and costs.** The merge
gate is `cmake --workflow --preset P` for each of debug, release, asan and
tsan, each running the tier-1 and tier-2 tests, near 500 of them, and tier 3
by hand in the VM. Adding the tool there means five things:

1. *Quick runs.* `--quick` fixes every exercise to a few rounds at small
   sizes - the sort at three rounds of 64 K keys a PE, the alltoall at three
   sizes and a handful of exchanges of every variant - about ten seconds in
   release, up to a minute under asan.
2. *Over the four node maps of §8.1*: all PEs on one node, one a node,
   uneven, and ranks interleaved across nodes. The differential test's
   reasoning applies: the intranode and internode paths are the poles, and a
   path right at both and wrong between shows in the middle two. Every
   exercise then crosses every path on every merge.
3. *Under each sanitizer.* asan sees an overrun in the tool or in the
   library on call patterns the unit tests never combine; tsan, on the
   one-node map only as every multi-node test is, sees races in the
   intranode path and, from phase 2, in the tool's own threads; debug has
   the assertions on. From phase 2 the debug and asan runs set
   `OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1`, which only matter once shared
   contexts exist.
4. *The plant tests*, one per oracle, each asserting the tool reported the
   defect it planted: an oracle that cannot fail is no oracle, and this
   keeps them honest for as long as the tree lives.
5. *The print test*, an exercise of `parallel_print` alone whose cases -
   everyone the same, by parity, a silent half, a team, every PE distinct,
   several windows, a truncated message - check their own lines against the
   labels the map predicts, so the format is pinned.

Cost: about a minute a preset in release, two or three under asan and debug,
less under tsan with its one map - against suites that take ten to thirty
minutes today. What it buys: the tool cannot rot. A tool that stopped
building, or whose oracle went quiet, is worthless the day it is needed, and
that day is when something else broke; and every merge gets a short pass of
whole-job patterns beside the unit tests. What it does not buy: stress. Ten
seconds finds nothing rare; duration comes from the runbook's runs and the
soak. The alternative is a `pacer` preset run by hand, which keeps the gate
faster at the price of the tool rotting between sessions. The author decides
after reading this; the brief registers the tests, which are one block of
CMake to remove.

Later, the multi-node maps join tier 3's subset, where the traffic crosses
netem's Pi-like link and its loss.

**The runbook** gains, in its §6 beside `osoe-bench`, the pacer's runs: a
ten-minute default run at 4 × 4, the accumulating sort at a 1 GiB heap, a
`threads` run from phase 2, a soak of hours from phase 4; their outputs kept
under `docs/design/evidence/pi-cluster/` as every session's are, and their
headline rates in `docs/pi-cluster-results.md`.

## 4. The exercises

Each names the reference idiom of design §8.6 it embodies, what it verifies,
what it reports, and the routines it covers. Sizes below are defaults; every
one is an option.

### 4.1 `sort` - the streaming bucket sort (U5, U8, U3)

Every PE owns a key range: node-major, so node *k*'s PEs own adjacent
subranges of one contiguous range, and a key's owner is found by two
divisions. Node membership comes from `SHMEM_TEAM_SHARED`: a PE's node is
its shared team's rank 0 translated to the world.

A round, on every PE: generate R 64-bit keys from a splitmix64 stream seeded
by (seed, PE, round), accumulating count, sum and xor as they are made; bin
them by owner into per-destination staging; for each destination,
`shmem_uint64_atomic_fetch_add` on its cursor (allocated with
`shmem_malloc_with_hints(SHMEM_MALLOC_ATOMICS_REMOTE)`) reserves room, then
`shmem_putmem_nbi` moves the bin - or, when the owner is a node-mate, stores
through `shmem_ptr` (`--no-ptr` forces the put); `shmem_quiet`, which the
spec says completes memory stores to symmetric objects too;
`shmem_team_sync`. The receiver then checks every key is in its range, sorts
its batch, checks the order, and accumulates the batch's count, sum and xor.
Two modes: `--sort rounds` recycles the buffers each round, bounded memory
for an unbounded soak; `--sort accumulate` appends across rounds until the
budget is full and sorts once at the end - the "large memory" run, where a
1 GiB heap a PE on the Pi holds 100 M keys a PE. A cursor past capacity is a
failure, not a silent drop: the capacity carries 1/8 slack plus 4,096 keys
over the expectation, which uniform keys never exceed in practice. The other
exchange, `--exchange alltoall`, sizes exactly and pulls: each PE bins into
symmetric staging, an `shmem_alltoall` carries every pair's count and
offset, and each receiver gets its bins with `shmem_getmem_nbi` from every
sender, one `quiet`, then a sync before staging is reused.

Oracle: after the last round, `shmem_sum_reduce` of the generated (count,
sum, xor) triple equals that of the received one; each PE's keys were all in
its range; each batch was in order. Reports: keys/s and MiB/s job-wide
(every byte over the slowest PE's time, as `osoe-bench` counts), imbalance
(largest over mean batch), time in generate, bin, exchange, sort and check.

Covers: `malloc_with_hints`, `atomic_fetch_add`, `atomic_set`, `putmem_nbi`,
`getmem_nbi`, `quiet`, `team_sync`, `shmem_ptr`, `team_translate_pe`,
`alltoall`, `sum_reduce`, `min_reduce`, `max_reduce`, `SHMEM_TEAM_SHARED`.

### 4.2 `alltoall` - every-to-every exchange, seven ways

Blocks of 8 B to 64 KiB a PE pair, on the world team and on the tool's
sub-teams. The collective forms: `shmem_alltoall` (a typed generic),
`shmem_alltoallmem`, `shmem_alltoalls` with destination and source strides
above one, the gaps filled with a canary and checked untouched afterwards.
Then the same exchange written by hand, as applications do: puts to every
peer, a quiet and a sync; gets from every peer after a sync; `putmem_signal`
to every peer and one `shmem_wait_until_all` over N signal words; and the
transposing forms, `iput` from a peer-minor layout and `ibput` of several
blocks in one call, as PRK's transpose does. Each element is a mix of
(source PE, destination PE, index, round), so every byte has one right value
and a wrong one names its origin. Reports each variant's time per exchange
side by side, the benchmark part.

Covers: `alltoall`, `alltoallmem`, `alltoalls`, `putmem`, `getmem`,
`putmem_signal`, `wait_until_all`, `iput`, `ibput`, `barrier_all`, `fence`,
`team_split_strided`.

### 4.3 `teams` - creation, queries, and collectives on many teams at once

At setup, from a per-job budget under `OSOE_MAX_TEAMS` (say 16 of the
default's 61 creatable), teams of every shape the spec allows:
`split_strided` with random start, stride and size, negative strides, a team
of one, a split of a split; `split_2d` with random `xrange`, giving each PE
a row and a column team; `team_create_ctx` on each. Every query is checked
against the arithmetic definition: `team_n_pes`, `team_my_pe`,
`team_translate_pe` both ways and between two teams, `team_get_config`,
`team_ptr` non-NULL exactly for node-mates. Then rounds in which each PE
alternates a collective on its row team with one on its column team, the
PEs out of phase with each other, so the library carries several teams'
collectives at once: reductions of every operator over a handful of types,
broadcasts from a rotating root, `collect` with ragged lengths including
zero, `fcollect`, `alltoall`, `sum_inscan` and `sum_exscan`, `team_sync`.
Every input is (PE, round)-determined, so every PE can compute every result.
Destroy and re-split happen only within the budget; `--teams-exhaust`, a
separate short mode, creates until the split returns nonzero, checks that
it did on every PE and that the job goes on, and stops. *As built since the
team redesign's step D: no budget - the exercise holds at most six numbers a
PE, four strided teams and a whole-team grid's two - a strided team renewed
every third round, destroyed and split anew, and `--teams-exhaust` destroying
what it made and splitting once more, which must succeed on every PE.*

Covers: `team_split_strided`, `team_split_2d`, `team_destroy`,
`team_create_ctx`, `ctx_get_team`, `team_get_config`, `team_n_pes`,
`team_my_pe`, `team_translate_pe`, `team_ptr`, `team_sync`, `sync_all`,
every `*_reduce`, `broadcast`, `collect`, `fcollect`, `sum_inscan`,
`sum_exscan`.

### 4.4 `mailbox` - point-to-point synchronization (U4)

Every PE has N slots, one per sender, each a payload area and a signal
word. Senders pick receivers at random and `putmem_signal` a message whose
payload is (sender, sequence) mixed through every byte and ends in a trailer
equal to its head; the signal carries the sequence (`SHMEM_SIGNAL_SET`) or a
count (`SHMEM_SIGNAL_ADD`). Receivers take turns at the waiting forms:
`wait_until_any`, `wait_until_some`, `wait_until_all` over the N words,
their `_vector` forms with a threshold per slot, `test_any` / `test_some` /
`test_all` polling with work between polls, `signal_wait_until`,
`signal_fetch`, and `wait_until` with each of the six comparisons in turn. A
message is checked whole on wake - the trailer proves the payload landed
before the signal - and the slot is reset by the receiver. At the end an
alltoall of sent and received counts must match pairwise. Reports messages
a second and, within a node, the wake latency distribution (sender
timestamp in the payload; across nodes clocks differ, so a ping-pong pair
gives the round trip instead).

Covers: `putmem_signal`, `putmem_signal_nbi`, `put_signal` typed, every
`wait_until*` and `test*` form, `signal_wait_until`, `signal_fetch`,
`signal_add`, `signal_set`, `SHMEM_CMP_*`.

### 4.5 `atomics` - histogram, fetching sums, dart throwing (U1, U3, U6)

A table of T words spread over the PEs. Order-independent oracles only:
adds and increments, whose total is known; xor, or and and, applied in
pairs across two passes so the table returns to its start; fetching adds,
whose fetched values sum to a closed form whatever their order (the
differential test's trick); `compare_swap` as bale's `randperm` uses it,
every PE claiming random empty slots until it holds K, after which every
slot is claimed once and the total is N × K; `swap` passing tokens whose
sum is conserved; `atomic_fetch` and `atomic_set` on float and double. The
nonblocking fetching forms in batches closed by `quiet`. Every type of
Tables 6 to 8 through the C11 generics, driven by one macro list, so every
typed routine is in the binary and called. Then the same on a private
context and a serialized one. Reports updates a second, remote and local.

Covers: every `atomic_*` and `atomic_*_nbi`, `ctx_` forms, `ctx_create`,
`ctx_destroy`, `ctx_quiet`, `SHMEM_CTX_PRIVATE`, `SHMEM_CTX_SERIALIZED`.

### 4.6 `rma` - ordering, strides, and many in flight (U2)

The halo idiom on a ring: a bulk put of round k's pattern to the right
neighbour, `shmem_fence`, an add to the neighbour's flag; the neighbour
`wait_until(flag, GE, k)` and checks the whole buffer, so a put overtaking
its fence on any path, on-node or across, is caught. Sizes from 8 B to
1 MiB. Then `iput` and `iget` with strides, `ibput` and `ibget` with block
sizes and strides, sized `put32/64/128`, `p` and `g`; `get_nbi` with many
in flight then one `quiet`, and `pe_quiet` per target; `put_nbi` bursts;
`shmem_ptr` loads and stores with a `fence` (U8) toward node-mates. Every
transfer's content is (source, target, offset, round)-determined and read
back.

Covers: `putmem`, `getmem`, typed `put`/`get`, `p`, `g`, `iput`, `iget`,
`ibput`, `ibget`, `put_nbi`, `get_nbi`, `fence`, `quiet`, `pe_quiet`,
`shmem_ptr`, `wait_until`.

### 4.7 `threads` - `SHMEM_THREAD_MULTIPLE` (U6)

T application threads a PE (default 4, under `OSOE_MAX_THREADS`), each
with a private context, a serialized one, and a share of the default
context and of a team context, running slices of `atomics` and `mailbox`
on each in turn; `ctx_fence` and `ctx_quiet` per context; `ctx_pe_quiet`;
`ctx_session_start` and `_stop` with `SHMEM_CTX_SESSION_BATCH` and
`SHMEM_CTX_SESSION_TOTAL_OPS` around each burst; `shmem_query_thread`
checked. Oracles as in the parent exercises, summed across threads. This is
the exercise to leave running under `OSOE_CHECK_QUIET=1
OSOE_TEST_PERTURB=1` in a debug build.

Each thread's slice on each context includes one blocking get of 20 KiB -
three pieces at the tests' landing zone of 8 KiB - from an array the next PE
seeded once, into a buffer of the thread's own that is poisoned first and
checked word by word, last word first. A blocking get's pieces go out
together, and any thread of the PE that finds no room for its own copies a
landed one out into the waiting thread's buffer: the library's newest
protocol, which `shmem.multiple` alone held until the exercise had this get
(round nine, C9-4: a get returning before a piece it counted was copied ran
`mix` 60 s clean).

After its four contexts, each thread's slice ends with a fifth phase,
`collectives`, which is not a context (T25, round six's correctness H1): the
thread splits its own copy of the exercise's team, made at setup, into a team
of a shape of its own - all the PEs, all but the first, the even ones, the
order reversed, turning with the slice and the thread - and on it runs a sum,
an inclusive scan, a broadcast from a turning root and an exclusive scan; then,
on the copy, a large sum and a large inclusive and exclusive scan - every
thread's copy is the same team, so that every slice has scans of one tree in
flight on several threads at once, which the teams' differing shapes leave to
chance; on the team a second sum; and destroys the team. The sizes run from 64
bytes to 40,000, either side of the library's class line and up to three
pieces, each stage's words its own, every result poisoned first and checked
against the sum of the members' words; the plant `threads-collective` has
thread 0 of PE 1 change a bit of the first result of each kind it checks. Until
the phase, no exercise had two threads of a PE lead collectives at once, so the
stress tester could not see a lane shared by threads - the library's `own_lane`
returning 0, or the scans' lane bound to 0, passed `mix`, `collectives` and
`teams` - which the library's own `shmem.lanes.threads` is the test of (the
phase is its counterpart here, among the other contexts' puts, gets and
atomics, and under the two test hooks). Each thread holds two team numbers, its
copy and its team, so `--threads` is refused where `2T + 4` passes
`OSOE_MAX_TEAMS`.

Covers: `init_thread`, `query_thread`, `team_create_ctx`, `ctx_create`
with each option, `ctx_session_*`, every `ctx_` RMA and AMO form, `getmem`
of several pieces from several threads, `team_split_strided` and
`team_destroy` from several threads at once, and `sum_reduce`, `sum_inscan`,
`sum_exscan` and `broadcastmem` small and large from several threads, each on
a team of its own.

### 4.8 `locks`, `memory`, `collectives`

`locks`: L distributed locks; each PE loops choosing one, `set_lock` or a
`test_lock` spin, a get, increment and put of the protected count at the
lock's home - deliberately not atomic, since the lock is what protects it -
`clear_lock`. Oracle: each count equals its acquisitions. Reports
acquisitions a second and the longest wait.

`memory`: a seeded, collective sequence of `malloc`, `calloc`, `align`,
`realloc`, `free`, `malloc_with_hints` with both hints, filling each block
and checking it after every realloc, `addr_accessible` on each, and an
fcollect of each block's address to check it is the same on every PE, up to
the budget and back down, many times.

`collectives`: the sweep `osoe-bench` does not do: every collective at
sizes from 8 B to 1 MiB, on the world and on a strided team, median and
fastest, each result verified. The benchmark half of `teams`.

### 4.9 `mix` - the stress (phase 4)

The world split into K teams by the seed; each team runs one exercise for a
slice, the teams out of step; slices rotate the exercises through the
teams; some slices put `sort` or `alltoall` on the world while the others
pause. Each slice verifies. This is where a collective on one team meets an
AMO storm into its members from another, a team is destroyed beside one in
flight, and `parallel_print` runs on a team whose members are busy
elsewhere. `--seconds` makes it a soak; the schedule prints in the header
and repeats from the seed.

A team is the PEs k, k+K, k+2K and so on, so what a team period moves between
node-mates - stores through `shmem_ptr`, puts to a PE of the node - depends
on the map: on a block map the PEs K apart share a node only while K is below
the PEs a node has, and at the default K a team there holds none, so only the
world's periods meet node-mates (the release review's pacer paragraph; T27).
`--mix-teams 1` makes the team the whole job, whose every period does.

### 4.10 Optional: the deprecated forms

Off by default, for coverage: the active-set collectives with `pSync` and
`pWrk` on partial sets and on the whole job, `shmem_barrier` and
`shmem_sync` with a stride, the `_to_all` reductions, `broadcast32/64`,
`collect32/64`, `fcollect32/64`, `alltoall32/64`, `alltoalls32/64`, the
deprecated atomic names, `shmem_wait`, `shmalloc` and `start_pes` in a
program of their own. Not 1.6's normative surface, but what the corpus
calls, and they run today.

## 5. Phases

Each phase is one brief, `docs/prompts/task-stress-phaseN.md`, and one
session; merges when verified as every slice does (four presets, tier 3
where it applies, the plants, a Pi run), and ends with the decisions it
raises. Something runs at the end of phase 1.

**Phase 1 - running** (`task-stress-phase1.md`). `tools/pacer/`: the
driver (arguments, header, checkpoints, failure accounting, failure lines
through `parallel_print`, `--stop-on-failure`, summary, exit status),
`parallel_print` (the one-collective route, PE labels with team names),
`sort` in both modes with both exchanges, `alltoall` with its seven ways on
the world and on the `even` team, `--plant` with four defects, the `print`
exercise. The smoke, plant and print tests over the four poles in every
preset. The runbook's §6 and a first 4 × 4 run. *Author decides after:* the
smoke tests' place, seen with their real cost; the label format seen in
use; whether the sort's rates warrant a line in `docs/pi-cluster-results.md`.

**Phase 2 - the newest code.** `threads`, `mailbox`, `teams` with
`--teams-exhaust`; the two test hooks on in the debug and asan smoke runs.
An hour of `threads` in a debug build with the hooks on, on the VM and on
the Pi. *Author decides after:* what to do about per-job team numbers if
the exercise makes the cost vivid (*decided 2026-10-02: the team redesign,
built as step D on 2026-10-03*); whether the open oracle firing reproduced,
and what it said.

**Phase 3 - the rest of the surface.** `atomics` over every type, `rma`,
`locks`, `memory`, `collectives`; the generated interposition shim
(`shmem_X` counting and forwarding to `pshmem_X`, produced by a script from
`pshmem.h`, linked only when asked) and its report: routines called, of
those declared, and the list not called, so "fully exercised" is a number.
Mechanical parts - the type lists, the shim generator, the ctest lines - go
to subagents. *Author decides after:* which uncalled routines are worth an
exercise and which are accepted (the cache routines are no-ops by design).

**Phase 4 - stress proper.** `mix` and the soak; the library's counters in
the checkpoint line; node-folded labels; the second `parallel_print` route
and its differential; the deprecated forms; a Pi soak of several hours,
recorded. *Author decides after:* whether `osoe-pacer` is installed with
`oshcc` and `osoe-stat` as a cluster check for a stranger (the release
point's item 5), and whether a soak joins the release checklist.

## 6. Coverage, by family

What each family of the 1.6 API is exercised by, so the claim can be
checked against the header rather than remembered.

| Family (spec section) | Exercise |
|---|---|
| Library setup, queries, `query_thread`, `query_initialized`, info (§9.1) | header, `threads` |
| Memory management, hints, `realloc`, `align` (§9.3) | `memory`, `sort` |
| Teams, splits, queries, `team_ptr`, configs (§9.4) | `teams`, every exercise's sub-team runs |
| Contexts, sessions (§9.5, §9.9) | `atomics`, `threads` |
| RMA blocking, nonblocking, strided, block-strided, sized (§9.6) | `rma`, `alltoall`, `sort` |
| AMOs, all types and operations, nbi forms (§9.7) | `atomics`, `sort` (fetch-add), `locks` |
| Signaling operations, `signal_fetch` (§9.8) | `mailbox`, `alltoall` |
| Collectives, scans, `sync` (§9.10) | `teams`, `collectives`, `sort`, `parallel_print`; from several threads of a PE at once, `threads` |
| Point-to-point synchronization, all forms (§9.11) | `mailbox`, `rma` |
| Memory ordering: `fence`, `quiet`, `pe_quiet` (§9.12) | `rma`, `sort` |
| Distributed locking (§9.13) | `locks` |
| `shmem_ptr`, accessibility (§9.1, §9.6) | `sort`, `rma`, `teams`, `memory` |
| Profiling interface (§10) | the shim |
| Deprecated (Annex F) | §4.10, optional |

## 7. Decided, and what remains

Decided with the author on 2026-09-26 (`docs/decisions/2026-09-26-night.md`):

1. `tools/pacer/osoe-pacer`, not installed until phase 4 decides - installed
   since 2026-09-27-night.
2. World PE numbers in Slurm's bracket syntax; a team's name before the
   bracket on any team but the world; node folding later, as an option.
3. `parallel_print` stays in the tool.
4. The phase order above, threads second.
5. The memory default of half the heap, and `--memory` to go higher, as a
   size or a share, refused with the variable to set when the heap cannot
   hold it.
6. Smoke tests in every preset: decided yes on 2026-09-27-night, after §3's
   explanation and the four phases' costs.
7. The briefs are `task-stress-phaseN.md`.

## 8. Prior art read for this

ISx (`corpus/isx`, trace U5): the reservation-by-fetch-add exchange and
checksum verification the sort follows. bale (`corpus/bale`): `histo`,
`ig`, `randperm` and `toposort` for the AMO oracles, and its conveyors for
the buffered pattern. PRK `Synch_p2p` (trace U4) for put-with-signal
pipelines and PRK `Transpose` for the strided alltoall. SOS's `mt_a2a`,
`mt_contention` and `mt_membar` for the threaded shapes, `many-ctx` and
`shmem_team_context` for contexts on teams. `pdsh`'s `dshbak -c` and
Slurm's hostlist syntax for the coalesced output. The existing `osoe-bench`,
`test-differential.c` (order-independent oracles) and `test-multiple.c`
(the threaded shape and the two hooks) in this tree.
