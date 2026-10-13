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
