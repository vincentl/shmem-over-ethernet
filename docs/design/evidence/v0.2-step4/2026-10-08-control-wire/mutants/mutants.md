# The control wire's mutants, and the tests that fail on them

Each a copy of the tree with one change, built in debug and run against the
tests of what it breaks (`mutants.sh`, `ctest -R` of the regular expression
named below, 120 s a test). The first four are of `3d30d1c`, before the
adversarial read (`mutants-3d30d1c.txt`; the busy bit's from a whole-suite
run stopped once its tests had hung, `busy-forgotten-3d30d1c.txt`); the last
four of the tree with the read's answers (`mutants-fixed.txt`).

| mutant | the change | fails |
|---|---|---|
| control-range | the pass takes the control wires' readable bits from one wire too far (`o->count + first + 1`), so slot 0's are never taken | 18 of 30: every barrier and multinode test, `shmem.progress`, `shmem.owners`, the stress pipeline - "1 receive buffers were held across a reap" |
| short-send-restart | after a short send, `flush_control` offers the segment again from its start | 12 of 30, under `OSOE_WIRE_SPLIT` - "segment 2 claims 0 bytes, less than its own header" |
| busy-forgotten | `settle_control` always clears the busy bit, so what queues behind a send in flight waits for the next enqueue | the four `shmem.barrier-am`, `shmem.progress`, the four `shmem.owners`, `shmem.multinode.0-1-2`, each hanging to its limit |
| self-only | the segment lookup never walks /proc past its own descriptors | 10 of 30: `shmem.team-storage.*`, `shmem.stat.*`, `shmem.unswept-wait` - which run `osoe-stat`, a process of its own, against a live job |
| no-refusal | the planes never checked | both `shmem.progress.forged-*`: a barrier arrival taken for one whose tree it is not, a get reply refused as one nobody asked for |
| plane-late | the planes checked only as a message completes, as first built | `shmem.progress.forged-get-reply-on-control`: refused as a reply nobody asked for, its landing looked up first |
| quiet-data-only | `osoe_progress_quiet` without its control wires - F1 as first built | all four `shmem.barrier-am`: a node whose ARRIVE its parent has not released found quiet |
| never-asks | a control segment never asks for an acknowledgement | four `shmem.deadlock` cases across nodes: never declared, since nothing answers an acknowledgement nobody requests |

The regular expressions: `shmem\.(barrier-am\.|progress|owners\.|wire-pair|multinode\.0-1-2$|unswept-wait|team-storage|stat\.)|shmem\.circulant\.(3|5\.stress)$|^stat\.|shmem\.pipeline\..*stress`
for the first four, `shmem\.progress\.forged|shmem\.barrier-am\.|shmem\.deadlock` for the last four.
