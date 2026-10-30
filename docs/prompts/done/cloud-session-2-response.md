# Cloud session 2: the response

*The second cloud session's summary of its work, written on 2026-10-13 as the brief asks. The
session followed `docs/prompts/cloud-session-2-prompt.md` on 2026-10-13, on the branch
`cloud-session-2` from `main` at `225d205`. The full record is the manifest,
`docs/prompts/cloud-session-2-manifest.md`: every command, every pass count and the evidence for
each item. This file is the short version.*

## In short

- **Part 1 is done whole**, in the brief's order. T32's rest is done. P30's C is built as the
  proposal designs it, with no departure from its protocol. P33 was modelled first, then built.
  P31's choice among the four ways is built, its defaults marked provisional, and H24's harness
  is written and was run here once as a sample. Part 1 was verified in the release, debug, asan
  and tsan presets, and tier 3's collect tests ran over a substitute for netem.
- **Wire and layout, plainly**: P30 is a wire change with no internal-region layout change. It
  moves the node segment to layout 39 and the rendezvous's up message to "osoeup-6". P33 changes
  both the internal region's layout and the wire, and leaves the node segment and rendezvous as
  P30 left them. Nothing negotiates either change: every node runs one build.
- **Part 2: one batch of six**, the settings' validation: D13, D12 and D17, each with a test that
  failed without it. The other batches were not started, to leave credit for this response.
- **Seven commits came before this one, each pushed when made.** Nothing was force-pushed or
  merged, and no pull request was opened. No other branch was touched, and none of the files the
  brief reserves. No new machine path entered the tree.

## The environment

The machine is the first session's: Linux 6.18.44 on four vCPUs and 15.7 GiB. What changed:

- **`ethtool` is installed**, so tier 3 now configures and builds, without a warning.
- **Tier 3 still does not run**: every test is refused at its first netem qdisc. The kernel is
  built without netem (`# CONFIG_NET_SCH_NETEM is not set`) and has no loadable modules. The
  token-bucket qdisc, `tbf`, is built in and works in a namespace.
- **The `debug`, `asan` and `tsan` presets build here without a warning.** `tsan`'s tests run
  only with `OSOE_HEAP_BASE=0x010000000000` (Findings), and so do `main`'s.

## The items

| Item | Outcome | Commits |
|---|---|---|
| T32's rest: `allgather_model.py`'s breakages name their property | done | `5c97305` |
| P30: the leads' scan of the node totals and the gather by ranges | done, as designed | `48a19ae`; the scan's zeros case `4ec2f9e` |
| P33: the direct exchange of the node totals | modelled, then built | `f19fdb6` |
| P31: the choice among the four; H24's harness | done, defaults provisional; sample taken | `58a82bc` |
| Part 1's verification | done; tier 3 by a substitute, tsan with the heap base set | `82f2533` |
| D13: `OSOE_HEAP_BASE=-4096` accepted | done | `b11863c` |
| D12: a set base that cannot be mapped is told to set it | done | `b11863c` |
| D17: `OSOE_TEST_NOTICE_SECONDS` refuses no bad value | done | `b11863c` |

### T32's rest (`5c97305`)

Every check message in `allgather_model.py` now begins with the property it holds. Each
breakage names the property it must fail, and `report` compares the two. The ranged arithmetic
judges its three breakages by name with `engine.judge`.

**Shown**: the whole model, run before and after, gives 66 lines with the same verdicts and
state counts, only the prefixes differing. A copy with two names deliberately wrong prints
UNEXPECTED and exits 1. `selftest.py` passes.

### P30 (`48a19ae`, `4ec2f9e`)

- **The scan**: `osoe_internode_scan_node_totals` is a pre-order tree over the node set's
  positions. It runs through the team number's small rows and counters with the reductions'
  bases.
- **The gather by ranges**: `osoe_internode_collect_ranges` is Bruck's allgather with a record
  past DATA[s] at each step and the share past READY[S - 1]. The last step is cut at the
  receiver's share. Each record is checked as it is read, and the whole result is checked at the
  end.
- **The rest**: collect's third way and the runs' order in `team.c`. `OSOE_COLLECTIVE_PLACES` is
  built early, because the tests force the scan. `team.tree.collect_way` names the way taken.
  `lane_model.py` models the scan beside the reductions.
- **Where the C added to the design**: the proposal's "Stage 2, as built" says what the C found.
  One check was added to the protocol's reading side: a record that runs on into its receiver's
  share is refused, naming the writer and the step.

**Shown**:

- 13 tests at tier 2, among them block maps of two to eight nodes, negative strides, a cyclic map
  that falls back to the members' vector, and an out-of-order map that falls back to each node's
  total. Three more are registered for tier 3. Each test checks every byte, the way and the
  count.
- A mutant whose last step is uncut fails 9 of 19 cases, each at the record that runs into its
  receiver's share.
- `lane_model.py`'s scan cases hold, and its breakage `scan_heap_bases` fails STUCK.
- The zeros case, collects of nothing back to back under perturbation, now runs under the scan
  too.

