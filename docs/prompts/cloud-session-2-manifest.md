# Cloud session 2: the manifest

*The record of the second cloud session (`docs/prompts/cloud-session-2-prompt.md`),
2026-10-13, on the branch `cloud-session-2` from `main` at `225d205`, for the lead's
review after the author pulls. Written after every item, each time committed and
pushed. Paths write the repository as `<repo>`.*

## The environment, since the first report

The machine is the first session's (`docs/prompts/cloud-session-1-manifest.md`,
Part 1): Linux 6.18.44-fc-v114 on four vCPUs and 15.7 GiB, `/dev/shm` 16 GiB,
`kernel.io_uring_disabled = 0`, `unshare -rn` working, `ulimit -l` held at 8192 KiB
for root with `CAP_IPC_LOCK`, liburing 2.9 from the setup script. Nothing needed
setting. What changed is `ethtool`, which the setup script now installs
(`/usr/sbin/ethtool`), and with it tier 3 configures and builds:
`cmake --preset tier3` and `cmake --build build/tier3 -j4` in 1 min 32 s, **0
warnings**. **It does not run here**: `ctest --preset tier3 --test-dir build/tier3`
fails all 123 tests in 16 s, each at its first `tc qdisc add dev link0 root netem`
(`tools/launch/tier3.sh`) with "Error: Specified qdisc kind is unknown". This
kernel is built with `# CONFIG_NET_SCH_NETEM is not set` (`/proc/config.gz`) and
has no loadable modules (`/lib/modules` is absent); the token-bucket qdisc, `tbf`,
is built in and works in a namespace. So tier 3's shaped links - their delay and
their loss are netem's - need the development VM, and the lead runs this
session's tier-3 tests there; where a change's tests were run here over the same
namespaces with `tbf` in place of netem, its entry says so. At `main`, the release
preset builds without a warning and the static checks pass 13 of 13.

## Part 1: `collect`'s places four ways

### 1. T32's rest: `allgather_model.py`'s breakages name the property each must fail

**Done.** Commit: the one that adds this entry. Files:
`tools/modelcheck/allgather_model.py`.

- Every check's message now begins with the property it holds, as the first
  session made every other model's: DEST, FORWARD, RESULT, PLACE, RANGE, SCAN,
  and in the leads' scan scan_model.py's SLOT and VALUE - 32 messages, prefixed
  by a script whose every pattern had to match its stated count, and the lines
  the prefixes made longer than 100 columns rewrapped by hand.
- Every breakage names the property it must fail, and `report(res,
  expect=...)` compares: `no_ready` DEST, `no_data_wait` FORWARD, `short_steps`
  RESULT, `staged_one_slot` SLOT, `staged_early_copy` FORWARD,
  `bases_in_node_order` PLACE, and the ranged `range_before_data`,
  `range_before_ready`, `wrap_as_one` and `no_cut` RANGE. The ranged arithmetic,
  which searches with its own code, judges its three breakages by name with
  `engine.judge`, as `circulant_model.py`'s arithmetic does: `heap_tree` SCAN,
  `no_cut` and `wrap_as_one` RANGE.
- The docstring says so; FORWARD's definition now covers a share copied out of
  a staging slot, which is what `staged_early_copy`'s check is and what the
  docstring already named it.
- **Every verdict unchanged**: the whole model before and after, `python3
  allgather_model.py`, 66 lines each, every state count and verdict identical
  and only the prefixes differing; no line UNEXPECTED; exit 0; 7 min 17 s here.
- **Without the change a misnamed breakage passes**: a copy with `no_ready`
  expecting RESULT and `heap_tree` expecting RANGE prints `<-- UNEXPECTED: fails
  DEST, where it must fail RESULT` and `fails SCAN, where it must fail RANGE`,
  and exits 1; under the old `expect_ok=False` both were accepted.
