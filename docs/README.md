# Where to find what

`README.md`, at the top of the repository, is the way in: what osoe is, how to
build it and run a program, every limit it has, and a table of the repository's
layout. This directory holds everything else, in three parts: using osoe, how it
works, and the record of how it was built. Start with the part that matches what
you want; each names the `README.md` of the directories that go with it.

## Using osoe

| Where | What |
|---|---|
| `README.md` | what osoe is; building, installing and running a program, under `srun` or on one machine; every limit and every environment variable |
| `docs/pi-cluster.md` | the runbook for the four Raspberry Pi 5 nodes osoe was measured on, under Slurm: checking the nodes, building, the smoke tests, the library against its floor, `osoe-bench`, how jobs end, and programs from the reading corpus |
| `docs/pi-cluster-results.md` | what the sessions on that cluster measured and found, newest last |
| `docs/release-checklist.md` | what a release needs - the release point, what every tag needs, and what the reviews rated for after it - ticked where it is done |
| `docs/reference/README.md` | where to get the OpenSHMEM 1.6 specification, which is not in the repository |

## How it works

| Where | What |
|---|---|
| `docs/design/shmem-ethernet-design.md` | the design notes: every protocol and data structure, and why it is as it is, in ten sections from the goals (§1) to the open questions (§10); §9 lists the invariants the code is held to. Comments in the code cite its sections, as `§4.10`; a reference of the form `§9.x.y` is to the OpenSHMEM specification |
| `docs/design/README.md` | the rest of `docs/design/`: two proposals (`osoe-pacer-proposal.md`, `team-resources-proposal.md`), a note on a possible successor API (`successor-api-stage-setting.md`), the virtual traces of the reference workloads of design §8.6, and the raw outputs the notes cite |
| `tools/modelcheck/README.md` | the model checker for osoe's lock-free and futex protocols, and how to write a model |
| `base/README.md`, `shmem/README.md`, `tools/README.md` | the code: how each directory relates to the rest, and where to start reading |

## The record of how it was built

[How osoe was made](../README.md#how-osoe-was-made) says in a page how the work
was directed, written and reviewed; this part is where each level of it is kept.

| Where | What |
|---|---|
| `docs/decisions/README.md` | the decisions in force, one line each, grouped by what they govern; each names who took it - the author, or Claude under a standing mandate - and links its dated record |
| `docs/decisions/<date>*.md` | the dated records: what was decided on a day or in a piece of work, by whom, why, and how to undo it, with what was built and found |
| `docs/prompts/` | the brief each working session was given: its goal, what to read first, what to build, how to verify it, and the questions to stop and put to the author. The open briefs are in the directory, the finished ones in `done/`; the table in its `README.md` gives each, in the order the work was done, with its outcome and merge commit |
| `CLAUDE.md` | the working agreements a session works under: when to delegate to subagents, how names are spelled, how a rename is audited, how review files are named |
| `docs/reviews/` | the adversarial reviews: `adversarial-review-prompt.md` is how one is run, `archive/` holds the finished ones, and `evidence/` the raw material of the first two rounds; its `README.md` is the guide |

**The evidence.** 908 files sit under `docs/design/evidence/` and
`docs/reviews/evidence/` - `git ls-files | grep -c '/evidence/'` counts them:
model runs, litmus tests, probe programs, logs, patches, and the raw output of
each session on the Pi cluster, which
`docs/design/evidence/pi-cluster/README.md` describes. They are kept as the
evidence behind what the records say, and are not to be read through: the design
notes, the Pi cluster's results and some of the records cite the files a claim
rests on.

Finished briefs and past reviews are history: kept so a line of code or a
decision can be traced to what produced it, not read to follow the project. The
history of git is the rest of the record: almost every commit names the model
that wrote it beside the author, and the records cite commits by hash.

**The development machines.** The records name the virtual machines their runs
were made in. `osoe` is the one `dev/lima/` defines, and the earliest briefs
call it `shmem`; `osoe-load` and `osoe-x86` were made for the round-three review
and are not defined here. `osoe-x86` runs an aarch64 kernel like the others,
with Rosetta registered for x86-64 programs: a run "in `osoe-x86`" is an aarch64
run unless it says x86-64. A fourth, `osoe-16k`, was a throwaway for the second
round's check of a kernel with 16 KiB pages, and
`docs/reviews/evidence/phase1-step2-slice1/portability/page-size-16k-vm.yaml`
defines it. Paths under `build/scratch/` name the scratch tree of the session
that made a run, which git ignores: a record cites them to say where its outputs
were, not to send a reader there.

**What was edited in the evidence.** The evidence, the finished briefs and the
review prompts were written on the author's machine and name its paths where a
command ran. On 2026-10-02 those paths were edited - the repository's own
directory reads `<repo>`, any other path under the home directory reads `~` - so
a script that names `<repo>` records what ran, and is not one to run as written.
The same day the four copies of the specification's examples
(`spec-example-*.c`, in
`docs/reviews/evidence/phase1-step2-slice1/conformance/`) were given the notice
the specification's permission asks for, and every other program then in the two
evidence trees, with the two files in `dev/lima/`, was given the project's
license line, `SPDX-License-Identifier: LGPL-3.0-or-later`: first in the file,
or second where the first line is a shebang, and last in the three probes of
that `conformance/` directory whose compiler errors `run.out` records by line
number, so that those numbers stay true. On 2026-10-04 the Unix account's name,
which `ls -l` had written as owner and group in ten logs under
`docs/reviews/evidence/phase1-step2-slice1/diagnosability/logs/` and as owner in
`docs/reviews/evidence/phase1-step2-slice1/portability/stale-segments.out`, was
replaced there by `user`, and two Pi nodes' hardware addresses by
`xx:xx:xx:xx:xx:xx` in four files under
`docs/design/evidence/pi-cluster/2026-09-26-night-f1/retest-new-port/`. Nothing
else in those files was changed.
