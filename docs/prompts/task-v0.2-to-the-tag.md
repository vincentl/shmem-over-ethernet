# v0.2 to its tag: every open item, Part 4 on Google Cloud, round ten

A brief for the sessions that take osoe from round nine's merge to the 0.2 tag,
written on 2026-10-11 from the author's direction of that day: **0.2 clears every
open item - nothing is held "for after the release".** Read it whole, then
`v0.2-open-items.md` beside it, which lists the 82 items by number; this brief
says in what order to do them, how, on which machines, and what the author must
decide first.

## Where it starts

- **`main` with round nine and its changes merged**: round nine's synthesis
  (`docs/reviews/archive/v0.2-before-part4-review.md`, *release after named
  changes*), its changes and their reads, the record
  `docs/decisions/2026-10-11.md`. If the branch `review-nine-fixes` is still
  open, finish it first: its plan is
  `<repo>/build/scratch/review-nine-fixes/plan.md`, its full verification
  script `verify-fixes.sh` beside it, and the auto-memory's newest handoff says
  where it stood.
- **The tag waits for Part 4** (`docs/decisions/2026-10-10.md` item 3), and now
  for every item of `v0.2-open-items.md`.
- **Not here**: the successor API (`docs/design/core-api-proposal.md`, another
  session's, uncommitted in the main checkout - leave it), and anything the
  author's answers below put out of 0.2.

## The machines

**Google Cloud, eight `c4-standard-16`** (the author, 2026-10-11, after weighing
`c2-standard-8`'s price): 16 vCPUs - eight cores - 60 GB, up to 23 Gbit/s of
egress (measure it: §2's facts and §4's bare stream). The account, every launch
and every deletion are the author's.

- **OS: Debian 13** (Google's `debian-13` image) on every node: the development
  VM's, with liburing 2.9, CMake 3.31 (the ThreadSanitizer preset needs 3.28),
  Slurm 24.11, GCC 14, Linux 6.12 - which does not charge io_uring's rings to the
  locked-memory limit; the Pi's 6.18 covers that. gVNIC, a compact placement
  policy, and one VPC whose MTU is 8896 (the author, 2026-10-11) in every
  session: a change between sessions makes their numbers incomparable.
- **A head node**, the author's choice of 2026-10-11 (his Terraform:
  `slurm-controller`, an `e2-standard-4`): it runs Slurm's controller, the
  builds' copying and the lead's `ssh`, so that no measured node does -
  `slurm-setup.sh` with `CONTROLLER` set, `build.sh` from it, which copies the
  tree to every node with `rsync` and builds it there. The head node exports
  `/shared` (NFS, root squashed) as a site's would; the kit does not use it - on
  the Pi, tests run from NFSv4 stalled in its opens under load
  (`docs/decisions/2026-10-11.md`, "On the Pi cluster").
- **Access the lead needs**: `ssh` from the author's Mac to the head node through
  IAP - an entry in the Mac's ssh configuration whose `ProxyCommand` is gcloud's
  IAP tunnel, the lead running no `gcloud` command itself - as the author's OS
  Login user, which also gives one user id on every node, as Slurm needs; the
  agent forwarded from the head node to every compute node; passwordless `sudo`
  on every node (OS Login's admin role); all TCP between the nodes; `apt` out
  through Cloud NAT, no node having an external address. No `gcloud`
  credentials of the lead's own: the author gives the names and each session's
  window.
- **A custom image** after the first setup - the packages, Slurm's daemons
  stopped, `RemoveIPC=no`, the limits - saves each later session its half hour.
- **The kit is written for `c4-standard-16`**, its 8, 16 and 32 PEs a node one
  PE a core, one a vCPU and two a vCPU. It was set for `c2-standard-8` on
  2026-10-11 (branch `kit-c2-standard-8`) and set back the same day when the
  author chose the eight cores (branch `kit-c4-standard-16`), keeping that
  change's fixes.

**The Pi cluster** for its items (P4, H22, T6, T9 to T12): only `~/osoe`;
`ssh` through the Mac; never to the compute nodes; one session held open while
jobs start (`docs/pi-cluster.md`). **The development VMs**: `osoe` (with Slurm),
`osoe-load`, `osoe-rocky`, `osoe-x86`; every run under `/tmp/osoe-vm.lock`.

## The order of the work

1. **Now, in the VMs, while the cluster is made** - the 128 items that need no
   cluster, in slices of related items, each slice a branch:
   - **the kit for the cluster's machine** - done on 2026-10-11, set for
     `c2-standard-8` and back to `c4-standard-16` - before the first session;
   - **the protocols' large items, models first**: the global exit's single
     carrier (P1), the hand-on wake (P2), Träff's rows apart from the ceiling
     (P3), the local release's hold (T16); and the models under the checker's
     weaker orders (T29) and of `active-set.c` (T28);
   - **`collect`'s places three ways** (A11): each node's total (P29), batched
     with `collectives.c`'s other items (P23, P24); then the scan, modelled
     first, and the crossover (P30, P31), a slice of their own since the
     allgather's steps change; a direct exchange of the totals beside them (P33, the
     author's 2026-10-13); their measurement in the VM (H24) once all
     are in, and on the cluster (H25) in a Part 4 session;
   - **the protocols' smaller items** (P5 to P28), a slice each where a layout
     or the wire changes (P14, P15, D26), batched otherwise;
   - **diagnostics** (D1 to D30), batched by file - `osoe-stat`, `fatal.c` and
     `abort.c`, start-up's messages, settings;
   - **tests** (T14 to T44 but those that need GC);
   - **build and tools** (B10);
   - the author's answers below decide A7 to A14's items.
2. **The cluster's first session**: the kit's shake-down (H1 to H3), then the
   first native x86-64 runs - the presets at 0, 2 and 3 helpers, tier 3, the
   models, SOS, the pacer, the litmus and the lost-wake test (B8, B9, T7, T35,
   T36, T45) - since those find defects that would spoil Part 4's numbers. Run on
   2026-10-12 (`docs/fast-link-results.md`): all but SOS, the Rocky 9 build and 13
   models, with §6, §8 and §9 beside them.
3. **Part 4's sessions** (H4 to H19, with H25 and H26), as the runbook orders them; each session's
   results brought back (X3) the same day; then the author's decisions A2 to A6,
   and their slices (H20, and whatever they change), and the runs that check
   them.
4. **The Pi's items** (P4, H22), whenever the Pi is free.
5. **Round ten**: a full adversarial review of the tree with everything above,
   as round nine was (`docs/reviews/adversarial-review-prompt.md`), its findings
   settled as round nine's were.
6. **The tag's checks** on the tree to be tagged (T1 to T13), the release note
   and the version (X1, X2), the checklist (X6), the README's sentences the runs
   make old, each carried by its run's item (X4 closed on 2026-10-12); the author tags.

## Working in small steps: the author's token budget

The author's weekly allowance of tokens is shared by the lead and every agent
it starts, and round nine's sessions spent most of it: an adversarial read or
an investigator costs half a million tokens or more. So, as the author asked on
2026-10-11:

- **One small step a turn**: one item, or one slice's one stage - build, verify,
  read, merge - then say what it cost and stop for the author's word before the
  next, where the step was large.
- **Watch the usage**: where the session has the app's usage tool, read it at
  each step; tell the author at each tenth of the week's allowance, and stop
  short of it.
- **Spend agents where they pay**: Sonnet subagents for mechanical work and
  searches; a fresh agent's adversarial read once a slice is whole, batching
  small items into one slice and one read; no agent rereading what the lead
  already knows - give it the paths and the questions.
- **Keep the context small**: read the part of a file the step needs, not the
  whole; run long jobs in the background and read only their summaries.

## How each piece of work is done

The project's rules, as every session has followed them:

- **A branch for each slice**, merged with `--no-ff` once verified, from the main
  checkout (`git worktree` for the work); the README true at every commit; a
  dated record in `docs/decisions/` for each slice, with its undo; the decisions
  index's line.
- **Models before protocols**: a change to a protocol or a data structure two
  threads or processes share is modelled first (`tools/modelcheck/`), its
  breakages shown to fail; §3.16's atomics only (acquire, release or seq_cst,
  `_explicit`).
- **Tests that fail without the change**: each fix's test fails on the tree
  before it, shown by a mutant beside a control that passes.
- **Static checks first** (`shmem.memory-orders`, `strict-c11`, `boundary`,
  `static-link`, `exports`, `settings-documented`, `license-lines`), then the
  full verification: debug, release with the floor's pins, AddressSanitizer and
  ThreadSanitizer at no helpers, two and three - ThreadSanitizer's verdict
  `ctest`'s own (`tsan.reports`) - tier 3, the models one at a time, the pacer,
  the single-node build (`verify-fixes.sh` is the latest form).
- **An adversarial read by a fresh agent** of every change before it merges,
  archived in `docs/reviews/archive/` under the change's name; its findings
  taken or answered in the record.
- **Evidence** in `docs/design/evidence/`, scripts writing `<repo>`, never a
  home directory; licence lines on every source file.
- **Subagents** for what is mechanical (CLAUDE.md); a subagent's report is
  evidence, not a verdict.
- **Measurements** alternate their trees on the day; a speed claim needs its
  timing.

## What the author decides first

**Answered on 2026-10-11** (`docs/decisions/2026-10-11.md`): A7 - RoCE and
Soft-RoCE stay out, design motivation and no code; A8 - the mesh schedule built
now; A9 - the session routines no-ops; A10 - `OSOE_AM_HUGEPAGES` retired; A12 -
cancellation unsupported; A13 - Träff's lower ceiling's tests accepted. That
evening: A11 - `collect`'s places built both other ways, each node's total and
a scan, each modelled and tested, and the crossover among the three measured
(P29 to P31, H24, H25); and A1's MTU, 8896.

What was put to the author, the lead's recommendation first:

- **A1, the cluster** - answered in part on 2026-10-11 (eight `c4-standard-16`,
  an MTU of 8896, a head node reached through IAP) and on 2026-10-12 (the next session the next
  day, about two hours): the later sessions' windows and the budget remain.
- **A7, the scope.** `task-v0.2.md` put out of v0.2 RoCE and Soft-RoCE, the mesh
  schedule and `process_vm_readv` for statics. *Recommended:* RoCE and Soft-RoCE
  stay out - a backend is new work, not an open item, and `c4-standard-16` has no
  RDMA; `process_vm_readv` stays out as the README says why; the mesh schedule
  is A8.
- **A8, the mesh's start-up schedule** (`docs/design/mesh-schedule-proposal.md`).
  *Recommended:* build it now - the circulant model, tier 3's timing at 64
  logical nodes, connections on demand - and waive a large system's measurement,
  which no machine at hand can give; the README then says so.
- **A9, the 1.6 session routines.** *Recommended:* no-ops by design, closed: the
  specification allows it, and they cannot change what a program computes.
- **A10, hugetlbfs.** *Recommended:* retire `OSOE_AM_HUGEPAGES`, which has no
  effect, unless Part 4 shows the receive path paying for small pages.
- **A11, `collect`'s O(P) length reduction.** *Recommended:* keep it, the author's
  choice of 2026-09-26, its cost in the README. *Answered:* both alternatives
  built beside it - each node's total, eight bytes a node, and a scan, O(log P)
  - modelled and tested, the default among the three from measurement, since
  shmem libraries switch algorithms at crossover points (the author).
- **A12, a thread cancelled in a large blocking get.** *Recommended:* unsupported,
  as the README says since 2026-10-11: OpenSHMEM defines no cancellation.
- **A13, Träff's at 257 to 512 nodes**, which 254 logical nodes cannot reach.
  *Recommended:* accepted through a lower ceiling's tests.
- **A14, round three's R6 and R7.** *Recommended:* R6 closed by `docs/README.md`
  as it stands; R7's seam note made with X7. *The author, 2026-10-12:* a fresh
  read first.
- **A2 to A6** come after Part 4's sessions, each from its section's results, as
  the runbook says. A5 was answered on 2026-10-12 from the first session's §8 -
  the landing zone from a per-node budget (P32, H28) - and A6 waits for §10's
  senders (H11).

## What it costs

The inventory's sizes are 96 small, 65 medium and 8 large, before each slice's
verification - about three hours of a VM - and read. With subagents for the
mechanical slices, the work in the VMs is some eight to twelve working sessions;
Part 4 is four cluster sessions of four hours and their analysis; the Pi's items
a day; round ten and its fixes two days; the tag's checks a day or two. The
cluster's cost is the author's to set: at about $0.79 a node-hour on demand
(`c4-standard-16`'s list price in `us-central1` as the lead recalled it - check
it), eight nodes for sixteen hours is about $100.

## Done means

Every item of `v0.2-open-items.md` done, decided or recorded as closed by the
author; round ten settled; the tag's checks green on the tree to be tagged; the
release note true of it; and the author's tag.
