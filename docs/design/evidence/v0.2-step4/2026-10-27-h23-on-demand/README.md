# H23's data wires on demand: the evidence (2026-10-27)

The record is `docs/decisions/2026-10-27.md`. The VM runs were in the development VM `osoe`
(aarch64, four CPUs, Linux 6.12), each under `/tmp/osoe-vm.lock`, beside other sessions' runs
that took the same lock.

- `model/data-wire-model.txt`: `tools/modelcheck/data_wire_model.py`'s run on the author's Mac -
  54 design cases holding, ten breakages failing their properties with their traces.
- `suites/first-on-demand.txt`: the debug build's internode suite run with
  `OSOE_MESH_DATA_WIRES=on-demand` before the tests of the eager mesh's own arithmetic said they
  were eager: 589 of 593. `suites/three-ways.txt`: the suite eagerly, on demand, and on demand at
  two progress helpers, with the new tests (`scripts/three-ways.sh`): 602 of 602, then 601 of 602
  twice, the one failure the disagreement's own test, since fixed.
- `mutants/`: `mutants.sh` and both runs' results, `mutants.txt`.
- `scripts/`: the development build (`dev-build.sh`, the tree packed by `pack.sh`), a suite run
  (`suite.sh`), the whole verification (`verify.sh`, `verify-and-tier3.sh`) and tier 3 at 64
  logical nodes (`tier3-64.sh`), whose outputs are `verify/` and `tier3/`.
