# Conformance review: is it OpenSHMEM?

Question 1 of `phase1-step2-slice1-review-prompt.md`, run against `main` at
`7ff9343` (slice merge `d542c63`). Specification: `docs/reference/OpenSHMEM-1.6.pdf`,
read through `pdftotext -layout`; cited below as spec §N. Design-document sections are
cited as design §N.

Every claim is labelled *documented* (spec or design text), *observed in source*,
*measured* (a program run in the Lima VM against a private build in
`build/scratch/review-conformance/`, never the shared presets), or *recalled*.
No web access was used. Evidence programs, the script that builds and runs them, and
its output are in `docs/reviews/evidence/phase1-step2-slice1/conformance/`
(`run.sh`, `run.out`, `fatal-under-start-pes.out`). The VM was shared with other
agents; every timing below is *measured under contention*, and none of the findings
depends on a timing except F1, which was reproduced three times.

## Summary verdict

**What is implemented conforms well; what is implemented is a small part of
OpenSHMEM 1.6; and the tree does not yet say so in a place a user would look.**

- The routines that exist were plainly written against the specification text and
  not against a summary of it. I read §4, §5-§8, §9.1-§9.5, §9.7-§9.9, §9.10.1-4,
  §9.11, §9.12, §10, Annex C and Annex F against the source and found the hard
  cases right: every empty-set answer of the wait/test family, the barrier placement
  of each memory-management routine, `SHMEM_CTX_INVALID` being ignored by exactly
  the routines the spec says ignore it, `shmem_realloc`'s failure contract, the
  size syntax of `SHMEM_SYMMETRIC_SIZE` down to the spec's own `3.1M = 3250586`
  example, the AMO type tables, thread-level negotiation, and nested and repeated
  initialization. The three spec examples that can be built (38, 42, 43) run
  correctly on one node and across logical nodes.
- I found **no case where the library aborts on behaviour the spec defines.** Every
  loud failure I examined sits on text the spec makes undefined, and Annex C itself
  offers "abort" as the example response for two of them.
- I found **eleven departures**, none blocking. One is a real defect in the error
  model (F1: a loud failure hangs the job when the program used `start_pes`). One
  confirms a sibling's suspicion from the spec text and by measurement (F2: under
  `OSOE_FORCE_REMOTE` the library contradicts spec §7's definition of
  `SHMEM_TEAM_SHARED`; the design's claim that this is legal is wrong). The rest are
  small, mechanical and mostly about deprecated surface the spec still requires.
- **The coverage gap is the headline.** "526 routines" is true as a symbol count and
  misleading as a measure of the API: 467 of the 526 are the type cross product of
  two families (AMOs and wait/test). There is no typed or sized put or get, no
  `shmem_p`/`shmem_g`, no strided or nonblocking RMA, no collective beyond
  `barrier_all`/`sync`, no lock, no `shmem_global_exit`, and no symmetric statics.
  Of the specification's 53 example programs, **7 can be compiled and linked and run
  as written**; the very first one (Example 5) cannot, because it calls `shmem_put`
  on a `static int`. All of that is recorded as deferred in design §7.4-§7.6, so it
  is not a discovery — but it is the honest answer to "is it OpenSHMEM?": *it is a
  conforming-in-style subset, not yet a library a third-party OpenSHMEM program will
  link against.*

## How many of the 526 do real work

*Measured* with `nm -D --defined-only libosoe.so` on the private debug build: 526
`pshmem_` symbols, 526 `shmem_` symbols, plus the seven unprefixed deprecated names
(`start_pes`, `_my_pe`, `_num_pes`, `shmalloc`, `shfree`, `shrealloc`, `shmemalign`).

| Group | Count | Status |
|---|---:|---|
| AMOs, plain and `ctx_` (`amo.c`) | 288 | Real: all reach `osoe_transport_rmw` |
| wait/test family incl. `signal_wait_until`, deprecated `wait` (`wait.c`, `signal.c`) | 179 | Real |
| Everything else | 59 | see below |

