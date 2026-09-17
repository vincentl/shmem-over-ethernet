# The §4.9 watch slot over a byte range

`range_watch_model.py` is the record of what was run for design §4.9's watch
slot: one slot per local PE, watching a byte range, under `SHMEM_THREAD_SINGLE`
through `SERIALIZED`. `engine.py` is the copy of the model checker it ran with
(`tools/modelcheck/`, whose docstring documents the instruction set).
`range_watch_model.out` is the recorded run.

It answers two questions §4.9 left open:

1. **How is a range published?** One word holding the range rounded out to an
   aligned power-of-two block, or two words holding offset and length. Both hold,
   including on x86-TSO and when a PE watches one range and then another. The
   block word is chosen: it is published and read atomically, so no stale half can
   be read, and the writer's test is a shift and a compare.
2. **Which ordering rules are load-bearing?** Publishing before the first check,
   and updating the variable before reading the slot. Each has a variant here that
   drops it and loses a wake, as does publishing with a plain store on TSO.

`plainput` is the exception that is accepted rather than fixed: a put's `memcpy`
is not a serializing store, so its notify is best effort (§4.10).

Run it with `python3 range_watch_model.py [substring ...]`; every case finishes in
under a second.
