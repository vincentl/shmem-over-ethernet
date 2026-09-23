# The chained release wake (design §5.3; phase 1 step 2 slice 3, part E)

`chained_wake_model.py` checks the release of one node's members by a chain of
futex wakes: the releaser wakes two, and every member that slept and finds its
target reached wakes two more. Four and five members, one to three epochs, the
releaser standing in for the node's progress thread. `engine.py` is the copy
the model was run with; it adds two instructions to the explorer in
`tools/modelcheck/` - a futex wait with a bitset, and a wake of up to *n*
sleepers that explores every choice the kernel could make of which ones.
`chained_wake_model.out` is the run.

What it found: the argument the design was first given - every waiter on a
release counter waits for the same value - is false. A member woken early
re-enters and sleeps on the same counter for the next epoch, absorbs a wake,
and passes nothing on (`unmasked`: a lost wake at two epochs, none at one,
because one epoch never puts a next-epoch waiter on the word). Waiting and
waking on the target's parity as a futex2 bitset keeps the two epochs' waiters
apart, and the design then holds for every case (`+live`: no trapped state).

What it does not cover: how long a chain takes. On the 4-core development VM
every chain width measured was slower than one wake-all at *L* = 8-32 (§5.3), so
the single-node release keeps the wake-all and only the progress thread's
release chains.
