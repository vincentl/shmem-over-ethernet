# 2026-10-10: v0.2 step 4's merge point

The merge point of v0.2's fourth step
(`docs/prompts/v0.2-step4-progress-helpers-prompt.md`, "The merge point"),
with Parts 0 to 3 on `main` at `c897948`. It covers four things:

- **The brief's runs.**
- **The arc's second full review** - round eight -
  `docs/reviews/archive/v0.2-step4-review.md` and its five reports.
- **The review's fixes.**
- **The author's items of 2026-10-09** (`docs/decisions/2026-10-09.md`):
  - design §3.12 rewritten as built (item 6);
  - the test that every `OSOE_` setting read is in the README (item 5);
  - question 4's exploration (item 1).

  The release notes' line on 0.1.0's pacer hang (item 4) is the tag's.

Part 4, a fast link, is in 0.2's scope and not built; it waits on the link
(item 3).

**Where it stands.** Merged into `main` at `f9efd10` on 2026-10-10, the
branch's tree whole; the brief moved to `docs/prompts/done/` after it.

## The brief's runs

At `c897948`, the lead's (`docs/design/evidence/v0.2-step4/2026-10-10-merge-point/`):

- **The four presets, tier 3 and the models at no helpers, and debug, AddressSanitizer and ThreadSanitizer at two and three:**
  - Part 3's verification at `232278a`, whose code `c897948` is
    (`2026-10-08-helpers.md`, "The verification").
  - The release preset at two and at three helpers ran here: 855 of 855 at
    each (`presets-helpers.sh`).
- **The SOS test suite at 4 × 2, no helpers:** the release point's results,
  as the runbook gives them (`sos.sh`):
  - 161 programs built and exited 0 on every map;
  - the two C11 `short *` programs that fail to build;
  - `b2b` and `shmemalign` failing for their known reasons.
- **SOS at two helpers:** 160.
  - `unit/spam` passed the sweep's 120 s launcher limit with no message of
    osoe's. The host then ran at a load of 68 on 8 cores: both VMs, and three
    of the review's investigators running models.
  - Alone on the quiet VM the next morning, `spam` passed six runs of six,
    alternating no helpers and two, in 25 to 32 s (`spam-repro.sh`,
    `spam-quiet/`). The review's conformance investigator ran it at two and
    three helpers, passing too.
  - The sweep's failure was the load.
- **The acceptance programs at 70 PEs:** the SOS team programs and the
  `split_shared` probe at 61, 62 and 64 nodes. Each clean at no helpers and at
  two, in the release point's 5 s (`acceptance.sh`).
- **The Pi cluster:** the merge point against step 3's (`512e913`), no helpers,
  alternating three times; `pi-compare.sh`, `pi-vs-step3-comparison.txt`.
  - **A barrier of two PEs behind a 1 MiB put stream:** 1,665 to 1,669 µs at
    the median, where it was 18,322 to 19,917. The p99 was 2,225 to 2,230
    against 28,776 to 29,896. This is the control plane's barrier number.
  - **A fetch-add behind the stream:** 3,871 to 3,883 µs, where it was 17,155
    to 19,931.
  - **The sending agent's CPU:** 0.29 to 0.31 s a GiB, against 0.38 to 0.40.
  - **Unchanged:**
    - the stream at 112.1 MiB/s;
    - every remote line of `osoe-bench` within 0.7%, its small latencies about
      a microsecond longer;
    - the mailbox at 35,205 to 38,173 messages a second, against 35,115 to
      37,496;
    - `shmem_init` within its spread.

## The review

Five investigators at once against `c897948`. Two stopped at the session's
usage limit with their reports nearly written, and were resumed the next
morning from their own context. Then fresh refuters for the two findings
rated *fix before release*, and the synthesis.

**The verdict:** release after named changes, the one named change made
below; nothing blocks.

- **Q5-1 stood after refutation:** 82 evidence programs without the licence
  line.
- **The other *fix before release*, the shape `fixed` leaving every data
  wire's unsent limit at its 16 KiB floor,** was lowered to minor. Its own
  investigator lowered it after re-measuring on a quiet VM, and the refuter
  independently.

The synthesis ranks the rest and gives each finding's fate.

**A correction to Part 3's record.** Part 3's first adversarial read found "no
word has two plain writers" (`2026-10-08-helpers.md`, "The adversarial read").
The review found one (correctness Q2-4), in step 3's code:

