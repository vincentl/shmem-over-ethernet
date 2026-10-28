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
second session's own jobs.

| # | What | Items | Settings | Minutes |
|---|---|---|---|---|
| 0 | The author's `terraform apply`; the start-up scripts, waited on by state | - | as §1 | 4 |
| 1 | Setup, build, the facts, the probe and tier 2 on a node; SOS fetched and built on the head node meanwhile | - | §1-§3 | 6.5 |
| 2 | The floor: `iperf3` alone | - | §4, no `echo-uring` | 1 |
| 3 | The link's measurement on two and three nodes | P38 | §19, its 5 rounds | 0.2 |
| 4 | The chooser at windows 4 and 8, a tree, the fixed (4, 8) | P37 | §14's four cases, 5 rounds, the window-1 and -2 shapes left out | 2.5 |
| 5 | Many PEs a node | H12 | the new section (below) | 12 |
| 6 | Gets and the per-node landing budget | H28 | §8, 1 round | 2.3 |
| 7 | Behind a stream: a barrier, a fetch-add, collectives, `nstat`, a profile | H15-H19 | §13 at 0 and 2 helpers, 1 round | 3.5 |
| 8 | The ladder's rungs 1 and 4 on the earlier trees | H6 | §16, 1 round, the trees built first | 6.5 |
| 9 | The cluster at size: `osoe-bench` at 8 and 16 PEs a node, start-up at 1, 8 and 32, SOS at 2 PEs, `mix` | H11, B8's SOS | §10, `mix` 300 s | 12.5 |
| 10 | The helpers under a site's binding, with and without `--cpus-per-task=2` | H26 | §5 under `binding.sh site`, 0 and 2 helpers, 2 rounds; last, since it restarts Slurm | 8 |
| 11 | Results brought back | - | §11 | 2 |

**About 61 minutes, about $7**, where the sections as scripted, with session 2's
gaps, would take two and a quarter hours. P37's five rounds are the exception to
one: the window's default turns on 18 to 28% against collectives' worst spread of
23%, for two and a half minutes. H26's two rounds: P36's default rides on it.

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

1. **H12's section** (`docs/fast-link/many-pes.sh`, §20 of the runbook): on a pair
   of nodes, `helpers.sh`'s workloads at 16, 32 and 64 PEs a node; the wake's
   cost at 256 PEs a node with a small `SHMEM_SYMMETRIC_SIZE` (256 PEs need about
   4.2 GiB of `/dev/shm`, so that point is the cluster's alone; the VM dry-runs it
   at 64); and `OSOE_CHECK_COLLECTIVES` on and off at each count.
2. **The chain** (`docs/fast-link/session-3.sh`): the sections above with their
   rounds, helper counts and lengths as settings of the kit's existing scripts
   (`KIT_ROUNDS`, `behind.sh`'s and `helpers.sh`'s helper lists, `floor.sh`
   without `echo-uring`, `scale.sh`'s `mix` length, SOS built in setup), each
   section's result written as it ends, the chain going on past a section that
   fails and saying so.
3. **The dry run** (`dry-run.sh`) of the chain whole, as before each session.

Left as they are: two measurements at once on disjoint pairs (no precedent, and
the kit's `scancel` by name would cancel the other's jobs); the `echo-uring`
client's hang, a fault of the kit's, not of the library's, and not run.
