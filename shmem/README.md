# shmem: the library

`libosoe`, the OpenSHMEM 1.6 library, on top of `base/`. `include/` holds the
public headers - `shmem.h`, `pshmem.h` for the profiling interface, `shmemx.h`
for extensions, of which there are none, and `mpp/`, which redirects the
historical location - and `cmake --install` puts them under the prefix. `src/`
holds what is compiled into the library, and `tests/` its tests, which
`tests/CMakeLists.txt` registers; `tests/slurm-fixtures/` has a README of its
own.

`cmake/CheckBoundary.cmake` sorts every file in `src/` into a layer, and its
`shmem.boundary` test fails when an include crosses where it may not. The layers
are the OpenSHMEM routines (`rma.c`, `team.c`, `barrier.c`); the runtime under
them (`bootstrap.c`, the agent thread, failure and abort); the boundary -
`transport.h` for operations between PEs, `internode.h` for a collective's
internode phase, and what both sides share, such as the node's control segment
(`node.h`) and the path between PEs of one node (`transport-local.c`) - and the
one backend below it, active messages over TCP and io_uring (`connection.c`,
`progress.c`, `wire.c`). Start with `src/rma.c`, which takes a put from the
routine to its call through `transport.h`, whose opening comment says what each
side may call; design §2 and §4.10 are the map.
