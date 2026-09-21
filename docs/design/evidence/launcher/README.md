# The launcher and signals (slice 2, C5)

`signal-experiments.out` is the second review's
`omissions/run-signal-experiments.sh`, run unchanged against the launcher of
commit 8cea8b9 in the development VM (4 KiB pages).

- **A** (SIGTERM to the launcher mid-job): 0 PEs and 0 objects 3 s later. The
  review saw every PE keep running and its heaps stay resident until the PEs'
  own sleep ended.
- **B** (SIGTERM to every PE, `-k`): the launcher also matches the `pkill`
  pattern, so it receives SIGTERM too and exits 143; nothing is left.
- **C, C2** (one PE exits without finalize, `-k`): that PE's node ends the job
  on its own node; node 0 learns nothing and runs to the launcher's timeout.
  Expected until step 4 (remote nodes cannot see a local abort word), and
  without `-k` the launcher ends such a job as soon as the first PE fails.
- **D, D2** (fork, system, popen inside a PE): unchanged, pass.