Of the 59: **44 do real work** (init/finalize/query ×7, the four team queries,
`team_ptr`, `ctx_get_team`, the six memory routines, `putmem`/`getmem` ×4, `ptr`,
seven signal routines, six ordering routines, three collectives, two info, two
accessibility). **Nine are no-ops because the spec says a no-op is the
implementation**: `shmem_pcontrol` (spec §10.1.1: "make no use of this routine"),
`shmem_ctx_session_start/stop` (spec §9.9: sessions "do not affect the completion or
ordering semantics"), and the six `SHMEM_CACHE` routines (Annex F.2.7). **Six are
stubs**:

| Stub | What it does when called | Conforming? |
|---|---|---|
| `shmem_team_split_strided`, `shmem_team_split_2d` | validate the parent handle (fatal on a bogus one, silent on `SHMEM_TEAM_INVALID`), assign `SHMEM_TEAM_INVALID`, return nonzero | Yes. Spec §9.4.6: "If the new team cannot be created ... `new_team` will be assigned the value `SHMEM_TEAM_INVALID` and ... return a nonzero value on all PEs" — and every PE takes the same branch, which the spec requires of team creation (§9.10 "must return identical return codes") |
| `shmem_ctx_create`, `shmem_team_create_ctx` | assign `SHMEM_CTX_INVALID`, return nonzero | Yes. Spec §9.5.1: "An unsuccessful context creation call is not treated as an error" |
| `shmem_team_destroy`, `shmem_ctx_destroy` | no-op on the `_INVALID` handle (required, §9.4.8, §9.5.3); otherwise `osoe_fatal`, naming the predefined handle or the pointer | Yes. Destroying a predefined team is undefined (§9.4.8); no other team or context can exist, so every other argument is a handle the library never issued |

So: **511 of 526 do the work the spec describes, 9 are required no-ops, 6 are stubs,
and all six stubs take a branch the spec defines.** None is silent in a way that
loses data. A routine that does *not exist* (e.g. `shmem_long_put`) fails at compile
or link time, which design §7.4 chose deliberately over a stub that aborts; I agree
with that choice, with the reservation in F10.

## Findings

Severity scale: *blocking* / *design change needed* / *clarification needed* / *minor*.

| # | Finding | Severity | Confidence |
|---|---|---|---|
| F1 | A loud failure hangs the job when the library was started by `start_pes` | design change needed (small) | high |
| F2 | `OSOE_FORCE_REMOTE` contradicts spec §7's definition of `SHMEM_TEAM_SHARED`; it also reroutes puts to a connection that does not exist | clarification needed | high |
| F3 | `SMA_SYMMETRIC_SIZE` is ignored, and `SHMEM_INFO` says it is honoured | minor | high |
| F4 | `shmem_test_any` / `shmem_wait_until_any` never return a later satisfied index | minor | medium |
| F5 | Calls into an uninitialized or finalized library segfault or mislead rather than fail loudly | minor | high |
| F6 | Deprecated AMO names (`shmem_long_inc`, `shmem_cswap`, ...) are absent and not listed as known nonconformance | minor | high |
| F7 | `shmem_pcontrol` is declared only in `pshmem.h` | minor | high |
| F8 | `<mpp/shmemx.h>` does not exist | minor | high |
| F9 | `SHMEM_SYMMETRIC_SIZE` rejects exponent notation | minor | low |
| F10 | The scope of the subset is recorded only in the design document | clarification needed | high |
| F11 | §10 item 4 (wrappers kept separate) and the plain-calls-`ctx_` layering | clarification needed | low |

### F1. A loud failure hangs the job under `start_pes`

- **Location:** `shmem/src/fatal.c:43` (`exit(EXIT_FAILURE)`), `shmem/src/deprecated.c:34-48`
  (`atexit(finalize_at_exit)`), `shmem/src/init.c:194` (every finalize is a barrier).
  Design §3.15, §7.4; spec §9.1.13.
- **Failure scenario:** a program initializes with `start_pes(0)`. PE 0 makes any call
  the library treats as fatal — the tree's own `test-bad-put.c` case, a put to a
  private address. `osoe_fatal` prints its diagnostic and calls `exit()`. `exit()` runs
  the handler `start_pes` registered, which is `pshmem_finalize`, which begins with
  `osoe_barrier_all`. PE 0 now blocks in a collective that the other PEs — alive,
  healthy, and waiting for something PE 0 will never send — will never join. The
  process that was supposed to die loudly is instead alive inside the library, so the
  development watchdog sees no dead PE, and nothing ends the job except an outside
  timeout.
- **Evidence (*measured*, 3/3):** `fatal-under-start-pes.c`, 2 PEs. Under `shmem_init`
  the job ends in under a second with exit 1. Under `start_pes` it prints the same
  diagnostic and then runs until `osoe-launch`'s timeout, three runs out of three, with
  `OSOE_WATCHDOG=1` and with `OSOE_WATCHDOG=0` (`fatal-under-start-pes.out`, and the
  last section of `run.out`). Under Slurm there is no such timeout by default.
- **Why it matters beyond a deprecated routine:** design §3.15 has step 4's agents end
  a `shmem_global_exit` by calling `exit(status)`. The same handler will run there, in
  an agent thread, and call a collective. The defect is in the pairing of "fatal means
  `exit()`" with "an atexit handler that is a collective", and step 4 inherits it.
- **Suggested change:** have `osoe_fatal` (and the future global-exit path) mark the
  process as aborting before `exit()`, and have `finalize_at_exit` return at once when
  it is set; or end fatal paths with `fflush(NULL)` + `_exit`. Add the `start_pes`
  variant of `shmem.bad-put` as a test with a short launcher timeout.
- **Confidence:** high; the mechanism is visible in source and the measurement is
  deterministic.

### F2. `OSOE_FORCE_REMOTE` versus spec §7's `SHMEM_TEAM_SHARED` (sibling's item: **confirmed**)

- **Location:** spec §7, `SHMEM_TEAM_SHARED` row; `shmem/src/job.h:95-100`;
  `shmem/src/team.c:54-69`; design §4.8 ("Returning `NULL` unconditionally is
  conforming"), §8.1, and commit `de540da` ("a node-sized SHARED whose members answer
  NULL is legal").
- **The exact wording** (*documented*, spec §7): "`SHMEM_TEAM_SHARED` refers to the
  team of all PEs that would mutually return a non-null address from a call to
  `shmem_ptr` or `shmem_team_ptr` for all symmetric heap objects. That is, `shmem_ptr`
  **and** `shmem_team_ptr` **must return a non-null pointer** to the local PE for all
  symmetric heap objects on all target PEs in the team."

  So it binds **both** routines, not only `shmem_team_ptr`. The text is a definition
  of the team's membership in terms of the pointer routines. Its consequence is that
  "returning NULL unconditionally" is conforming **only if `SHMEM_TEAM_SHARED` is then
  the singleton {self}** — spec §9.1.9 alone ("Otherwise, a null pointer is returned")
  permits the NULL, but §7 forbids a multi-member SHARED whose members get it. Design
  §4.8's sentence is true of §9.1.9 read alone and false once §7 is read with it, and
  the commit message's claim is simply wrong.
- **Evidence (*measured*):** `team-shared-contract.c` checks, for every world PE, that
  membership of SHARED (`shmem_team_translate_pe(WORLD, pe, SHARED) >= 0`) coincides
  with a non-null `shmem_ptr`. It reports *consistent* on maps `0,0,0,0`, `0,0,1,1`
  and `0,1,2,0` — **production behaviour conforms**. With `OSOE_FORCE_REMOTE=1` on
  `0,0,1,1` it prints `PE 0: PE 1 is in SHMEM_TEAM_SHARED but shmem_ptr returns NULL`.
- **A second defect found by the same run (*measured* twice, deterministic in source):**
  the program then puts to its node-mate and the library aborts with
  `osoe: PE 0: no connection toward node 0, which holds PE 1`. `osoe_is_local` is the
  dispatch predicate of `osoe_transport_put/get/rmw` (`transport-am.c:462,475,491`) as
  well as of `osoe_transport_direct`, so the flag does change the route — to one that,
  per connection-per-node-pair, cannot exist. Design §8.1 and the comment at
  `transport-am.c:113-131` both say the flag "denies the pointer to a node-mate without
  changing the route". The suite does not see it because the force-remote tests check
  only what `shmem_ptr` answers (`shmem/tests/CMakeLists.txt:225-231` says as much).
  This is a correctness item for whoever owns §8.1; I report it because it makes the
  flag unusable for the differential purpose it kept.
- **Severity:** clarification needed — it is a test-mode flag and no production path is
  affected. But the design text should stop claiming legality. Either shrink SHARED to
  {self} under the flag (which the commit rejected for diff stability), or state that
  the flag deliberately leaves the library nonconforming with §7 and keep it out of any
  conformance run. And split the predicate: one for "may hand out a pointer", one for
  "dispatch".
- **Confidence:** high on the spec reading and the measurement.

### F3. `SMA_SYMMETRIC_SIZE` is ignored

- **Location:** `shmem/src/bootstrap.c:103-107` reads `getenv("SHMEM_SYMMETRIC_SIZE")`
  directly, bypassing the `spec_env()` helper at lines 20-29 whose comment says it was
  written as the general rule *because* "`SHMEM_SYMMETRIC_SIZE` has a value and will
  need it". `shmem/src/init.c:64-65` prints "Each also has a deprecated SMA_ spelling".
  Spec §8 ("currently supported for backwards compatibility"), Annex F table
  (`SMA_SYMMETRIC_SIZE`, last version supported: Current).
- **Evidence (*measured*):** `heap-size-probe.c` allocates 2 MiB. Default: succeeds.
  `SHMEM_SYMMETRIC_SIZE=1m`: fails, as it should. `SMA_SYMMETRIC_SIZE=1m`: succeeds —
  the variable had no effect. `SMA_SYMMETRIC_SIZE=garbage`: no diagnostic, although
  spec §8 makes an invalid value an error.
- **Fix:** one line, `spec_env("SYMMETRIC_SIZE")`; add the SMA case beside the existing
  `12q` test. **Severity** minor, **confidence** high.

### F4. `test_any` and `wait_until_any` always return the lowest satisfied index

- **Location:** `shmem/src/wait.c`, `scan()`, `case MANY_ANY: if (ok) return i;` over an
  ascending loop. Spec §9.11.3 and §9.11.10: "If an entry *i* in *ivars* within the
  wait set satisfies the wait condition, a series of calls to `shmem_wait_until_any`
  must eventually return *i*."
- **Evidence (*measured*):** `test-any-fairness.c`: elements 0 and 3 both satisfy, no
  mask; 100,000 calls each of `shmem_test_any` and `shmem_wait_until_any` returned
  index 0 every time and index 3 never.
- **Reading:** literal nonconformance with a normative sentence. The intended idiom
  (spec Examples 39, 43) masks each returned index in `status`, under which the
  ascending scan is fine — hence *medium* confidence that anyone would call this a bug.
  But the sentence carries no such condition, and a caller that does not mask starves.
  A rotating start index (a per-thread or per-call-site counter; even `seq` of the
  watch would do) satisfies the text at no cost. **Severity** minor.

### F5. An uninitialized library is not loud

- **Location:** every entry point except `shmem_my_pe`/`shmem_n_pes` (`init.c:248-257`,
  which do test `initialized`). Spec Annex C makes all of it undefined; design §3.15
  promises loud failure.
- **Evidence (*measured*, `uninitialized-calls.c`, one PE):** before `shmem_init`:
  `shmem_malloc`, `shmem_free`, `shmem_long_wait_until`, `shmem_barrier_all`,
  `shmem_team_sync` die with SIGSEGV; `shmem_putmem` and `shmem_long_atomic_inc` abort
  with the misleading `PE 0 is not in 0..4294967295` (`world_size - 1` with
  `world_size == 0`); `shmem_team_my_pe(SHMEM_TEAM_WORLD)` returns 0, a valid-looking
  answer; `shmem_quiet` and `shmem_finalize` return silently. After the last
  `shmem_finalize`: `shmem_ptr`, `shmem_malloc`, `shmem_putmem`, `shmem_barrier_all`
  segfault, because `session_end` leaves `osoe.bootstrap.world_size` and the heap table
  in place while the mappings are gone.
- **Conforming** (undefined behaviour), so *minor* — but it is the one class where the
  library's own standard is not met, it is the commonest beginner error, and a
  use-after-finalize `shmem_ptr` that dereferences a stale table is worse than quiet.
  One `osoe_require_initialized(routine)` in `osoe_check_pe`, `osoe_symmetric_offset`,
  `osoe_team_of` and `osoe_ctx_of` covers nearly every entry point; zeroing `osoe` in
  `session_end` covers the rest.

### F6. The 1.4-deprecated AMO names are missing

- **Location:** absent from `shmem/include/shmem.h`; spec §9.7.1.1-§9.7.1.8 deprecation
  blocks and Annex F table: `shmem_TYPENAME_{fetch,set,cswap,swap,finc,inc,fadd,add}`
  and the C11 `shmem_{fetch,set,cswap,swap,finc,inc,fadd,add}`, "Last Version
  Supported: Current"; Annex F.1: "The deprecated API must be supported".
- **Evidence (*measured*):** `deprecated-amo-names.c` fails to compile: implicit
  declaration of `shmem_long_inc`, `_finc`, `_cswap`, `_swap`, `_add`, `_fadd`.
- This is 30 routines (int/long/longlong for cswap, finc, inc, fadd, add; those plus
  float/double for fetch, set, swap) and 8 generic macros, each one line over the
  existing generators. Pre-1.4 codes — which is most published SHMEM code — use these
  names. Design §7.4 lists the deprecated *routines* it chose to provide and its two
  known nonconformances; this set is in neither list. **Severity** minor,
  **confidence** high. The active-set collectives and `shmem_barrier` are in the same
  Annex F state but are covered by the design's explicit "constants, not routines"
  decision.

### F7. `shmem_pcontrol` is not declared by `<shmem.h>`

- **Location:** `shmem/include/pshmem.h:168`; spec §10.1 ("Any user code must be able to
  control the profiler"), §10.1.1 synopsis, §10 item 5; spec §5 (`shmem.h` holds the
  interfaces of the specification).
- **Evidence (*measured*):** `pcontrol-from-shmem-header.c` includes only `<shmem.h>`
  and fails: implicit declaration of `shmem_pcontrol`. Move the `shmem_` prototype to
  `shmem.h`; keep `pshmem_pcontrol` where it is. **Severity** minor, **confidence** high.

### F8. `<mpp/shmemx.h>` does not exist

- **Location:** `shmem/include/mpp/` holds only `shmem.h`. Annex F.2.1: "implementations
  must provide all OpenSHMEM-specified header files from the mpp header directory such
  that these headers can be referenced ... as `#include <mpp/shmem.h>` `#include
  <mpp/shmemx.h>`".
- **Evidence (*measured*):** `mpp-shmemx-header.c`: `fatal error: mpp/shmemx.h: No such
  file or directory`. A three-line redirect. (`mpp/pshmem.h` is not named by the text.)
  **Severity** minor, **confidence** high.

### F9. `SHMEM_SYMMETRIC_SIZE=1e6` is rejected

- **Location:** `base/size.c`; spec §8: "Non-negative integer or floating point value
  with an optional character suffix".
- **Evidence (*measured*):** `1e6` aborts as invalid. Everything the spec spells out is
  right (see "What is conforming"). Whether "floating point value" admits an exponent
  is not said; `strtod`-based implementations accept it (*recalled*, unverified).
  Termination on an invalid value is itself conforming. **Severity** minor,
  **confidence** low — a note in `SHMEM_INFO`'s text would settle it.

### F10. The subset is not stated where a user would look

- **Location:** `shmem/include/shmem.h:5-6` ("Only the routines osoe implements are
  declared"), `README.md`, `SHMEM_VERSION` output (`init.c:98`: "osoe, OpenSHMEM 1.6").
- **Scenario:** a user takes any OpenSHMEM program, sees `SHMEM_MAJOR_VERSION 1`,
  `SHMEM_MINOR_VERSION 6`, and gets a wall of implicit-declaration errors, or — for the
  11 spec examples that *do* link — a runtime abort on the first `static` symmetric
  object. *Measured* by `spec-example-coverage.py` in the evidence directory (method: split
  the spec text into its 53 code examples, compare each called `shmem_*` name with the
  exported symbols and header macros): 7 examples build and avoid statics (6, 12, 38,
  42, 43, 51, 55); 11 build but use statics; 35 call a missing routine.
  `shmem_global_exit` alone appears in 13 examples, `shmem_put`/`shmem_p`/`shmem_g` in
  13. (Example 12 builds but depends on `shmem_team_split_2d` succeeding, so really 6.)
- Spec §5 says an implementation "that provides C bindings can claim conformance"; it
  does not define partial conformance, so the version constants are a claim the tree
  cannot yet back. **Suggestion:** a short "Conformance status" section in `README.md`
  generated from, or at least checked against, the export list; and make
  `SHMEM_VERSION` print "subset" until §7.5 lands. **Severity** clarification needed.

### F11. Spec §10 item 4 and the plain→`ctx_` layering

- **Location:** `amo.c`, `rma.c`, `signal.c`, `quiet.c`: every plain routine is a wrapper
  that calls `pshmem_ctx_X`. Spec §10 item 3 requires documentation of layering; item 4
  requires that "wrapper functions must be kept separate from the rest of the library"
  so a tool author can lift them into a profiling library.
- The layering is documented (`export.h:17-20`, design §7.4), which meets item 3. Item 4
  is about language-binding layers (its example is Fortran-on-C), and I do not think it
  binds here; but the wrappers and the definitions share object files, so if it does,
  it is not met. Consequence either way: a tool that interposes only `shmem_ctx_putmem`
  never sees `shmem_putmem` traffic, which is the MPI-style behaviour and defensible.
  **Severity** clarification needed (one sentence in `pshmem.h`), **confidence** low.

## The loud-where-undefined policy, case by case

I looked for the opposite error — aborting on something the spec defines — and did not
find one.

| Library aborts on | Spec text | Verdict |
|---|---|---|
| PE out of range in RMA, AMO, signal, `pe_quiet` (`symmetric.c:25`) | Annex C: undefined | Defensible. Note `shmem_ptr`, `shmem_team_ptr`, `shmem_pe_accessible`, `shmem_team_translate_pe` answer NULL/0/-1 instead, which is right: they are queries |
| Address outside the symmetric heap (`test-bad-put.c`) | Annex C: "may detect a Put to a non-symmetric variable and choose to abort" | Defensible, and the message says statics are not yet supported, which is the honest reason most programs will hit it |
| Misaligned AMO/wait target | The C type guarantees alignment; a misaligned one is a program bug, and an unaligned 64-bit atomic faults on aarch64 anyway | Defensible |
| `SHMEM_CTX_INVALID` in RMA/AMO/signal/session | §9.5, §9.7, §9.8: invalid context is undefined. §9.12.1-2 and §9.9 say "no operation" for fence, quiet, sessions — and those three, and only those, test for it first (`quiet.c:36,44`, `session.c:36,46`) | Correct in both directions. `pe_quiet` also ignores it, which the spec does not ask for but which matches "same semantics as quiet" |
| A team handle the library never issued | §9.4.x: "otherwise invalid, the behavior is undefined" | Defensible. `SHMEM_TEAM_INVALID` gets the defined answer in every routine I checked: -1, -1, nonzero, -1, NULL, nonzero+`CTX_INVALID`, no-op (`test-bad-team.c` and `team.c`) |
| `shmem_team_destroy` of a predefined team, `shmem_ctx_destroy(SHMEM_CTX_DEFAULT)` | §9.4.8 undefined; §9.5.3 only covers created contexts | Defensible |
| `shmem_align` with a bad alignment (`malloc.c:148`) | §9.3.4: "otherwise, the behavior is undefined" | Defensible, and the reason given (layout divergence across PEs) is the right one |
| `shmem_free`/`shmem_realloc` of a non-block | not addressed; undefined by omission | Defensible. Note the abort comes after the entry barrier, so all PEs die together |
| Unknown `cmp` or `sig_op` | not a Table 13 / §9.8.2 value | Defensible |
| Invalid `SHMEM_SYMMETRIC_SIZE` | §8: "returning a nonzero value from `shmem_init_thread` or causing program termination" | Explicitly permitted |
| Zero-length operations | Annex C: `len == 0` with a null pointer is *supported* | Handled: `nelems == 0` returns before any check (`rma.c:24,34`), `putmem_signal` with `nelems == 0` skips the data check and still signals, empty wait sets never validate `ivars` |

The places where the library is **quiet** and arguably should not be are F5 and
`shmem_finalize` with no matching init (`init.c:187`, silent return).

## What is conforming, and the evidence

- **Initialization lifecycle (spec §9.1.1, §9.1.4, §9.1.5, §9.1.13, §9.2).** *Measured*
  with `lifecycle.c` on maps `0,0` and `0,1`, 0 failures: `query_initialized` and
  `query_thread` callable before init; `init_thread(MULTIPLE)` returns 0 with
  `provided == SERIALIZED` and `query_thread` agreeing; a nested `shmem_init`; a
  non-last finalize leaving the heap intact and usable; a nested `init_thread(SINGLE)`
  reporting the first level (permitted by §9.2.1 Notes); the last finalize returning
  the library to uninitialized; re-initialization at a different level with a working
  allocator; `start_pes` twice on top of that, then the matching finalize, then implicit
  finalization at exit with no `/dev/shm` leftovers (the launcher checks). The thread
  constants increase (*measured* at compile time). `init.c:191-194`: every finalize is
  a `barrier_all`, which is §9.1.4's "semantically equivalent to `shmem_barrier_all`".
- **Memory management (spec §9.3).** *Observed in source*, `malloc.c`: zero size (and zero
  count) returns NULL with no barrier; exit barrier for malloc/calloc/align/with_hints
  including on failure (every PE fails alike); entry barrier for free; both for a real
  realloc and only the appropriate one for its degenerate forms; realloc failure leaves
  the block untouched and preserves hints on success (§9.3.3); `calloc` overflow is a
  failed allocation; default alignment 16 with a static assertion against
  `max_align_t`; `shmem_align` returns a multiple of the alignment or NULL. The barriers
  quiet the default context because they are `osoe_barrier_all` (§9.3 preamble).
- **`shmem_ptr`, `shmem_team_ptr`, accessibility (spec §9.1.7-§9.1.10, §7).** *Measured*
  (`team-shared-contract.c`, three maps): SHARED membership and non-null `shmem_ptr`
  coincide exactly; `shmem_team_ptr(SHARED, local index)` equals `shmem_ptr(world
  rank)`; `shmem_ptr` to self is the object. NULL for an AM-carried PE is what §9.1.9's
  "Otherwise, a null pointer is returned" provides for. `shmem_addr_accessible`
  returning 0 for a static is the documented known nonconformance.
- **Completion (spec §9.12).** *Measured* with `completion-probe.c`, 100 rounds of 64 KiB
  with 1 KiB AM chunks and wire splitting on, maps `0,0,0`, `0,1,2`, `0,1,1,0`,
  `0,1,0,1`: after PE 0's `shmem_pe_quiet({1})` returns — with a second put to another
  PE deliberately left unquieted — a **third** PE's get from PE 1 sees every byte. That
  is the "visible to all PEs" half of §9.12.3, which a test reading from the origin or
  the target cannot see. `barrier_all` is quiet-then-sync and `sync` is not
  (`barrier.c:91-118`), matching §9.10.1 Notes and §9.10.3; sync's arrival is a locked
  increment, which covers §9.10.3's "completion and visibility of previously issued
  memory stores".
- **Put-with-signal (spec §9.8).** Same probe: when `shmem_signal_wait_until` returns on
  the target, all 64 KiB of a multi-chunk put are present, 100/100 on every map. The
  signal update is a boundary rmw, so §9.8.1's atomicity against other signal updates,
  `signal_fetch` and the waits is the AMO guarantee. `signal_add`/`signal_set` and their
  `ctx_` forms match the §9.8.5/§9.8.7 synopses.
- **AMOs (spec §9.7, Tables 6-8).** The three type lists in `shmem.h:192-205` match the
  spec's tables entry for entry, and each operation is generated over the right table.
  *Measured* (`amo-signed-values.c`, maps `0,0` and `0,1`): negative operands and
  `cond` values through 32- and 64-bit compare_swap/fetch_add/swap, `INT32_MIN`, float
  bit patterns, `size_t`/`ptrdiff_t`/`int32_t`/`uint64_t` dispatch, the `ctx`-first
  generic spelling, and signed versus unsigned comparison in `shmem_test`.
- **wait/test (spec §9.11).** All six Table 13 constants exist and are integer constant
  expressions (*measured*: used as `case` labels under `-std=c99/c11/c17
  -pedantic-errors -Werror` and C++17). Empty-set answers match the text exactly:
  `wait_until_all` returns, `test_all` 1, `*_any` `SIZE_MAX`, `*_some` 0. `*_some` tests
  every element (required by §9.11.4/§9.11.11). Deprecated `short`/`ushort` scalar
  forms, `shmem_TYPENAME_wait` for the four listed types, and the `long`-typed
  `shmem_wait`/`shmem_wait_until` functions behind the generic macros are all present.
  Spec Examples 38, 42 and 43 run verbatim on three maps.
- **Constants and handles (spec §6, §7).** *Measured* by `header-properties.c`: every
  size constant works as an array bound; handles work in static initializers and compare
  unequal; option bits are disjoint; `_SHMEM_*` spellings present; `shmem.h`,
  `shmemx.h`, `pshmem.h`, `mpp/shmem.h` include cleanly together under C99, C11, C17
  and C++17. Non-spec names in `shmem.h` all carry the `OSOE_` prefix, which is §5's
  exception 2.
- **Environment (spec §8).** *Measured*: `20m` → 20971520; `3.1M` → 3250586 (the spec's
  own example, including the ceiling); `.5m` = `0.5m` = 524288; `20kk` → 20480 and `5mb`
  → 5 MiB (characters after the multiplier ignored, as specified); `1.5` → 2 (ceiling);
  `+5m`, leading space, `12q` rejected with a message naming the value. `SHMEM_VERSION`,
  `SHMEM_INFO`, `SHMEM_DEBUG` and their `SMA_` forms go through the precedence rule
  (*observed in source*, `bootstrap.c:130-132`).
- **Profiling interface (spec §10).** 526 strong `pshmem_` / weak `shmem_` pairs
  (*measured* by `nm`; the tree's own `shmem.profiling_aliases` test asserts the
  binding). No `pshmem_` forms of the generic macros, which item 3 requires. The seven
  unprefixed deprecated names lacking a shifted form is the documented known
  nonconformance, and I agree the spec does not say what the name should be.
- **Library information (spec §9.1.11-12).** Version from the constants; vendor string
  statically asserted to fit `SHMEM_MAX_NAME_LEN` with its terminator.

## What I could not check

- **Thread levels beyond negotiation.** I checked what is *reported*. Whether
  `SERIALIZED` is actually delivered (a different thread per call, §9.2 item 3's
  "a blocked thread will not prevent progress of other threads") belongs to the
  correctness question; `test-thread.c` exists and I did not audit it.
- **§4.1's progress requirement** ("should progress regardless of when that PE next
  engages in an OpenSHMEM call"). The agent thread design answers it; I did not measure
  a compute-bound target.
- **§3.2 atomicity domains.** WORLD and SHARED have separate epoch counters, but AMO
  exclusivity is per word and domain-independent here; nothing to test until a context
  can be created on SHARED.
- **Install layout.** I checked the source include tree, not an installed prefix
  (whether `mpp/` is installed; whether a `pkg-config`/`oshcc` equivalent exists —
  Annex B is informative only).
- **`SHMEM_INFO` on many nodes**, Fortran (removed in 1.5; not applicable), and the
  behaviour of `shmem_finalize` called from a thread other than the initializer
  (undefined, §9.2 item 1).
- F2's put abort and F5 were run twice per case; both are deterministic code paths, not
  timings. F1 was run three times. F9's claim about other implementations is *recalled*.

## Lessons for a successor API

1. **The type cross product is 89% of the symbol table and none of the design.** 467 of
   526 routines differ only in a C type name, all funnel into one `rmw(width, op)` and
   one `wait(width, signed, cmp)`, and the header needs an arity-counting `_Generic`
   trick because the context is a *leading optional* argument. A successor wants
   width-typed entry points (`u32`/`u64`, bytes for RMA) and a mandatory context
   argument; the profiling name-shift then doubles dozens of symbols, not hundreds.
2. **`SHMEM_TEAM_SHARED` is defined in terms of `shmem_ptr`**, which welds a locality
   *policy* (who may I load/store) to a team *membership* and to a separate atomicity
   domain with its own collective state. F2 exists only because of that weld. Expose
   locality as a query on (pe) and let teams be plain PE sets.
3. **"Memory stores" under fence and quiet** (Table 14) forces every ordering routine
   to begin with a serializing store and forces `shmem_ptr` to return NULL wherever the
   transport cannot order the caller's stores — the reason an AM-carried node-mate can
   never be handed a pointer. A successor should make raw-pointer access explicitly
   unordered.
4. **Every allocation is a world barrier plus a default-context quiet.** That is
   O(job) synchronization per `malloc` at a million PEs, mandated by the API shape, not
   by any implementation need here — the allocator is already deterministic and needs
   no agreement. A successor could make allocation local-and-deterministic with an
   explicit, separate "publish" collective, or team-scoped.
5. **Implicit finalization (`start_pes`) makes process exit a collective**, which is
   exactly F1. Anything that must run at `exit()` cannot be allowed to block on peers.
   Drop it, and specify what an abort path may and may not do.
6. **Wait-on-any-address** means the library cannot know which words are waited on
   until a wait is published; the §4.9 watch mechanism, exact-notification rule and
   `set_range` hints all exist to recover that. A successor with first-class
   waitable objects (created, not pointed at) removes the mechanism and gives an RDMA
   backend a registration target.
7. **Undefined-by-silence is everywhere** (uninitialized calls, invalid handles, invalid
   PEs). The project's loud policy is the right response, but it had to be decided
   thirty times. A successor should define the error behaviour once: "invalid argument
   → job abort with diagnostic", and give `init` a return code that is actually used.
8. **Deprecated surface the spec still mandates** (F6, F8, pSync constants, `SMA_*`,
   `_SHMEM_*`, `SHMEM_CACHE`) is pure cost. A successor needs a removal policy with
   dates.
9. **Scale pointer, not a conformance finding:** `shmem_team_translate_pe` and
   `shmem_ptr` resolve through `osoe.node.local_of_rank`, a private
   `malloc(4 × world_size)` per PE (`node.c:391`, *observed in source*): 4 MiB per PE
   and about 1 GiB per 256-PE node at 1M PEs, for a table that is -1 almost everywhere.
   The API does not force it — the node's `gtids` list already answers the question in
   O(local) — but the API's flat integer PE space is what makes the dense table the
   obvious first implementation.
