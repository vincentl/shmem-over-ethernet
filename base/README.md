# base: the library's building blocks

The pieces `libosoe` is made from that need nothing from the OpenSHMEM layer
above them, among them futex waits and wakes (`futex.h`) with the deadlines they
take, the epoch counters a collective's members wait on, the stall watch behind
the rule that no wait gives up merely because time has passed, memory fences,
the check that the kernel has the io_uring the library needs (`ring-check.c`,
the full build's alone), the parsers for
what a job starts from - its settings, Slurm's host lists and the logical node
map - and SipHash, with which a caller proves it knows a job's secret without
sending it.

It builds as an object library, `osoe_base`, folded into `libosoe` and installed
nowhere; `osoe-launch` links it, and `osoe-stat`, `probe` and `osoe-pacer` take
a header or a file of it. It includes nothing from `shmem/`: where it needs what
only the layer above knows, `sweep.h` declares hooks that `shmem/src/` replaces,
and `sweep.c` here defines them to do nothing. Start with `futex.h`, the one
interface to the futex calls - futex2's in the full build (`futex-futex2.c`),
the classic `FUTEX_WAIT_BITSET` and `FUTEX_WAKE_BITSET` in the single-node build
(`futex-classic.c`) - then `wait-word.h` and
`epoch.h`, which use it; every header opens with a comment saying what its piece
is for and which section of the design notes it follows. `tests/` holds a test
program for nine of the pieces.