- **The word:** each of a lead's ⌈log₂ N⌉ receivers puts the same 16-byte head -
  epoch, collective, bytes - into the lead's reduce-scatter grant.
- **Who writes it:** with helpers, those puts come from different helpers, at
  once, with nothing ordering them.
- **Why it is harmless:** the bytes are identical, and the reader reads after
  every writer's fenced add, so nothing goes wrong.
- **Where it shows:** it is 39 of the 95 ThreadSanitizer reports Part 3's
  verification passed. Its verification read them as one slot's reuse.
- **What follows:**
  - **The brief's stop-and-ask, "a word two owners must write",** names this
    kind. It is a word two owners do write, not one they must, so the design's
    rule is not false; the code departs from it.
  - **One writer** is on the release checklist, with step 3's field per
    receiver. The synthesis puts the question of doing it before the tag to the
    author.

## The fixes

On the branch `step4-items`, after the author's two items (`cca1c3a`: design
§3.12 rewritten as built, and `shmem.settings-documented`). The review's ones
are:

**The licence line** (readiness Q5-1):

- `SPDX-License-Identifier: LGPL-3.0-or-later` is now in the 82 programs and
  scripts v0.2's steps 2 to 4 added to the evidence without it:
  - first in the file, or second under a shebang;
  - last in the two whose lines a review cites by number
    (`2026-10-07-profile/classify.py`,
    `2026-10-08-helpers/verification/tsan-gate.py`);
  - shell parsed by `bash -n` and Python by its parser, unchanged.
- **`shmem.license-lines`** (`cmake/CheckLicenseLines.cmake`):
  - **What it does:** it globs the project's directories and needs the line in
    every file of the kinds that carried it on 2026-10-02, but the
    specification's four examples. It globs rather than asking git, since the VM
    has none.
  - **Shown to fail** on a copy of the tree with:
    - a script's line removed;
    - a new script without it;
    - one of the four examples gone.
  - **It counted** 682 files, every one with the line, before question 4's
    evidence (below) added two more, which carry it.
  - **The README's licence paragraph and `docs/README.md`'s record of the
    evidence's edits** say so.

**The README, made true where the review found it behind the tree**:

- **The settings rows:**
  - `tree` measures the link too;
  - the shape `fixed` leaves each data wire's unsent limit at its 16 KiB floor
    unless `OSOE_LINK_*` are set;
  - the unsent limit is a data wire's, a control wire keeping the system's own;
  - the zero-copy refusal's 3,274 nodes are at the defaults with no helpers,
    its "512 KiB a round trip" and "seconds" corrected;
  - `OSOE_AM_CHUNK`, `OSOE_AM_BUFFER` and the budget are *checked* among a
    node's PEs;
  - `SHMEM_DEBUG` prints the unsent limit;
  - the testing hooks are read by a lead of more than one node alone.
- **The single-node build:** its settings preamble names the helpers', the
  shape's and the link's settings among those that do nothing there - as does
  `SHMEM_INFO`'s note in that build (`cmake/OsoeVersion.cmake`).
- **The rendezvous** compares the shape and the link set by hand.
- **The Limits section:**
  - a blocking get between nodes moves a piece at a time (question 4, below);
  - under `srun -K1`, callers of `shmem_global_exit` on several nodes can lose
    output (conformance C4);
  - "a helper thread" in the deadlock limit is a thread of the program's own,
    osoe's helpers not counting (Q5-4);
  - past 8,192 nodes a node's ring cannot be made;
  - helpers' cost on four Pi nodes (scale S4).
- **The scaling section:**
  - Träff's reduction runs on at most 512 nodes at the default fan-out ceiling,
    65,536 only with a higher one (scale S2);
  - a node pair's puts and gets go by one connection, so a network that caps
    one flow binds (S7).
- **`osoe-launch`'s paragraph:** a job of tens of logical nodes on an
  overloaded machine can end as a vanished host, its keepalive probes dropped
  by the loopback, and `OSOE_KEEPALIVE=0` loses nothing there (correctness
  Q2-1).
- **"How osoe is tested":**
  - what ThreadSanitizer's reports at a helper count are, and what its gate
    cannot tell apart (Q2-2);
  - the deadlock detector's helper half held by its model alone (Q2-3);
  - which presets run at two and three helpers.
