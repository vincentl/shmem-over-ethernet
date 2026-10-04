# Where to find what

Start with the decisions index, `docs/decisions/README.md`: what is decided and
still holds, and who decided it. What waits on the author is at the end of the
newest record of the author's own decisions - the dated files named for a day
alone, `docs/decisions/<date>.md` - and what a release needs is in
`docs/release-checklist.md`. Then:

| Question | Where |
|---|---|
| What is decided and still holds? | `docs/decisions/README.md`, one line a decision |
| What happened on a given day, and why? | `docs/decisions/<date>.md`, the dated records |
| How does osoe work, and why that way? | `docs/design/shmem-ethernet-design.md` |
| What is being built next? | `docs/prompts/`: the open brief; finished ones are in `docs/prompts/done/` |
| How do I run it on the Pi cluster? | `docs/pi-cluster.md`, the runbook |
| What did the Pi cluster measure? | `docs/pi-cluster-results.md` |
| How is a review run? | `docs/reviews/adversarial-review-prompt.md`; past reviews in `docs/reviews/archive/` |
| What does the specification say? | The specification is not in the repository; `docs/reference/README.md` says where to get it |
| How do I build and run a program? | `README.md` at the top of the repository |

Finished briefs and past reviews are history: kept so a line of code or a
decision can be traced to what produced it, not read to follow the project.

The records name three development virtual machines. `osoe` is the one
`dev/lima/` defines; `osoe-load` and `osoe-x86` were clones of it, made so that
runs could go side by side, and are not defined here. `osoe-x86` runs an
aarch64 kernel like the others, with Rosetta registered for x86-64 programs: a
run "in `osoe-x86`" is an aarch64 run unless it says x86-64. Paths under
`build/scratch/` name the scratch tree of the session that made a run, which
git ignores: a record cites them to say where its outputs were, not to send a
reader there.

The evidence under `docs/design/evidence/` and `docs/reviews/evidence/`, the
finished briefs and the review prompts were written on the author's machine and
name its paths where a command ran. On 2026-10-02 those paths were edited - the
repository's own directory reads `<repo>`, any other path under the home
directory reads `~` - and the four copies of the specification's examples
(`spec-example-*.c`, in `docs/reviews/evidence/phase1-step2-slice1/conformance/`)
were given the notice that the specification's permission asks for. The same
day every other program in those two trees, and the two files in `dev/lima/`,
was given the project's license line, `SPDX-License-Identifier:
LGPL-3.0-or-later`: first in the file, or second where the first line is a
shebang, and last in the three probes in that `conformance/` directory whose
compiler errors `run.out` records by line number, so that those numbers stay
true. The specification's examples are the specification's, and have none.
On 2026-10-04 the Unix account's name, which `ls -l` had written as owner and
group in ten logs under `docs/reviews/evidence/phase1-step2-slice1/diagnosability/logs/`
and as owner in `docs/reviews/evidence/phase1-step2-slice1/portability/stale-segments.out`,
was replaced there by `user`, and two Pi nodes' hardware addresses by
`xx:xx:xx:xx:xx:xx` in four files under
`docs/design/evidence/pi-cluster/2026-09-26-night-f1/retest-new-port/`, as the
newer files of that tree already have them. Nothing else in those files was
changed. A script that names `<repo>` records what ran; it is not one to run
as written.
