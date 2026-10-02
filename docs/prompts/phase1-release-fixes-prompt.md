# Brief: the fixes the round-three review asks for before the first release

Written 2026-09-28 by the review's lead session as the handoff for the next
session; the author has not yet read the review. Read first:
`docs/reviews/archive/phase1-release-review.md` (the verdict and the ranked
list), then the five deliverables beside it. Every finding's evidence is under
`build/scratch/review-r3/` (git-ignored): `reports/<investigator>.md` and the
programs, logs and raw outputs each cites. `CONTEXT.md` there is the brief the
investigators worked from.

## Where the review stopped

- The six review files are written and uncommitted; commit them first, as
  they are (the review's names and severities are the reviewers', and later
  fixes are recorded in the decisions, not by editing the review).
- Not done: the refutation pass by fresh subagents (its launch failed when
  the author stopped the review). The lead re-read the cited source for every
  fix-before-release item and re-ran what could be re-run; those are marked
  "lead-verified". A refutation pass is still worth an hour: give each of the
  sixteen items to a fresh agent with the finding's evidence and ask it to
  refute.
- Two Lima VMs made for the review: `osoe-x86` (Rosetta, `rosettad` stopped)
  and `osoe-load` (the baseline and load runs; logs in
  `build/scratch/review-r3/load/`). Delete both when done:
  `limactl delete -f osoe-x86 osoe-load`. The worktree
  `build/scratch/wt/review-r3` can go too.

## The three findings that matter, and what to build

### 1. `shmem_global_exit` under `srun -K1` (conformance C2, readiness R5)

Measured: the caller's `exit` is killed by Slurm's KillOnBadExit once the
other PEs `_exit(status)` first; a 200 ms `atexit` handler never finishes and
`srun` returns 143 for a status of 7. Two designs, the author's call:

- **Hold the other PEs' exit until the caller's process has ended.** The
  caller's node-mates can wait on its pidfd (liveness already holds one);
  remote nodes cannot see it, so their leads wait for a second goodbye - "the
  caller has ended" - from the caller's lead, bounded by the abort grace. This
  keeps §3.15's rule that only the caller runs `exit`. Model it in
  `abort_exit_model.py` first: the model's `caller_exiting` and glibc 2.41's
  exit lock are the cases to keep.
- **Or state the limit** beside `-K1` in the README: a nonzero global exit
  leaves the caller about 50 ms.

Either way, `shmem/tests/global-exit.sh` must run one case under a killing
launcher (`osoe-launch` without `-k`, and `srun -K1` where Slurm exists) and
check the launcher's own status.

### 2. Start-up that hangs silently (readiness R8, portability N1, N2)

Three pieces, all cheap:

- **A failure before the control segment exists is invisible to the other
  PEs** (`node.c:611-628`'s `/dev/shm` refusal; settings the lead alone
  validates; a PE exiting before `shmem_init`; a disagreeing heap size or
  `OSOE_JOB_SECRET` where the README and §6.2 say the job ends). Publish the
  failure where the others can see it before the segment exists - a word in
  the job object, which every PE opens first - and run the refusal tests with
  `osoe-launch -k` so the suite sees a survivor that hangs. 138 measured cases
  and the table are in `reports/diagnosability.md`.
- **A node that cannot reach a peer prints nothing** (`listener.c:409-481`,
  `:550-598`): keep §3.15's rule that no wait gives up, and print one notice
  after about 30 s, repeated each minute, naming the peer's entry, address,
  port and the last connect error. Bind the rendezvous listener to
  `INADDR_ANY` as the mesh's already is (`job.c:147`, `rendezvous.c:283`), and
  say in the README that a node's name must resolve to the same address on
  every node.
- **The rendezvous acts on a hello before the secret is proved**
  (`listener.c:366-375`): move the count and node-claim refusals after the
  confirmation, or make them non-fatal for an unproved caller; bound the
  confirmation read by `HELLO_NS` as the hello is (`:384`); add a "Trust"
  paragraph to the README (a private cluster network is assumed; the secret
  protects the join only). The fuzzer is `build/scratch/review-r3/trust-kernel/`.

### 3. Team numbers spent per node (scale P1, conformance C4.1)

Measured by two investigators independently: every split claims from world
PE 0's counter (`team.c:813-840`), so a split of `SHMEM_TEAM_SHARED` on every
node - one line of code - spends one number per node and fails on the 62nd
node at the default, some nodes getting teams and others `SHMEM_TEAM_INVALID`.
Raising `OSOE_MAX_TEAMS` costs every PE about 208 KiB of address space a
number.

