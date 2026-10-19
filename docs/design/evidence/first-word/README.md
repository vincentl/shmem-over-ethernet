# The first word: a survivor ended under its own line (2026-10-18)

What `shmem.death-in-wait.barrier.L4` did about one run in 26, and what showed
why; `docs/decisions/2026-10-18.md`, "The silent survivor of
shmem.death-in-wait.barrier.L4", says what was found and changed. Every run was
made in the `osoe` VM (aarch64, Debian 13, four CPUs), idle unless it says
otherwise, from a git archive or a working tree's tar extracted in its `/tmp`,
under the VM's lock.

**The scripts.** `setup.sh` extracts a tree and builds it in each preset named;
`refresh.sh` extracts a working tree's tar over one and rebuilds;
`repeat.sh` runs the test's own command line, as `ctest -N -V` lists it, a
number of times, keeping each failure's output and a tally of every run;
`make-trace.py` makes the traced copy of the end paths (never committed);
`mutants.sh` and `loaded.sh` take the fix apart in the fixed tree and run the
test against each piece; `verify.sh` verifies a commit whole, as the batches'
verification does, then runs the test in release before and after.

**A tally's line**: the run's number, the script's status (0 a pass), the
milliseconds the run took (in `repeat.sh`'s tallies), and how many lines of
each kind the survivors wrote: `agent` the agent's "ending: the job is
aborting on behalf of PE 3", `sweep` the main thread's sweep's
"shmem_barrier_all: the job is aborting on behalf of PE 3", `lead` the lead's
"local PE 3 ... has ended without finalizing". A run passes with one `lead` and
two of the others.

- `kept-batch-15.txt`: the failure batch 15's verification kept, debug, at
  main `5ef7e197`'s tree: PE 1 exits 1 and says nothing.
- `before-debug-tally.txt`, `before-debug-failures.txt`: 1000 runs at main
  `5ef7e197`, debug. 39 fail, each with a non-lead survivor silent (run 518
  both); every run takes 109 to 138 ms, and 408 of the 1,960 non-lead lines are
  the sweep's.
- `traced-debug-tally.txt`, `traced-debug-failures.txt`,
  `traced-debug-passing.txt`: 1000 runs of the traced copy, debug; 8 fail. Each
  failure's output and its trace, every PE's events in time order, in
  microseconds from the first: `agent` a PE's agent thread, `other` any other
  of its threads, here its main thread. In every failure the silent PE's agent
  has begun its end ("abort_end: this thread ends the process") when its main
  thread's sweep sees the abort, enters `fatal`, and takes the first word
  ("first word: won"); the agent then loses it and calls `_exit`, and the main
  thread's "fatal: flushed" and "fatal: wrote" never come. The passing file's
  third run is the near miss: the main thread's write lands between the
  agent's lost word and its `_exit`.
- `after-debug-tally.txt`: 1000 runs as fixed, debug: none fail, every run
  with both non-lead lines. Two take about 230 ms, as two of the traced copy's
  do, which has no wait for the word.
- `mutants.txt`: `mutants.sh`'s run, each mutant made from the fixed tree:
  without the wait, `shmem.fatal-order` fails and the death test passes 1000
  runs; with the word held across a flush and a `stat` again but the waits
  kept, both pass.
- `loaded.txt`: `loaded.sh`'s run, beside four busy loops: before the fix 36
  of 1000 fail, as fixed none, and without the wait - the word still taken
  right before the write - 3, each with a survivor silent. Each tally's
  summary, then the failing runs' outputs.
- `release.txt`: 1000 runs in release at main `5ef7e197` and at the
  verification's tree (`verify.sh`), the same way: 25 fail before, each with a
  survivor silent, and none after.