- `selftest.py`: exit 0.
- **Left**: `engine.report`'s `expect_ok`, the older spelling the first session
  kept for this model, now has no caller. Removing it changes the engine, which
  the brief keeps for a change that reruns the whole suite, so it stays; the
  next engine change can drop it.
- **For the VM**: `ctest --test-dir build/release -R "^model\.(allgather|selftest)$"`.

### 2. P30's C: the leads' scan of the node totals and the gather by ranges

**Done.** Commit: the one that adds this entry. As the proposal's stage 2
designs it; its new section, "Stage 2, as built", says the five points the design
left open or the build found, and plainly what is a wire change and what a
layout one.

**Files**: `shmem/src/internode.h`, `internode-generic.c`, `internode-generic.h`,
`internode-am.c`, `transport-single-node.c` (the two calls);
`collectives.c` (the third way, `places_from_scan`, `places_way`, the gather's
own places); `team.h`, `team.c` (the runs' order); `bootstrap.h`, `bootstrap.c`
(`OSOE_COLLECTIVE_PLACES`); `node.h`, `node.c`, `metrics.c`,
`tools/stat/segment.h` (layout 39); `rendezvous.c` ("osoeup-6");
`internal-region.h` (comments: what the gather's lines hold, and zeroed whole);
`shmem/tests/test-collect-places.c`, `shmem/tests/CMakeLists.txt`;
`tools/modelcheck/lane_model.py`; the README, design §5.5, the proposal, the
design and model READMEs, and the evidence's README and directory.

- **The calls.** `osoe_internode_scan_node_totals(team, epoch, own, &before,
  &total)`: a pre-order tree over the node set's positions at the fan-out's
  ceiling, rooted at position 0, through the team number's small rows and its
  arrival and release counters with the reductions' bases, which it advances as
  `in_pieces` does; its pieces' header a `collective` of its own (bit 30), which
  `describe` names. `osoe_internode_collect_ranges(team, epoch, buffer, bytes,
  share_start, share_end, reverse)`: Bruck's allgather with READY and DATA as
  before, each step's record (epoch and two ranges, 40 bytes) past the
  receiver's DATA[s] counter before the fenced add, the share (24 bytes) past the
  READY[S - 1] counter and the shape's room at entry, the last step cut at it,
  each record checked - epoch, within the result, continuing what the receiver
  holds, and not running on into the receiver's share - and the whole result
  checked at the end. The shape's layout word takes a third value, ranges.
- **collect's third way** (`collect_varying`): the scan where the runs meet the
  positions in order or in reverse, never large and so with no
  `osoe_internode_reduce_enter`; the lead's own members' places only, a vector
  of its L members and one; the gather by ranges. The walk keeps the runs'
  order (`node_runs_order`).
- **The setting**, P31's, built here because the tests need the scan forced:
  `OSOE_COLLECTIVE_PLACES` = `link` (the default, as before) | `members` |
  `totals` | `scan`, falling back where the team does not allow the way.
  Checked between a node's PEs (the header, `open_control`) and between nodes
  (the rendezvous). The README's settings table has its row, so
  `shmem.settings-documented` passes.
- **The counts**: `team.tree.collect_lengths` is 1 under the scan, and
  `team.tree.collect_way` names the way (1 members, 2 totals, 3 scan).
- **Wire and layout**, plainly: the internal region's layout is unchanged - no
  size, no offset - but what a lead puts into a peer's region during a collect
  is new (records, shares, the shape's third value, the scan's pieces): **a wire
  change**, which nothing negotiates, every node running one build. **The node
  segment's layout is 39** (`team.tree.collect_way` in the tree state's room, the
  header's `collective_places` in room before the rendezvous words; `ready`,
  `failed` and `aborting` stay at 448, 464 and 512 on x86-64, measured with a
  program on `node.h`). **The rendezvous's up message is "osoeup-6"**, 88 bytes.
- **The model beside the reductions** (`lane_model.py`): a leads' scan, kind
  `q`, is a small collective on its own pre-order tree through the same small
  slots and counters. Five design cases - the scan between small reductions, a
  large reduction and a broadcast turned to another root, two threads of a node
  leading a scan and another team's small reduction at once, five nodes where
  the two trees give node 1 different numbers of children, and five nodes with a
  turned broadcast of two pieces (117,603 states) - each ok and live; the
  breakage `scan_heap_bases`, its bases counted by the reductions' tree, fails
  STUCK as named. The 33 earlier cases' lines are identical to the model's run
  before the change; the whole model in 19 s.
- **Tests**: `shmem.collect-places.scan.*`, twelve at tier 2 - block maps of two,
  three, four, five, seven and eight nodes, negative strides on four of them,
  the cyclic map (its world to the members' vector, its first rank of each node
  the scan) and `-m 1,1,0,0,2,2`, numbered out of the ranks' order (to each
  node's total) - and `shmem.collect-places.unordered.1-1-0-0-2-2` at the
  default; each checks every byte, the way and the count. Two of the block
  case's three teams now tolerate a two-node map (the third, which would span
  one node, is left out by name). Over tier 3's links, three more registered
  (`tier3.<link>.collect-places.scan.*`), not runnable here.
