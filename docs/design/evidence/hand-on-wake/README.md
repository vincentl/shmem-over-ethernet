# The hand-on wake for a reopened connection buffer (P2), modelled first

P2 of `docs/prompts/v0.2-open-items.md` (round four's scale S1; design §3.11,
"Wake count tracks divisibility"): a buffer reopened wakes one writer waiting
for room, not every one; a writer that reserves and leaves room wakes the next;
and a woken writer that finds too little passes the wake on. The author decided
on 2026-10-28 to build it, its model first (`docs/decisions/2026-10-28.md`).
This is the model, as the first step; nothing in the library is changed yet.

`turnover_wake_model.py` models a connection's open buffer as `ROOM` units,
writers each needing `least` of them and reserving by a CAS, the turnover's wait
word of `base/wait-word.h` (the word, its count of waiters, the two-sided
order), and the agent, which seals and reopens the buffer whenever it holds a
reservation (`ROOM := CAP`) and then wakes. `wake_all` is the library as built;
`hand_on` wakes one at the open and after a reservation that leaves room - the
reservation's CAS, then its load of the count, the other side of a waiter's
registration and then its look at the room - and a woken writer that finds too
little passes the wake on once a generation, then loads the word afresh before
it sleeps, since its own bump changed it. `engine.py` is the copy of
`tools/modelcheck/engine.py` it ran with; `turnover_wake_model.out` the run.

What it found: `hand_on` is live in every case run - two and three writers
needing one or two units of a buffer of two, the largest 11.6 million states,
the engine's check that a finished state stays reachable from every state run
whole - and passes at most once a writer a generation. Without the once a
generation (`pass_always`), two writers too large for the room pass the wake
back and forth: PASSES fails. On the way, a first version's agent, bounded to
one open more than the writers and opening on a writer's request as well,
reported a lost wake that was the bound's: it reopened buffers that were still
fresh and stopped before the last writer's turn; the agent opens on a
reservation alone now, which bounds its opens by the writers' reservations.

What it cannot say. Every design here is live, the bare wake-one too: the agent
opens again after every reservation, and wakes again, so a wake-one only waits
longer. The hand-on is a performance change, and the model has no clock. The
design notes measured a bare wake-one 10 to 25% slower than waking every writer
at 4 and 16 writers in the VM, the chain's scheduling delay a link; the hand-on
is a chain too, shorter where the room runs out. Nor can it see starvation: a
writer whose wake goes to others every generation is allowed by every check
here, where waking every writer cannot starve one. So the build is to be timed -
the node's rate, not only its writers' sleeps, at 4 to 64 writers - before it
replaces waking every writer.
