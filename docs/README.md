# Where to find what

Start with the status page (Claude Docs, "osoe — project status"): where the
project stands, what waits on the author, and the release checklist. Then:

| Question | Where |
|---|---|
| What is decided and still holds? | `docs/decisions/README.md`, one line a decision |
| What happened on a given day, and why? | `docs/decisions/<date>.md`, the dated records |
| How does osoe work, and why that way? | `docs/design/shmem-ethernet-design.md` |
| What is being built next? | `docs/prompts/`: the open brief; finished ones are in `docs/prompts/done/` |
| How do I run it on the Pi cluster? | `docs/pi-cluster.md`, the runbook |
| What did the Pi cluster measure? | `docs/pi-cluster-results.md` |
| How is a review run? | `docs/reviews/adversarial-review-prompt.md`; past reviews in `docs/reviews/archive/` |
| What does the specification say? | `docs/reference/OpenSHMEM-1.6.pdf` |
| How do I build and run a program? | `README.md` at the top of the repository |

Finished briefs and past reviews are history: kept so a line of code or a
decision can be traced to what produced it, not read to follow the project.

The evidence under `docs/design/evidence/` and `docs/reviews/evidence/`, the
finished briefs and the review prompts were written on the author's machine and
name its paths where a command ran. On 2026-10-02 those paths were edited - the
repository's own directory reads `<repo>`, any other path under the home
directory reads `~` - and nothing else in those files was changed. A script that
names `<repo>` records what ran; it is not one to run as written.