- **The quoted numbers** given the tree they were taken on, or taken again
  (scale S6):
  - the loopback stream, which holds at the tip on a quiet VM: 10.1 to 10.8
    GiB/s, 0.71 to 0.76;
  - the reopening's wakes;
  - Träff's 2 and 4 MiB;
  - the uncapped fetch-add's 17 to 20 ms.
- **The death message the README quotes** is the one printed: "ended its control
  wire without its goodbye", with a helper's line beside it at helpers (Q5-3).
- **The x86-ordered run of steps 2 to 4** (portability P3).
- **The provenance sentences:** ten merges and one kept work-in-progress commit
  without the model's line; seven review points (Q5-5).
- **Building's numbers**, measured again (below).

**The design notes:**

- §3.3's unsent limit: a data wire's, set by each owner, and the floor under
  `fixed`.
- Invariant 19 and the ring's passage, for owners: each owner of the node's
  wires submits to its own ring, and alone owns its wires' sockets.
- §3.15's keepalive no longer says contention cannot stop it.

**The code, comments and one message:**

- **Comments:** `init.c`'s descriptor comment (two sockets a peer, about 385
  nodes; P4); `wire.c`'s keepalive comment; `wire-settings.h`'s two.
- **The message:** the helper-count refusal cites §3.12 rather than a header.

**The indexes** (Q5-6):

- the reviews guide;
- the evidence count in `docs/README.md`;
- a row for step 3's Pi session;
- an index of step 4's evidence;
- the helpers record's evidence directory;
- a step 4 section in `docs/pi-cluster-results.md`;
- the decisions index: its line on the large reductions, a line for the
  settings check, and the licence check on the licence line.

**The release checklist** takes the after-release items, in one list ("From
the review of v0.2 step 4's merge point").

## Question 4

Question 4's exploration (`docs/design/blocking-get-proposal.md`, for the
author's decision; evidence in
`docs/design/evidence/v0.2-step4/2026-10-09-question4/`) found two things:

- **On the Pi,** a blocking get of 64 KiB or more takes 1.23 to 1.28 times a
  put's time, and a nonblocking get and a quiet 1.00 to 1.05.
- **Over the quiet VM's loopback,** from a mebibyte a blocking get takes 6 to 10
  times and a nonblocking get 2.7 to 3.5. The nonblocking get is held there by
  the landing zone's four pieces.

It recommends issuing a blocking get's pieces as a nonblocking get does,
models first, before Part 4's measurements, and putting the landing zone's
size to Part 4. The README's limits now state the blocking get's cost, as
the 2026-10-07 record asked until it is built.

## The author's answers to the synthesis's questions

On the morning of 2026-10-10, to the synthesis's three questions:

1. **0.2 is not tagged yet.** The author holds the tag to see whether more
   hardware - a link above a gigabit, for Part 4 - can be had.
2. **Keepalive on one machine's loopback** (correctness Q2-1): the author asked
   for more, and was told the mechanism, the evidence and the fix.
   - **The mechanism:** the loopback's backlog drops the probes of thousands of
     idle sockets when the machine is overloaded. On one machine keepalive can
     only give a false verdict: a dead logical node's sockets are closed by the
     kernel they share, and a stopped one's kernel still answers.
   - **The fix recommended:** keepalive off by default when every peer is a
     loopback address, unless `OSOE_KEEPALIVE` is set. It goes in the bootstrap
     rather than in `osoe-launch`, since tier 3's namespaced nodes have links
     of their own, where keepalive is tested.
   - Open until the author says.
3. **The reduce-scatter grant's head** (correctness Q2-4; the brief's
   stop-and-ask): to be changed, not only documented, right after this
   merge.
   - **The change:** a head field for each receiver, which the lead checks one
     by one. This ends the word two helpers write, and step 3's open item with
     it - only the last receiver's head surviving, so receivers that disagree
     can pass.
   - **Its model first**, then its own verification and read.

## Building's numbers

Measured again for readiness Q5-2 and portability P2, at the fixes' tree before
its last documents, on the development VM idle (load 0.02, four cores;
`docs/design/evidence/v0.2-step4/2026-10-10-merge-point/building.sh` and `building/`).

- **The release workflow as the README gives it,** one file and one test at a
  time:
  - the build in 67 s, the 857 tests in 599 s, every one passing;
  - the README: about eleven minutes, one for the build and ten for the
    tests.
- **The counts:**
  - the full build 897 tests - 857 without the models, 458 of them `internode`,
    and 40 models;
  - the single-node build 404 without the models, the full build's 399 and its
    own five.
