# 2026-09-27, afternoon: the stress tester's third phase

The session worked `docs/prompts/task-stress-phase3.md` on the branch
`task-stress-phase3`, begun from phase 2's tip while phase 2's last
measurements ran, in the brief's order: `atomics`, `rma`, `locks` and
`memory`, `collectives`, the shim, then the tests, the runbook, the
measurements and this record. The author had said, leaving for the
afternoon: "you may also continue onto phase 3 and then phase 4. at decision
points, think carefully, and make a good choice" - so every choice below was
made without them, and each says how to undo it.

## What was built

| File | What it is |
|---|---|
| `atomics.c` | every atomic operation of Tables 6 to 8 on every type, 71 cases in six families, on three contexts, blocking and nonblocking, checked by oracles that do not depend on order |
| `rma.c` | a fenced halo from 8 B to 1 MiB raising the next PE's flag three ways; every transfer class over Table 5's 24 types and the five sized forms, 140 cases; `shmem_ptr` stores and loads among node-mates |
| `locks.c` | eight locks guarding counts that are not atomic, `clear_lock` alone completing the section's puts |
| `memory.c` | the heap's routines in a collective, seeded sequence up to three quarters of the budget and back |
| `collectives.c` | every typed collective - 314 cases - and a timing sweep of each family from 8 B to 1 MiB on the world and on the even PEs |
| `generate-shim.py` | writes `osoe-pacer-counted`'s shim from the preprocessor's expansion of `pshmem.h`: a counting definition for each of the 1,794 routines declared |

and in the driver: the five exercises in the default set; `--help` lists the
plants from the table rather than by hand; `--coverage FILE` in the counted
build. Commits: `4527008` (atomics), `22da866` (rma), `ee2b9ec` (locks,
memory), `168c712` (collectives), `66c7f61` (twelve more plants), `b3f9822`
(`--coverage`, runbook), `4535eab` (the counted build), `b1732dd` (pronouns),
`55446d4` (names), `075b4da` (`rma`'s spellings, item 8), and this record.
The branch was rebased onto phase 2's merge before it merged; the hashes are
the merged ones, and where a run below names an earlier hash, it says what
separates that tree from the merged one.

## 1. Every check has a plant: thirty-five more

| Exercise | Plants, each PE 1's | What they fire |
|---|---|---|
| `atomics` (9) | `atomics-add`, `-fetch`, `-fetch-count`, `-claim`, `-attempts`, `-swap`, `-set`, `-bitwise`, `-fetched-bit` | a slot's total; the fetched values' sum and hash; a slot's final count; a claim's token and the table's count; the claim budget; the tokens' sum and hash; a set value, at both ends; the state after a pass; a fetched bit |
| `rma` (9) | `rma-halo`, `-flag`, `-put`, `-gap`, `-get`, `-get-gap`, `-single`, `-ptr`, `-ptr-null` | the halo's bytes; its flag; an inbox's elements; its gaps; a get's elements (and a node-mate's loads, reading the same source); a get's gaps; `p` and `g`'s values; the words stored through `shmem_ptr`; a NULL pointer |
| `locks` (2) | `locks-count`, `-holder` | a count against its acquisitions; the holder word |
| `memory` (8) | `memory-free`, `-realloc`, `-refused`, `-granted`, `-calloc`, `-align`, `-accessible`, `-agree` | a block at free; the kept bytes at realloc; a refused realloc's block; a grant no heap makes; calloc's zeros; an alignment; `addr_accessible`; the PEs' agreement |
| `collectives` (7) | `collectives-reduce`, `-scan`, `-data`, `-gap`, `-status`, `-timed`, `-timed-data` | a reduction forced past what it absorbs; a scan's prefix; a gather's bytes; alltoalls' gaps; a nonzero status; the timing sweep's values and bytes |

Every test requires each line its plant must print (item 2 of the morning),
on 0,0,1,1; a plant fires on the first case of its kind PE 1 takes part in,
so the round in each pattern is fixed by the case order and the seed.

