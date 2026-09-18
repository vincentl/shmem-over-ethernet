# How a writer decides whether to wake a waiter

Design note for §4.9 (point-to-point synchronization), §4.10 (the boundary) and
§4.11 item 3 (watch records under `MULTIPLE`). It searches the option space
behind one line of `shmem/src/watch.c` — `block_of` — and recommends a
replacement. Nothing here is implemented; the deliverable is the decision and
the evidence for it.

---

## 1. The question

A PE blocks in `shmem_wait_until` on its own symmetric variable. Some other PE
— or, later, a progress thread applying a remote operation from a *different
process*, or a NIC nobody can see — writes that memory. The writer must decide,
cheaply, whether to wake the waiter.

`osoe_tp_notify` runs after **every** intranode put and rmw. The 8-byte put
itself is a `memcpy` that costs nothing measurable; the filter costs 0.63 ns;
a wake costs 150–540 ns. So the filter is the whole cost of notification when
it says no, and is irrelevant when it says yes. The design freedom is entirely
in *how precise a filter costs what*.

**This note deliberately does not treat "the published watch is one 64-bit word,
atomically loadable" as a constraint.** §4.11 item 3 already replaces the one
slot with a set of records and a per-PE bitmap, and invariant 31 already makes
the writer walk every overlapping record. Once the writer walks a list, "one
atomic load" has stopped governing. Every option below is judged against the
final design — several threads per PE, three regions, a cross-process writer,
and a polling backend — not against what is cheap in the current slice.

---

## 2. What any solution must satisfy

Stated in the final design's terms, because that is what the answer must hold
for.

1. **Never say no to a write that touched a watched range.** That loses a wake.
   §4.10 promises notify is *exact for rmw*, and the safety-sweep deadline turns
   a lost wake into a latency spike rather than a hang (§3.10) — but a filter
   that relies on the sweep has given up the property the section is for.
2. **May freely say yes to a write that did not.** Spurious wakes are legal;
   the waiter re-checks. A filter is therefore conservative by construction and
   the only question is its cost.
3. **The three §4.9/§4.11 ordering rules are not negotiable and do not depend on
   the predicate.** Publish before the first predicate check (invariant 13);
   load `seq` before the variables; the writer updates the variable before it
   loads the watch. Both publication stores are `seq_cst`, because this is a
   two-sided protocol (§3.16, invariant 24).
4. **Every overlapping record is woken, not the first** (invariant 31).
5. **Offsets, never addresses** (invariant 20). A PE's heap is at a different
   address in each sibling process, so anything published must be in
   `(region, offset)` space.
6. **Three regions.** `HEAP`, `INTERNAL` for library scratch and collective
   control words (§5.8), and `STATICS` with §4.3 phase 1.5. Watching statics is
   *certain*, not speculative: spec Example 27, the canonical put-with-signal
   example, waits on a `static uint64_t`. `watch.c` today calls `osoe_fatal`
   for any region but `HEAP`, and `osoe_tp_notify` silently returns for them.
7. **Several waiting threads per PE**, each with its own record and futex word
   (§4.11 item 3), on up to ~128–256 PEs per node.
8. **The writer may be in another process.** Anything the filter touches lives
   in the node segment and is read through a shared mapping. Anything whose
   correctness depends on process-private state is not merely slow here, it is
   silently wrong.
9. **A backend may have no writer at all.** A RoCE backend sees no target-side
   event (`docs/reviews/roce-transport.md` §4.7); its per-node wake poller keeps
   a *shadow copy of the watched bytes* and compares. Whatever is published must
   therefore tell a poller **which bytes to shadow**, not only whether a given
   write matters.
10. **ISO C11 `<stdatomic.h>` only**, four named orders, no intrinsics, no
    assembly, no `__atomic_*` (§3.16). Nothing links but libc and liburing
    (invariant 15).

Requirement 9 is the one that is easy to miss and it eliminates a whole family
of answers on its own.

---

## 3. What is wrong today, honestly

`block_of` rounds the watched range out to the smallest **aligned power-of-two
block** containing it. Three separate problems, of very different sizes.

**(a) Two latent defects, confirmed.** `block_of` returns `64` for its `k >= 63`
saturation case, but `k + 1` is stored in six bits and `64 & 63 == 0`;
`block_covers` then computes `k = 0u - 1u` and shifts by 4294967295 — undefined
behaviour, confirmed under UBSan. Separately, `block_of`'s loop does not
terminate when the range's ends differ in bit 63. Both are unreachable with a
bounds-checked heap today. They are the strongest argument for a change, because
they are a *class* — a variable shift derived from a packed field, and a loop
whose termination depends on the inputs — and the replacement must not have an
equivalent.

**(b) Inflation on array waits.** Measured on this machine (do not redo; these
are the driving numbers):

| Watched array in the default 256 MiB heap | Worst block | Mean block | Placements over 64 KiB |
|---|---|---|---|
| 8 B (a scalar ivar) | 8 B | 8 B | 0% |
| 128 B | whole heap | 2,368 B | 0.17% |
| 8 KiB (1024 `uint64` ivars) | whole heap | 122 KiB | 12.5% |

Driven through the real allocator (`shmem/src/alloc.c`, first fit, 16-byte
alignment) over 2000 churn-then-allocate trials, an 8 KiB wait array publishes
over 64 KiB in **7.3% of runs**.

**(c) No room for a region.** `block_of` spends all 64 bits: 58 for the block
index and 6 for the shift. §4.11 item 3's record must carry a region, and §5.8
and §4.3 make `INTERNAL` and `STATICS` waits certain. The encoding cannot be
extended without redesigning it.

