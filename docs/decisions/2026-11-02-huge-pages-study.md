# 2026-11-02: the huge-pages study - what was read, probed, measured and left

The author's answer 9 of 2026-11-02 (`docs/decisions/2026-11-01.md`, "The
author's answers"): `OSOE_AM_HUGEPAGES`, retired on 2026-10-11 and still in the
tree, is neither removed nor kept until the code's complexity and the cost of
real huge-page support have been explored - hugetlbfs heaps as design §4.1
sketched them, transparent huge pages by `madvise` on the `/dev/shm` objects,
and what each buys at 256 PEs a node - and the estimate brought back. This is
the study's record; the proposal it produced is
`docs/design/huge-pages-proposal.md`, which ends in a recommendation and five
questions for the author. Done by a session the lead started, on a branch, with
nothing merged and nothing pushed.

## What was read

- `shmem/src/node.c`: the heap object's size (`heap_size`, `internal_offset`,
  `internal_bytes`, `heap_bytes`), the lead's choice of base among five
  candidates every one a multiple of 4 GiB (`heap_base_candidates`,
  `create_own_heap`, `MAP_FIXED_NOREPLACE`), `map_siblings` mapping every
  node-mate's heap and statics object where the kernel puts them with
  `MADV_DONTDUMP`, the control segment's layout and creation (`create_control`,
  `align_up`, `page_round`) and the `/dev/shm` capacity check with
  `OSOE_SHM_CAPACITY_CHECK`; `shm-object.c`, the creation that reclaims a dead
  job's leftover and `osoe_shm_room`'s `statvfs`; `connection-area.c`, the
  `MAP_HUGETLB` branch and its warning (lines 408 to 423) and `layout_of`;
  `am-configuration.c` and `.h`, `init.c`'s info line, `unknown-settings.c`,
  and the `am-hugepages` row of `shmem/tests/CMakeLists.txt`.
- `tools/stat/segment.c`, the discovery of a live job's unlinked segment
  through `/proc/<pid>/fd` (`walk_descriptors`, which keeps targets under
  `/dev/shm/` alone) and `tools/stat/stat.c`'s `--gc` of a dead job's heaps by
  the names its segment records; `tools/launch/launch.c`'s listing of
  `/dev/shm/osoe.<job>.*` after a job.
- Design §3.4 (the area, its table's `OSOE_AM_HUGEPAGES` row, "Huge pages are
  off by default", the NUMA first-touch paragraph), §4.1 (the heaps, "Costs",
  "Choosing B") and §6.2's last paragraph on the heaps' pages; the README's
  "Memory" table, its `OSOE_AM_HUGEPAGES` row and `osoe-launch`'s section;
  `docs/ceilings.md`'s `/dev/shm` and `vm.max_map_count` rows; the playbook's
  C14 (`docs/prompts/large-cluster-playbook.md`, uncommitted in the main
  checkout, read there) and the 2026-10-11 record's item 7.
- The kernel's side from memory of `mm/shmem.c` and the administrator's guide
  on transparent huge pages, then checked by the probe below rather than
  trusted: the sysfs `shmem_enabled` governs the internal shmem mount; a tmpfs
  mount follows its own `huge=`; `shmem_get_unmapped_area` aligns a mapping to
  the huge page where the mount allows one; hugetlbfs reserves at `mmap` and
  shares page tables between processes at 1 GiB-aligned ranges.

## What was probed

`docs/design/evidence/huge-pages-study/shmem-huge-probe.c`, run by
`run-probe.sh` in the development VM (Debian 13, Linux 6.12, aarch64, 4 KiB
pages; `vm-facts.txt` has the VM as shipped): a 256 MiB object mapped twice,
once where the kernel chooses and once on a 2 MiB boundary, advised or not,
touched whole, with `ShmemHugePages`, the mappings' `ShmemPmdMapped` and the
process's `VmPTE` read after (`probe-output.txt`). Under each setting in turn,
each restored: the VM as shipped; the sysfs knob at `advise`; `/dev/shm`
remounted `huge=advise`, `within_size` and `always`. Its findings, which the
proposal's table carries:

- `MADV_HUGEPAGE` on a `/dev/shm` object gives nothing under the sysfs knob at
  `advise`: 0 kB huge, `VmPTE` 1,072 KiB. A `memfd` object under the same knob
  is huge whole: 262,144 kB, `VmPTE` 44 KiB. So the knob the task named is not
  `/dev/shm`'s setting; the mount's `huge=` is.
- `/dev/shm` at `huge=advise` honours the `madvise` (huge whole) and nothing
  without it; `within_size` and `always` give huge pages with no advice and no
  code.
