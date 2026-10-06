# tools: the programs beside the library

One directory for each program, or group of tests, that this tree builds besides
`libosoe`: `oshcc`, which compiles a program against osoe (`wrapper/`);
`osoe-launch`, which runs a job on one machine (`launch/`); `osoe-stat`, which
reads a running or hung job (`stat/`); `osoe-pacer`, the stress tester
(`pacer/`); `osoe-bench`, which times the library (`bench/`); `probe`, which
checks a kernel (`probe/`); `echo-uring`, the bare io_uring echo the library is
judged against (`echo-uring/`); and the model checker with the models of osoe's
protocols (`modelcheck/`, which has a README of its own). `corpus/` is not
built: it holds the scripts that fetch public OpenSHMEM programs to read. Of
these, `cmake --install` installs `oshcc`, `osoe-stat`, `osoe-pacer` and
`osoe-launch`; `echo-uring` is the full build's alone, the single-node build
having no io_uring to measure. The table of the repository in the root
`README.md` says what each directory builds and which of its programs are
installed.

`osoe-bench` and `osoe-pacer` are OpenSHMEM programs, linked against `libosoe`.
`osoe-stat` includes nothing from `shmem/src`, so that a copy built from one
commit reads the job of another (`stat/segment.h` says why). The library's tests
start the jobs they run with `osoe-launch`. Start with `launch/launch.c` and
`wrapper/oshcc.in`, which a program's author meets first: each opens with a
comment saying what it does.