- **The models four at a time:** 1,087 s, and `model.abort` was killed by the
  kernel for want of memory - 39 of 40.
  - **Peak resident memory**, sampled each second: `abort_exit` 2,542 MiB,
    `circulant` 1,771, `posted_bitmap` 1,174, `reservation` 1,063; `abort` at
    1,704 when the kernel stopped it, still rising. Run alone to its end, by the
    fixes' adversarial read, it peaked at 3,051 MiB, the largest.
  - Four at once want about 8 GiB at their worst, and the VM's 8 GiB held, besides, 2.8 GB of
    RAM-backed `/tmp`, 1.9 GB of it Part 0's build trees, removed since.
  - **Part 3's verification ran them one at a time** in 52 minutes, the longest
    `model.deadlock_owners` at 11.
  - **The README:** 52 minutes one at a time, two at a time on 8 GiB and one at
    a time on 4 GiB, the largest 3 GiB. The `models` preset is labelled
    "about an hour, one at a time", for the half an hour of the 30 models'
    days.
- **A container's defaults,** emulated as round five did:
  - **The emulation:** a private 64 MiB tmpfs on `/dev/shm` in a mount
    namespace, and io_uring refused by the tree's `deny-io-uring`. The release
    suite ran one test at a time.
  - **The result:** 675 of 857 failed. The run kept no test's output, so three
    of the 675 were run again the same way with theirs, and each was a
    container's refusal: `/dev/shm` "has 62 MiB free" against the 271 MiB one PE
    needs, or io_uring "Operation not permitted" named (`container-spot.sh`,
    `building/container-spot.txt`).
  - **The README:** about 680, where it said 560, an estimate on the 729-test
    suite.

## The verification

**At `4f1014d`, the fixes' tree** (`docs/design/evidence/v0.2-step4/2026-10-10-merge-point/verify.sh`;
its outputs in `verify-4f1014d/` beside it). The static checks ran first, then
Part 3's verification with the release preset added at two and three helpers.

- **The static checks:** 12 of 12.
- **debug and release:** 857 of 857 at no helpers, two and three; release with
  the floor pins on.
- **AddressSanitizer:** 842 of 842 at two and three helpers, and 841 at none.
  `shmem.helper-wake.0-1-2-3` failed there once: node 3's wire's owner woken
  twice in a hundred rounds, where ten are asked, and the other helper never.
  It is the load S1 saw (below).