### P33 (`f19fdb6`)

**Modelled first**, in `exchange_model.py`. Each team number keeps two sets of slots and two
counters, chosen by the exchange's parity, because nothing waits before a lead writes a slot.
The design is ok and live on two to four leads, with and without a gather after; the largest case
has 211,168 states. Each breakage fails where a collect of nothing has no gather: one set of
slots fails SLOT and one counter fails VALUE. Each holds when a gather follows, which is why the
design must not lean on the gather.

**Then the C**: each lead puts a 16-byte slot, its epoch and total, into every other lead and
adds to that lead's counter, fenced after. That is one hop where P29's tree takes a level each
way. The exchange is collect's fourth way, `OSOE_COLLECTIVE_PLACES=exchange`. Its words take room
for 64 nodes, and a larger team falls back to each node's total.

**Shown**:

- `shmem.collect-places.*`, 29 of 29. Among them are the exchange's eight cases, and runs of
  collects of nothing under `OSOE_TEST_PERTURB` at the default and under the exchange.
- Two mutants beside their control, three rounds each. One set of slots fails 7 or 8 of the
  exchange's cases each round. One counter fails the zeros case each round.

### P31 and H24's harness (`58a82bc`)

**The choice**: `osoe_places_cost` and `osoe_places_choose` live in `collective-shape.c`, and
`shape_timing.py` holds the same code line for line. They cost each way under the link's three
numbers, in the closed form the shapes' chooser uses.

- The members' vector wherever some node is not one run.
- Otherwise the cheapest of three: each node's total, the exchange (within its room) and the scan
  (where the runs are ordered).
- Each node's total where no link is known.

**The defaults, provisional**:

- On the Pi's link: the exchange to 60 nodes, then the scan.
- On a loopback's: the exchange to 46 nodes, each node's total to 128, then the scan.

**Shown**:

- `shmem.shape.places-grid` holds the C to the model's way and four costs on all 6,210 inputs of
  `places-grid.txt`.
- `model.shape_timing` checks what must hold whatever H24 moves.
- Two mutants. With the default as before P31, exactly the two `pi-link` cases fail. With an
  exchange that costs no hop, only the grid fails, the C and model differing on 5,940 of its
  6,210 lines.

**H24's harness**, `tools/bench/collect-places.sh`:

- It runs `osoe-bench places` by each way and by the default, at 8 to 254 logical nodes, with
  three patterns of lengths.
- `ROUNDS` repeats the jobs, and every job's measured link is kept with the way the default takes
  on it.
- `TIER3=1` runs the jobs over tier 3's links.
- Two smoke tests keep the harness and `osoe-bench places` working.

The sample took 16 min 15 s, every job exited 0, and the medians are in the manifest. Its numbers
are this machine's (Findings).

### Part 1's verification (`82f2533`)

- **debug** and **asan**: the collect and team tests, 146 of 146 each.
- **tsan**: 73 of 73 at 0 and at 2 helpers, with `OSOE_HEAP_BASE=0x010000000000`.
  - The gate passed both runs.
  - At 2 helpers it passed three reports, all in the circulant's grant words, which is code from
    `main`. None was in P30's or P33's words.
- **Tier 3's 27 collect tests**: 27 of 27 over a copy of `tier3.sh` with `tbf` in place of
  netem. This kept the namespaces, the bridge, the addresses and the small shape's MTU, as shown
  from inside a rank, but not netem's delay or loss.

### D13, D12 and D17 (`b11863c`)

- **D13**: `OSOE_HEAP_BASE` must start with a digit and fit 64 bits. `-4096` is now refused by
  name. Before, `strtoull` turned it into 2^64 - 4096, which is page-aligned and so passed both
  checks.
- **D12**: a lead given a base it cannot map names the base and the kernel's reason, in words.
  Before, it said to set `OSOE_HEAP_BASE`, which was already set.
- **D17**: `osoe_bootstrap_read` refuses a bad `OSOE_TEST_NOTICE_SECONDS` by name, before any
  start-up wait.

**Shown**: four new tests (`shmem.refuse.heap_base_signed`, `.heap_base_not_mapped`,
`.test_notice_seconds.2s` and `.0`) were run against the tree without the code change, and all
four failed. With the change, all four pass.

## Where the tests stand at the end

- **The release preset, models aside**: last run whole at `b11863c`, with **919 tests: 917
  passed, 2 skipped**. The skips are the root refusal tests, as in the first session. Every one
  of this session's items that changed C or tests, P30, P33, P31 and the batch, ran this suite
  whole. `4ec2f9e` added one
  test after that run; the three zeros cases passed three rounds each, 9 of 9.
- **The static checks**: 13 of 13 at every item.
- **The models changed**:
  - `allgather_model.py` whole, in 7 min 17 s;
  - `lane_model.py` whole, 39 cases, in 19 s;
  - `exchange_model.py`, 9 cases, in 17 s;
  - `shape_timing.py` with its two grids.

  `selftest.py` passed with each. The whole model suite was not run, since no change reached the
  engine.
