# Where a node's progress runs, the gets, the digits: the evidence

The runs behind `docs/decisions/2026-09-27-night.md` items 1, 2, 6, 7 and 8,
4 × 4, release, from the author's Mac (only a hop from it carries the key the
compute nodes accept).

- `local-driver.sh`: `mailbox` and the sort's pull, bound, unbound, and bound
  with the first PE's agent widened to the node's cores partway through. Its
  pull-widened hung at start (item 6) and was run again by
  `local-driver2.sh`, which also gives the agent a 0.1 ms slice (`set-slice.c`,
  through `agent-slice.sh`) and runs the pull with 1 and 4 MiB landing arenas.
  `local-driver3.sh`: unbound with the slice; the sort at 16-bit digits (main,
  `3d5bbe1`) then 11-bit (the branch, `6ac31fc`); ten minutes of `mix` with
  `waits`. The second and third hold a session open on every node throughout.
- `agent-sample.sh`: one line a thread of every PE on a node, from `/proc`;
  `analyze.py` turns two samples into shares of a core. `agent-widen.sh`,
  `agent-slice.sh`: what the drivers do to the lead's agent.
- `samples/`: the samples and the widening's and slice's output, a file a run.
- `outputs/`: the pacer's output of every run (`mix-waits.txt.gz`).
- `summary-*.txt`, `nstat-*.txt`: each driver's log, and each node's TCP
  retransmissions and timeouts before and after each run.
- `removeipc-hang/`: job 828 - `osoe-stat`, each PE's descriptors and wait,
  pi-001's journal around the start, the samples showing single-threaded PEs
  - and `probe.txt`, the removal reproduced on pi-002.
