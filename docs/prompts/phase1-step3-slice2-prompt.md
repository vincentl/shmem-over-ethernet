# Implement phase 1, step 3, second slice: the waits get their words

You are continuing **§7.1 step 3** of **osoe** (OpenSHMEM Over Ethernet). Slice 1
is merged (`83f0a23`): every node pair has a TCP connection, the lead's agent
drives them through one io_uring and waits on its sockets and its wake word in
one submission, and the leads meet over sockets in a tree. What it measured is
this slice's starting point (§8.4, "The library against its floor"):

- a get costs **2.05×** `echo-uring`'s round trip, which is the two extra wakes
  a library round trip needs — the design's cost, not a defect;
- a put followed by quiet costs **4.0×**, and a stream runs at **3%** of the
  floor, and both are one step-2 stand-in: three waits in `transport-am.c`
  still wake the lead and then sleep 20 µs to look again (`poll_lead`). With a
  yield in its place, in a scratch build, they measured 1.1× and 34–60%.

**This slice builds the waits §3.11 and §4.6 specify, pins the library to its
floor, and puts the transport on a link that delays, drops and fragments.**

The author's goals, which are the axes to judge your own work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

Machine setup: "Setting up on a new machine" in `phase1-step2-slice1-prompt.md`.
Meaning of phase/step/slice: `docs/prompts/README.md`.

## Read first