- **The mutant beside its control** (`docs/design/evidence/v0.2-step4/
  2026-10-13-collect-scan/`): the last step uncut. The control passes all 19 of
  `shmem.collect-places.*`; the mutant fails the 9 scan cases whose teams span a
  node count that is not a power of two - on the four- and eight-node maps the
  splits span three, five and six - each at the record that runs on into its
  receiver's share, naming the writer and the step, and passes the other 10. A
  first run of it, before the record's overlap check existed, failed the same 9
  at the end's whole-result check instead ("a broken invariant"): that is why
  the check was added (the proposal's "Stage 2, as built").
- **Runs here**: the release preset without a warning; the static checks 13 of
  13; the release suite, models aside (`ctest --test-dir build/release -LE model
  -j4`), **895: 893 passed, 2 skipped** - the two refusal tests root skips - in
  5 min 10 s, 13 more than at `main` (P29's tests' one more map and P30's
  twelve); `lane_model.py` whole, 39 cases as expected, in 19 s; the
  single-node build (`-DOSOE_SINGLE_NODE=ON`) without a warning, its collect and
  team tests 61 of 61.
- **Not verified here, for the VM**: tier 3's three new tests and P29's
  (`ctest --test-dir build/tier3 -R collect-places`), netem being absent; and
  aarch64 - the release and debug suites, and the header offsets (128-byte
  lines there: `ready` should stay at 512, the field fitting the padding before
  it, as here).

### 3. P33: collect's node totals by a direct exchange among the leads

**Done.** Commit: the one that adds this entry. Modelled first, then built;
its design section is the proposal's new "A direct exchange of the totals".

**Files**: `tools/modelcheck/exchange_model.py` (new) and the models'
`CMakeLists.txt` and README; `shmem/src/internal-region.h` (the exchange's
words), `internode.h`, `internode-generic.c`, `internode-generic.h`,
`internode-am.c`, `transport-single-node.c`, `collectives.c`, `bootstrap.h`,
`bootstrap.c`, `node.h`, `metrics.c`, `tools/stat/segment.h`;
`shmem/tests/test-collect-places.c`, `shmem/tests/CMakeLists.txt`; the README,
design §5.5, the proposal, the evidence's README and directory.

- **The model**, `exchange_model.py`: each lead puts its total into its slot at
  every other lead and adds to that lead's counter; a lead whose counter shows
  N - 1 reads its slots. Its collects run with the gather after - abstracted as
  its one fact that matters here, no lead returning before every lead entered -
  or without, as a collect of nothing has none. Design: two sets of slots and a
  counter each by the exchange's parity, ok and live on two to four leads with
  and without gathers (211,168 states the largest). Breakages: `one_slot`
  fails SLOT and `one_counter` fails VALUE where a collect of nothing stands
  alone, and each holds with a gather after every exchange - the gather alone
  would order them, which is why the design must not lean on it. All nine
  cases as expected, in 17 s. Two slips in the model's own first draft were
  fixed before any result was used: a counter captured late by its lambdas,
  and `one_slot` changing the counter with the slots.
- **The C**: `osoe_internode_exchange_node_totals(team, epoch, own, totals)` and
  `osoe_internode_exchange_most_nodes()` at the seam; the generic
  implementation's puts begin at the next position on, each a 16-byte slot (the
  epoch and the total) and a fenced add, the wait on the parity's counter
  against its cumulative base, each slot's epoch checked as read - naming the
  sender of another collect's total - and cleared. collect's fourth way where
  every node is one run and the team spans no more nodes than the room,
  `places_from_node_totals` taking the totals from the exchange in place of the
  reduction; `OSOE_COLLECTIVE_PLACES=exchange` (falling back to `totals`);
  `team.tree.collect_way` 4; `collect_lengths` the team's nodes, as P29's.
