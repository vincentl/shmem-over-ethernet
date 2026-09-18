# The "blocked in" record, read without the job's cooperation

`blocked_record_model.py` is the record of what was run for design §3.14's
"blocked in" record: the routine a PE is waiting in, a routine-specific detail,
and when it started, published by the PE and read by `osoe-stat` from another
process while the PE may be publishing its next wait. `engine.py` is the copy of
the model checker it ran with (`tools/modelcheck/`); `blocked_record_model.out`
is the recorded run.

It is the first record in the control segment with more than one word in it, so
it is the first that can be read torn. Everything else a reader takes is a single
word, where §3.16's single-writer publication is enough.

It answers one question: **can an external reader see a torn record?** It cannot,
for the protocol as designed — two `seq_cst` read-modify-writes around the field
writes, and a reader that loads the generation, reads the fields, and loads the
generation again. Publication *k* writes *k* to both fields, so an accepted pair
whose halves disagree came from two different publications.

| Variant | What it shows |
|---|---|
| `nogen` | no generation at all: the reader takes one field from each of two publications |
| `bumpafter` | the writer stores the fields and bumps afterwards, so the window never covers the writes |
| `hoist` | one field store moved above the opening bump — what a first bump weaker than `seq_cst` permits. The engine models SC and x86-TSO, where store-store order is kept and a `release` store would pass, so the reordering is exhibited directly rather than claimed of a memory model this engine does not have |
| `noverify` | the reader does not re-read the generation after the fields |

Why two `seq_cst` read-modify-writes and not a textbook seqlock: the usual form
uses relaxed stores with a release fence between them, and §3.16 permits neither
relaxed orders nor fences on protocol words. `fetch_add` naming `seq_cst` is a
rule the project already has, and a locked read-modify-write is a full barrier on
both targets, so the field writes cannot escape the window in either direction.

The cost is two locked instructions per *sleeping* wait. The record is published
only after the first predicate check fails, so a wait that returns immediately
pays nothing — which is what §8.6's U1 and U3 measure as "wait with nothing
pending".

Run it with `python3 blocked_record_model.py [substring ...]`; every case
finishes in well under a second.
