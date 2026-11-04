# 2026-11-02: the cluster tests - tier 3's programs under `srun`

The author's answer 4 of 2026-11-02 (`docs/decisions/2026-11-01.md`): before any
large-cluster session, a harness running osoe's cross-node test programs under
Slurm's `srun` through `ctest`, built here, in the development VM's one-node
Slurm. The large-cluster playbook's gate A4 and item C1
(`docs/prompts/large-cluster-playbook.md`) are what it is for: until it existed,
every test of a job started it with `osoe-launch -n N -m MAP` - 239
registrations in `shmem/tests/CMakeLists.txt` - so the suite that checks results
had run on logical nodes of one machine only, and a cluster got the test programs
by hand (`docs/pi-cluster.md` §4, §5 and §7). Built by a session started for it
on branch `claude/adoring-cohen-777120` from `6f6b81d5`, the 0.2.0 tag.

## The ask

A CMake option under which the tier-3 list's programs - `osoe_tier3_test`'s
registrations, each a map, a program and an environment - are registered a
second time as `srun -N<nodes> --ntasks-per-node=<P> program`, a block map to
`--ntasks-per-node`, a cyclic one to `--distribution=cyclic`, each with its
registration's environment, labelled `cluster` and `internode`, with a timeout,
and a `ctest --preset` for it; a `-D` for the partition and the site's `srun`
flags, `-K1 --cpu-bind=none` by default; a wrapper keyed on `SLURM_PROCID` for
what `osoe-launch -e RANK:NAME=VALUE` gives; what does not map left out and
named in the README with the reason each; checked in the VM and on the Pi
cluster, with the four presets' counts unchanged and a planted failure failing;
the README, the runbook and the playbook brought to it. No change to the
library; nothing decided about defaults or protocols without the author.

## What was built

- **The option and its settings** (`shmem/tests/CMakeLists.txt`, after tier 3's
  block): `OSOE_CLUSTER_TESTS`, off by default; `OSOE_CLUSTER_PARTITION`, empty
  for the site's default; `OSOE_CLUSTER_SRUN_FLAGS`, `-K1 --cpu-bind=none`;
  `OSOE_CLUSTER_NODES`, the nodes the site gives at once, a test of more
  registered `DISABLED`, empty for no limit. The full build's: with
  `OSOE_SINGLE_NODE` or under ThreadSanitizer it is a configure error, as tier 3
  is, for tier 3's reasons. `srun` is found on `PATH` or named by `OSOE_SRUN`.
- **The translation** (`osoe_cluster_command`): a map of `count` ranks over
  `nodes` nodes is *block* when rank *i*'s entry is *i* / (count / nodes),
  *cyclic* when it is *i* mod nodes, and *arbitrary* otherwise. Block is
  `srun <flags> -N<nodes> --ntasks-per-node=<P>`, cyclic the same with
  `--distribution=cyclic`, arbitrary `sh tools/launch/srun-map.sh <map> <flags>
  --`. The environment goes before it as `cmake -E env NAME=VALUE...`, which
  `srun` exports to every task with `ctest`'s own - `OSOE_PROGRESS_HELPERS`,
  and `OSOE_TEST_PRINT_LINK` from `osoe_internode_label`.
- **`tools/launch/srun-map.sh`**, for the maps `srun`'s distributions cannot
  give: it writes the hostfile `--distribution=arbitrary` takes, one host a
  rank, from the allocation's nodes in `scontrol show hostnames`' order - the
  order Slurm numbers a step's nodes, so that node *k* of the map is
  `SLURM_NODEID` *k* - and runs `srun -n<count> --distribution=arbitrary` with
  the flags; `--nodes` is refused beside `arbitrary`. Outside an allocation it
  allocates the map's nodes with `salloc -N<nodes> --ntasks-per-node=<the
  fullest node's ranks>` and the partition among the flags, and runs itself
  inside. The hostfile is removed when the step ends; the step's status is the
  script's. The brief expected uneven maps left out; they are carried because
  `0,1,2,0,1,0`, the interleaved map, is uneven - three ranks on node 0, two on
  node 1, one on node 2 - and 19 of the list's 37 registrations run on it, the
  collectives, the teams, the lock, the statics and the deadlock detector among
  them; without it the harness would hold 14. Question 1 below.
- **`tools/launch/rank-environment.sh`**: `RANK:NAME=VALUE... -- PROGRAM`, run
  as every task of a step, exports each assignment in the task whose
  `SLURM_PROCID` is its rank and runs the program. **No registration of tier
  3's needs it**: the one that uses `-e`, `tier3.pi.start-up.wrong-address`,
  names a namespace's address and is left out. Tier 2's 70 registrations that
  use `-e` are not in the list. It has its own test, `cluster.rank-environment`.