**And now the part that cuts against changing anything.** For a **scalar** wait
— an 8-byte ivar at an 8-aligned offset, or a 4-byte ivar at a 4-aligned offset
— `block_of` picks `k = 3` (or `2`) and the block is **exactly the ivar**. It is
not approximately right; it is right. Measured spurious coverage: 0.000%, for
both write models below. And the usage study finds the multiple-element wait
forms have **no public application users at all**
(`docs/reviews/evidence/q2/q2-usage.md` §1: "`wait_until_{all,any,some}
[_vector]` … none"), while single-variable `wait_until` is HOT in the NVSHMEM
AI stack and warm in PRK, ISx, hclib and bale.

So problem (b) is real, measured, and in code nobody calls. The case for a
change rests on (a) and (c), and on convergence with §4.11 — not on speed.

---

## 4. Measured evidence

All measurements in the Lima VM (`shmem`, Debian 13, kernel 6.12, aarch64,
Apple Silicon host, ARMv8.5 with LSE and LSE2, GCC 14.2.0, 4 vCPU), `-O2
-std=c11`. Probes were run from a scratch directory and removed; the model and
its recorded run are in `docs/design/evidence/watch-options/`. Figures labelled
*recalled* are not checked here; figures labelled *arith* are arithmetic on
measured numbers.

### 4.1 Writer-path cost of each filter

8-byte `memcpy` followed by the filter, uncontended, filter word on its own
line, 20 M iterations, three runs agreeing to ±0.1 ns.

| Writer path | Cost |
|---|---|
| `memcpy` 8 B alone | 0.00 ns |
| **+ one acquire load and today's block test** | **0.63 ns** |
| + one acquire load and a 64-bit signature test (hash, AND) | 0.63 ns |
| **+ one acquire load and a packed exact-interval test** | **0.85 ns** |
| + two acquire loads, `(offset, length)` in two words, exact | 1.05 ns |
| + one `_Atomic unsigned __int128` load (a libatomic call), exact | 1.89 ns |
| + `active` bitmap load, zero waiters | 0.63 ns |
| + `active` bitmap load, 4 waiting records | 5.03 ns |
| + `active` bitmap load, 16 waiting records | 16.31 ns |
| + 64-bit **union** signature, any number of waiters | 0.63 ns |

Two things to read off. A per-record scan costs about **1 ns per waiting
record**, so the §4.11 bitmap is affordable for the handful of threads that are
actually blocked and is not affordable at 64. And a signature is the only
candidate whose per-PE prefilter is O(1) in the number of records, because
signatures *union*.

### 4.2 The cost of saying yes

| Event | Cost |
|---|---|
| `futex_wake`, no waiter, `FUTEX_BITSET_MATCH_ANY` | 177.4 ns |
| `futex_wake`, no waiter, mask `1 << 7` | 177.2 ns |
| `fetch_add` + `futex_wake`, one waiter asleep and re-sleeping | 537.7 ns |

Consistent with the design's existing figures (150.3 ns and 270.0 ns), measured
with a different harness; the ratio to the 0.63 ns filter is the point, and it
is between 240× and 850×.

### 4.3 Spurious coverage fraction

Probability that a filter says yes to an 8-byte write that does **not** touch the
watched range, over 400 random 16-byte-aligned placements in a 256 MiB heap ×
400 probe writes. `docs/design/evidence/watch-options/` does not hold this
script; it was a scratch probe and the numbers are reproduced here in full.

Two write models, because they disagree:
- **uniform** — the written word is anywhere in the heap.
- **adjacent** — the written word is within 32 KiB of the watched range. This is
  the U2/U4 shape: a payload put into a buffer beside the flag, then an rmw on
  the flag.

| Filter | Range | uniform mean | uniform p99 | adjacent mean | adjacent p99 |
|---|---|---|---|---|---|
| block (today) | 8 B | 0.000% | 0.000% | 0.000% | 0.000% |
| exact interval | 8 B | 0.000% | 0.000% | 0.000% | 0.000% |
| sig 64/1 grain 16 | 8 B | 1.548% | 3.000% | 1.593% | 3.500% |
| sig 64/2 grain 16 | 8 B | 0.106% | 0.500% | 0.092% | 0.750% |
| sig 64/3 grain 16 | 8 B | 0.006% | 0.250% | 0.025% | 0.250% |
| sig 128/3 grain 16 | 8 B | 0.001% | 0.000% | 0.014% | 0.250% |
| block (today) | 128 B | 0.001% | 0.000% | 1.238% | 18.250% |
| exact interval | 128 B | 0.000% | 0.000% | 0.000% | 0.000% |
| sig 64/2 grain 16 | 128 B | 4.926% | 8.500% | 4.940% | 8.500% |
| sig 128/4 grain 16 | 128 B | 0.269% | 1.000% | 0.242% | 1.000% |
| block (today) | 8 KiB | 0.024% | 0.500% | **35.718%** | **90.250%** |
| exact interval | 8 KiB | 0.000% | 0.000% | 0.000% | 0.000% |
| sig 64/1 grain 16 fixed | 8 KiB | 99.977% | 100.000% | 87.506% | 92.000% |
| sig 64/4 grain 16 fixed | 8 KiB | 99.996% | 100.000% | 87.604% | 91.500% |
| sig 64/4 adaptive grain | 8 KiB | 0.604% | 3.250% | 3.508% | 8.500% |
| sig 128/4 adaptive grain | 8 KiB | 0.057% | 0.500% | 3.199% | 7.250% |

Three results worth naming:

- **Today's block is exact for scalars and catastrophic for arrays with local
  traffic.** 0.000% at 8 B; 35.7% mean and 90.3% p99 at 8 KiB under adjacent
  writes. The *uniform* number for the same case is 0.024%, which is why the
  inflation looks harmless until you model where writes actually land.
- **A fixed-grain signature saturates to "yes" at 8 KiB.** 64 bits over 16-byte
  grains holds a few dozen grains; an 8 KiB range needs 512. This is the
  transactional-memory result (§7) reproduced exactly.
- **An adaptive grain rescues the signature's array case and does not rescue
  its scalar case.** The scalar regression from 0.000% to 0.006%–1.5% is
  irreducible: a hash has collisions and an interval does not.

### 4.4 128-bit atomics: what GCC actually emits

`_Atomic unsigned __int128`, naturally aligned, `atomic_load_explicit(acquire)`,
`atomic_store_explicit(seq_cst)`, `atomic_compare_exchange_strong_explicit
(seq_cst, seq_cst)`. GCC 14.2.0.

| Target | load | store | CAS | `atomic_is_lock_free` | `__atomic_always_lock_free(16,0)` |
|---|---|---|---|---|---|
| aarch64 `-march=armv8-a` | `b __atomic_load_16` | `b __atomic_store_16` | `bl __atomic_compare_exchange_16` | 0 | 0 |
| aarch64 `-march=armv8.2-a+lse` | same | same | same | 0 | 0 |
| aarch64 `-march=armv8.4-a` | same | same | same | 0 | 0 |
| x86-64, no flags | `jmp __atomic_load_16@PLT` | `jmp __atomic_store_16@PLT` | `call __atomic_compare_exchange_16@PLT` | 0 | 0 |
| x86-64 `-mcx16` | **identical** | identical | identical | 0 | 0 |

**Nothing is inlined, on any target, at any `-march`, with or without `-mcx16`.**
`-mcx16` changes the emitted code not at all in GCC 14. Every 128-bit atomic is
an out-of-line call through the PLT into an ifunc. Measured: 1.89 ns against
0.63 ns for a 64-bit acquire load, on the best possible CPU for it.

**Does it link?** No, not without `-latomic`; the link fails with six undefined
references. With `-latomic` the binary gains `NEEDED libatomic.so.1`, which
violates invariant 15. Linking it statically —
`-Wl,--push-state,-Bstatic,-latomic,--pop-state` — works and leaves
`NEEDED: libc.so.6` alone. That is the same category as §3.16's accepted
"outline atomics on ARMv8.0 link statically from `libgcc.a`": a GCC support
library, statically linked, no runtime dependency. So the invariant-15 objection
is soluble, at the price of a link-line rule that has to be right in every
consumer of the library, not just in osoe's own build.

**Is it lock-free?** `atomic_is_lock_free` and `__atomic_is_lock_free(16, p)`
both return **0** on every configuration tested, on both architectures. But the
implementation, read out of `/usr/lib/aarch64-linux-gnu/libatomic.so.1`, *is*
lock-free. The `_16` entry points are ifuncs whose resolvers read
`ID_AA64ISAR0_EL1` and the hwcaps and return one of:

| Selected when | 16-byte load |
|---|---|
| `HWCAP_USCAT` (LSE2) present | `ldp x0, x1, [x0]` + `dmb ishld` — a pure read |
| LSE128 present | LSE128 forms (`swpp`, `caspal`) |
| **neither — ARMv8.0, Cortex-A72** | `ldxp x0, x1, [x5]` / `stxp w4, x0, x1, [x5]` retry loop |

The last row is the Raspberry Pi 4. **On the deployment target, a 16-byte atomic
"load" is a store-exclusive loop.** Confirmed two ways: the ldxp/stxp sequence
is present in the library at the resolver's non-LSE2 target address, and it is
by construction the only ARMv8.0 way to access 16 bytes atomically. On this VM,
which has LSE2, an `atomic_load_explicit` of a `PROT_READ` page succeeds — so
the VM takes the `ldp` path, and the VM cannot reproduce the Pi's path. This is
therefore *read from the disassembly and the resolver*, not executed on A72
hardware.

**What that costs.** A filter word is read by every writer on the node and
written only by its owner. Turning the read into a read-modify-write inverts its
sharing behaviour. Measured, T threads on one shared word:

| T | acquire load | locked `fetch_or(&w, 0)` |
|---|---|---|
| 1 | 0.70 ns | 7.64 ns |
| 2 | 0.29 ns | 17.10 ns |
| 4 | 0.15 ns | 23.90 ns |

The load gets *cheaper* per operation as readers are added — the line stays
shared. The locked operation gets 3× more expensive going from 1 to 4 threads,
and that is on 4 cores. §4.9 contemplates 128–256 local PEs. `fetch_or(&w, 0)`
is a stand-in for `ldxp/stxp`, not the instruction itself; the sharing behaviour
is what is being measured and it is the same.

**The lock-table question.** libatomic *does* carry a lock table — the
aarch64 library's undefined symbols are exactly `memcmp`, `memcpy`,
`pthread_mutex_lock`, `pthread_mutex_unlock`, and its `.bss` is 0x1008 bytes,
a page of mutexes plus a word. It is reached by the **generic**, size-dispatched
`__atomic_load` / `__atomic_store` entry points, for sizes and alignments with
no instruction. It is **not** reached by the fixed-size `_16` entry points on
aarch64. The reasoning in the task prompt is therefore *correct in principle and
does not bite here*: a lock table in the library's own `.bss` is process-private,
so a word read from another process through a shared mapping under a lock-based
implementation is silently wrong — but aligned 16-byte atomics on aarch64 never
take that path. The hazard is real for any *other* width one might be tempted by
(a 24-byte record, an unaligned 16-byte one), and that is worth writing down.

**Does §3.16 permit it as written?** Yes, narrowly, and the rule would still
need amending. `_Atomic unsigned __int128` with `atomic_load_explicit(...,
memory_order_acquire)` is ISO C11 `<stdatomic.h>` with a named ISO order and no
intrinsic, no assembly, no `__atomic_*`. What it is not is *portable* — `__int128`
is a GCC extension type, absent on 32-bit targets — and what it silently
introduces is a libatomic dependency and a call per notify. §3.16's list of
forbidden things does not mention it because the list was written about
*orderings*. If this option were taken, the rule would have to gain a sentence
naming `__int128` and the static libatomic link; it would be an amendment, not
a reading. It is not taken, so the rule stands unchanged.

### 4.5 Kernel-side filtering

Verified on 6.12 aarch64 with the futex2 syscalls (`__NR_futex_wait` 455,
`__NR_futex_wake` 454, `__NR_futex_waitv` 449):

- `futex_wait` with `FUTEX2_SIZE_U64` → `EINVAL`. **Invariant 18 re-verified.**
- `futex_waitv` exists (`EINVAL` for a zero-length wait list, not `ENOSYS`).
- **The bitset does filter.** A waiter with mask `1 << 3`: `futex_wake(mask =
  1 << 7, 1000)` woke **0**; `futex_wake(mask = 1 << 3, 1000)` woke **1**.
- A `futex_wake` with no waiter costs **177 ns** whatever the mask.

### 4.6 The model

`docs/design/evidence/watch-options/coverage_model.py`, 26 cases, all as
expected, each under a second.

The interesting part is what it shows about the **existing** model. The §4.9
model's watched ranges are a few bytes wide, so every element of every case lies
inside one 16-byte grain. Here, a deliberately wrong predicate that keeps only a
range's two ends — `endpoints` — **passes** the scalar case and **passes** two
elements one word apart, and loses a wake only on `spread array`, whose elements
are 64 bytes apart:

```
endpoints: scalar (passes)              SC  states=  40 -> ok
endpoints: two near elements (passes)   SC  states= 423 -> ok
endpoints: spread array                 SC  states= 214 -> LOST WAKE
```

That is the demonstration the project's rule asks for: the existing model would
not catch a predicate that misses the middle of a range, and this one does.

Three further broken predicates, each losing a wake with a trace:

- `satzero` — a saturating length whose saturated encoding means "no watch"
  instead of "to the end of the region". **Saturation must be permissive.**
- `sigfirst` — a signature whose writer tests only the first grain of its own
  multi-grain write. **The writer must test every grain it touched, or say yes.**
- `endpoints` — above.

And the three §4.9/§3.16 ordering rules, re-run against each candidate
predicate: `checkfirst`, `notifyfirst` and `plainpub` lose a wake for `exact`
exactly as they do for `block`, with the same traces. **The predicate and the
protocol are independent.** A change of predicate is not a change of protocol,
and this is the evidence for saying so.

---

## 5. The options

### A. An exact interval in one packed word — *recommended*

> **Publish `region | offset | length`, packed into one 64-bit word, with the
> length saturating to "the rest of the region". Drop the power-of-two block.**

```c
/* region (2) | offset in 4-byte grains (40) | length in 4-byte grains (22).
 * Zero length means no watch; LEN_MAX means "to the end of the region". */
enum { OSOE_WATCH_GRAIN = 4, OSOE_WATCH_LEN_BITS = 22, OSOE_WATCH_OFF_BITS = 40 };
```

Publication rounds the range **out** to whole 4-byte grains and saturates a
length above 16 MiB. The writer's test is one acquire load, a region compare and
an interval overlap — no variable shift, no loop.

**Correctness.** The filter says yes exactly when the grain-rounded watched
interval intersects the grain-rounded written interval, in the same region. It
can only over-report, by at most three bytes at each end and by "the rest of the
region" above 16 MiB. The model checks it on SC and TSO for a scalar wait, two
near elements, a saturated spread array, and a write spanning several grains,
and checks that the saturated encoding must be the permissive one (`satzero`
loses a wake).

**Cost on the writer.** 0.85 ns against 0.63 ns: **+0.22 ns** per intranode put
and rmw. Spurious coverage 0.000% at every range size and under both write
models.

**Cost on the waiter.** Nothing. The publication is one `seq_cst` store, as
today.

**As the range grows.** Linearly and exactly, to 16 MiB. Above that it
degenerates to "the rest of the region", once, with no cliff and no dependence
on where the allocator put it. Today's scheme degenerates to the whole heap for
a 128-byte array 0.17% of the time.

**Multiple records per PE.** This *is* §4.11 item 3's `rng` word, which already
says "one 64-bit word packing region, offset, and a saturating length, with 0
meaning none. A saturated length means 'to the end of the region'". Adopting it
now makes phase 1 and `MULTIPLE` the same word instead of two. The per-PE
`active` bitmap and the O(k) scan are unchanged and measured at ~1 ns per
waiting record.

**Cost to the RDMA poller.** This is where it earns its keep. `roce-transport.md`
§4.7 recommends a per-node wake poller that shadow-compares each local PE's
watched block, "falling back to (3) for large watched blocks" — i.e. to burning
a core per waiter. That fallback exists *because* `block_of` can inflate an 8 KiB
array to 64 KiB or to the whole heap. With an exact interval the poller compares
exactly the bytes the program is waiting on, never more, so the fallback is
needed only above 16 MiB, which no wait reaches. §4.7's "the common case is an
8-byte ivar, so the poller compares L ≤ 128 words per sweep" becomes true of
*every* case, not just the common one.

**What it does not fix.** It does not remove a single wake in the case that
matters most: `wait_until(counter == N)` after N `signal_add`s still takes N
wakes, because every one of them is a genuine write to the watched word. Only
option C addresses that.

*(Rejected within this option:*
- *Two words, `(offset, length)`, stored and loaded separately. Sound — §4.9's
  model already passes `tworange`, §4.11's already passes `tworange`, and the
  argument generalizes: if the writer reads any stale component, its load
  preceded the waiter's store of that component, which precedes the waiter's
  next predicate check, so the check sees the write. It costs 1.05 ns against
  0.85 ns and a second store on publication, and buys an offset field nobody
  needs.*
- *Keeping the power-of-two block and adding two region bits at the top. It
  works today only because a 256 MiB heap uses 28 of the block's 58 offset bits.
  It leaves the encoding with no stated bound, keeps the variable shift and the
  loop, and keeps the cliff.)*

### B. Double-wide atomics

> **Rejected.** Not because it is unsound — because it is not free, not inlined
> anywhere, and its "load" is a store on the deployment target.

The idea was that a 128-bit atomic word removes quantization entirely: an exact
`(offset, length, region, flags)` read in one atomic load. It does remove
quantization. It costs:

- **A PLT call per notify, on every target.** GCC 14 emits
  `__atomic_load_16`/`__atomic_store_16`/`__atomic_compare_exchange_16` at
  `-march=armv8-a`, `armv8.2-a+lse`, `armv8.4-a`, x86-64, and x86-64 `-mcx16`
  alike. 1.89 ns against 0.63 ns, measured.
- **A read that is a write on the Pi 4.** Without LSE2, libatomic's 16-byte load
  is an `ldxp`/`stxp` retry loop. The filter word is read by every writer on the
  node; a shared read scales (0.70 → 0.15 ns as readers go 1 → 4) and a locked
  read-modify-write anti-scales (7.64 → 23.90 ns). At 128 local PEs this would
  serialize the node's entire notify path on one cache line.
- **A link-line rule.** `NEEDED libatomic.so.1` unless statically linked;
  statically linked it is fine and is the same category as §3.16's `libgcc.a`
  outline atomics.
- **A §3.16 amendment.** Permitted as the rule is literally written; not in its
  spirit; would need a sentence naming `__int128` and the static link.

The lock-table hazard the option was suspected of does not apply: aarch64's
fixed-size `_16` entry points are lock-free instruction sequences, and the
process-private `pthread_mutex` table is reached only by the generic
size-dispatched entry points. That reasoning is worth keeping for any future
temptation to publish a record wider or less aligned than 16 bytes, where it
*would* bite, and silently, across processes.

And the decisive point is the first one in §1: option A gets exactness in 64
bits. There is nothing left for 128 bits to buy.

### C. Publish the predicate, not the range

> **Deferred, and recorded as the next step after A.** It is the only option
> that removes wakes an interval cannot, and it needs a boundary change.

If the record carried `(region, offset, width, cmp, cmp_value)` the writer could
evaluate the predicate on the value it just applied and wake only when the wait
is actually satisfiable: **zero** wakes, not fewer. The writer does not even
need to load the ivar — an rmw knows the value it produced.

**Why this matters more than it looks.** The interval is already exact for the
scalar case. The wakes that remain are wakes on *genuine* writes to the watched
word — and the dominant hot pattern in the corpus produces a storm of them.
`docs/reviews/evidence/q2/q2-usage.md` §2 item 9: pplx dispatch issues
`putmem_signal_nbi_warp(..., SIGNAL_ADD, ...)` **per token per expert**, and the
receiver runs `wait_until(&numRecv[group], CMP_EQ, n)`. Every one of the n − 1
adds that is not the last one bumps `seq` and wakes a thread that re-checks and
re-sleeps. At 537.7 ns on the writer plus a scheduler round trip on the waiter,
for n in the hundreds or thousands, per step. PRK Stencil is the small version:
four neighbours `int_inc(flag)`, the waiter wants `== k`, three wakes in four
are useless.

**Where it breaks.**

- **`wait_until_all` over N elements.** The writer would need the current value
  of every element to evaluate the conjunction — N loads of memory it does not
  own, on the fast path. Worse, those loads would have to be *coherent* loads in
  the §4.10 sense: the whole reason `osoe_tp_load` exists is that reading an
  ivar with a plain load is not sound on a backend whose NIC atomics the CPU
  cannot see. The boundary cannot require a writer to evaluate a predicate over
  memory it does not own.
- **`wait_until_any` / `_some`.** `any` would be sound to evaluate from the one
  element the writer touched — it is a disjunction, so the writer's element
  satisfying it is sufficient. `some` needs the index set only when returning,
  which the waiter computes after waking, so the writer still only needs the
  disjunction. So `any` and `some` are actually *easier* than `all`.
- **The `status` mask** of `wait_until_some_vector` is computed by the waiter
  after it wakes and never by the writer. Not a constraint.
- **The `_vector` forms** give each element its own `cmp_value`, so the record
  would have to carry an array — out of scope for a published word. Fall back to
  the interval.
- **Puts.** A `memcpy` writer does not know the value it produced without
  reading it back, and notify for puts is best effort anyway (§4.10). Fall back
  to the interval.
- **Soundness under §3.16.** The writer evaluates the predicate on the value its
  own locked rmw produced, under the exclusivity spec §3.2 already guarantees.
  It never reads the waiter's memory. Two concurrent writers each evaluate their
  own post-value, and whichever produced the satisfying value wakes. A value that
  was momentarily satisfying and then changed is not caught — but that is already
  true today, because the waiter's own re-check can see the later value. So the
  writer's "no" is exactly as strong as the waiter's own check, and no weaker.
- **Staleness across two successive waits** is handled by the same argument as
  the two-word interval: any stale component the writer reads implies its load
  preceded the waiter's store of that component, which precedes the waiter's next
  check.

**The hybrid.** One flag bit in the record: `OSOE_WATCH_PREDICATE`. Scalar waits
set it and publish a second word holding `cmp_value` (64 bits, so it needs its
own word) with `cmp` and `width` in the first word's spare bits. Array waits and
the vector forms leave it clear and the writer uses the interval alone. **The
fast path does not change at all**: the predicate is consulted only after the
interval has already matched, which is the rare path.

**What it costs the boundary.** `osoe_tp_notify` must learn the value that was
applied and its width, or gain a sibling `osoe_tp_notify_rmw(pe, region, offset,
width, value)`. That is a §4.10 change, and §4.10 changes are expensive on
purpose. It also duplicates `satisfied()` below the boundary.

**What it costs the RDMA poller.** Nothing; it helps. A poller that has the
predicate can compare its shadow, evaluate, and skip the futex bump entirely
when the value does not satisfy — which turns the signal-storm case from "n
futex wakes" into "one".

**Decision: not now.** It needs a boundary change, it duplicates comparison
semantics below the boundary, and it is worthless until the interval is exact
(with `block_of`, an 8 KiB array's spurious coverage swamps it). Record it as
the follow-on, with U4 as its justification, and revisit when
`osoe_tp_set_range` (`roce-transport.md` §9.2) is on the table, since both are
boundary changes about telling a backend what the program is waiting for.

### D. A signature instead of an interval

> **Rejected.** It regresses the case that dominates the corpus, it needs an
> adaptive grain to be usable at all, and it is not invertible, which destroys
> the RDMA poller.

The attraction is real and worth stating properly: one load, one AND, no grain
budget, no saturation cliff, region hashed in for free, and it **unions** — so
§4.11's per-PE prefilter is the OR of the thread masks, the same mechanism at
both levels, O(1) in the number of records where an interval list is O(k).
Measured at 0.63 ns, the same as today's block and cheaper than the interval.

The objections, in order of weight.

**1. It is not invertible, so a polling backend cannot use it.** Requirement 9:
a RoCE backend has no writer-side event and must shadow-compare the watched
bytes. A signature says whether a *given* address might be watched; it cannot
enumerate what to shadow. A poller holding only a signature would have to
compare the whole region every sweep. `roce-transport.md` §4.7 calls the wake
poller the thing that turns "a core per waiter" into "a shared core per node";
a signature takes that back. This objection alone is decisive, and it is the one
the framing of the question ("cheap for the writer") hides.

**2. It regresses the scalar case, which is the corpus.** Today: 0.000%.
Signature, 64 bits: 1.548% at one hash, 0.106% at two, 0.006% at three.
Mean added writer cost at m = 3 is 0.006% × 537.7 ns ≈ **0.03 ns** (*arith*),
which is small — but it is a regression from exactly zero on the only wait form
that has public users, in exchange for fixing a form that has none.

**3. It saturates.** 64 bits over 16-byte grains holds a few dozen grains. An
8 KiB array needs 512 and the filter says yes to **99.98%** of all writes in the
heap. Making it usable needs an adaptive grain — coarsen until the range fits
in ~8 grains — which is *locality-sensitive signatures* from the TM literature
(§7), and which re-introduces exactly the quantization the signature was
supposed to remove, plus hash collisions on top. At 128 bits, 4 hashes, adaptive
grain, an 8 KiB array still costs 3.2% under adjacent writes, against 0.000% for
the interval.

**4. The union property buys less than it looks.** It is O(1) in records where
the interval list is O(k) at ~1 ns each. But the records that are set are the
threads actually *blocked*, which is bounded by cores — 4 on a Pi. And a union
signature saturates faster than a single one: k blocked threads is k times the
occupancy, so the regime where O(1) matters is the regime where the answer is
always yes.

*(Also rejected: a counting variant — a per-PE array of 32 counters, a waiter
incrementing the counter of each grain it watches. It makes "no" exact per
bucket and makes unwatch exact, but it costs the waiter O(grains) locked
increments to publish and spreads the writer's working set over 32 cache lines
per PE, i.e. 4096 lines at 128 PEs.)*

### E. Let the kernel filter

> **Rejected. Measured: 177 ns against 0.63 ns.**

`futex_wake` takes a 32-bit bitmask and osoe passes `FUTEX_BITSET_MATCH_ANY`
today. The kernel's futex queue could be the watch table: the waiter waits with
the mask of its watched grains, the writer wakes with the mask of the grains it
wrote, and the kernel does the AND. **Verified today that this works**: a waiter
on `1 << 3` is not woken by `futex_wake(1 << 7)` and is woken by
`futex_wake(1 << 3)`.

It is a 32-bit signature with m = 1, evaluated in the kernel, with all of option
D's imprecision and none of its cheapness. A `futex_wake` with nobody waiting
costs **177 ns** — 280× the userspace load — and the mask does not change that.

**Does a userspace gate rescue it?** A per-PE "is anyone watching at all" word,
loaded for 0.63 ns, with the syscall only when it is non-zero. That is exactly
what `active != 0` already is, and it rescues the case where nobody is waiting —
which is the case the current design already handles for 0.63 ns. It does
nothing for the case the filter exists for: when a waiter *is* present, every
write to that PE becomes a 177 ns syscall instead of a 0.63 ns load and a rare
537 ns wake. At any spurious-coverage rate below ~33% that is a straight loss,
and option A's rate is 0%.

**`futex_waitv`?** Present on 6.12 (verified). It lets one thread wait on up to
128 words, which would matter if each ivar had its own futex word — but the
kernel implements only 32-bit futexes (`FUTEX2_SIZE_U64` → `EINVAL`, re-verified
today, invariant 18) and OpenSHMEM's wait types include 64-bit ones, so ivars
cannot be futex words at all. §4.11 already rejects `futex_waitv` on the
grounds that one word per waiter is enough, and nothing here changes that.

### F. Others considered and dropped quickly

- **`userfaultfd` write-protect on the watched page.** The kernel-and-MMU member
  of the family. Page granularity (4 KiB, or 16 KiB on a Pi 5 — §4.3), a fault
  per write at ~1–2 µs (*recalled*), and it would break `shmem_ptr` direct
  access outright (§8.6 U8). Three orders of magnitude the wrong way.
- **An interval tree or segment tree per PE.** Correct and O(log k), against a
  linear scan of k ≤ 64 records at ~1 ns each. It needs a concurrent-safe
  structure in shared memory, written by the waiter and read by every writer in
  every process. A worse constant and a much worse protocol, for a k that never
  gets large.
- **Hash the offset into per-PE watch buckets, futex-hash-bucket style.** See the
  counting variant under D.
- **Futex directly on the `ivar`.** Already rejected in §4.9 and re-verified
  today: the kernel implements only 32-bit futexes.
- **A per-node generation counter with wake-all.** Already rejected in §4.9: the
  §3.11 thundering herd at 256 local PEs.

---

## 6. Recommendation

> **Replace `block_of` with §4.11 item 3's packed word: `region | offset |
> saturating length`, exact to a 4-byte grain up to 16 MiB. Keep the protocol
> exactly as it is. Record the published predicate (option C) as the follow-on,
> justified by U4, and take it when the boundary next opens.**

The case, in one paragraph. It is exact where today's scheme is inflated and
equally exact where today's scheme is already exact; it costs 0.22 ns on a path
whose next stop is a 537 ns wake; it removes a class of defect — a variable
shift from a packed field and a loop whose termination depends on its inputs —
rather than the two instances of it; it makes room for the region that §5.8 and
§4.3 make certain and that today's encoding has no bits for; it is the word
§4.11 has already specified and model-checked, so phase 1 stops diverging from
`MULTIPLE`; and it removes the reason `roce-transport.md` §4.7 needs a
"fall back to spinning for large blocks" clause.

### The strongest argument against it, in its own words

*"You are changing the one function in this file that is measurably correct.
`block_of` is **exactly** the watched bytes for every scalar wait — k lands on 3
for an 8-aligned `uint64` and on 2 for a 4-aligned `uint32` — and the
measurement says 0.000% spurious coverage under both write models. Single-flag
waits are the corpus: the usage study found* no public application users at all
*for `wait_until_{all,any,some}` and its vector forms, and HOT usage of scalar
`wait_until` in pplx, Triton-distributed, PyTorch, PRK and hclib. So the
inflation you measured — 122 KiB mean for an 8 KiB array, 7.3% of placements
over 64 KiB, 35.7% spurious coverage under adjacent writes — is a defect in code
that nothing calls. In exchange you propose to add 0.22 ns to* every *intranode
put and rmw that osoe will ever perform, forever, on the exact path §8.4 calls
the intranode floor and defines as "`memcpy` plus the synchronization the API
requires". Fix the two UB cases with three lines — clamp `k` to 62 and bound the
loop — and you have removed the only defect that can actually bite, at zero
cost. The rest is tidiness."*

That is a fair statement of the position and the numbers in it are mine. Three
answers, and the third is the one that decides it.

1. The 0.22 ns is 0.22 ns on top of 0.63 ns, on a path whose alternative outcome
   costs 537 ns. It is a 35% increase in the filter and an invisible increase in
   the operation; U1's figure of merit is small-operation latency, and 0.22 ns
   does not appear in it.
2. "Fix the two UB cases with three lines" leaves the shape — a shift count
   decoded from six bits of a packed word — which is what made both of them
   possible. A bounds-checked encoding with no variable shift cannot have a third.
3. **The region has to go in the word, and there are no bits.** §5.8 puts
   collective control words in `OSOE_REGION_INTERNAL` and says a generic
   collective runs "over put, fenced rmw, and watch"; §4.3 phase 1.5 adds
   `STATICS`, and spec Example 27 waits on a `static uint64_t`. `watch.c` today
   is `osoe_fatal("watching region %d is not supported")` and `osoe_tp_notify`
   is a silent early return for anything but `HEAP`. That must change, the
   encoding has no room, and §4.11 has already chosen what replaces it. The
   choice is between doing it once and doing it twice.

The honest summary is that this is a **correctness-and-convergence change with a
measurable but negligible cost**, not a performance fix. The performance numbers
in §4.3 justify the *shape* of the replacement; they do not justify the change on
their own, and the memo should not pretend otherwise.

---

## 7. Prior art, and what transfers

Labelled: *(web)* checked against a source in this session, *(recall)* not
checked here.

**Cache-coherence snoop filters and coarse-grain tracking.** RegionScout and the
Region Coherence Array track coherence for large *fixed-size aligned* regions
and use the region's state to suppress broadcasts; some directory designs
augment each entry with a counting Bloom filter *(web)*. **What transfers:** the
whole family uses a *set of fixed-size aligned regions*, never one
variable-size block chosen to contain an arbitrary range. That is the
distinction `block_of` gets wrong. A coarse filter is fine; a single
smallest-enclosing-power-of-two is not, because its size is chosen by the
*alignment* of the range rather than by its length. **What does not transfer:**
hardware can afford a CAM and a per-line state machine; we have one word.

**Transactional-memory read/write-set signatures — the closest analogue.** Bulk
and LogTM-SE summarize unbounded read/write sets in bounded hardware as Bloom
signatures and detect conflicts on coherence traffic. The literature's results
are exactly the ones measured in §4.3: the false-positive rate **grows with
transaction size**, signature-only HTM produces very high abort rates from
address-signature false positives, and more than 99% of transactions experience
a repetitive false conflict *(web)*. The mitigation the literature converged on
is **locality-sensitive signatures**, which share bits between contiguous
addresses to cut occupancy *(web)* — which is the "adaptive grain" of §4.3
under its proper name, and the reason to say plainly that the adaptive-grain
signature is not a new idea but a known-necessary patch. **What transfers:** the
saturation result, quantitatively. **What does not:** a transaction's read set is
genuinely unbounded and scattered, so hardware has no interval to publish; a
wait set is one contiguous array, so it does.

**Packet classification and TCAM range expansion.** Representing an arbitrary
W-bit range as ternary prefixes takes up to **2W − 2** entries in the worst case;
a 16-bit port range expands to 30, and two such fields to 900 *(web)*. This is
the formal statement of `block_of`'s problem: a range is not a prefix, and
covering an arbitrary range with **one** prefix forces the prefix out to the
nearest common ancestor — which is exactly "round out to the smallest aligned
power-of-two block". **What transfers:** the diagnosis, precisely. **What does
not:** the remedy. TCAM literature spends 2W − 2 entries or re-encodes the field
(Gray coding gets 2W − 4, later work gets W *(web)*); we have the option they do
not, which is to store the range's two endpoints and compare, because we have an
ALU and they have a parallel match array.

**Hardware watchpoints.** ARMv8's `DBGWCR<n>_EL1` has a `MASK` field naming
3 to 31 masked address bits — i.e. an **aligned power-of-two region from 8 bytes
to 2 GB, with the start required to be aligned to the size** — and a BAS field
selecting bytes within 8, which must be all-ones whenever MASK is non-zero
*(web)*. x86 debug registers watch 1, 2, 4 or 8 bytes each, four of them
*(recall)*. **What transfers:** ARM's watchpoint MASK *is* `block_of`,
independently invented, and the consequence in the field is that a debugger
watching an arbitrary region either over-watches or spends several watchpoint
units on it. The hardware's answer is **several aligned blocks**, not one; osoe
has one. **What does not:** hardware compares in the load/store unit for free
and cannot afford an interval comparator per watchpoint; we can.

**Interval and segment trees.** The right answer when k is large and queries are
stabbing queries. Here k is the number of *blocked threads on one PE*, bounded
by cores, and the per-record scan is ~1 ns. Does not transfer; see §5.F.

**Linux futex hash buckets.** §3.11 already records the relevant property —
futex waits hash into kernel buckets by address, and unrelated words collide on
the bucket spinlock, so keep futex words few and cache-line aligned. That is an
argument about the futex words, not about the filter, and it is already applied.

**`userfaultfd` and page-level write tracking.** See §5.F. The MMU's granularity
is a page and its cost is a fault.

---

## 8. What other implementations do

`docs/reviews/core-api.md` §2.7 is the collection and the claim in the task
holds up when read against it, with one correction.

- The **fallback** every portable implementation uses is "poll with backoff plus
  progress", cited there to OSSS-UCX, OSHMPI, GPI-2 and NVSHMEM.
- SOS is the exception that proves it: it **blocks on Portals target counters**,
  but only in `THREAD_SINGLE`, and the study records that the reason is not
  documented (§7.2 C6). It also "forces polling when CMA/XPMEM paths exist,
  i.e. it met this problem and avoided blocking".
- The substrates that *can* notify natively all have a target-side counter:
  Portals CT, UET remote-write counters, uGNI `POST_CQWRITE`, CXI
  `FI_RMA_EVENT`. None of them is a *filter* — they count everything landing in
  a bound region, which is coarser than what osoe publishes. CXI is marked
  "C/?" because no shipping OpenSHMEM blocks on it under manual progress and its
  wait objects have a documented race.

**So "everyone spins" is close to right and is itself the argument.** It says
the value in §4.9 is not in the precision of the filter but in *blocking at
all* — osoe blocks, on an oversubscribed Pi where a spinning waiter takes a core
from a progress thread or a mid-`memcpy` writer, and that is the differentiator.
It also says the bar is low: an implementation that blocks with a slightly
over-eager filter is still strictly ahead of one that burns a core. That is a
reason to prefer the simple exact interval over the clever signature, and a
reason not to spend a boundary change on option C until something measures it.

---

## 9. What must be verified before committing

1. **An init-time bound on the heap.** `SHMEM_SYMMETRIC_SIZE` is parsed with no
   upper limit (`shmem/src/boot.c`), and a packed 40-bit grain offset covers
   4 TiB. Add a `_Static_assert` on the field widths and an `osoe_fatal` at
   init if `heap_size` exceeds what the offset field can name. Aliasing must be
   impossible, not merely improbable — this is the same failure mode as the two
   defects being removed.
2. **The grain, decided rather than assumed.** 4 bytes suits every AMO and wait
   type the spec has. The deprecated `short` waits are 2 bytes and would round
   out to 4, which is spurious-only. Confirm no wait type is smaller, and
   confirm `osoe_sym_word_offset`'s alignment guarantee covers it.
3. **The field split.** 2/40/22 gives 4 TiB of offsets and 16 MiB of exact
   length. Check that against the largest plausible `wait_until_all` array and
   against `OSOE_REGION_INTERNAL`'s planned size in §5.8, and record why.
4. **Run the models.** `docs/design/evidence/watch/range_watch_model.py` with the
   new predicate substituted, and
   `docs/design/evidence/multiple/watch_list_model.py` likewise, so both records
   still describe what the code does. This memo's model
   (`docs/design/evidence/watch-options/`) shows the predicate and the protocol
   are independent, but the two committed records should not be left describing
   a predicate that is gone.
5. **UBSan and a fuzz of `pack`/`covers`.** Exhaustively for small heaps, random
   for large ones, against a reference implementation in `unsigned __int128`
   arithmetic, asserting the one-sided property: `reference_overlaps ⟹ covers`.
   Never the converse.
6. **Re-measure the writer path in the tree**, not in a scratch probe. 0.63 →
   0.85 ns was measured on a standalone benchmark with the filter word on its
   own line and no other traffic. The number that matters is the one from §8.4's
   intranode floor harness.
7. **On real Pi 4 hardware, when it exists**, confirm the filter is still a
   plain `ldr` and the cost still falls with readers. Everything in §4.4 about
   Cortex-A72 is read from libatomic's disassembly and its ifunc resolver, not
   executed on an A72; that conclusion is only used to *reject* an option, so a
   surprise there changes nothing, but it should be checked when the hardware
   arrives.
8. **Before option C is ever taken**, measure the wake storm it targets: a
   `signal_add`-per-token producer against a `wait_until(count == n)` consumer,
   at n = 1, 16, 256, and count the wakes and the writer time. §4.2's 537.7 ns
   makes the arithmetic look compelling and arithmetic is not a measurement.

---

## 10. Evidence index

| Where | What |
|---|---|
| `docs/design/evidence/watch-options/coverage_model.py`, `.out` | 26 exhaustive cases: each candidate predicate under SC and TSO; `endpoints`, `satzero`, `sigfirst` each losing a wake; `checkfirst`, `notifyfirst`, `plainpub` re-run against each predicate |
| `docs/design/evidence/watch-options/README.md` | why the §4.9 model cannot see a middle-of-range miss, and what this one adds |
| §4.1–4.5 above | the VM measurements, with method; scratch probes, removed |
| `docs/design/evidence/watch/`, `docs/design/evidence/multiple/` | the committed §4.9 and §4.11 records, unchanged |
