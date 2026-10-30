# Cloud session 3: the manifest

*The record of the third cloud session (`docs/prompts/cloud-session-3-prompt.md`),
2026-10-13, on the branch `cloud-session-3` from `main` at `aeabc5e`, for the lead's
review after the author pulls. Written after every item, each time committed and
pushed. Paths write the repository as `<repo>`. The response,
`docs/prompts/cloud-session-3-response.md`, is the complete account; this file is
the per-item log behind it.*

## The environment, since the second report

Unchanged from the second session's (`docs/prompts/cloud-session-2-manifest.md`):
Linux 6.18.44-fc-v114 on four vCPUs and 15.7 GiB, `/dev/shm` 16 GiB,
`kernel.io_uring_disabled = 0`, `vm.mmap_min_addr = 4096`, `ulimit -l` held at
8192 KiB for root with `CAP_IPC_LOCK`, liburing 2.9, `ethtool` installed, gcc 13
as `cc` with gcc-14 and clang beside it, cmake, ninja and Python 3. The kernel
still has no netem (`# CONFIG_NET_SCH_NETEM is not set`; `tc qdisc add dev lo
root netem delay 1ms` says "Specified qdisc kind is unknown"), and `tbf` is built
in. Nothing needed setting.

At `main` (`aeabc5e`): `cmake --preset release` and `cmake --build build/release
-j4` in 51 s, **0 warnings**; the static checks (`ctest --test-dir build/release
-R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' -j4`)
**13 of 13**.

## Part 1: the author's decisions

### 1. P35: the leads' exchange capped at 16 nodes until H25

**Done.** Commit: the one that adds this entry. Neither a wire nor a layout
change: the default's choice among ways every node already has.

