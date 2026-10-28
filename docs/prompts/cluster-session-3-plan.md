# The third cluster session: one chain, about an hour

*The author's plan of 2026-10-27, decided item by item in conversation
(`docs/decisions/2026-10-27.md`); the kit is `docs/fast-link.md` and
`docs/fast-link/`, the machines eight `c4-standard-16` and a head node, about
$6.60 an hour.*

## Why it is shorter than the kit's sections as written

As the sections are scripted, the session's open items come to 105 to 115
minutes of jobs, and the second session (106 minutes for seven items) spent 39
minutes measuring nothing:

- **15 minutes of gaps** between sections, each waiting for the author's word;
- **9 minutes** waiting on the machines' start-up scripts by a `pgrep -f` that
  matched itself (`docs/fast-link.md` §1 now waits by the units' state);
- **15 of the floor's 17 minutes**, and 25 of the first session's 26, an
  `echo-uring` client that met the kit's 300 s timeout in one direction - a
  fault of the kit's that no result depended on.

And the repetitions bought little: every large effect of sessions 1 and 2 - the
helpers' +23 to +64%, the zero-copy send, the landing zone's gets, the fan-in,
the ladder - showed the same order in any single round, the rounds' spreads 1 to
7% at the median; where three rounds could not separate two cases (`collect`'s
places, H27's pings), more would not have. So a section runs **one round**
unless a default turns on an effect near the noise.

## The chain

One script, the author approving the plan once and bringing the cluster up, the
lead watching and stopping only at a failure. Every section in turn, serially -
one measurement at a time, as both sessions ran. Times are estimates from the
second session's own jobs, adjusted by the kit's dry run of the chain whole in
the development VM on 2026-10-28, the VM's own minutes in the last column. The
script is `docs/fast-link/session-3.sh` (`docs/fast-link.md` §21;
`docs/decisions/2026-10-27-session-3-kit.md`).

| # | What | Items | Settings | Minutes | In the VM |
|---|---|---|---|---|---|
| 0 | The author's `terraform apply`; the start-up scripts, waited on by state | - | as §1 | 4 | - |
| 1 | Setup, build, the facts, the probe and tier 2 on a node; SOS fetched and built on the head node meanwhile | - | §1-§3 | 5 | 0.9 |
| 2 | The floor: `iperf3` alone | - | §4, `PARTS=iperf3`: no `echo-uring`, nor the floor test | 1 | - |
| 3 | The link's measurement on two and three nodes | P38 | §19, its 5 rounds | 0.2 | 0.0 |
| 4 | The chooser at windows 4 and 8, its trees at 8, the fixed (4, 8) | P37 | §14's four cases, `link-d4 link-d8 tree-d8 fixed-k4-d8`, 5 rounds | 2 | 1.3 |
| 5 | Many PEs a node | H12 | §20, `many-pes.sh` (below) | 7 | 3.6 |
| 6 | Gets and the per-node landing budget | H28 | §8, 1 round | 2.3 | 2.0 |
| 7 | Behind a stream: a barrier, a fetch-add, collectives, `nstat`, a profile | H15-H19 | §13 at 0 and 2 helpers, 1 round | 5 | 4.6 |
| 8 | The ladder's rungs 1 and 4 on the earlier trees | H6 | §16, 1 round, the trees built first | 6 | 4.7 |
| 9 | The cluster at size: `osoe-bench` at 8 and 16 PEs a node, start-up at 1, 8 and 32, SOS at 2 PEs, `mix` | H11, B8's SOS | §10, `mix` 300 s, SOS built by `sos.sh` in section 1 | 9.5 | 7.6 |
| 10 | The helpers under a site's binding, with and without `--cpus-per-task=2` | H26 | §5 under `binding.sh site`, 0 and 2 helpers at 8 PEs a node, 2 rounds, a PE bound to one CPU (`--cpu-bind=threads`) and to two; last, since it restarts Slurm | 8 | 6.9 |
| 11 | Results packed, and brought back | - | §11: `out.tar.gz` | 2 | 0.0 |

**About 52 minutes, about $6**, where the sections as scripted, with session 2's
gaps, would take two and a quarter hours; 61 before the dry run, which found
§13's operations taking twice the time the estimate gave them and H12 and H11
well under theirs. The unknowns are H12's collectives at 256 PEs a node, about
two minutes, SOS's 161 programs on eight nodes, about three, and T35's litmus
in tier 2 on x86-64. P37's five rounds are the exception to one: the window's
default turns on 18 to 28% against collectives' worst spread of 23%, for two
minutes. H26's two rounds: P36's default rides on it.

## What each result decides

- **P38** (§19): whether the link's measurement is now steady from job to job;
  P37's reading rests on it, so it runs first.
- **P37** (§14): `OSOE_COLLECTIVE_PIPELINE`'s default of 8, decided on
  2026-10-25 and built once this times it.
- **H12** (new): P2, decided after H12's numbers, and P24's
  `OSOE_CHECK_COLLECTIVES` cost at many PEs a node, before it may become a
  default.
- **H26** (§5 bound): P36, `OSOE_PROGRESS_HELPERS=auto` as the default - the
  question being only whether two helpers still beat none when every PE is bound
  to its core.
- **H28, H15-H19, H6, H11**: the README's rows and sentences (landing budget,
  behind a stream, the ladder, the cluster at size); no default turns on them.
- **B8**: closed by H11's SOS on x86; its 13 models are pure Python and run in the
  development VM, its Rocky 9 build is T3's.
- **T36**: closed by T29's models under x86-TSO and T35's litmus
  (`shmem.quiet.litmus.L2`), which section 1's tier 2 runs natively on x86 for the
  first time.

## Before the window, in the development VM

Done on 2026-10-27 and 28 (`docs/decisions/2026-10-27-session-3-kit.md`):

1. **H12's section** (`docs/fast-link/many-pes.sh`, §20 of the runbook): on a pair
   of nodes, `helpers.sh`'s workloads at 16, 32 and 64 PEs a node; the wake's
   cost at 256 PEs a node on a 16 MiB heap (256 PEs ask about 7.9 GiB of
   `/dev/shm`, the heap and the library's region of about 15.6 MiB each, so that
   point is the cluster's alone; the VM dry-runs it at 64); and
   `OSOE_CHECK_COLLECTIVES` off and on at each count. `multistream` now gives the
   PEs' sleeps a 64 KiB, S1's measure of the turnover's herd, and the profile at
   256 the progress thread's wakes.
2. **The chain** (`docs/fast-link/session-3.sh`, §21): the sections above with
   their rounds, cases, helper counts and lengths as settings of the kit's
   scripts (each script's `ROUNDS`, `window.sh`'s `CASES`, `behind.sh`'s and
   `helpers.sh`'s helper lists, `floor.sh`'s `PARTS`, `scale.sh`'s `mix` length,
   SOS built by `sos.sh` in the setup), each section's result written as it
   ends, the chain going on past a section that fails and saying so.
3. **The dry run** (`dry-run.sh`'s `session-3`) of the chain whole: 31.6 minutes,
   every section done, the static checks 17 of 17 after it.

Left as they are: two measurements at once on disjoint pairs (no precedent, and
the kit's `scancel` by name would cancel the other's jobs); the `echo-uring`
client's hang, a fault of the kit's, not of the library's, and not run.

## Running it

1. **The author**: the machines as for the second session - eight `c4-standard-16` and
   `slurm-controller` (`cd scratch && terraform apply`) - and the window, told to the lead.
2. **The lead**, from the Mac, as soon as the head node answers: the tree as a bundle of
   `main`, cloned there by its branch, the bundle having no `HEAD` - the chain needs the
   clone's `.git` for §16's trees - and the node list:
   ```bash
   git bundle create build/osoe-main.bundle main
   scp build/osoe-main.bundle slurm-controller:
   ssh slurm-controller 'git clone -q -b main osoe-main.bundle osoe && printf "slurm-node-0%s\n" 1 2 3 4 5 6 7 8 > NODES'
   ```
3. **The chain**, started in an ssh session that forwards the agent - the setup, `sos.sh`,
   `trees.sh` and `binding.sh` reach the nodes by ssh - and kept open to the end by `tail`,
   the chain in a session of its own, so that a dropped connection costs the agent, not the
   chain, and the connection kept from idling out while a section is silent; run in the
   background from the lead's shell:
   ```bash
   ssh -A -o ServerAliveInterval=30 slurm-controller 'cd osoe; setsid nohup docs/fast-link/session-3.sh ~/NODES ~/out > ~/session-3.txt 2>&1 < /dev/null & tail -f --pid=$! ~/session-3.txt'
   ```
   Its first section waits for the machines' start-up scripts by their units' state. Each
   section's line lands in `~/out/sections.txt` as it ends; a `FAILED` line is read in
   `~/out/SECTION.log` while the chain goes on. To stop it: `kill -TERM -$(cat
   ~/out/chain.pid)`, its process group; `pkill -u $USER -x timeout`, each job's timeout
   being a group of its own; then `scancel -u $USER`. `SECTIONS="..."` runs what is left.
4. **Back**: `~/out.tar.gz`, packed by the chain's last section, copied into
   `docs/design/evidence/v0.2-step4/<date>-fast-link/`; then the author's
   `terraform destroy`.