- **Other presets**: debug and asan, 146 of 146 each, and tsan, 73 of 73 at 0 and 2 helpers with
  the gate, all at P31's commit. The single-node build is clean at every item that changed
  C.
- **Tier 3**: 27 of 27 by the substitute, at P31's commit. **Not run under netem.**

## What needs someone else

**The author**:

- **P31's defaults stay provisional** until H24 and H25 measure the ways. One thing bears on how
  to read those measurements: H24's logical nodes share one machine's CPUs, so the exchange's
  N(N - 1) messages are charged together (Findings). Whether to cap the exchange's default at
  what a cluster can measure is the author's call.
- **The exchange's room is 64 nodes**, chosen here to keep the internal region small (P33's
  entry). Room for 256 would add 8.25 KiB to every team number past 256 nodes.

**The lead, at the merge**:

- **Close** in `docs/prompts/v0.2-open-items.md`: T32, P30, P33, P31 with its defaults
  provisional, D12, D13 and D17. H24's harness exists, but the measurement stays open, as does
  H25.
- Give the brief's row in `docs/prompts/README.md` its outcome and merge commit.
- **Run in the development VM** (aarch64). Each manifest entry ends with its list.
  - Tier 3's collect tests under netem: `ctest --test-dir build/tier3 -R collect`.
  - The release and debug suites on aarch64, and the node segment's offsets there. With
    128-byte lines, `ready` should stay at 512.
  - tsan as `docs/fast-link/verify.sh` runs it, which needs no heap base set there.
  - H24 itself: `TIER3=1 ROUNDS=3 sh tools/bench/collect-places.sh build/release <dir>`.
  - D12's own `0x1000`, which should be refused as an address below `vm.mmap_min_addr`. This
    sandbox's limit is 4096, so here it maps.

## Decisions and deviations

- **The branch is `cloud-session-2`**, as the brief says. The session's harness proposed another
  name.
- **The brief's "design §5.8"**: collect's places live in §5.5, in the paragraph "collect's
  places", and that is where this session wrote. §5.8 was not edited.
- **Files beyond the brief's list**, which the items needed:
  - `bootstrap.c` and `.h`, `node.c` and `.h`, `metrics.c` and `rendezvous.c`;
  - `internal-region.h`, `internode-am.c`, `internode-generic.h` and `transport-single-node.c`;
  - `tools/stat/segment.h` and `osoe-bench.c`;
  - the tests, the README and the proposal.

  A concurrent change to any of these will meet this branch at the merge.
- **OSOE_COLLECTIVE_PLACES was built at P30**, not P31, because P30's tests force the scan. P31
  made its default choose.
- **The exchange's room is 64 nodes**, not the 256 first tried (above).
- **Tier 3 ran by a substitute, and tsan with the heap base set**, both said where they occur.
- **The H24 sample was taken with the harness's first form**: one round, no links kept. Rounds
  and links were added after the sample showed the need.
- **No subagents were used.** The mechanical parts were small enough to do directly.
- **Part 2 began with the settings' batch**, the most self-contained of the six.

## Findings, proposed and not fixed

- **H24 on one machine charges the exchange for every lead's messages at once.** On loopback the
  exchange was the slowest way from 16 to 64 nodes, 1.6 to 3.6 times the others. Its N(N - 1)
  messages land on the same four CPUs, where on a cluster each lead's CPU carries its own. Tier
  3's namespaces share the VM's CPUs in the same way, and netem's delays cost no CPU. So H24
  will charge the exchange as this sample did. Only a cluster of tens of nodes can settle where
  the exchange stops winning, and H25's eight nodes reach eight.
- **Job-to-job variance here was as large as the differences between the ways.** At 2 nodes the
  members' vector took 111 µs in one job and 283 to 338 µs in four more. That is why the harness
  now has `ROUNDS`.
- **At 254 nodes the members' vector took 2.5 times every other way.** That holds for its
  fastest collect too, though with one PE a node its vector is the size of each node's total's.
  This session did not explain it. The likeliest cause is that each job measures its own link on
  a loaded machine and a large reduction's shape follows it. The harness now keeps every job's
  link.
- **ThreadSanitizer as root maps the heap at address 0.** The tool's interceptor treats
  `MAP_FIXED_NOREPLACE` as a hint, and replaces a hint outside its ranges with none. The
  library's first two candidates, `0x100000000000` and `0x300000000000`, are outside them, so
  the kernel is asked for address 0 itself. A user gets `EPERM` and the library takes
  `0x010000000000`; root may map page 0, and the tool then ends the process. A tsan build could
  begin its candidates at `0x010000000000`; for now `OSOE_HEAP_BASE` works around it.
- **`engine.report`'s `expect_ok` has no caller** since T32. Drop it at the next change to the
  engine.
- **A failed start can leave its control segment behind.** In D12's test before the fix, the
  launcher reported "left behind: /dev/shm/osoe.N.0.0.0" after the lead refused at the heap's
  mapping. It may belong with D22's family. It was not followed here.
