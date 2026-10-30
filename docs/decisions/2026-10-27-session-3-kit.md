# 2026-10-27 and 28: the third session's kit, built and dry-run

## The brief

From the session that planned the third cluster session with the author the same day
(`docs/decisions/2026-10-27.md`): prepare the kit as `docs/prompts/done/cluster-session-3-plan.md`
lays it out, so that the session is one unattended chain of about an hour - H12's section, the
chain, and the chain's dry run in the `osoe` VM, timing each section and adjusting the plan's
estimates where they are off. Committed on this session's branch, not merged, not pushed.

## What was built

- **H12's section**, `docs/fast-link/many-pes.sh`, the runbook's §20. Between the list's first
  two nodes, so that every PE of a node sends to the other on the one connection between them:
  at 16, 32 and 64 PEs a node, `helpers.sh`'s five workloads, the collectives twice,
  `OSOE_CHECK_COLLECTIVES` off and on; at 256, the two put streams, the 1 MiB one again
  profiled (`profile.sh`, both nodes, five seconds, five in), and the collectives off and on.
  23 jobs, one round, no helpers, a 16 MiB heap at every count.
- **What answers P2**: `multistream`'s node lines now give the PEs' voluntary context switches
  over the window, `program_sleeps`, and `sleeps_per_64kib` - the measure the release fixes'
  review used for S1 (0.75 at one writer, 8.4 to 9.1 at 16 and 16 to 23 at 32 under the rule
  then, 1.0 waking one). `summarize.py` prints them beside the rate and the progress threads'
  CPU a GiB, and its counters that moved now include `am.reserve_stalls`.
- **What answers P24**: `summarize.py` sets each `osoe-bench` line's median with the check
  beside its median without, and their ratio, wherever a case ran both
  (`collectives-check-on` and `-off`).
- **The chain**, `docs/fast-link/session-3.sh`, the runbook's §21: the plan's sections 0 to
  11, each in a subshell of its own, its line written as it ends - done or FAILED, its minutes
  beside the plan's, its jobs and failed jobs - its table by `summarize.py`, the chain going on
  past a failure but for the wait and the setup's packages, Slurm and build, where it stops;
  `out/summary.txt` at the end with the setup's verdicts (the probe on every node, the job of
  32 PEs a node, tier 2, T35's litmus) and SOS's count; `SECTIONS` to run what is left after a
  stop. The wait is by each machine's `google-startup-scripts.service` state (`LoadState`,
  `ActiveState`, `SubState`, `Result`, `ExecMainExitTimestamp`), checked another way - each
  pending unit's status and last lines - past four minutes, failing past ten; an ssh refused
  says the agent is missing at once.
- **SOS on the head node**, `docs/fast-link/sos.sh`: the suite fetched outside the tree
  (`~/osoe-corpus`), the library built there for `oshcc` (the `osoe` target alone), each
  program built beside its source as `docs/pi-cluster.md` §8 builds them, as many at once as
  the machine has CPUs, and
  the directory copied to every node at `build/sos`, `done.txt` its record of a whole build;
  started in the setup's background at `nice -n 19` - after Slurm's setup on a head node off
  the list, after the build where this machine builds too - and waited for while it lives,
  ten minutes at most.
- **The kit's scripts given the plan's settings**: `floor.sh`'s `PARTS` (the three parts, all
  by default; `echo-uring`'s fault described, not fixed), `window.sh`'s `CASES` (`link-dD` and
  `tree-dD` for the chooser and its trees at a window), `trees.sh`'s `TARGETS` (each old tree's
  library and the three tools its cases run, not its tests), `dry-run.sh`'s `session-3` (the
  chain whole at its own settings, with a stand-in `git` serving `git archive` from tars on a
  machine without git).

## Choices the plan left to the kit

Each is a setting, so that the lead or the author can change it without an edit.

1. **P37's "a tree" is the chooser's trees at the new window, `tree-d8`.** `link-d4` against
   `link-d8` answers the default's change; `fixed-k4-d8` is the second session's best tree; and
   where the link's message cost reads low the chooser takes Träff's for reductions at both
   windows, so only a tree at 8 says whether the chooser's trees match the best fixed one
   there. §14's own `tree` is CASES's `tree`, at today's window.