The first pass left ten checks no plant reached; the mutation list, written
before any mutation ran, found them, since a check whose removal no test
notices is the brief's first stop. The ten: a fetch-add slot's final count,
the claim budget, the halo's flag, `p` and `g`'s values, a NULL `shmem_ptr`,
a block damaged before its `realloc` (the `free` check had been catching the
one plant for both), a `realloc` refused, one granted, a collective's status,
and both of the timing sweep's checks. Two needed the code to change, not
only a plant:

- **A NULL `shmem_ptr` no longer skips the node-mate's flag.** The first
  version reported it and went on, and the node-mate would have waited for
  that flag for ever; now the stores are skipped and the flag raised, so the
  node-mate's own check says what is missing.
- **A `realloc` no heap can grant**, once a run: `SIZE_MAX / 4` bytes, which
  must come back NULL, alike on every PE - the round's hash says so - with the
  block untouched. The refused path had been reachable only when the heap was
  full, which no run arranges. osoe refuses it, as §9.3.3 asks.

## 2. `atomics`, where the brief left a choice (Claude)

- **A generic cannot reach a typedef's routine.** The brief asked for every
  typed routine called through the C11 generics. `_Generic` selects on the
  type, and on LP64 `int32_t` is `int`, `int64_t` and `ptrdiff_t` are `long`,
  `uint64_t` and `size_t` are `unsigned long`: `shmem_atomic_add` on an
  `int32_t *` calls `shmem_int_atomic_add`, and `shmem_int32_atomic_add` is
  reached only by its name. So each operation alternates between the generic
  and the type's own name, and both are called for every type. *Undo*: names
  only, or generics only, in `atomics.c`'s wrappers.
- **Six families, one a round**, each over its table's types - add, fetch_add,
  claim (compare_swap), swap, set_fetch (Table 7, float and double among
  them), bitwise (Table 8) - 71 cases; round r runs case r mod 71, on the
  default, a private or a serialized context by r mod 3, blocking or
  nonblocking by parity: 426 rounds run every combination once, and the quick
  run's 71 every case once.
- **What a slot must hold is replayed, not exchanged.** Every PE's stream is
  seeded by (PE, round, family), so each PE replays every PE's stream to know
  what it was sent: O(N) work a round and no communication. *Undo*: an
  alltoall of what each PE sent where, O(N) memory instead.