- **Before release, the README sentence**: in the team row and the Limits,
  "every split claims job-wide numbers; a split made on each node - of
  `SHMEM_TEAM_SHARED`, say - spends one per node".
- **The fix proper, a day, and the author should decide whether it precedes
  the tag**: number single-node teams from a per-node range. A team whose
  members all live on one node never sends an arrival, a piece or a probe
  with its number on the wire, and no other node ever meets it, so its number
  need be unique only among the teams that share that node (design §10
  already says so for `split_2d`'s colouring). Split the space: numbers
  `[3, S)` job-wide, claimed at PE 0 as now, for teams spanning nodes;
  `[S, OSOE_MAX_TEAMS)` claimed at the node's own lead for teams whose member
  set, computed from the rank map at split time, lies on one node. Nothing in
  the internal region or the control segment changes shape; what changes is
  who claims, and `split_2d`'s row teams of a whole-node-wide grid become
  per-node too. Check: `internode-am.c`'s arrival scan and the detector never
  see a single-node team's number; `osoe-stat --summary` labels per-node
  numbers by node. Test: the investigators' programs
  (`build/scratch/review-r3/scaling/split_shared.c`,
  `conformance-b/src/shared-split.c`) on 64 or more logical nodes (below).

## Testing at hundreds of nodes without hundreds of nodes

- **`osoe-launch -m` gives up to 254 logical nodes on one machine** (README:
  "up to 1,024 processes on 254 logical nodes"), each with its own loopback
  address and TCP between them. That is the tool for resource questions: a
  job of 254 nodes × 1 PE with `SHMEM_SYMMETRIC_SIZE=4m` and
  `OSOE_SHM_CAPACITY_CHECK=0` (the check is sized for real heaps) shows the
  per-node split failing at node 62, the connection area at 253 buffers a
  node, the detector's O(n) round, and the mesh's 2n connections; the
  `scaling` report's `sizes.c` gives the arithmetic beyond that and matched a
  measured segment. Generate the map with `seq -s, 0 253`.
- **Slurm's own simulation** (`slurmd -N` with `--enable-multiple-slurmd`, or
  the front-end mode) runs many virtual nodes on one host under real Slurm
  variables. It answers the *bootstrap* question - a 500-name
  `SLURM_STEP_NODELIST`, every `--distribution`, uneven layouts, the
  hand-written multi-node fixtures replaced by captured ones - which the
  review found untested (portability N4), and it costs an afternoon in the
  `osoe` VM where Slurm is already installed. It does not answer resource
  consumption: every virtual node is the same host, the same memory and the
  same NIC, so it measures nothing `osoe-launch -m` does not, and its
  hostnames all resolve to one address, which is exactly the shape N1 and N4
  found fragile. Use it for the bootstrap; use `osoe-launch -m` and arithmetic
  for scale; use tier 3's namespaces (`tools/launch/tier3.sh`) where a real
  link between logical nodes matters.
- The one thing neither gives is the per-message cost of one progress thread
  serving 256 PEs on a many-core x86-64 node (scale P2): that needs such a
  node, and the README should say so until one has run.

## The rest of the sixteen, in the order the lead would do them

The C11 `shmem_signal_add(ctx, ...)`/`shmem_signal_set(ctx, ...)` generics
(`shmem.h:182-185`, a few lines and a compile test); `shmem.floor`'s stream
pin out of the default suite or explained; the README's three scaling
sentences (P1-P3), the thread limit's wording in four places (C3), the eight
unlisted departures (C4), Python 3 in the requirements, a version string at
the tag, `-K1 --cpu-bind=none` in the first example, the `/dev/shm` epilog
line, the three settings rows' ranges, the two RDMA sentences (DC, and the
internode phase below the seam), the model-checking sentence (the active-set
protocol has no model in the tree; every model runs under SC or TSO), the
x86-64 sentences (untested network path; nodes must share an architecture),
the two Pi numbers (the barrier's 180 µs is the bound figure; the slice's
cost to small collectives), the repository scrub (paths in 61 evidence files,
the docs index's private page, five fixtures' address, four notices, two SPDX
lines), and the torn "blocked in" record (one acquire fence or four atomic
words). The stopped-node experiment from `reports/newest-work.md` should
become a test; it is the only thing that holds the silent-peer check's gate.

## After the fixes

The release point's remaining conditions on the tagged tree: the SOS sweep
(done this round at 4 × 2 on the Pi: 161 pass, the same four exceptions), the
Pi runbook end to end, an hour of `mix` on the cluster, the tag and a release
note of the known limits, which this review's Limits findings are the draft
of.