2. **The floor's "`iperf3` alone" leaves out the floor test too** (`PARTS=iperf3`), as the
   author's words say: `shmem_test_floor` took 10 s in the second session, and
   `PARTS="floor iperf3"` adds it.
3. **H12 at one heap, 16 MiB, at every count**, so that 16 to 256 compare; **no helpers**, a
   pair's one peer being one helper's at any count and `auto` giving a pair none; **the
   profile at 256 only**, on a job of its own so that the measured stream is not profiled; the
   check off and on at 256 too, the count the design targets.
4. **H26 with the mailbox among §5's workloads**: P36's default rides on whether two helpers
   still beat none when bound, and the mailbox is where two helpers lost at more PEs than
   vCPUs; **8 PEs a node**, one a core, the most that `--cpus-per-task=2` fits; **the first
   variant each PE bound to one CPU** (`-K1 --cpu-bind=threads`, `H26_BOUND`), as a site's
   `srun` binds a task of one CPU, and the second two CPUs a PE (`-K1 --cpus-per-task=2`,
   `H26_RESERVED`), on a `c4-standard-16` the PE's whole core - see the dry run, below, for
   why the binding is asked for. Each variant first records its tasks' allowed CPUs
   (`binding-VARIANT/cpus-allowed.txt`).
5. **H11's `scale.sh` at its default of no helpers**, the library's default at the session.
6. **The packages installed by the chain on every machine at once**, since the start-up
   scripts leave out `linux-perf`, which §13's and §20's profiles need, with `PERF="sudo -n
   perf"` there (the image's `perf_event_paranoid` is 3).

## An independent read of the cluster's paths

What the VM cannot run - the wait, the packages, Slurm's setup, `sos.sh`'s copies, the binding's
switch, 256 PEs a node, `perf` through `sudo` - was read by a Sonnet agent given the scripts and
the cluster, not the lead's view of them (`f6b46f95` is what it found, fixed):

- **Not yet started read as done.** A unit not started reads `inactive` and `success` as one
  that finished; the runbook's own command prints `ExecMainExitTimestamp`, which the wait now
  requires (or `active` and `exited`, should the image keep the unit so).
- **SOS's build beside Slurm's restart.** `cmake` at four jobs on the four-vCPU head node while
  `slurm-setup.sh` restarts the controller and runs an `srun` under `set -e`: SOS now starts
  after Slurm's setup, at `nice -n 19`, and its wait in section 9 is bounded; its whole build
  is marked by `done.txt` in its directory, so that a rerun into another `OUTDIR` uses it.
- **Tier 2's exit status** was read after a command substitution in the same word, always 0;
  it is kept first.
- **`profile.sh` under `sudo`**: root's data file in `/tmp` could not be removed by the user,
  and `perf report` of call-graph data printed graphs, not the table; the data now goes in a
  directory of the user's own, the table is `-g none`, and the progress threads' callers
  follow it. `perf` had never run through the script: the dry run now runs it as the cluster
  will, by `sudo -n`, the VM's `perf_event_paranoid` being the image's 3.
- **The binding left on** if `binding.sh site` failed part-way: `none` is now tried then too.
- **The stop recipe**: `timeout` makes a process group of its own, so `kill -TERM -PID` missed
  each job's `salloc`, and `scancel -n osoe-kit` the builds' jobs; the chain records its PID
  (`out/chain.pid`), and the recipe is the group, `pkill -u $USER -x timeout` and
  `scancel -u $USER`.
- **An idle connection**: nothing reaches the `tail` for minutes at a time, and an idle cut
  would end the agent; the command now has `-o ServerAliveInterval=30`.
- **`dry-run.sh` passed its own `SECTIONS=session-3` to the chain**, which refused it - the
  first dry run, queued, would have ended at once (`CHAIN_SECTIONS` now).
- The bundle without a `HEAD`: a plain clone of it checked out `main` on the Mac's git 2.55, as
  on the head node in the second session; `git clone -b main` is used anyway.

Checked and fine by the same read: `set -u` against `common.sh`, prefix assignments reaching
`launch` and `beside`, `PERF` reaching the `srun --overlap` step, `sos.sh`'s target and rpath,
`build.sh`'s exclusion of `/build`, 256 PEs a node under Slurm's defaults, the four trees'
commits in `main`, the subshells' signals, first contact's host keys.

## The dry run

`dry-run.sh`'s `session-3` in the `osoe` VM, under its lock, on `f6b46f95`: the chain whole on
five logical nodes at its own settings (rounds, cases, helper counts, lengths, `mix` for 300 s),
with H12's 256 PEs a node at 64, tier 2 three of its tests, SOS's sources the corpus's
`tests-sos`, the four trees served by the stand-in `git` and `perf` by `sudo -n`, as on the
cluster (`docs/design/evidence/v0.2-step4/2026-10-27-session-3-kit/`). **31.6 minutes, every
section done**; the static checks 17 of 17 on its build after it; `/dev/shm` at most 96 MB in
use, the memory available never under 4.8 GB.

| Section | Jobs | Failed | Minutes in the VM |
|---|---|---|---|
| 0 wait | - | - | 0.0 (one machine: none) |
| 1 setup | 3 | 0 | 0.9 (no packages or Slurm; the probe on all five, the job of 32 PEs a node, tier 2's three) |
| 2 floor | 0 | - | 0.0 (the VM has no `iperf3`) |
| 3 link-cost | 41 | 0 | 0.0 |
| 4 window | 20 | 0 | 1.3 |
| 5 many-pes | 23 | 0 | 3.6 |
| 6 gets | 15 | 3 | 2.0 |
| 7 behind | 20 | 0 | 4.6 |
| 8 trees | 20 | 0 | 4.7, the four trees' builds in it |
| 9 scale | 173 | 4 | 7.6 |
| 10 binding | 42 | 0 | 6.9 |
| 11 pack | - | - | 0.0 |

The seven failed jobs are the dry run's and the suite's own: `get-pieces` (three) and SOS's
`bigget` and `bigput` asked more than the dry run's 32 MiB heap holds (`dry-run.sh`'s, for
`/dev/shm`; the cluster runs the default 256 MiB, where the first session ran `get-pieces` at
64 MiB), and `shmemalign` and `shmem_team_b2b_collectives` end as `docs/pi-cluster.md` §8 says
they do. SOS: 161 of 163 programs built, the two that pass `short *` to the C11 generics not.

**What the dry run changed** (`d5b61886`): at the dry run's `WAKE_PES=64` the wake point's files
overwrote the count of 64's - the same names - and now carry `-wake`; the profile's callers
report printed perf's sixty warnings on unwinding and nothing else, its stderr now dropped; the
litmus's verdict took two lines.

**H26's bound variant was not bound.** In the VM's own Slurm (`task/affinity`, `CR_Core`,
read-only: `binding-check*.sh`), a whole-node allocation's step - as the kit starts every job,
`salloc --exclusive` then `srun` - of two tasks of one CPU gave each task every CPU, with
`--overcommit` (the runbook's `-K1 --overcommit`) and without it: a step uses all of its
allocation's CPUs unless told otherwise, and binds none of them to a task of one CPU.
`--cpu-bind=threads` or `cores` bound each task to its own CPU, at two tasks and at four, and
`--cpus-per-task=2` each to two. So H26's first variant, as written, would have measured
unbound PEs - §5's own rows over again. It is `-K1 --cpu-bind=threads` now, each PE on one
CPU as a site's `srun` gives a task of one CPU, and the second `-K1 --cpus-per-task=2`; both
are settings of the chain (§5's H26 paragraph). On a `c4-standard-16` - eight cores of two
hyperthreads - the second is the PE's whole core, which leaves the PE and its helpers its
sibling hyperthread; a core of their own a PE would be `--cpus-per-task=4` at 4 PEs a node, a
change of count the author did not ask for.

**The recheck** (`vm-recheck.sh`, on `d5b61886`): the wake point alone at 64 PEs a node with
`perf` by `sudo -n` - its callers report now reads `try_to_wake_up`, 11.8% of every sample, from
`futex_wake`, `osoe_futex_wake`, `osoe_progress_poll`, `agent_main`: the turnover's wake of
every writer, by the progress thread - then the kit's own dry run, every section at small
sizes, the paths the chain does not take among them (`floor.sh`'s three parts, `window.sh`'s
own cases): every section done and no job failed, but §16's, which needs `git archive` and the
VM has no `git`, as on 2026-10-19; and the static checks 17 of 17 again.

**A preview, not a result**: on four vCPUs the VM's numbers say nothing of the cluster's, but
they show H12's measures move. The PEs' sleeps a 64 KiB of the 1 MiB put stream were 3.3 at
16 PEs a node, 5.7 at 32 and 12.8 at 64 - the herd's growth with the writers, at about a
fifth of them where S1's rule then gave a half - and at 64 the profile put a tenth of every
sample in `osoe-agent`'s `try_to_wake_up`. The check's cost ranged 0.87 to 1.23 a line, one
round, 128 PEs on four vCPUs. P37's four cases: `link-d8` and `fixed-k4-d8` within 2% of each other
from 1 MiB; `link-d4`'s broadcasts 36 to 37% slower than `link-d8`'s from 256 KiB, its
reductions 0 to 19%.

## The chain's expected length

From the second session's own jobs - the same scripts, the same lengths: a ten-second
`multistream` job 10.3 to 10.7 s there, a twenty-second mailbox 20.3 to 20.7, a shape 5.7 to
7.7 - and, where the VM's job list differs from the plan's, the VM's:

| Section | Plan | Expected | Why |
|---|---|---|---|
| 0 wait | 4 | 4 | the second session's start-up scripts, 2.5 to 3.6 minutes |
| 1 setup | 6.5 | 5 | the second session's 4, with `linux-perf` and T35's litmus in tier 2 |
| 2 floor | 1 | 1 | the second session's `iperf3`, 56 s |
| 3 link-cost | 0.2 | 0.2 | 10 s |
| 4 window | 2.5 | 2 | 20 shapes of about 6.3 s |
| 5 many-pes | 12 | 7 | 18 jobs at 16 to 64 PEs a node, about 2.8 minutes; at 256, the streams and the profile about 1, the two collectives' jobs about 2 (512 PEs' 4 KiB `alltoall` moves 256 MiB from each node a call, 400 calls) |
| 6 gets | 2.3 | 2.3 | three `get-pieces` and twelve streams |
| 7 behind | 3.5 | 5 | each operation of `hol` other than the fetch-add took 20 to 22 s in the VM, its time alone and then behind the stream: 2 × (13.5 + 5 × 21 + 21 + the profile's reports) |
| 8 trees | 6.5 | 6 | the builds of the library and three tools alone, about a minute on every node at once; 20 jobs of 10.5 to 20.5 s |
| 9 scale | 12.5 | 9.5 | the `mix`'s 5; `osoe-bench` whole at 8 and 16 PEs a node, about a minute; SOS's 161 programs at about a second each, `bigget` and `bigput` about 12 s |
| 10 binding | 8 | 8 | 40 jobs of §5's lengths, and two switches |
| 11 pack | 2 | 2 | a tarball of some tens of MB, brought back over IAP |

**About 52 minutes, about $6 at $6.60 an hour**, against the plan's 61 - perhaps 47 to 60, the
unknowns being the collectives at 256 PEs a node, SOS on eight nodes and the litmus on x86-64.
The plan's table now carries these minutes, and the chain's own lines say each section's
beside them.

## Left for the session

- The ssh session that starts the chain must forward the agent and stay open: the setup,
  `sos.sh`, `trees.sh` and `binding.sh` reach the nodes by ssh. The chain runs in a session of
  its own (`setsid nohup`), so a dropped connection costs those sections the agent, not the
  chain.
- Never run on a cluster: the wait, the packages step and `sos.sh`'s copies; H26's two steps
  under `binding.sh site` (`task/affinity` with `task/cgroup`; the VM's Slurm has the first
  alone); 256 PEs a node. `perf` by `sudo -n` has run, in the VM, where its paranoia is the
  image's.
- For the author, not blocking: H26's second variant gives each PE its whole core on this
  hardware, the first one CPU of it. If a core reserved for the helpers is what P36 needs, it
  is `H26_RESERVED='-K1 --cpus-per-task=4'` with 4 PEs a node, at the session.
- `echo-uring`'s client fault is the kit's and is left as it is.

43 items, none closed: H12 has its section, the third session its chain.
