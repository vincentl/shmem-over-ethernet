# Models for `SHMEM_THREAD_MULTIPLE` (design §4.11)

Exhaustive interleaving models of the concurrent protocols §4.11 adds, in the
style of the review's models (`docs/reviews/evidence/`). Python 3, standard
library only.

| File | What it checks |
|---|---|
| `engine.py` | The explorer: SC and x86-TSO memory, futexes, ghost state and checks, state-space reductions, progress lines and limits |
| `get_completion_model.py` | A quiet completes every nonblocking get issued before it was called, on a shareable context; starvation table |
| `dirty_set_model.py` | A quiet waits for every put issued before it was called, with concurrent issuers and quiets |
| `watch_list_model.py` | No lost wake with per-thread watch records, overlapping ranges, array waits, record reuse, SC and TSO |
| `selftest.py` | The engine's reductions change no verdict |
| `*.out` | Recorded runs |

Run a model, or only the cases whose names contain a substring:

```bash
python3 get_completion_model.py
```

```bash
python3 dirty_set_model.py "unlocked"
```

Each model checks the design and deliberately broken variants. A result line ends
in `ok`, `VIOLATION`, `LOST WAKE` or `STUCK`, followed by a trace for anything other
than `ok`. `<-- UNEXPECTED` marks a result that differs from what the case
expects. The last line reads `ALL AS EXPECTED` when every case matched.

**Speed.** About 40,000 to 55,000 states a second. The watch model and selftest run
in under a minute each, and the dirty-set model in about five minutes. The
get-completion model's largest cases take longer; progress lines every 15
seconds show states, depth and the largest memory value, so an unbounded loop
shows up as depth or a value that keeps growing. Runs stop with `TIME LIMIT`,
`STATE LIMIT` or `DEPTH LIMIT` rather than hang.

**Fingerprints.** The visited set stores a 96-bit hash of each state. Each result
line prints the bound on the chance that a state was wrongly skipped
(`miss<...`). Pass `exact=True` to `explore` to store full states.

A copy of these scripts, for reuse outside this design task, is in `tools/modelcheck/`
at the repository root.