**Files**: `shmem/src/collective-shape.h` (the constant and the chooser's
comment), `collective-shape.c` (`osoe_places_choose`), `collectives.c` and
`bootstrap.h` (comments); `tools/modelcheck/shape_timing.py` (`choose_places`,
the grid's node counts, `check_places`); `shmem/tests/places-grid.txt`
(regenerated); `shmem/tests/CMakeLists.txt` (a 17-node case); the README's
`OSOE_COLLECTIVE_PLACES` and `collect` rows and its internode count; design
§5.5's paragraph on the choice; the model README's `shape_timing.py` row.

- **The cap**: `OSOE_PLACES_EXCHANGE_DEFAULT_NODES_MOST = 16` in
  `collective-shape.h`, and `EXCHANGE_DEFAULT_NODES_MOST = 16` in
  `shape_timing.py`. `osoe_places_choose` and `choose_places` allow the
  exchange on no more nodes than its room and than the cap, so past 16 nodes
  the default takes each node's total or the scan, whichever the link's cost
  makes cheaper. `OSOE_COLLECTIVE_PLACES=exchange` still forces the exchange to
  its room of 64 (`places_way`, unchanged).
- **The defaults it gives** (`python3 tools/modelcheck/shape_timing.py places`),
  before and after:
  - the Pi's link, 1 or 16 PEs a node: `2-60 exchange, 61-1024 scan` before;
    `2-16 exchange, 17-25 totals, 26-1024 scan` after;
  - a loopback's link, 1 or 16 PEs a node: `2-46 exchange, 47-128 totals,
    129-1024 scan` before; `2-16 exchange, 17-128 totals, 129-1024 scan` after.
- **The grid**: `shape_timing.py places-grid shmem/tests/places-grid.txt`
  regenerated it with 17 added to its node counts, so that the cap's own
  boundary is a line: 6,480 lines where it had 6,210. Of the 6,210 old inputs,
  960 change their way - every one at 31, 46, 47, 60, 61 or 64 nodes, from the
  exchange to each node's total or the scan - and none changes a cost. The 270
  new lines at 17 nodes are 144 totals, 90 members and 36 scan: never the
  exchange, which the costs alone would take there (on the Pi's link 200 µs
  against each node's total's 450).
- **PLACES** (`check_places`) gains: on the Pi's link and a loopback's, 17
  nodes, ordered and not, do not take the exchange - which the costs alone
  would cross on both links.
- **A case end to end**: `shmem.collect-places.pi-link.unordered.1-0-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16`,
  seventeen logical nodes of one PE on an out-of-order map under the Pi's link
  set by hand, where the scan is not allowed: the world's collects must take
  each node's total. The registration's `pi-link` cases take an optional fourth
  field naming the way (`totals` here), the others keeping the exchange. 0.2 s.
  The README's descriptive internode count goes from 509 to 510.

**Shown** (each against the change with the cap removed, then restored):

- **The C without the cap** (`osoe_places_choose`'s `allowed` back to `nodes <=
  exchange_nodes`), rebuilt: `ctest --test-dir build/release -R
  '^(shmem\.shape\.places-grid|shmem\.collect-places\.)' -j4` 37 of 39 -
  `shmem.shape.places-grid` fails ("6480 lines ..., 1140 where the C and the
  model differ", the first "the C chose exchange: 17 17 1 0 64 86000 7000
  116000000 8 4 1024 0 totals ..."), and the 17-node case fails on every PE:
  "the world spans 17 nodes, each one run, out of order, and its lead here took
  way 4 and summed 17 lengths for a collect's places under
  OSOE_COLLECTIVE_PLACES=link, not each node's total and 17". Restored and
  rebuilt: 39 of 39.
- **The model without the cap** (a copy with `exchange = nodes <= room`):
  `places-grid-compare` exits 1 ("is not what shape_timing.py places-grid
  writes now"), and `check` exits 1 with "PLACES: on the Pi 17 nodes take the
  exchange past the default's 16" and the same on loopback.
- **In the sandbox, with the change**: release without a warning; `ctest -R
  '^(shmem\.shape\.|model\.shape_timing)'` 13 of 13 (`model.shape_timing` in 84
  s); the static checks 13 of 13; the release suite, models aside (`ctest
  --test-dir build/release -LE model -j4`), **921: 919 passed, 2 skipped** (the
  root refusal tests, `shmem.wire.zero-copy-refused` and
  `shmem.internode.zero-copy-refused.0-1-2`), in 246 s.

**The brief's "design §5.8"**: the choice among the four is described in §5.5,
"collect's places", where P31 put it and the second session wrote (its
response, "Decisions and deviations"); §5.8 holds the large collectives' shape
chooser and fcollect's allgather but nothing of the places' choice, so the
default and its reason went into §5.5's paragraph and §5.8 is unchanged.

**For the VM**: `ctest --test-dir build/release -R '^(shmem\.shape\.|model\.shape_timing|shmem\.collect-places\.)'`.

### 2. P34: ThreadSanitizer run as root, the heap kept off address 0

**Done.** Commit: the one that adds this entry. Neither a wire nor a layout
change: the flags of one `mmap` in a ThreadSanitizer build.

**Why it landed at 0**, found here as root in the `tsan` preset (gcc 13.3's
libtsan, x86-64), with `OSOE_HEAP_BASE` unset:

- `osoe-launch -n 2 -t 30 -- build/tsan/shmem/tests/shmem_test_barrier` ends at
  start: `ThreadSanitizer: mmap at bad address: addr=0x000000000000
  size=0x000010f53000 res=0x000000000000`, rank 0 exiting with the tool's 66.
- `strace -ff -e trace=mmap` under the same job shows the library's first
  candidate, `0x100000000000`, reaching the kernel as
  `mmap(NULL, 284504064, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED_NOREPLACE, 4, 0) = 0`.
- So: the tool's interception of `mmap` replaces an address outside its
  application memory with none where the call does not say `MAP_FIXED`, and
  refuses it, `EINVAL`, where it does. The first two candidates are its shadow
  and its meta ranges on x86-64 (and outside its application memory on aarch64
  too, by §4.1's ranges). `MAP_FIXED_NOREPLACE` is not `MAP_FIXED` to the tool,
  so it asked the kernel for address 0 itself. A user is refused, `EPERM`,
  below `vm.mmap_min_addr`, and the library takes the third candidate,
  `0x010000000000` - the same job under `setpriv --reuid=65534` starts, both
  heaps at `0x10000000000`. Root may map page 0 (`CAP_SYS_RAWIO`), the kernel
  does, and the tool, finding its result outside its application memory, ends
  the process.

**The fix** (`shmem/src/node.c`): under ThreadSanitizer - `__SANITIZE_THREAD__`
(gcc) or `__has_feature(thread_sanitizer)` (clang), `UNDER_THREAD_SANITIZER` -
the heap's `mmap` says `MAP_FIXED` as well as `MAP_FIXED_NOREPLACE`. The kernel
reads the two together as `MAP_FIXED_NOREPLACE` alone (since 4.17, the
library's stated floor, `do_mmap` sets `MAP_FIXED` itself for that flag); the
tool refuses a candidate it keeps, `EINVAL`, and the library tries the next, so
no request for address 0 is ever made: the candidate at 0 is never taken. And
`heap_refusal` names `EINVAL` under the tool: "ThreadSanitizer keeps that
address for itself, outside its application memory" - so a base
`OSOE_HEAP_BASE` sets there is refused by name. Other builds are unchanged
(`MAP_FIXED` only where the macro is 1). The macro was checked with gcc 13 and
14 and clang 18, each with and without `-fsanitize=thread`: 1 with, 0 without.

**Tests** (`shmem/tests/CMakeLists.txt`):

- `shmem.heap-base.candidates` (new, every preset): `cmake -E env
  --unset=OSOE_HEAP_BASE SHMEM_DEBUG=1 osoe-launch -n 2 -- shmem_test_rma`,
  passing on a start-up line naming the heap at `0x...` and failing on a rank's
  bad status, a signal, a timeout or a leftover.
- `shmem.refuse.heap_base_not_mapped` (D12's): under ThreadSanitizer its reason
  is the tool's - "ThreadSanitizer keeps that address for itself" - where it
  was "it runs past" in every preset. Before this change it failed under the
  tool too, as root by the tool's death and as a user by "the kernel refuses an
  address that low", the tool having asked for address 0.

**Shown**, in the `tsan` preset, as root, `OSOE_HEAP_BASE` unset:

- **Without the fix** (`node.c` as at `main`, the tests as above), `ctest
  --test-dir build/tsan -R '^shmem\.(heap-base\.candidates|refuse\.heap_base)'`:
  3 of 6 fail - `shmem.heap-base.candidates` ("rank 0 killed by signal 6", the
  tool's report in its log "mmap at bad address: addr=0x000000000000"),
  `shmem.refuse.heap_base_not_mapped` (the same death), and the gate,
  `tsan.reports`, which reads both reports. As a user (`setpriv --reuid=65534`)
  D12's case says "the kernel refuses an address that low (vm.mmap_min_addr)"
  of `0x7fffffffffff0000`.
- **With it**: the same 6 of 6; as a user, D12's case names the tool, and the
  2-PE job maps both heaps at `0x10000000000`, as root does.
- **The whole `tsan` preset**, root, nothing set (`ctest --test-dir build/tsan
  -j4 --timeout 300`): **623 tests, 622 passed, 1 skipped**
  (`shmem.wire.zero-copy-refused`, root's `CAP_IPC_LOCK`), the gate passing, in
  4 min 28 s - where the second session ran only subsets, and only with
  `OSOE_HEAP_BASE=0x010000000000` set.
- **release**, rebuilt: no warning; the static checks and the heap tests
  (`-R 'heap-base\.candidates|refuse\.heap_base|heap-shift'` with the static
  checks' expression) 30 of 30.

The README's `OSOE_HEAP_BASE` row and design §4.1 (a bullet beside the
candidates) say what is now true.

**Left**: MemorySanitizer's interception rewrites an address outside its
application memory the same way; osoe has no msan preset, so the macro names
ThreadSanitizer alone.

**For the VM**: as a user, which never met the defect, `ctest --preset tsan -R
'heap-base|refuse\.heap_base'` and the gate; and as root if the VM allows it,
`sudo ctest --test-dir build/tsan -R 'heap-base|refuse\.heap_base'` - aarch64's
first two candidates are outside the tool's application memory too.

## Part 2: what the cluster needs next

### 3. P32: the landing zone from a per-node budget

**Done.** Commit: the one that adds this entry. Neither a wire nor a layout
change: the zone's size was already the lead's to lay out and every PE's to
check (`landing_arena_bytes`, layout 23 on); only its default moves.

**The rule** (the author's A5, `docs/decisions/2026-10-12.md`): unset,
`OSOE_GET_LANDING_BYTES` is the node's budget of 4 MiB split among the node's
PEs, rounded down to whole 1 KiB extents, and no less than 256 KiB nor more
than 1 MiB a PE: 1 MiB to each of 1 to 4 PEs a node, 819 KiB to 5, 682 KiB to
6, 512 KiB to 8, 273 KiB to 15, 256 KiB from 16. Each node counts its own PEs,
so the nodes of a job may differ; the setting still replaces it, and is still
checked among a node's PEs. The blocking get stays as built.

**Files and functions**:

- `shmem/src/am-configuration.h`: `OSOE_GET_LANDING_NODE_BUDGET_BYTES` (4 MiB),
  `OSOE_GET_LANDING_LEAST_DEFAULT_BYTES` (256 KiB) and
  `OSOE_GET_LANDING_MOST_DEFAULT_BYTES` (1 MiB) in place of
  `OSOE_DEFAULT_GET_LANDING_BYTES`; `osoe_get_landing_default_bytes(local_count)`;
  and `osoe_am_configuration_read` takes the node's PE count.
- `am-configuration.c`: the default's function, and the reader's default from it.
- The reader's four callers pass `b->local_count`: `node.c` (the lead's layout
  and every PE's check), `connection-area.c`, `transport-am.c`'s settings check.
- `node.c`'s check between a node's PEs: the zone's comparison moved after the
  job's identity, `local_count` among it, so that a PE counting the node's PEs
  otherwise is told the job differs, not that the zone does.
- `init.c`: each PE's `SHMEM_DEBUG` start-up line names its zone - "landing
  zone N bytes", after the heap - which is how the tests see the default, and
  how a cluster's run records it; `SHMEM_INFO`'s text gives the default.
- Tests: `test-am-size.c` (the rule at 1 to 256 PEs a node, and the setting
  over it at 1 and 64), `test-get-pieces.c` (the zone the rule gives, and a get
  a piece's room short of it going out whole), `test-context.c` (its unset
  zone by the rule, a path no registration takes), `landing-default.sh` and its
  eight cases, `shmem/tests/CMakeLists.txt`.

**Tests that fail without it**:

- `shmem.am-size`: `osoe_get_landing_default_bytes` and the reader at 1, 2, 3,
  4, 5, 6, 8, 15, 16, 17, 64 and 256 PEs a node, and `OSOE_GET_LANDING_BYTES=4096`
  over it at 1 and 64.
- `shmem.landing-default.{L1,L4,L5,L8,L16,L17,0-0-0-0-0-1,set.0-0-1}`
  (`landing-default.sh`, tier 2): a job of 1, 4, 5, 8, 16 and 17 PEs on one
  node, one of a node of five PEs and a node of one, and `OSOE_GET_LANDING_BYTES=4096`
  on a map of two nodes; every PE's start-up line must name the zone its own
  line's count of its node's PEs gives - the script computes it in shell, apart
  from the C - or the setting. A heap of 8 MiB, so that seventeen PEs ask little
  of `/dev/shm`. 0.03 to 2.6 s each.
- `shmem.get-pieces.0-1`, one PE a node: a get of 983,040 bytes - a piece's
  room, 64 KiB, short of the 1 MiB zone - finds no room at no issue, where the
  256 KiB zone before held four of its pieces.

**Shown**: the change with the default as before - `osoe_get_landing_default_bytes`
returning 256 KiB, the start-up line and the tests kept - rebuilt, `ctest -R
'^shmem\.(landing-default|am-size|get-pieces|start-up\.landing|context|get-nbi)'`:
7 of 38 fail - `shmem.am-size` ("1 PEs a node: a zone of 262144 bytes, not
1048576" and seven more counts), `shmem.landing-default.{L1,L4,L5,L8,0-0-0-0-0-1}`
("a PE of a node of 5: a zone of 262144 bytes, 838656 due"), and
`shmem.get-pieces.0-1` ("a get of 983040 bytes into a zone of 1048576: 6 issues
found no room"); L16, L17 and the set case pass, as they must, the rule giving
256 KiB there too. Restored: 39 of 39 with `shmem.heap-base.candidates`, and the
get-pieces case "a get of 983040 bytes into a zone of 1048576: 0 issues found no
room".

**What it costs**: the control segment of a node of 4 PEs grows by 3.05 MiB,
measured from PE 0's mappings in a job of 4 nodes × 4 PEs here (1,839,104 bytes
at 256 KiB a zone, 5,033,984 by default); the README's memory table's 4 × 4
column follows (2.1 to 5.2 MiB, what every PE maps 1.06 to 1.07 GiB), and its
64 × 64 and 500 × 256 columns, at 256 KiB a zone either way, do not move.

**In the sandbox**: release without a warning; the static checks 13 of 13; the
release suite, models aside, **930: 928 passed, 2 skipped** (the root refusal
tests), in 244 s; the single-node build (`cmake -S . -B build/single-node
-DOSOE_SINGLE_NODE=ON`) without a warning, its suite 421 of 421 in 59 s, the six
one-node `landing-default` cases among them.
The README's `OSOE_GET_LANDING_BYTES` rows (the settings and their costs), the
gets' two Limits entries - the measurements kept as taken, with a zone of
256 KiB, the default then - the memory table and the `/dev/shm` paragraph, its
internode count (512), and design §4.6's "As built: nonblocking gets" say what
is now true.

**For the VM**: `ctest --test-dir build/release -R '^shmem\.(landing-default|am-size|get-pieces|context|get-nbi|start-up\.landing)'`,
and the README's 4 × 4 figure with the VM's 128-byte lines. H28 measures the
split on the cluster (B14's `gets.sh`).

### 4. B14: the kit's missing sections

**Done for six of its seven; H7's stopped**, as the brief allows: it needs the
library changed. Commit: the one that adds this entry. No library file changes:
the kit's scripts, probes, summary and runbook only; no wire or layout change.

**The four trees** the lead's map named are in `git log` as named:
`3ee3357` (2026-10-07, the author's answers to step 4's §8 - the tree before
Part 0's first rung), `de3dc42` (2026-10-07, the first rung merged: a put fills
the room the open buffer has), `49c62d3` (2026-10-08, the second and third
rungs merged), `2dcf8d6` (2026-10-08, the fourth rung merged: the pass parses
each provided buffer where the kernel put it); `44f24bb`, the fifth rung, which
§6 also listed, is not needed for rungs 1 and 4.

**Each section, in the kit's form** - `common.sh`'s `kit_start` and `launch`,
its settings as environment variables with defaults, `rotate`'s order each
round, and its `run.txt` - with its runbook section and its line in
`dry-run.sh`:

- **H25, `collect`'s places: `collect.sh`, §12.** `osoe-bench places quick`
  by each way forced (`OSOE_COLLECTIVE_PLACES` members, totals, scan,
  exchange) and by the default, link, every node at once at 8, 16 and 32 PEs a
  node, three rounds, the ways in another order each: H24's
  `tools/bench/collect-places.sh` as the kit runs jobs under Slurm.
  `summarize.py` gains the places lines - each way's medians side by side for
  each PE count and pattern - and, for each default job's measured link, the
  way `shape_timing.py`'s `choose_places` takes on it (P35's cap included).
- **H28, the landing budget: `gets.sh`, §8.** A loop over `PES_PER_NODE` (1,
  4, 8 and 16 by default) for `multistream get`, and `ZONES` "default
  262144 1048576", "default" the zone unset - the script unsets
  `OSOE_GET_LANDING_BYTES`, so the library's own (P32) runs, and each PE's
  start-up line names it; the default's files carry no zone, which
  `summarize.py` prints as "default (unset)". The first session's sweep is
  `ZONES="262144 1048576 4194304 16777216" PES_PER_NODE=8`.
- **H15 to H19, behind a stream: `behind.sh` with `profile.sh`, §13.** At 0,
  1, 2 and 4 helpers, three rounds: `hol` - now any number of nodes, two PEs
  each, each node's first PE streaming to the next node's but the last's (on
  two nodes PE 0's stream alone, as before) - timing a fetch-add (H16), the
  barrier of the nodes' second PEs (H15), and their reduction and broadcast of
  8 bytes and 64 KiB (H17), alone and behind the streams; `nstat -n`, a
  `multistream put` and `nstat` on every node (H18); and the same stream with
  every node's CPUs profiled beside it (`perf record -a -g` by `PERF`, then
  its report by thread name, library and symbol; H19) - a step of the job's
  own allocation, `srun --overlap`, through `common.sh`'s new `beside`, since
  a job holds its nodes whole. `summarize.py`'s hol table is by operation and
  helper count, and it prints nstat's lines of loss and retransmission and a
  profile's top lines.
- **H13, the window and the chooser: `window.sh` with `shapes.c`, §14.** A new
  probe, `shapes`: a sum reduction and a broadcast over the world at 64 KiB,
  256 KiB, 1 MiB and 4 MiB, in osoe-bench's line form (osoe-bench's own
  `collectives` stops at 64 KiB). By `OSOE_COLLECTIVE_SHAPE` link and tree and
  every fixed shape of fan-outs 2, 4, 8 and windows 1, 2, 4, 8, at 8 PEs a
  node, three rounds. `build.sh` builds `shapes` with the other probes.
- **H27, latency tuning: `tuning.sh` with `tuning-node.sh`, §15; `facts.sh`,
  §2.** Each round the image's settings and the tuned - coalescing off by
  `ethtool -C` (each parameter by its own call, so that one a driver refuses
  leaves the others set), busy polling on (`net.core.busy_read`,
  `busy_poll`, 50) - in another order: `ping` from the first node to the
  second, `osoe-bench quick latency atomic barrier` one PE a node on the two,
  `hol` there, the mailbox on every node, and `floor.sh` whole (`FLOOR=1`);
  each node's settings kept before and put back at the end, and by a trap when
  the script is ended. `facts.sh` records `net.core.busy_read`, `busy_poll`
  and `kernel.perf_event_paranoid`.
- **H6, the ladder's code rungs: `trees.sh`, §16.** The four trees written by
  `git archive` into `build/trees/COMMIT`, copied to every other node as
  `build.sh` copies the tree, and built on every node at once, each with the
  kit's `multistream` and `hol` compiled by its own `oshcc`; then between the
  first two nodes, two PEs each, every tree and the kit's own in another order
  each round: `multistream put` at 64 KiB and 1 MiB, `hol`, the mailbox. §6
  points to it.
- **H7, io_uring's task-run flags and registered files: stopped, §17.** `main`
  has no setting that turns `COOP_TASKRUN`, `DEFER_TASKRUN` or registered
  files on: Part 0 measured them in experiment builds chosen by an environment
  variable and merged none (`docs/decisions/2026-10-07-one-thread-profile.md`,
  "Measured and not taken"), and no commit holds them (`git log --all
  -i --grep=taskrun` finds only the merge that records them not taken). A
  section needs that setting in the library, which the brief keeps out of the
  kit's work; §17 says so.

**Other files**: `hol.c` (operations, any number of nodes; its line gains
`op=` and `bytes=`, its count is `operations=` where it was `fetch_adds=`,
which nothing read; the ladder's four-PE fetch-add runs as before, with no team
split); `summarize.py` (above, and the mailbox's lines name their case, which
tuned and default share); `dry-run.sh` (`SECTIONS` picks sections; the new
sections at small sizes); the runbook's opening, its table of what Part 4
decides, §2, §6, §8, §11 (what to bring back for the new sections, and what
the kit once left out now in it); `docs/README.md`'s row for the kit.

**The dry run**, here, after `OUT=<scratch> KIT_LAUNCHER=osoe-launch bash
docs/fast-link/build.sh <nodes>` (the release build and the six probes,
`shapes` among them, the probes' only warnings `multistream.c`'s three
`-Wformat-truncation`, from `main`): `SECTIONS="facts gets collect behind window
tuning trees" bash docs/fast-link/dry-run.sh <dry>/out`, five logical nodes on
four vCPUs - every section exit 0, **no job failed**, 8 min 21 s:

| Section | Jobs | What it showed |
|---|---|---|
| facts | 1 | the new keys read |
| gets | 6 | the default's start-up lines "landing zone 1048576 bytes" at 2 PEs a node; summarized |
| collect | 5 | the four ways' and the default's places lines, summarized side by side |
| behind | 40 | hol's six operations at 0, 1, 2 and 4 helpers, alone and behind; nstat's retransmissions; the profile's file "perf: command not found" |
| window | 6 | `shapes`' lines at the chooser's two and four fixed shapes, in osoe-bench's table |
| tuning | 11, and the floor twice | `eth0` refused the coalescing call, busy polling set to 50 and put back to 0, ping and the rest in both states |
| trees | 20 | the four trees built ("built, 3 warnings" each - the probe's three), every case on each and on the kit's own |

`tuning-node.sh` then came to set each coalescing parameter by its own call,
since one refused parameter failed the whole call: `SECTIONS=tuning` again,
exit 0, no job failed, 51 s - `eth0` refusing `adaptive-tx` alone, the others
set, busy polling 50 and back to 0 at the end. The records, the repository and
the scratch directory written `<repo>` and `<dry>`, are
`docs/design/evidence/v0.2-step4/2026-10-13-kit-b14-dry-run/` (18 files, its row
in that README). The static checks' licence and settings tests, with the new
files staged: 4 of 4.

**Not shown here, and for the cluster**: every Slurm path is unexercised in this
sandbox, which has no Slurm - `launch`'s `salloc`/`srun` as before, and new:
`beside`'s `srun --overlap` inside the job's own allocation, and `trees.sh`'s
copy to other nodes (`ssh`, `rsync`) and its build by `srun`; `perf` (absent
here; `PERF="sudo -n perf"` or `kernel.perf_event_paranoid=-1` on the cluster);
`ethtool -C` on a NIC that takes the coalescing settings; and any number. The
first cluster session should run each new section once with `ROUNDS=1` before
its full run, as the first session did the kit.

**For the lead**: the runbook's new sections are §12 to §17, after §11, so that
the open items' references to "FL §11" for H15 to H19 still find their text,
which now points to §13; the brief's sections numbered otherwise would move
them.

## The end of the session

**Part 3 was not started**, to keep the credit for the response; every item of it
remains (the response, "What remains of Part 3").

**The final tree, `48fcda2`'s** (no library or test file changed after P32):

- `tsan`, as root, nothing set (`ctest --test-dir build/tsan -j4 --timeout
  300`): no warning, **631 tests, 630 passed, 1 skipped**, the gate passing, 4 min
  20 s;
- `debug` (`cmake --preset debug`, `ctest --test-dir build/debug -LE model -j4`):
  no warning, **930 tests, 928 passed, 2 skipped**, 6 min 12 s;
- release's static checks: 13 of 13.

The complete account is `docs/prompts/cloud-session-3-response.md`.
