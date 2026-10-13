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