- **Fetched values by closed forms**: fetch-adds of one hand out a
  permutation of 0 to k - 1 from a slot's start, whatever the order; each
  issuer adds what it fetched, and a hash of it, into two 64-bit words at the
  slot's PE, which then hold k(k - 1)/2 and a hash sum a PE computes. This
  checks what `threads` discards (phase 2's oracle review). The two words are
  themselves added by `shmem_uint64_atomic_add`: a broken 64-bit add fails
  this check too, which is a failure either way.
- **Bits predicted where one PE owns them**: in the bitwise family each PE
  owns bit (PE mod width), so a fetching form's return has that bit known
  exactly when N is no wider than the type; beyond, only the state after each
  pass is checked.
- **Claims at half load**: sixteen claims a PE into a table of thirty-two a PE
  (bale's randperm), so an attempt succeeds at least half the time, capped at
  128 attempts - a PE that cannot claim says so rather than spinning.

## 3. `rma`, where the brief left a choice (Claude)

- **Bytes, not values**, for every class but `p` and `g`: RMA moves bytes, so
  the check is every byte of the receiving region against a (source, target,
  index, round) pattern, the canary where nothing lands, the bytes past the
  end among them. `p` and `g` carry values, compared as values: a `long
  double`'s padding bytes, six of sixteen on x86, are nobody's.
- **One transfer case a round** of 140 - four classes over the 29 kinds (24
  types, 5 sizes) and `p`/`g` over the 24 types - on the default context in
  even passes and a private one in odd, the put side and the get side taking
  opposite spellings (which, item 8); sized forms have no generic.
- **The halo alternates everything that orders**: `putmem`, `putmem_nbi` or a
  typed put; the flag raised by atomic add, `shmem_signal_add` or
  `shmem_signal_set` (the standalone signal routines nothing called before);
  waited for by `wait_until` or `signal_wait_until`; on either context.
- **`shmem_ptr` stores are fenced before an atomic flag**, the U8 pattern, and
  the node-mate checks once each flag is up; loads read each node-mate's
  source as its owner filled it.

## 4. `locks` and `memory` (Claude)

- **`clear_lock` alone completes the section's puts**: the count's put is not
  quieted before the release, since the specification says the release
  completes it (§9.13), and that is what is checked. The holder word's put is
  quieted, so that reading it back in the section proves the section is this
  PE's alone.
- **Symmetric is checked without assuming one address**: every allocation's
  result, as an offset from a block made at setup, goes into a hash whose
  largest and smallest over the team must agree - so the check holds for an
  implementation, or osoe's `OSOE_HEAP_SHIFT` test mode, where a symmetric
  object sits at a different address on each PE.
- **`--quick` caps `memory`'s peak at 8 MiB**, so the smoke test's filling and
  checking stays short.

## 5. `collectives` (Claude)

- **Three typed cases and one timing case a round**: the typed cases cover
  the 314 typed routines in 105 rounds - the quick run - at one to eight
  elements, the timing case one family at one size on one team, five calls.
- **Closed forms from the member list**: a sum is the members' base plus
  their offsets, a product has one factor of 3 among ones, a maximum or
  minimum is the base plus the extreme offset, a bitwise result is the
  members' bits, a scan's prefix is a prefix of the offsets, and the data
  collectives move pattern bytes. Every check is O(N) a PE.
- **The timing lines are the slowest PE's median and the fastest call
  anywhere**, a line a (team, size) for the data families and one for
  `team_sync` and `barrier_all`.
- **Every call is preceded by a sync of its team**: a dest must be ready on
  every PE before any calls (§9.10), which osoe's `alltoall` says in as many
  words; it also gives the timing a common start.

## 6. The shim (Claude, with a subagent)

- **A subagent wrote the generator** from a stated specification, as CLAUDE.md
  and the brief both ask for mechanical work, in its own files and with no
  edit to any file this session was changing; it was then fitted by hand. It
  reads the preprocessed `pshmem.h`, since the declarations are X-macro
  expansions, and found 1,794 routines, one - `shmem_pcontrol` - variadic,
  whose wrapper counts the call and forwards only `level`: C cannot forward an
  unknown argument list, and the specification reserves those arguments.
- **Each definition is `(shmem_NAME)(...)`**, the name in parentheses, so that
  the C11 generics' function-like macros of the same name - `shmem_put`,
  `shmem_wait_until` - cannot expand in it.
- **Annex F's seven routines without a prefix** - `start_pes`, `_my_pe`,
  `_num_pes`, `shmalloc`, `shfree`, `shrealloc`, `shmemalign` - are wrapped
  under those names, which are what a program calls; the subagent's first
  version defined `shmem_start_pes`, which nothing calls, so the seven would
  have read "never called" for ever. With it, the generated file compiles
  under the tree's full warnings, nothing relaxed.
- **The report is collective and counts nothing of its own**: at finalize the
  counts are summed over the world with `pshmem_` routines, and world PE 0
  writes `shim called=X declared=Y`, each count, and the list never called,
  to the file `--coverage` names - an option only the counted build takes.
- **A quick run understates what the exercises reach**: a case runs on one
  context and in one of the blocking and nonblocking forms per pass over the
  cases, so the Pi's coverage run takes whole cycles: `--rounds 426`,
  `atomics`' cycle, at first, and 560, `rma`'s, since item 8.

## 7. Small things

- **`--help` lists the plants from the driver's own table**, with the
  exercise each needs: the hand-kept list had fallen a plant behind once.
- **A pronoun**: the tests' pending block said the author would decide "once
  he has seen" the cost; nothing gives the author's pronouns, so it now says
  "they".

## 8. What the coverage run found in the tool: `rma`'s spellings (Claude)

The counted build's first full-cycle run on the Pi listed `shmem_int16_put`,
`shmem_int32_get` and their `ctx` forms among the routines never called -
typed transfers `rma` was written to reach. A case's spelling flipped every
two rounds, `(round / 2) % 2`, and the 140 cases are a multiple of four, so
every pass gave each case the spelling it had in the first: half the typed
cases only ever put by the generic and got by the type's name, the other half
the other way, and half of Table 5's typed transfer names were never called
on either context. The exercise said, and its record said, that both
spellings run for every type. Nothing checked that they did, and no test
could have: each call that ran was checked, and the ones missing leave no
trace but in a count.

So the spelling is now the case's and the pass's, `(case / 2 + pass / 2) % 2`,
and the context the pass's: four passes, 560 rounds, run every case in both
spellings on both contexts. The coverage run takes 560 rounds (runbook §6).
The collectives' spelling was already the pass's - 314 cases, an odd number
of pairs - and `atomics` reaches every typed atomic: the shim's list has none.
*Undo*: the one line in `transfer`.

The lesson is the shim's reason for being: a check can only fail on what
runs, and whether something runs is a count, not a check.

## The oracles reviewed: what wrong output would each still accept?

| Check | Accepts, still |
|---|---|
| atomics add, fetch_add | two errors that cancel within a slot in one round |
| atomics fetched values | a permutation of the right values in a wrong order - which is right |
| atomics claim | nothing a claim or a count would show |
| atomics swap | a token swapped for one of equal value |
| atomics bitwise | where N is wider than the type, which PE's operation moved a shared bit |
| rma | premature completion that the data outruns, as the rows and tables of phase 2 |
| locks | a mutual exclusion that fails only when no count is lost and no holder word is overwritten between the put and the read |
| memory | an allocator that overlaps two blocks whose patterns happen to agree where they overlap |
| collectives | a result right for the inputs but computed from a stale dest another collective left - excluded by the syncs |

## Verification

Of `762bf8a` - the merged tree less item 8 - frozen in its own worktree while
work went on, and on the VM, one run at a time under its lock
(`build/scratch/pacer/verify4.sh`, `mutate3.sh`):

- **The four presets whole**: debug 539 tests, release 561, asan 538, tsan
  366, every one passed.
- **The pacer's 69 tests thirty times over under eight busy loops**, release:
  every repetition passed.
- **Every check removed in turn**, 35 mutations (`apply3.py`), each built and
  the pacer's tests run: the control passed, and every mutation was killed,
  by plant tests of its own exercise alone - the ones whose lines the removed
  check prints. Some by more than one: `rma`'s byte comparison by the four
  plants that print through it, the claim table's count by `atomics-claim`
  and `atomics-attempts`, since a PE that stops claiming leaves the count
  short too, which that plant's test requires.

Of `075b4da`, item 8's one line, which moves no check: the pacer's 69 tests
on the four presets, all passed; on the Pi, a quick `rma` and the counted
build's 560 rounds of every exercise, no failure. The pacer's tests take 22 s
of test time in release (5.8 s wall at `-j4`), 62 s in debug, 91 s under
asan, and under tsan, where only the one-node map runs, 4 s for two tests.

## The measurements

The Pi at 4 × 4, release, from `~/osoe-pacer-phase3` at `23f0667` - the
counted build's commit before the rebase, which differs from the merged tree
by names (`55446d4`) and item 8 - ten minutes an exercise, each node's
retransmission timeouts read before and after every run
(`docs/design/evidence/pi-cluster/2026-09-27/pacer/phase3/`):

| Exercise | Ten minutes | Failures |
|---|---|---|
| `atomics` | 103.0 M updates, 171,700 a second - at the last checkpoint 43,100 to node-mates and 129,300 across nodes | 0 |
| `rma` | 4.53 M transfers, 7,540 a second, 694 MiB/s with the halo | 0 |
| `locks` | 4.44 M acquisitions, 7,410 a second; the longest wait at any checkpoint 80 ms | 0 |
| `memory` | 1.68 M heap operations, 2,770 a second, 2.2 GiB/s filled and checked | 0 |
| `collectives` | 184,000 calls, 1,913 rounds of the sweep | 0 |

The quick run of the nine default exercises passed in 6.6 s. No run added a
retransmission timeout but these: the collectives' sweep added four (one on
pi-001, three on pi-002), and each of the two coverage runs two, where the 1
MiB alltoall puts 48 MiB a node through each link at once - the same incast
the second session's F1 recorded, and nothing a check saw.

**The collectives' sweep**, the slowest PE's median and the fastest call, at
1 MiB a PE on the world (a block of 1 MiB to each PE for `alltoall`):

| | median | fastest | the bound |
|---|---|---|---|
| broadcast | 28.0 ms | 27.3 | 27 ms: the root's three copies at k = 4 over one link |
| reduce | 29.7 | 29.1 | the same tree |
| fcollect | 148.5 | 118.2 | 107 ms: twelve MiB into each node at line rate |
| alltoall | 575 | 519 | 429 ms: 48 MiB each way a node |
| scan (inclusive) | 134 | 131 | - |

and on the even team, 8 PEs, the same families at 27.6, 28.7, 62.4, 163 and
136 ms. The bandwidth-bound families sit where the second session's
measurements put them (`docs/pi-cluster-results.md`: broadcast 27.2 ms at k =
4, fcollect 120 ms). The scan does not: 134 ms on 16 PEs and 136 on 8.
`internode-generic.c` moves a scan larger than its 4 KiB slot "a piece at a
time, each piece up and down before the next", so 1 MiB is 256 trips up and
down the tree, about half a millisecond each, and the team's size barely
matters. At 8 bytes every family costs 130 to 530 µs, a team sync 170.

## For the author

1. **The shim's list of routines never called** (the brief's own question).
   Of the 1,794 routines declared, 560 rounds of every exercise called 1,404
   (`075b4da` on the Pi; `23f0667`'s 426 rounds, before item 8, called
   1,306). The 390 left:
   - **104 deprecated forms** - the `_to_all` reductions (44), the deprecated
     atomic names (30), the active-set collectives (12), Annex F's names
     without a prefix (7), the cache routines (6), `shmem_wait` (5) - which
     are phase 4's already (its brief, item 5);
   - **117 `put_signal` forms** beyond `putmem` and `uint64_t`, typed and
     sized, blocking and not, on a context and off one;
   - **160 typed wait and test forms** beyond `uint64_t`;
   - **nine singles**: `shmem_init` (the tool initializes with
     `init_thread`), `shmem_finalize` (called after the report is written),
     `shmem_global_exit` (called by no clean run), `shmem_pcontrol`,
     `shmem_query_initialized`, `shmem_pe_accessible`, `shmem_sync_all`,
     `shmem_getmem` and `shmem_alltoallsmem`.

   The phase 4 brief takes the two families and the singles an exercise
   already has a place for (its item 5): each typed wrapper could be wired
   to the wrong width, and only a call finds that. The author may strike
   them; nothing else in phase 4 depends on them.
2. **The smoke tests' place** is still open (the morning's item 1); the block
   is 69 tests now, 22 s of test time in release, 91 under asan.
3. **The scan is latency-bound at large sizes** (the measurements): 134 ms
   for 1 MiB where a reduce takes 30, the same on 8 PEs as on 16, since its
   pieces go one at a time. Pipelining them as broadcast and reduce pipeline
   theirs would bring it near reduce's time; whether that is worth doing
   before the release is the project review's.

## Next

Phase 4, `docs/prompts/task-stress-phase4.md`.