- **The registrations**: 45 tests labelled `cluster` and `internode`, `TIMEOUT`
  300 (the floor 400). Eight are the harness's own checks: `cluster.map.<map>`
  for `0,0,0,0`, `0,0,1,1`, `0,1,0,1`, `0,1,2,3`, `0,1,2,0,1,0` and
  `1,1,0,0,2,2`, each kind of translation run with every task comparing its
  `SLURM_NODEID` with its rank's entry of the map; `cluster.rank-environment`;
  and `cluster.internode.0-0-0-0`, the library on one node under `srun` with
  tier 2's stress settings, which the VM runs end to end. The other 37 are
  tier 3's `osoe_tier3_test` registrations, once each where tier 3 has three
  shapes - the cluster's link is whatever it is - with the same map, program,
  environment and arguments, kept in step with tier 3's list by hand (the
  comment there says so); the two with pass and fail expressions
  (`deadlock.*`, `keepalive.default-on.0-1`) keep them. The stress settings
  tier 3 and the cluster tests share moved to one variable,
  `osoe_stress_wire_environment`, defined before both blocks; tier 3's
  registrations are otherwise untouched. By node count: one node 1 (and the
  eight checks' 1 to 3), two 7, three 21, four 2, five 1, seven 3, eight 1,
  thirteen 1.
- **The presets** (`CMakePresets.json`): `cluster` configures the release
  build with `OSOE_CLUSTER_TESTS=ON`, builds under `build/cluster`, and its
  test preset runs the label `^cluster$` alone, `noTestsAction: error`; a
  workflow over the three.
- **The documents**: the README's "How osoe is tested" bullet, its presets
  paragraph, its repository table and the Limits sentence on the suite across
  machines; `docs/pi-cluster.md` §4 made the suite's run, the by-hand smoke
  test kept under it, and §5 naming `cluster.floor`; design §8.2's "Tier 4
  under `ctest`"; the decisions index; and the playbook's A4, C1 and G1
  (question 5 below on how they reached the tree).

## What was checked

In the development VM (Debian 13 aarch64, one-node Slurm of four CPUs,
`select/cons_tres`, `task/affinity`), on this tree:

- `cmake --preset cluster -DOSOE_CLUSTER_NODES=1` configures; `ctest -N -V`
  shows every generated command (the record's evidence,
  `docs/design/evidence/cluster-tests/2026-11-02-vm/commands.txt`): 45 tests,
  the block and cyclic ones plain `srun`, the arbitrary ones through
  `srun-map.sh`, each with its tier-3 environment.
- The build without a warning; `ctest --preset cluster`: the three tests of one
  node pass - `cluster.map.0-0-0-0`, `cluster.rank-environment`,
  `cluster.internode.0-0-0-0` - and the 42 of more are reported `Disabled`.
- `srun-map.sh` by hand: a one-node map through the hostfile, outside an
  allocation (its `salloc`) and inside one; `-p debug` and `--partition=debug`
  reaching the allocation; a map that skips an id, a malformed one and a
  missing `--` refused with status 2; a program exiting 3 giving the script
  status 3; no hostfile left behind (the first version `exec`'d `srun`, and its
  trap never ran: five were left). `rank-environment.sh` by hand: three ranks,
  two assignments on one rank and one on another, each rank seeing its own and
  nothing else; no `SLURM_PROCID` and a rank that is not a number refused.
- **Planted failures**: `SHMEM_SYMMETRIC_SIZE=abc` in `cluster.internode.0-0-0-0`'s
  environment - every PE refuses it by name, `srun -K1` ends the step, `ctest`
  fails the test; and `collect-places block` registered on `0,0,0,0` with
  `OSOE_TEST_MAP=0,0,1,1` - every PE's check that the lead's way and sum are
  the map's fails (`test-collect-places.c:260`), and the test fails. Both
  removed again; the file is as before them.
- **The four presets unchanged**: `debug` 1,214, `release` 1,261 (1,214 but the
  47 models, which its test preset leaves out), `asan` 1,174, `tsan` 841 - the
  counts of `docs/decisions/2026-10-30.md` - with no test labelled `cluster`
  in any; the single-node build 632, as before (`OSOE_CLUSTER_TESTS` is off and
  `OSOE_SINGLE_NODE` with it on a configure error, not tried beyond the
  message's path).
- The static checks on this tree: `shmem.memory-orders`, `shmem.strict-c11`,
  `shmem.boundary`, `shmem.static-link`, `shmem.settings-documented`,
  `shmem.settings-known`, `shmem.pass-expressions` and
  `shmem.single-node.configure`, 8 of 8. The new cache variables are read as
  the build's options by `CheckSettingsKnown.cmake`'s scan, as `OSOE_TIER3`
  is; the scripts read no `OSOE_` name.

**On the Pi cluster: not run.** `pi-login.local`, the head node's name on this
Mac, did not resolve the day the harness was built - no Raspberry Pi 5 on the
Mac's network - so the harness's run on four nodes, block and cyclic, at
`OSOE_PROGRESS_HELPERS` 0 and 2, is owed; `docs/pi-cluster.md` §4 has the
commands, and the README says the harness has not yet run on a cluster. What
the Pi would show that the VM cannot: `cluster.map.*`'s placement on real
nodes - whether `SLURM_NODEID` follows `scontrol show hostnames`' order under
`--distribution=arbitrary`, which `srun-map.sh` assumes and the interleaved
and unordered maps rest on - and tier 3's programs across real kernels and a
real link.

## What was left, and why

Named in the README's bullet as well. Tier 3's registrations not made through
`osoe_tier3_test`, each needing the launcher's own doing:

- `tier3.<shape>.global-exit.*`, six a shape: `global-exit.sh` judges the job by
  the launcher's line for every rank's exit, by `-k`, which leaves the
  survivors to the library, and by how soon after the caller the launcher
  returned; under `srun` only the step's status survives, which the runbook's
  §7 checks by hand (`srun -K0 ... shmem_test_global-exit 7 1 compute; echo $?`).
- `tier3.<shape>.vanished-node.*`, three a shape: `TIER3_CUT`, a link cut inside
  `tier3.sh`'s namespace; on a cluster the cable is pulled by hand (§7).
- `tier3.<shape>.differential`: `cmake/Differential.cmake` runs one program under
  `osoe-launch` over four maps of logical nodes, one of them a single node, and
  diffs the transcripts.
- `tier3.pi.start-up.wrong-address`: `refusal.sh`, with `-e
  1:OSOE_STEP_NODELIST=10.99.0.77,10.99.0.2`, the namespaces' addresses, and the
  launcher's timeout as the test's end.
- The kill-based tests, `dead-pe.sh` and `death-in-wait.sh`, and tier 2's 70
  registrations with `-e`: not in tier 3's list, so not in this one; the
  runbook's §7 runs `shmem_test_dead-pe` by hand.

Not done: the Pi runs (above). Not built: a cluster `global-exit` test checking
the step's status alone, which would be a lesser test than `global-exit.sh`
under another name (question 3).

## For the author

1. **The uneven maps through a hostfile.** The brief left them out; the harness
   carries them, since the interleaved map is one of them and most of the list
   runs on it. `srun-map.sh` rests on Slurm numbering a step's nodes in
   `scontrol show hostnames`' order, which the VM's one node cannot show and
   the Pi's `cluster.map.0-1-2-0-1-0` and `cluster.map.1-1-0-0-2-2` would;
   and its `salloc` outside an allocation is one allocation a test, where
   `ctest` inside `salloc -N4` is one for all. Keep it, or strike it to the
   brief's block and cyclic (then 14 tests of tier 3's 37, and the README's
   bullet rewritten)?
2. **`OSOE_CLUSTER_NODES` empty by default**, so that a site without the
   setting registers every map and `srun` refuses the ones it cannot give,
   where a value disables them in `ctest`'s report. The alternative is a
   default of the Pi's four. Which?
3. **A `global-exit` test for the cluster**: `srun -K0` with the step's status
   checked against the caller's, as §7 does by hand - a lesser check than
   `global-exit.sh`'s, as a test of its own under `cluster.`? Or §7 by hand
   stands.
4. **The cluster's preset runs the tests one at a time** (`ctest`'s default),
   since two steps on the same nodes would share their cores and `/dev/shm`;
   nothing stops `-j`. Should the tests carry `RUN_SERIAL`?
5. **The playbook's amendments twice.** A4, C1 and G1 were first amended in
   the main checkout's working copy, while this branch's base still lacked the
   playbook (`e1be26bc` committed it that morning); the sibling session's
   commit `a3e4bc89`, the intranode combine's model, then swept that edit into
   itself from the shared working tree. The same three replacements are on
   this branch, from `e1be26bc`, so the two merge without a conflict - but
   `a3e4bc89`'s message does not name them. Nothing to do but know it.

## To undo

Remove the block after tier 3's in `shmem/tests/CMakeLists.txt` (from the
cluster tests' comment to its `endif()`), the four `cluster` entries of
`CMakePresets.json`, `tools/launch/srun-map.sh` and
`tools/launch/rank-environment.sh`, and the README's bullet; put the stress
settings' `set()` back inside tier 3's block under its old name if wanted
(the shared variable is a rename, nothing more). The runbook's §4 then goes
back to the by-hand smoke test, kept under the new text.

*2026-11-04*: merged into main at the author's word and the five questions
decided ([2026-11-04](2026-11-04.md)).