- **Its words, and a decision**: per team number, two counters' lines and two
  sets of 16-byte slots a node position, between the scans' area and the lanes,
  sized by the job's node count **to 64 nodes**
  (`OSOE_INTERNAL_EXCHANGE_NODES_MOST`). I first sized them to 256, to let H24
  force the exchange across all of tier 3's logical nodes, but that adds 8.25
  KiB to every team number in every PE's region past 256 nodes - a third more -
  for a way the cost model loses past a few dozen nodes on every link it knows;
  at 64 it adds 2.25 KiB from 64 nodes on and 384 bytes on two to four. H24's
  harness reports the exchange's fallback past 64.
- **Wire and layout**, plainly: **the internal region's layout changes** - the
  exchange's words move the lanes by their size, computed alike on every node
  from the job's node count - and what a lead puts into a peer's region is new,
  **a wire change**. The node segment stays at P30's layout 39 and the
  rendezvous at "osoeup-6". The README's memory figures follow, computed with a
  program on `internal-region.h` whose "without" column reproduces the old
  figures exactly: a team number 24.6 KiB at 4 nodes, 28.1 at 64, 29.3 at 500
  and 29.6 at a thousand (24.25, 25.9, 27.0, 27.4 before); the region 15.73,
  16.60 and 16.88 MiB at the memory table's three sizes (15.63, 16.04, 16.32).
- **Tests**: `shmem.collect-places.exchange.*`, eight at tier 2 - block maps of
  two, three, four and seven nodes, two negative strides, the out-of-order map
  (which the exchange takes, needing no order) and the cyclic map - and
  `shmem.collect-places.{link,exchange}.zeros.0-0-1-1-2-2-3`, the world
  collecting nothing 96 times with lengths every eighth, under
  `OSOE_TEST_PERTURB`; and two over tier 3's links, registered. 29 of 29.
- **Two mutants beside their control**
  (`docs/design/evidence/v0.2-step4/2026-10-13-collect-exchange/`), three rounds
  each: the control 29 of 29 every round; one set of slots fails 7 or 8 of the
  exchange's cases each round (each case's collects include one of nothing),
  each named by a slot holding the next collect's epoch; one counter fails the
  zeros case every round and one more in two, each named by a slot holding
  nothing. Neither fails a case the exchange does not take.
- **Runs here**: the release preset without a warning; the static checks 13 of
  13; `model.exchange`, `model.lane` and `model.selftest` passed; the release
  suite, models aside, **905: 903 passed, 2 skipped** (the root refusal tests),
  in 5 min 15 s - 10 more than at P30's commit; the single-node build without a
  warning, its collect and team tests 61 of 61.
- **For the VM**: the tier-3 tests (`tier3.<link>.collect-places.exchange.*`);
  aarch64's suites; and the README's memory figures at the defaults there,
  which the program on `internal-region.h` computes the same on any machine.