- With the mount at anything but `never` the kernel places `mmap(NULL, 256
  MiB)` on a 2 MiB boundary itself; as shipped it does not (64 KiB).
- `VmPTE` for two mappings of 256 MiB touched whole: 1,068 KiB in small pages,
  44 to 48 KiB in huge ones - the 2 MiB arithmetic, 512 KiB of entries a
  mapping, as predicted.
- Debian 13 ships `enabled=always`, `shmem_enabled=never`, `/dev/shm` without
  `huge=`, a root-owned hugetlbfs at `/dev/hugepages` with no pages reserved
  and `vm.nr_hugepages=0`. RHEL 9's values were not read on a machine: the
  Rocky VM was removed on 2026-11-01, and the proposal says they are the
  kernel's defaults as far as the documentation says.

## What was measured

`random-puts.c` and `osoe-bench quick` on the release build of the branch
(`6423f7db`, the three `madvise` lines under `OSOE_HUGE_PAGES_PROBE`), built
without a warning from a `git archive` under `/var/tmp` in the VM and run by
`run-measurement.sh` under the lock in four configurations with the mount
restored after (`measurement-output.txt`): `/dev/shm` as shipped; remounted
`huge=advise` with the patch on; the same without it, as control; and
`huge=within_size` with no code. The proposal's table carries the numbers; in
short:

- The pages were huge under the `madvise` at `huge=advise` and under
  `within_size` alone, 388 to 390 MiB of `ShmemHugePages` with two PEs' heaps
  mapped, and not otherwise.
- A PE's `VmPTE`: 456 to 464 KiB in small pages, 60 to 72 KiB in huge ones,
  the two 192 MiB arrays' entries gone from 768 KiB to 3 KiB, as the
  arithmetic says.
- Random 8-byte puts into a node-mate's heap: 11.6 and 10.7 M a second in
  small pages, 12.5 and 13.2 in huge ones - 8 to 15%, repeatable over three
  runs, a floor since the VM's second-stage translation is still in small
  pages. In sequence: no gain, and 15% slower under `within_size` in all three
  runs, unexplained here.
- Over TCP between two logical nodes and in `osoe-bench quick`, the
  configurations differ by less than the noise of four PEs and two agents on
  four CPUs; one outlier (the remote bandwidth at 4 KiB under `within_size`,
  413 MiB/s against about 7,000) in one run.

The VM was left as it was found: `/dev/shm` without `huge=`, `shmem_enabled` at
`never`, no huge pages reserved; the build tree under `/var/tmp/osoe-huge`
removed.

## What was written

- `docs/design/huge-pages-proposal.md`: the objects as they are, the
  arithmetic of page tables, TLB reach, faults and memory rounding at 16 to 256
  PEs a node, the probe's table, three options with the files and functions
  each touches and a line count - a THP `madvise` setting, about 90 lines of C;
  hugetlbfs objects as files or `memfd`s, about 200 lines and a layout bump;
  `memfd` heaps of ordinary shmem under the sysfs knob, about 120 - the
  measurement, the NUMA question in a paragraph, a recommendation to remove the
  setting and write the mount option into the README and the playbook, with
  `memfd` heaps named as the design to build if a cluster shows the need, and
  five questions.
- `docs/design/README.md`'s line for it; this record; the evidence directory.
- On the branch alone, a measurement patch (`6423f7db`): `madvise(...,
  MADV_HUGEPAGE)` on the own heap, each node-mate's heap and the connection
  area when `OSOE_HUGE_PAGES_PROBE` is set - three call sites, not a setting,
  not for `main`; question 5 of the proposal asks whether it is reverted before
  any merge or kept as a test hook.

## What was left

- No change to the library for `main`: `OSOE_AM_HUGEPAGES` is where it was,
  the 2026-10-11 decision stands as the intent, and the removal waits on the
  author's answer to question 1.
- No run on a two-socket machine: the NUMA paragraph is design, unmeasured.
- No RHEL 9 reading of the THP defaults, and no x86-64 run: the TLB-reach
  figures for x86 cores are from their published sizes, not measured.
- No hugetlbfs run: with `vm.nr_hugepages` at 0 in the VM and the heaps' whole
  reservation the cost the proposal argues against, nothing a run here would
  change was in question; the arithmetic is the estimate.
- The README's and the playbook's text about the mount option, which the
  recommendation asks for, is not written: it follows the author's decision.

*2026-11-04*: merged into main without the measurement patch, and the five
questions answered by their recommendations ([2026-11-04](2026-11-04.md)):
`OSOE_AM_HUGEPAGES` removed the same day.
