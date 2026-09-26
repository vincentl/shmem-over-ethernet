# Coverage predicates for the §4.9 watch slot

`coverage_model.py` is the record of what was run for
`docs/reviews/archive/watch-notification-options.md`. `engine.py` is the copy of the
model checker it ran with (`tools/modelcheck/`); `coverage_model.out` is the
recorded run. Run it as `python3 coverage_model.py [substring ...]`; every case
finishes in well under a second.

It answers one question: **does replacing the coverage predicate change the
§4.9 protocol?** The answer is no — the same three ordering rules are
load-bearing for every predicate, and the same three broken variants fail with
the same traces — and the model says so by running each candidate predicate
through `checkfirst`, `notifyfirst` and `plainpub`.

It also says out loud what `docs/design/evidence/watch/range_watch_model.py`
cannot see. That model's watched ranges are a few bytes wide, so every element
of every case lies inside one 16-byte grain. A predicate that covered a range's
two **ends** and not its middle passes it. Here, `endpoints: scalar` and
`endpoints: two near elements` pass and `endpoints: spread array` loses a wake,
which is the demonstration: the elements of `SPREAD` are 64 bytes apart, so a
grain-level or endpoint-level miss is reachable.

| Case family | What it shows |
|---|---|
| `design block/exact/sig` | each candidate predicate holds, on SC and TSO, for a scalar wait, two near elements, a spread array, and a write spanning several grains |
| `endpoints` | a predicate that keeps only a range's two ends loses a wake — and only for a range wider than one grain, which is why the §4.9 model could not find it |
| `satzero` | a saturating length that encodes "none" instead of "to the end of the region" loses a wake. Saturation must be permissive |
| `sigfirst` | a signature whose writer tests only the first grain of its own multi-grain write loses a wake. The writer must test every grain it touched, or say yes |
| `checkfirst`, `notifyfirst`, `plainpub` | the §4.9 and §3.16 ordering rules, re-checked against each new predicate. Invariant 13, the writer's update-before-load, and the seq_cst publication store |
