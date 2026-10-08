# The adversarial read's S1: four one-token mutants, and the tests that fail on them

Each a copy of the tree at `95f3339` with one change, built in debug and run
against `shmem.owners.*` and `shmem.owner-map` (`mutants.sh`; output
`mutants.txt`). With no helpers every one of them passes every other test,
since an owner's index of a wire is then its peer slot.

| mutant | the change | fails |
|---|---|---|
| attach-slot | `wire.c`'s mesh attaches each wire at its peer slot, not its owner's index | `shmem.owners`, all four: "an io_uring completion names wire 2 of the ring's 2" |
| receive-slot | `progress.c`'s `receive` reads `o->wire[slot_of(o, i)]` | `shmem.owners`, all four: receive buffers held across a reap |
| metrics-index | `progress.c`'s `conn_metrics` indexes the records by the owner's index | `shmem.owners`, all four: "the record of slot 2, node 2, counts 0 messages" |
| mark-slot-word | `connection-area.h`'s mark chooses its word by the peer slot | `shmem.owner-map`, at 300 nodes (below 64 wires an owner the word is the same either way) |

The read's third mutation, a wake whose owner is computed apart from the
mark's, is gone with the second computation (`osoe_connection_area_owner`).
