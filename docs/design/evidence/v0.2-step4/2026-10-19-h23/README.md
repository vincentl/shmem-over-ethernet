# H23's evidence (2026-10-19)

The record is `docs/decisions/2026-10-19.md`. Every run was in the development VM `osoe`
(aarch64, four CPUs, Linux 6.12), alone on it, under `/tmp/osoe-vm.lock`.

- `neighbour-table/`: why tier 3 at 64 logical nodes did not start. `diagnose.sh` built `main` at
  `86c9d9bc` (the tier3 preset) and started `osoe-bench barrier quick` at 32 and 64 nodes with
  the kernel's neighbour table at its default, then at 64 and 128 with it raised; `summary.txt`
  is its output - the `seconds=` and `table_fulls_added=` columns are empty, the VM's `awk`
  having no `strtonum` and the VM no `bc`, and the status and the kernel log's 250 overflow lines
  are the evidence. `stderr-64-default-table.txt`: the 64-node start's notices, the calls timing
  out as in H24's.
- `timing/`: each lead's "connected its mesh in X us" (`SHMEM_DEBUG`) at 16 to 128 nodes over
  tier 3's Pi-like link and 16 to 254 over loopback, the slowest and the median lead a job
  (`measure.sh`, the table raised for the runs and set back after). `before-alone.txt`: the old
  order alone, three rounds (`before-alone.sh`); `alternating.txt`: the old order and the
  schedule alternating round by round (`alternating.sh`), the tree before H23's schedule with the
  debug line alone (`h23-mesh-build`'s first commit) against the schedule's.
- `model/`: `mesh_model.py`'s run (`mesh-model.txt`), `mesh_timing.py check` and its `table` at
  64 to 1,024 nodes and at 2,048 and 4,097 (nine minutes on the author's Mac), and `tier3`, the
  model at four CPUs beside the VM's runs.
- `mutants/`: the nine mutants, each applied to the build's tree, built, its test run, and the
  tree put back (`mutants.py`); every one failed its test.
- `verify/`: the whole verification (`batch.txt`, by `/tmp/batch-verify.sh` and then tier 3's
  suite and the mesh models, `batch-and-tier3.sh`) on `f8e370da`, and the final tree's again after
  the boundary's fix (`final.txt`, `final.sh` and `extra.sh`: the internode tests at two progress
  helpers and the mesh's notice with a lead held back).
