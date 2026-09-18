# The §4.9 watch slot over a byte range

`range_watch_model.py` is the record of what was run for design §4.9's watch
slot: one slot per local PE, watching a byte range, under `SHMEM_THREAD_SINGLE`
through `SERIALIZED`. `engine.py` is the copy of the model checker it ran with
(`tools/modelcheck/`, whose docstring documents the instruction set).
`range_watch_model.out` is the recorded run.

It answers two questions §4.9 left open:

1. **How is a range published?** One word packing region, first grain and a
   saturating grain count (`shmem/src/watch_range.h`), or two words holding offset
   and length. Both hold, including on x86-TSO and when a PE watches one range and
   then another. The one word is chosen: it is published and read atomically, so no
   stale half can be read, and the writer's test is two shifts and two compares.
2. **Which ordering rules are load-bearing?** Publishing before the first check,
   and updating the variable before reading the slot. Each has a variant here that
   drops it and loses a wake, as does publishing with a plain store on TSO.

`plainput` is the exception that is accepted rather than fixed: a put's `memcpy`
is not a serializing store, so its notify is best effort (§4.10).

`endpoints` is why the cases are shaped as they are. It is a predicate covering
only a range's two ends, and it **passes** the scalar case and the straddling
case and loses a wake only on the three-element array. An earlier version of this
model had every element of every case inside one grain, so `endpoints` would have
passed all of it: a model that cannot distinguish a predicate with a hole in the
middle from a correct one proves nothing about the predicate. Elements here are
one grain apart for that reason.

Which coverage predicate to use is a separate question, answered in
`docs/design/evidence/watch-options/` and `docs/reviews/watch-notification-options.md`.
This model is the protocol around whichever one is chosen.

Run it with `python3 range_watch_model.py [substring ...]`; every case finishes in
under a second.