- **ThreadSanitizer:** 600 of 600 at no helpers. At two helpers 12 tests
  failed, at three 19, every one the tool's reports alone in a heap mapping, as
  the gate (`tsan-gate.py`, Part 3's) judged them. *Corrected 2026-10-11:* the
  gate could not show that. The tool ends a process that has reported anything
  with its own status, 66, and the gate read any 66 as the reports alone, so a
  failed check or a crash beside a race passed it, and a race in a test that
  passed was never read (the ninth review's C9-1). Run again so that the
  tool's status was left to each process and its reports read from files, by
  that review's refuter on `main` at `3b45472` with a gate of its own, the
  suite passed its own checks at every count, but for one test's timeout in the
  tool's slow path; its races were all in heap mappings and named a helper's
  thread, and tests that end a lead left helpers' threads unjoined
  (`docs/reviews/archive/v0.2-before-part4-refutations.md`) - which the gate in
  `ctest` since 2026-10-11 (`cmake/OsoeThreadSanitizer.cmake`) fails, and the
  change beside it removed (`docs/decisions/2026-10-11.md`).
  - Of the reports, 22 of 42 at two helpers and 49 of 87 at three were two
    helpers' landings of one word, every one in a Träff's reduction's grant:
    correctness Q2-4's word, which the next change removes
    (`tsan-*-classified.txt`).
  - Three quarters to four fifths of the reports lay in the library's own words
    at the end of the heap mapping; at `232278a`, two thirds. The README's
    paragraph takes both runs: a dozen to twenty tests, two thirds to four
    fifths.
- **Tier 3:** 113 of 113.
- **The 40 models,** one at a time: every one.
- **`osoe-pacer` at no helpers and two:** `mix`, `mailbox` and `collectives` with
  the Pi's link a minute each, and `mix` ten minutes, every one `failures=0` and
  no deadlock.

**`shmem.helper-wake`'s failure,** and `d91804e`.

- **What happened:** the test counts each helper's wakes while PE 0 puts to
  each peer node a hundred times with a quiet after each put. It asks the wire's
  owner to have been woken ten times and the other helper at most twice.
- **Why:** the owner can still be awake when the next put comes - more so on a
  loaded machine, where it can be preempted between publishing a round's
  acknowledgement and going to sleep. A wake sent to the wrong helper would show
  as the other count, which stayed at 0.
- **Why not a looser bound:** the scale review's mutant, a helper that spins
  200 µs before it sleeps, woke its owner one to three times. That is the same
  count, so a lower bound would lose the one test that guards a helper against
  spinning (S1).
- **The fix, `d91804e`:** a peer's hundred rounds that wake their owner fewer
  than ten times are run again, three attempts in all. A spinning helper falls
  short in each; a wake sent elsewhere fails at once.
- **Checked** (`helper-wake-check.sh`, `helper-wake-check/`):
  - **Alone:** 20 of 20 in release and 20 of 20 under AddressSanitizer, no run
    needing a second attempt.
  - **The scale review's mutant,** a helper spinning before it sleeps: at 200 µs
    and at 30 µs, 10 of 10 failing each, every attempt short.
  - **Under AddressSanitizer beside four busy loops on the VM's four cores:** 9
    of 20 failed, retries and all - 66 attempts past the first.
- **What the retries do:** they hold the test under the suite's own load, four
  tests at a time, which is where it failed, but not on a machine its busy loops
  overload. There a helper preempted on every round looks like one that spins,
  and only the owner's sleeping flag could tell them apart.
- **What follows:** the release checklist asks for a test that waits for the
  owner to sleep before each put before the tag's run of the suite under load,
  and the README says what the test asks of its machine.

**At the tip that merges** (`verify-final.sh`, `verify-final/`). What changed
since `4f1014d` is the test's retries, comments and documents. So the tip ran:

- the static checks;
- the four presets built, with their warnings counted;
- `shmem.helper-wake` three times in each;
- the single-node build, which the full verification does not build,
  configured, built and its suite run - the one build `SHMEM_INFO`'s note
  compiles in.

At `8e36752`, the tip that merges, every part passed:

- **The static checks:** 12 of 12.
- **The four presets:** each built without a warning.
- **`shmem.helper-wake`:** three runs in each of debug, release and
  AddressSanitizer, every one passing - ThreadSanitizer's preset leaves it out.
- **The single-node build:** built without a warning, its 404 tests every one
  passing, and `SHMEM_INFO`'s note in it naming the helpers', the shape's and
  the link's settings among those it checks as values only.

## The fixes' adversarial read

A fresh agent read `git diff c897948 4f1014d` against the synthesis and this
record, and ran the checks broken on copies
(`docs/reviews/archive/v0.2-step4-review-fixes-adversarial-read.md`).

**Nothing fatal; nine should-fix, each a sentence or a number, all made:**

1. §3.12's "application threads ask the lead", false with helpers.
2. §3.12's helpers on the Pi, which copied the record's paragraph that left out
   the lines that moved - the paragraph completed with a note too.
3. The README's "no test can reach" the detector's helper half, past the 126
   tests the review ran.
4. The global-exit limit, which `osoe-launch` without `-k` meets too.
5. The zero-copy send's locked-memory limit, absent from its row.
6. Two numbers for helpers on the Pi wider than their evidence.
7. The models' memory, taken from a run where `model.abort` was killed while
   still growing: alone, it peaked at 3,051 MiB, the largest.
8. This record's `a1c1f14`, which the branch holds as `cca1c3a`.
9. "Four proposals" and the reviews row, behind the new proposal and evidence.

**Of the eleven notes, made:**

- N1, the comments that still put the unsent limit on every wire or in the
  agent's hands;
- N2, §3.12's goodbyes and owner 0's wake word;
- N4, the suite's count given two ways;
- N5, the synthesis's three omissions;
- N7, the provenance's reason for `de1e2f8`;
- N8, keepalive's claim "between nodes" made a claim of what was seen;
- N11, 2.7 to 3.5, and the container's three looked at.

**Not changed:**

- **N6,** a contraction in an evidence program (`get-pieces.c`'s `reps`). The
  evidence records what ran, and is edited only as `docs/README.md` says.
- **N9 and N10,** a verification at the tip and the single-node build. They are
  the tip's run above.