1. `CLAUDE.md`.
2. Design §3.11 (one futex word per resource class, never shared; the wake
   count tracks divisibility; the handoff bit, and "model the word layout
   before implementing it"; no spinning before blocking), §4.6 (quiet, the
   acknowledged sequence it waits on, and §4.6's "waiting on 64-bit counters":
   bump a companion word, then wake), §3.5 (reservation), §3.8 (reply credits,
   the DRAINING cap), §3.10 (the lead's claimed wake: the same problem in the
   other direction), §3.16 (memory orders and their table), §3.14 ("waiters per
   wake word" is on its list), §8.2–§8.4.
3. `shmem/src/transport-am.c`: `poll_lead` and its three callers —
   `reserve_waiting`, `take_credit`, and quiet's wait in
   `osoe_transport_wait` — and `am_wait_*`, which every wait must keep using.
   Then where each predicate moves: `progress.c` (buffers turn over, credits
   come back as replies land, `acked` advances), `connection.[ch]`,
   `progress-host.c`.
4. `tools/modelcheck/lead_wake_model.py` and `chained_wake_model.py`: the two
   wake protocols already modelled, one in each direction.
5. The slice-1 commits `5c95b0e` (the measurements) and `c825990` (the
   rendezvous, and what its model found when the code came first).

**`docs/reviews/` is a committed record: read it, never edit it.**

## What slice 1 left for this slice, deliberately

- **`poll_lead`, in three waits.** Its own comment says it must not survive
  step 3, and at 256 PEs in one quiet it is 256 locked adds and 256 syscalls
  per period on one line (second review, atomics F3). Its cost is measured
  above. It is the only item of §7.1 step 3's list not yet built — "per-class
  wake words".
- **The library-to-floor ratios are measured and not pinned** (§1, §8.4, §10),
  because a pin before this slice would have pinned the stand-in.
- **Tier 3 runs only the floor harness.** `tools/echo-uring/netem-sweep.sh`
  runs `echo-uring` across two network namespaces; none of the library's tests
  has met delay, loss or a small MTU (§8.1's "what loopback does not test").
- **Under load, a library round trip costs its wakes**: 31 µs against the
  harness's 4 beside a parallel build. That is the design (§3.12: only the lead
  drives the transport), not a defect. Measure it on a Pi-like link in part C;
  do not redesign it here.

Out of scope, and not to be rediscovered as defects: the eager mesh (§6.2 says
what on-demand connections would need), spinning before blocking (§3.11 rules
it out in phase 1), and step 4 — liveness, abort propagation,
`shmem_global_exit`.

## Order of work

Each part should be able to fail and be corrected before the next depends on
it.

### Part A — one word per wait, as §3.11 specifies

Replace `poll_lead` in all three waits. §3.11 and §4.6 give the shape: each
resource class gets its own 32-bit word per connection, never shared; the
progress thread moves the predicate, then bumps the word, then wakes; a waiter
loads the word, then rechecks its predicate, then sleeps on the word at the
value it loaded, sweeping as every wait does (`am_wait_*`, §3.15). The classes
map onto the three waits as acknowledgements (quiet), buffer turnover
(reservation) and reply credits; the model decides whether §3.11's "buffer
space" and "a reservable buffer" need separate words here.

Two things the specification asks for and the stand-in never had to answer:

- **No syscall per event when nobody waits.** An acknowledgement lands on
  every quiet's path; a `futex_wake` for each is the cost F3 named. So the
  progress thread must know whether anyone waits on a word, which makes each
  wait a two-sided protocol — like §3.10's claimed `lead_sleeping`, in the
  other direction, and with many waiters rather than one.
- **The wake count tracks divisibility.** Buffer space and acknowledgements
  wake every waiter, since each checks its own target; a credit wakes one, and
  gets §3.11's handoff bit, so a thread already running cannot barge past the
  one that was woken.

**Model it before writing it**, with the trapped-state check on, and with a
broken variant for each rule: the waiter count read before the word's bump; a
waiter registering after its recheck; wake-one on a divisible class; no
handoff bit, where a barging thread strands the woken one; two classes sharing
a word (review C4). Derive the orders from the model under C11, keep them
minimal, and add a §3.16 row for each new shape. Export waiters per word
through §3.14 if a layout bump is needed for it, and say what an older
`osoe-stat` does.

Then measure put and quiet, and the stream, against `echo-uring`, idle and
beside a parallel build, the way slice 1 did (`5c95b0e`).

### Part B — pin the library to its floor

§1's first two rows, as a registered test in the release preset beside
`env.echo-uring`: a get, a fetch-add, a put and quiet, and a stream, each
against `echo-uring` on the same link. Pin what §8.4 found stable — the
fastest trips measured side by side in pairs, the median of the pairs — and
report the medians as findings, not pins. Choose the pins after measuring, not
before. Check them by breaking the library: put `poll_lead`'s sleep back, and
the put-and-quiet and stream pins must fail. Run them through the load loop.

### Part C — tier 3: a link that delays, drops and fragments

The tier-2 multi-node tests, run across network namespaces over veth with
`netem` (§8.2, §8.3): a Pi-like link (1 Gbit/s, 100 µs each way), loss, and a
small MTU. Each logical node's PEs share one namespace, and its lead listens
on that namespace's address, which the host list names — so `osoe-launch`
needs a way to start each logical node in its own namespace. It needs root and
changes the machine's network state while it runs, so it is not a default
ctest: say how it is registered and run, and clean up after itself as
`netem-sweep.sh` does. Choose the subset of tests that exercises the transport
(the internode maps, the tree, the differential's multi-node maps), and say why.
Measure the library's ratios on the Pi-like link: there the wire should
dominate the wakes, and whether it does is a result.

### Part D — what the notes must now say

§7.1 step 3 is done or says what is left; §10's "pin the §1 floor ratios"
moves to closed or says why not; §8.1–§8.2 say what tier 3 now tests.

## Rules

The Rules and Testing sections of `phase1-step2-slice1-prompt.md`, as amended by
the later slices, apply. In addition:

- **Complexity is a correctness criterion.** State in every commit message the
  cost in N, L, P and T, in time and memory, per PE and per node. A wake that
  costs O(waiters) where one would do, or a word per PE per connection, is a
  defect at 256 PEs per node.
- **Model any new protocol before writing it**, with a broken variant that fails
  the property it targets and the trapped-state check on. Slice 1 wrote the
  rendezvous before its model, and the model then found a hang the tests had
  missed; this slice's waits are exactly the kind of protocol that loses wakes.
- **Memory orders come from the C11 model and are minimal** — §3.16's table, and
  a row for any new shape.
- **Run the load loop** (`build/scratch/load/load-loop.sh`, or `load-some.sh`
  for a subset) before calling any part done, and never while editing the tree:
  `shmem.boundary`, `shmem.strict-c11` and `shmem.static-link` read or rebuild
  the source, and a mid-edit tree fails them.
- Layout bumps: at most one per part; say what an older `osoe-stat` does. A
  separate branch fixes `osoe-stat`'s decoding of the watch word for layouts
  10 to 13 (`claude/friendly-feistel-ebc97d`); rebase onto it if it lands first.

## Working method

- State a short plan, then proceed. **Do not stop for approval. Stop only on a
  surprise**, saying what you expected, what happened, and the options:
  - a model contradicts §3.11's design, or a pinned decision;
  - a wait's word cannot be added without a change above the boundary;
  - with `poll_lead` gone, a put and quiet still costs more than about 1.5× the
    floor's round trip idle — the wakes, not the waits, then dominate, and that
    bears on §3.11's "no spinning" and on §3.12;
  - tier 3 finds a defect that loopback hid and that is a design question
    rather than a bug below the boundary;
  - tier 3 cannot run without leaving the machine changed.
- Branch `phase1-step3-slice2`. Small commits, warning-free, all four presets
  green in the VM, the load loop on every test added or changed, messages that
  say what was verified and how, the design doc changed in the commit that makes
  it true.
- Short progress notes at the end of each part.
- When done: summarize commits, verification, design-doc changes, measurements
  with their caveats, deferrals, open questions; add a row to
  `docs/prompts/README.md`; and add to `docs/design/successor-api-stage-setting.md`
  §2 any lesson this slice paid for. **Ask before merging into `main`.**
