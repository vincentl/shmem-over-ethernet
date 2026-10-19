# Cluster session 2: bring the cluster back, then the cluster's items

Written 2026-10-19 for a clean session of its own. Read first: the auto-memory's START
HERE handoff, `CLAUDE.md`, this brief, the runbook `docs/fast-link.md` (§0 to §17),
cluster session 1's record (`docs/decisions/2026-10-12.md`) and results
(`docs/fast-link-results.md`), and the open items (`docs/prompts/v0.2-open-items.md`,
78 at `fcdfc480`; the 19 placed `GC` are this session's).

## Who does what

- **The author** owns the Google Cloud account, the Terraform (`scratch/main.tf`,
  gitignored, his - with its state beside it), every launch and its window (A1, answered
  per launch), and the teardown. **The lead runs no `gcloud` and no `terraform` command**
  (`docs/prompts/task-v0.2-to-the-tag.md`, "Access the lead needs"): it gives the author
  the commands below, waits for his word that they ran, then works over `ssh`.
- **Cost**: about $0.79 a node-hour on demand for a `c4-standard-16` (a list price
  recalled, to be checked) - eight nodes and the head node, about $7 an hour. Session 1's
  window was 60 to 90 minutes. Mind the clock; bring everything back (§11) well before
  the window ends.
- **Never push**: the author approves each push. Never touch the author's untracked
  files (`probes.out`, `scratch/`).

## 1. Before the window, in the VM (the lead)

So that cluster time is spent measuring, not building:

- **P9's change** (decided 2026-10-18: highest-free-first kept, a failed claim moving to a
  word chosen by local id), built, tested and merged in the `osoe` VM as the night of
  2026-10-18 did (`/tmp/batch-verify.sh`, a mutant for any new check). On the cluster
  it is timed, not built: the contention needs many leads claiming at once - plan its
  measurement with the PEs a node the hardware allows (H12's 16 to 64), before and after.
- **The kit's dry run** in the VM (`docs/fast-link/dry-run.sh`, as cluster session 1's
  record and the overnight log describe), green on `main`, and a git bundle of `main`
  (`git bundle create`) ready for the head node.
- Then tell the author you are ready, and give him section 2.

## 2. Bring the cluster back (the author)

The machines are deleted between sessions; nothing on them survives. From the
repository root, in the author's terminal:

1. Quotas that held the first apply (raised then; check they stand):
   ```bash
   gcloud compute project-info describe --flatten="quotas[]" --format="table(quotas.metric,quotas.limit,quotas.usage)" | grep -i cpus_all_regions
   gcloud compute regions describe us-central1 --flatten="quotas[]" --format="table(quotas.metric,quotas.limit,quotas.usage)" | grep -i c4
   ```
   All regions at least 132 CPUs; C4 in `us-central1` at least 128 (eight nodes of 16).
2. The machines - eight `c4-standard-16` and `slurm-controller` (`e2-standard-4`) in
   `us-central1-a`: Debian 13, gVNIC, the VPC's MTU 8896, a compact placement, OS Login,
   IAP for every `ssh`, Cloud NAT, no external address (`docs/decisions/2026-10-12.md`,
   "The machines"; the runbook's §0):
   ```bash
   cd scratch && terraform plan && terraform apply
   ```
3. Access: the `~/.ssh/config` entries session 1 used (IAP's `ProxyCommand`, the OS Login
   user, `ControlMaster`, `ForwardAgent`), the key added to the agent (`ssh-add`), then
   `ssh slurm-controller hostname`, and from there `ssh slurm-node-01 hostname`.
4. The window, told to the lead.

## 3. Setup and facts (the lead; the runbook's §1 to §3, about half an hour)

The bundle to the head node and cloned there; `slurm-setup.sh` with
`CONTROLLER=$(hostname -s)`; `build.sh`, which copies the tree to every node and builds
it there (all eight without a warning); `facts.sh` (§2: the CPU, gVNIC, the MTU 8896, the
sysctls); the probe on every node and tier 2 on one (§3). Compare with session 1's facts:
a change between sessions makes their numbers incomparable.

## 4. The items, in the author's order

**Cluster session 2** (the author's order, `handoff` memory):
- **H4**, §5's helper sweep - 0, 1, 2 and 4 helpers, the collectives beside the streams.
- **H8**, §7: the zero-copy send's default, and a third buffer.
- Riding along: **H14** (the link measured on two and three nodes only), **H27** (§15's
  latency tuning: coalescing off, busy polling).
- **P9**'s timing (above).

**Then, as the window allows - cluster session 3's**: H25 (§12, `collect.sh`; P31's
defaults and P35's cap against it), H28 (§8's loop over PE counts, the zone unset), H26
(B11's `binding.sh` site binding, then §5's streams and collectives under it), H15 to H19
(§13, `behind.sh`), H13 (§14, `window.sh`), H6 (§16, `trees.sh`). And when there is room:
H11 (§10, the cluster at size), H12 (many PEs a node), B8 (the native x86-64 runs that
session 1 left), T36 (an x86-only lost wake).

Each result that a decision waits on is written as a question for the author, not
decided: A2 (helpers' default) after H4, A3 (zero-copy's default) after H8, A4 (the
ladder's defaults), A6 (the receive pool) after H11 - each self-contained, with the
numbers, the options and one recommendation (the "plans self-contained" memory).

## 5. Bring it back, then the author's teardown

Before the window ends (§11): every output to the Mac, the evidence under
`docs/design/evidence/` with scripts writing `<repo>` and `~`, never `/Users/...`; the
results into `docs/fast-link-results.md`; the day's record in `docs/decisions/<date>.md`;
the open items closed and counted; the questions for the author. Then tell the author
the machines can go (`cd scratch && terraform destroy`, his). Merge to local `main` at
green points; never push.
