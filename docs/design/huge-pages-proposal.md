# Huge pages for osoe's shared objects: what they would cost and what they buy

**For the author's decision.** `OSOE_AM_HUGEPAGES` was retired on 2026-10-11
(`docs/decisions/2026-10-11.md`, item 7) and is still in the tree, documented
as having no effect. On 2026-11-02 the author asked (`docs/decisions/2026-11-01.md`,
answer 9) that before it goes or stays, the complexity and the cost of real
huge-page support be explored: hugetlbfs heaps, which design §4.1 sketched and
never built; transparent huge pages by `madvise` on the `/dev/shm` objects; and
what each buys at 256 PEs a node, where the large-cluster playbook's C14 names
the page tables as the cost. This is that exploration. What was read, probed and
measured is in `docs/decisions/2026-11-02-huge-pages-study.md`; the probe, the
program and their outputs are in `docs/design/evidence/huge-pages-study/`.

The short version: **the premise the study began with is wrong in one place,
and that place decides most of it.** The kernel knob
`/sys/kernel/mm/transparent_hugepage/shmem_enabled` does not govern `/dev/shm`.
It governs the kernel's *internal* shmem mount alone - `memfd_create`, System V
shared memory, anonymous shared mappings. A `/dev/shm` object gets huge pages
only by the `/dev/shm` mount's own `huge=` option, which is a site's to set,
and once a site sets it to `within_size` or `always` every osoe object has
huge pages with no code at all. So a `madvise` setting in osoe buys something
only on a site that mounted `/dev/shm` with exactly `huge=advise`, and a
hugetlbfs heap reserves every PE's whole heap at the moment it is mapped, which
is 71 GiB a node at 256 PEs and the default heap. The recommendation at the end
is to remove the setting, write the mount option into the README and the
playbook with the numbers below as the reason to ask a site for it, and leave
`memfd` objects - the one shape of osoe code that would make huge pages
osoe's own to ask for - as a design the playbook can call for if a cluster shows
the need.

## What the objects are today

Every PE's symmetric heap is a `/dev/shm` object, `<control segment>.heap.<local
id>`, holding `SHMEM_SYMMETRIC_SIZE` rounded to the page and then the
library's internal region - 282 MiB at the defaults, 256 of heap and 26.2 of
region (`node.c`, `osoe_node_open`: `heap_size`, `internal_offset`,
`internal_bytes`, `heap_bytes`). The lead creates its own at the first free of
five candidate bases, every one a multiple of 4 GiB, with `MAP_FIXED_NOREPLACE`
(`create_own_heap`, `heap_base_candidates`); every other PE maps its own at the
base the lead published; and every PE maps every node-mate's heap wherever the
kernel puts it (`map_siblings`), and its statics object beside it, each with
`MADV_DONTDUMP`. The control segment, one a node, is a `/dev/shm` object of
about a megabyte and 27 KiB a PE (`create_control`); the connection area, one
a node on a job of several nodes, is a `/dev/shm` object of 512 KiB a peer node
(`connection-area.c`, `osoe_connection_area_open`), and it is the one place the
tree asks for huge pages: `MAP_HUGETLB` when `OSOE_AM_HUGEPAGES=1`, which Linux
refuses for a file that is not on hugetlbfs, `EINVAL`, before it looks for a
page - so the PE warns and maps ordinary pages (lines 408 to 423; design §3.4).
Objects are created by `osoe_shm_create` (`shm-object.c`), which reclaims a
dead job's leftover of the same name; the capacity check asks `statvfs` of
`/dev/shm` for the node's whole request, the control segment and P heap
objects (`node.c`, `osoe_shm_room`); and `osoe-stat` finds a live job's
unlinked control segment through `/proc/<pid>/fd` of any PE holding it open
(`tools/stat/segment.c`, `walk_descriptors`, which keeps only targets under
`/dev/shm/`), and its `--gc` removes a dead job's heaps and statics by the names
the segment records, `<segment>.heap.<n>` (`tools/stat/stat.c`,
`gc_segment`); `osoe-launch` lists `/dev/shm/osoe.<job>.*` after a job for what
was left behind (`tools/launch/launch.c`).

Every byte a process touches in any of these costs it an 8-byte page-table
entry per 4 KiB page, in its own page tables: Linux shares no page tables
between processes for shmem (only hugetlbfs shares, below). Heap pages are
committed only when written; the capacity check counts the objects whole.

## What huge pages would buy, by arithmetic

**Page tables.** A 4 KiB page costs 8 bytes of last-level page table, one
2,048th of what it maps; a 2 MiB page costs 8 bytes of the level above, one
262,144th. At the default heap object of 282 MiB:

| PEs a node | A PE touching every byte of every node-mate's heap, 4 KiB pages | the same, 2 MiB pages | the node, 4 KiB | the node, 2 MiB |
|---|---|---|---|---|
| 16 | 8.8 MiB | 18 KiB | 141 MiB | 0.3 MiB |
| 64 | 35 MiB | 71 KiB | 2.2 GiB | 4.4 MiB |
| 256 | 141 MiB | 282 KiB | 35 GiB | 71 MiB |

The 35 GiB is the playbook's 34 GB, and it is the pathological case: every PE
touching every byte of every heap, which an all-to-all through `shmem_ptr`
could do and a stencil never does. What every job pays is nearer the diagonal:
a PE's own heap is 564 KiB of page tables fully touched, and the lead's agent
holds entries for every byte a remote put ever landed in any heap, so the
lead's process is where the tables grow on a job of many nodes. The connection
area is small beside the heaps: 250 MiB at 500 nodes is 500 KiB of entries a
PE fully touched, 125 MiB a node at 256 PEs; 32 MiB at 64 nodes is 63 KiB a PE.

**The TLB.** This is where huge pages pay at any PE count. A second-level TLB
holds one to three thousand entries on the cores osoe runs on - 1,280 on a
Neoverse N1, 1,536 on Skylake, 2,048 on Sapphire Rapids and Zen 3, 3,072 on
Zen 4 - a reach of 4 to 12 MiB in 4 KiB pages and 2 to 6 GiB in 2 MiB pages. A
256 MiB heap is 65,536 small pages: a random access into it, or into any
node-mate's, misses the TLB and walks the tables, and the walk's own reads miss
the caches once the tables are gigabytes. That is the pattern of a hash table,
a histogram, bale's `indexgather`, GUPS, and every intranode AMO to a scattered
address; a streaming put of a mebibyte touches 256 pages in order and the walk
hides behind the copy. The measurement below is the random pattern.

**Faults.** A first touch of 70.9 GiB of address space is 18.6 million page
faults in 4 KiB pages, 36,300 in 2 MiB pages - but a 2 MiB fault zeroes 2 MiB,
so the time moves rather than goes.

**What they cost in memory.** A huge page is committed whole: a byte written
in an untouched 2 MiB costs 2 MiB. Where a job writes a heap densely this is
nothing; where it writes sparsely it is the difference between the touched
pages and the touched 2 MiB regions. The internal region is the sparse case the
tree owns: 26.2 MiB a PE, of which a job touches the team numbers it uses and
the lanes' slots its collectives fill - at 2 MiB granularity it rounds to 28 MiB
a PE at worst, 7 GiB a node at 256 PEs where the same job might have touched a
tenth of that in small pages. The connection area is touched sparsely too, as
§3.4 says - a halo exchange faults six peers' buffers - but a peer's two
buffers are 512 KiB and round to 2 MiB at worst, 4× on the peers touched and
bounded by the area's 250 MiB at 500 nodes. The capacity check is unaffected:
it counts every object whole already.

**The Pi.** A Raspberry Pi 5 runs a 16 KiB page kernel, where a PMD huge page is
32 MiB (`hpage_pmd_size`): a 282 MiB heap holds eight of them, the connection
area at four nodes holds none, and a byte written costs 32 MiB. Nothing in this
proposal should assume 2 MiB; it is read from
`/sys/kernel/mm/transparent_hugepage/hpage_pmd_size`.

## What the kernel does, probed

A probe of a hundred lines (`shmem-huge-probe.c`) creates a 256 MiB object, maps it twice
- once where the kernel puts it, once at a 2 MiB boundary inside a reservation
- advises `MADV_HUGEPAGE` or not, touches every page, and reports
`ShmemHugePages` from `/proc/meminfo`, the mappings' `ShmemPmdMapped` from
`/proc/self/smaps`, and the process's `VmPTE`. Run in the development VM
(Debian 13, Linux 6.12, aarch64, 4 KiB pages) under each setting, each
restored after (`run-probe.sh`, `probe-output.txt`):

| Setting | `MADV_HUGEPAGE` | `/dev/shm` object | `memfd` object | the kernel's placement of `mmap(NULL, 256 MiB)` |
|---|---|---|---|---|
| as shipped: `shmem_enabled=never`, `/dev/shm` without `huge=` | yes | 0 huge; `VmPTE` 44 → 1,068 KiB | - | 64 KiB aligned, not 2 MiB |
| `shmem_enabled=advise` (the sysfs knob) | yes | **0 huge**; `VmPTE` → 1,072 KiB | **all 256 MiB huge**; `VmPTE` 40 → 44 KiB | the `memfd`'s on 2 MiB |
| `shmem_enabled=advise` | no | - | 0 huge | - |
| `/dev/shm` remounted `huge=advise`, sysfs `never` | yes | **all 256 MiB huge**; `VmPTE` 40 → 44 KiB | - | on 2 MiB |
| `/dev/shm` remounted `huge=advise` | no | 0 huge | - | on 2 MiB |
| `/dev/shm` remounted `huge=within_size` | no | all 256 MiB huge; `VmPTE` 44 → 44 KiB | - | on 2 MiB |
| `/dev/shm` remounted `huge=always` | no | all 256 MiB huge; `VmPTE` 44 → 48 KiB | - | on 2 MiB |

So:

- **The sysfs knob is not the setting.** `shmem_enabled` is the policy of the
  kernel's internal shmem mount (`mm/shmem.c`, `shmem_init` copies it into
  that mount's superblock; the documentation's "Hugepages in tmpfs/shmem" says
  the same). A `/dev/shm` object follows its own mount's `huge=`, which Debian
  13 and RHEL 9 leave unset - `never` - with `enabled` at `always` for anonymous
  memory and `shmem_enabled` at the kernel's `never`. (Debian 13 read in the VM;
  RHEL 9 is the kernel's defaults as far as its documentation says, and was
  not read on a machine here: the Rocky VM was removed on 2026-11-01.) The
  task's premise, that `shmem_enabled=advise` or looser is what `madvise`
  needs, holds for `memfd` objects and not for the objects osoe has.
- **Alignment takes care of itself once the mount allows huge pages.** With
  `huge=` at anything but `never`, `shmem_get_unmapped_area` places a mapping of
  2 MiB or more on a 2 MiB boundary, so `map_siblings`' `mmap(NULL, ...)` needs
  no alignment code; the own heap's bases are multiples of 4 GiB already, and
  under `OSOE_HEAP_BASE` the README's "a multiple of the page" would become "of
  the huge page" - and `OSOE_HEAP_SHIFT`, §8.2's test mode, shifts by rank ×
  shift, which a test at a shift that is not a multiple of 2 MiB would map in
  small pages, harmlessly. The internal region begins at
  `page_round(SHMEM_SYMMETRIC_SIZE)`, a 2 MiB boundary at the default and any
  size that is one; at the specification's own `3.1M` it begins inside a huge
  page shared with the heap's tail, which is the same object and harmless. The
  object's tail - 282.2 MiB is not a multiple of 2 MiB - takes small pages
  under `within_size` and a huge page past the end under `always`. The control
  segment's layout is cache-line and page arithmetic only (`align_up`,
  `page_round`); nothing in it or in the connection area's `layout_of` depends
  on the page's size, so neither breaks. The statics object is mapped
  `MAP_FIXED` over the program's `.data` and `.bss`, which the linker aligns to
  the page and no more: it never gets huge pages and should not ask.
- **Without a remount nothing osoe can do to a `/dev/shm` object gives it huge
  pages.** `MAP_HUGETLB` is refused, `MADV_HUGEPAGE` is accepted and does
  nothing, and `MADV_COLLAPSE` (Linux 6.1) on a shmem mapping follows the same
  mount policy.

## The options

### 1. Transparent huge pages for the objects osoe has, by `madvise`

**The code.** A setting, `OSOE_HUGE_PAGES=0|1` (not `OSOE_AM_HUGEPAGES`: it
is the heaps' setting now, below the `OSOE_` prefix as a library setting, not
the AM layer's), read in `bootstrap.c` beside `OSOE_SHM_CAPACITY_CHECK` and
checked for agreement in `check_agreement`, since a node's PEs must map alike
(about 15 lines); `madvise(p, bytes, MADV_HUGEPAGE)` after the three mappings
that can use it - the own heap in `create_own_heap`, each node-mate's heap in
`map_siblings`, the connection area in `osoe_connection_area_open` - three
lines each, as the measurement patch on this branch has them; the control
segment left alone, a megabyte and sparse. One notice a node when the setting
is on and the mount cannot honour it: the honest check is after the fact,
`ShmemPmdMapped` of the own heap from `/proc/self/smaps_rollup` is zero after
its first page is touched - about 30 lines in `node.c`, or 40 to parse
`/proc/self/mountinfo` for `/dev/shm`'s `huge=` before mapping and say which
value the mount has. The info line in `init.c`, the entry in
`unknown-settings.c`, the row in the README's table and its `SHMEM_INFO` line
(`new-setting-needs-info-line`: the `shmem.settings-documented` and
`settings-known` checks hold both), a paragraph in the README's "Memory", the
design's §3.4 and §6.2 rewritten, and the ceilings' `/dev/shm` row. A test
that the setting is accepted and refused by name (`shmem/tests/CMakeLists.txt`,
the `am-hugepages` row's shape) runs anywhere; a test that the pages are huge
needs the mount, so it reads `/proc/mounts` and skips with a line where
`/dev/shm` has no `huge=` - which is every preset's VM as shipped, so the
suite would never run it unless `dev/lima/osoe.yaml` remounts `/dev/shm`
`huge=advise` for the VM, 5 lines there. Removing `OSOE_AM_HUGEPAGES` with it
is about 25 lines out of six files.

About 90 lines of C, 30 of test, 60 of documentation; a day with the VM's
remount and the presets.

**What it buys.** Exactly what `huge=within_size` on the mount buys with no
code, on the one kind of site that mounted `/dev/shm` with `huge=advise`
instead: a site that wants huge pages only for programs that ask, because
every other tmpfs user on the node - the shell's `/dev/shm`, MPI's own
segments, a container's - would round up to 2 MiB under `within_size` too. On
a site with `/dev/shm` at `never`, the default of both distributions, the
setting does nothing and says so; on a site at `within_size` or `always` it is
redundant. The site's remount is a line in `fstab` or a `systemd` drop-in for
`dev-shm.mount` - `Options=...,huge=within_size` - which the playbook's A0 can
ask for beside `vm.max_map_count` and the neighbour table.

### 2. hugetlbfs objects

**Two shapes.** (a) A file on a hugetlbfs mount, named as the `/dev/shm`
objects are: `/dev/hugepages/osoe.<job>...heap.<n>`. The mount is root's,
mode 0755 on Debian 13 as mounted (`/dev/hugepages`), so a site mounts one
users may write to, or sets `uid=`, `gid=`, `mode=`; the mount must have the
page size wanted. (b) `memfd_create(name, MFD_HUGETLB | MFD_HUGE_2MB)`, which
needs no mount - the kernel's internal hugetlbfs - and has no name: a
node-mate opens it as `/proc/<creator's pid>/fd/<n>`, the route `osoe-stat`
already uses for a live job's unlinked segment (`segment.c`), with the pid and
descriptor number published in the creator's slot of the control segment. Both
need the pool: `vm.nr_hugepages`, root's, set at boot or when memory is still
unfragmented, 2 MiB pages by default on both architectures (1 GiB pages exist
on x86-64 and arm64 and need a heap of a gibibyte or more to be worth one).

**The code, (a).** `osoe_shm_create` and `osoe_shm_open_ours` take a directory
and use `open` where they use `shm_open` (glibc's `shm_open` is `open` under
`/dev/shm` with the name checked), with the reclaim-a-leftover path unchanged
(about 30 lines); a search of `/proc/mounts` for a writable hugetlbfs mount of
the page size, with a refusal by name (40 lines); `ftruncate` to `heap_bytes`
rounded up to the huge page, 2 MiB a PE more (5 lines); the mapping as it is
- a hugetlbfs file needs no `MAP_HUGETLB`, and `MAP_FIXED_NOREPLACE` at a base
that is a multiple of 4 GiB satisfies the huge page's alignment. The capacity
check splits in two: `/dev/shm` for the control segment, the statics and the
connection area, and the pool's free pages
(`/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages`) for P heap
objects, with a message naming `vm.nr_hugepages` (40 lines with the message).
The fallback §4.1 sketched - no pool, or a pool too small, so the heaps go to
`/dev/shm` as today - is a decision the lead takes and publishes in the control
segment's header for the node's PEs to follow, since every PE must map every
heap by the same rule: a field, a layout bump of `OSOE_NODE_LAYOUT`, and the
0.1.0-beside-0.2 rule about where `ready` and `failed` sit (`node.c`'s static
assertions) to respect (30 lines). `osoe-stat --gc` lists a second directory
for a dead job's heaps and `names_own_child` learns the second prefix (40
lines); `osoe-launch`'s leftover check after a job lists it too (20 lines); the
README's site epilog, `rm -f /dev/shm/osoe.<job>.*`, gains a second path. What
fails differently: a pool with fewer free pages than a heap maps succeeds at
`mmap` only if the whole heap's pages are reserved then - hugetlbfs reserves at
`mmap` and fails with `ENOMEM` - so the refusal is at start and by name, which
is better than `/dev/shm`'s `SIGBUS` at the first write past the space; with
`MAP_NORESERVE` it would be the `SIGBUS` instead, and the check above is the
guard either way.

**The code, (b).** The `memfd` route is the same minus the mount search, the
second directory in `--gc` and the launcher, and the leftover reclaim - a
`memfd` dies with its last process, so a dead job leaves no heap to collect,
and `--gc` keeps only the control segment, the statics and the area to remove
- plus the slot's two fields and the `/proc/<pid>/fd/<n>` open in
`map_siblings`, which fails when the creator has died, a case `fail_rendezvous`
already handles, and which needs the node's PEs in one PID namespace, a rule
D26 already makes, and `/proc` not mounted `hidepid`, which `osoe-stat`
already documents. About 200 lines of C either way, 150 of tests - the
capacity refusal, the fallback, the object kind - of which the ones needing a
pool run only where root set `vm.nr_hugepages`, so by hand in the VM and not
in the suite, and 80 of documentation: `docs/design/README.md`, §4.1, §6.2,
§3.14, the README's Memory and "Names a failed start leaves", the ceilings. Two
to three days, the model of the fallback's agreement included.

**What it buys, beyond option 1.** Pages that are huge for certain - no
`khugepaged`, no fallback to small pages when memory is fragmented, which THP
does silently and a long-running node reaches; and **shared page tables**:
hugetlbfs shares the last-level table between processes mapping the same file
at 1 GiB-aligned ranges (`huge_pmd_share`, x86-64 and arm64), which the heaps'
4 GiB bases give it, so the node's page tables for the heaps come to about
nothing where option 1's 2 MiB entries are 71 MiB a node at 256 PEs. Also 1 GiB
pages, for a heap of gibibytes.

**What it costs that option 1 does not.** Every heap is committed whole when
it is mapped: at 256 PEs and the default object, rounded to 2 MiB, **71 GiB of
the node's memory in the pool before a byte is written**, and the pool is taken
from the node's memory at boot for every job on the node, osoe's or not, and
counted by the hugetlb cgroup controller, not the memory one Slurm's
`--mem` uses. That is what §3.4 means by trading away the demand paging that
makes a large node count affordable, and it holds for the heaps more than for
the area. A site would reserve it only on nodes dedicated to osoe jobs of a
known heap, and `SHMEM_SYMMETRIC_SIZE` would stop being the user's to raise at
will. The fallback softens it to "huge where the pool allows", which a user
cannot see without `osoe-stat`.

### 3. `memfd` objects without hugetlbfs, under the sysfs knob

A shape the probe suggested and the task did not name: the heaps as
`memfd_create` objects of ordinary shmem, with `MADV_HUGEPAGE` under option 1's
setting. The sysfs `shmem_enabled=advise` then applies - the probe's second row
- and that knob is a line in `/etc/sysctl.d`'s neighbour, `tmpfiles.d`, as
easy for a site as `vm.max_map_count`, and it touches no other tmpfs user on
the node. The code is option 2(b)'s object change without the pool: about 120
lines of C, 80 of tests runnable in every preset once the VM's image sets the
knob, and no new failure modes - and it loses the named leftover and its
reclaim, which a `memfd` makes impossible, so `--gc` and the launcher's check
shrink. It changes a robustness property the project has spent review rounds
on (§3.14, the leftovers' story) for the better, and `osoe-stat`'s discovery
not at all, since the control segment stays named. It is the one option under
which huge pages are osoe's to ask for rather than the mount's to give; it is
also a change of object kind to the heaps with nothing wrong with the present
kind, and the measurement below says what it is worth.

## The measurement in the VM

On the release build of this branch in the development VM - Debian 13, Linux
6.12, aarch64, four CPUs, one NUMA node, 4 KiB pages, idle - four
configurations in turn, the mount restored after (`run-measurement.sh`,
`measurement-output.txt`): **A**, `/dev/shm` as shipped; **B**, remounted
`huge=advise` with the branch's three `madvise` lines on
(`OSOE_HUGE_PAGES_PROBE=1`), option 1's path; **C**, `huge=advise` without
them, the control; **D**, `huge=within_size` and no code, the recommendation's
path. Under each: `random-puts.c`, two PEs each putting 16 Mi 8-byte values to
random places of the other's 192 MiB symmetric array and then in sequence,
three runs, both PEs on one node (`osoe-launch -n 2`) so that a put is a store
into a node-mate's heap; the same across two logical nodes of one PE at 2 Mi
puts, two runs, over TCP; and `osoe-bench quick` at four PEs on two logical
nodes. Each PE prints its own `VmPTE` and the node's `ShmemHugePages` before
`shmem_finalize`, with the heaps still mapped.

| | A, as shipped | B, `huge=advise` + `madvise` | C, `huge=advise`, no `madvise` | D, `huge=within_size`, no code |
|---|---|---|---|---|
| `ShmemHugePages` with two PEs' heaps mapped | 0 | 388 MiB | 0 | 390 MiB |
| a PE's `VmPTE` | 456 to 464 KiB | 64 to 72 KiB | 452 to 464 KiB | 60 to 64 KiB |
| random puts into a node-mate's heap, M updates/s, the median of the three runs' two PEs | 11.6 (the first run 7.2 to 7.4: the hypervisor backing the VM's memory) | 12.5 | 10.7 | 13.2 |
| the same in sequence | 62 | 58 (49 to 62) | 60 | 52 (50 to 53) |
| random puts over TCP to the other logical node, M/s | 2.8 to 7.3 | 5.6 to 9.1 | 4.4 to 6.4 | 6.6 to 7.9 |
| `osoe-bench quick`: local put bandwidth at 1 MiB, MiB/s | 52,900 | 50,700 | 50,600 | 54,600 |
| remote put bandwidth at 4 MiB, MiB/s | 8,270 | 10,490 | 8,440 | 9,280 |
| remote put and quiet at 8 bytes, median µs | 34.9 | 38.7 | 35.2 | 31.0 |

What it says, and what it cannot:

- **The pages were huge, and the page tables went as the arithmetic says.**
  `ShmemHugePages` at 388 to 390 MiB is the two arrays, the heaps' other
  touched pages and the internal regions, huge whole; a PE's `VmPTE` at 60 to
  72 KiB against 456 to 464 is the two 192 MiB arrays' entries, 2 × 384 KiB,
  gone to 2 × 1.5 KiB, the rest being the program's own. Option 1's `madvise`
  under `huge=advise` and the mount's `within_size` alone give the same pages.
- **Random access gained 8 to 15%** on the steady runs (B 12.5 and D 13.2
  against A's 11.6 and C's 10.7 M updates a second), repeatable across the
  three runs of each, and in the direction the TLB predicts - smaller than a
  bare-metal core would show, because this is a VM: the guest's 2 MiB page is
  mapped by the hypervisor in its own pages, so a guest TLB miss still walks
  the second stage, and the gain measured here is a floor for the real one,
  not a ceiling.
- **Sequential access did not gain, and under D ran 15% slower** (52 against
  62) in all three runs, where B's spread (49 to 62) covers both. Sequential
  stores miss the TLB once a page, which the hardware walker hides behind the
  stores; why `within_size` costs here and `advise` does not is not explained
  by this run, and a bare-metal measurement is the one to believe for it.
- **Over TCP and in `osoe-bench quick` the differences are inside the noise** of
  four PEs, two agents and the launcher on four CPUs: the random-over-TCP line
  varies two-fold between two runs of one configuration; D's remote bandwidth
  at 4 KiB (413 MiB/s against A's 7,100) is one run's outlier at the start of
  the bandwidth test, and its remote line at 4 MiB is the second best of the
  four. Nothing here says the connection area gains or loses by huge pages; its
  2 MiB rounding of a sparsely touched area is the cost the arithmetic names,
  and the gain, if any, is in a lead's agent landing puts at scattered
  addresses, which this VM cannot separate from its noise.

So the measurement's one hard number is the page tables, seven times smaller at
two PEs and, by the arithmetic, five hundred times at the diagonal of 256; its
one soft number is 8 to 15% on the GUPS pattern between node-mates, a floor;
and it says the mount option and the `madvise` setting give the same pages,
which is the recommendation's premise.

## The NUMA question beside it

Linux places a shmem page on the NUMA node of the CPU that first writes it.
For a PE's own heap that is usually the PE - but a remote put lands first
through the lead's agent, and an intranode put through the writer, so a heap
written by others before its owner is placed by them; for the connection area
every buffer is first written by whichever local PE sends to that peer first.
A shared policy on a shmem mapping - `mbind(MPOL_BIND or MPOL_PREFERRED, node)`
on the owner's mapping of its heap - is stored on the file's range
(`shmem_set_policy`), so it governs whichever process faults the page: each PE
binding its own heap to the node of the CPU it runs on, read from
`sched_getcpu` and `/sys/devices/system/cpu/cpu<n>/node<m>` with no libnuma, is
about 40 lines and a setting, `OSOE_NUMA=local|0`, off by default and doing
nothing on a node of one NUMA node - every machine osoe has run on: the Pi, the
VM, the `c4-standard-16`. It belongs in the same batch as any mapping-flag
change because it is the same three call sites. But it cannot be measured here
or tested in the suite, and a setting built blind is the project's one taboo;
the recommendation is to write it in the playbook's C14 as the change to make
on the first two-socket node, with this paragraph as its design, and not
before.

## Recommendation

Among the four put to the author:

- **Remove the setting** - (a) of the 2026-11-01 record's point 9 - *recommended*,
  with two additions that are text: the README's "Memory" and the ceilings'
  `/dev/shm` row say that a `/dev/shm` mounted `huge=within_size` gives every
  object transparent huge pages, what it costs in rounding (the internal
  region's 28 MiB a PE at worst), and the measured gain; and the playbook's A0
  records `/dev/shm`'s mount options among the site's facts, and its C14 asks
  the site for `huge=within_size` on the ladder's nodes before the rung at 64
  PEs a node, with the random-access line as the before-and-after. Zero code
  in the library, the site's one line, and the decision of 2026-10-11 kept.
- **Keep it documented as a no-op**: nothing for it; a setting whose row says
  "has no effect" costs every reader of the table a pause.
- **Replace it with a THP `madvise` setting** (option 1): not recommended. It
  works only under `huge=advise`, which a site that can set the mount would set
  to `within_size` instead unless it has other tmpfs users to protect; on every
  other site it is a no-op with a notice, the thing being removed with a better
  message.
- **Build hugetlbfs objects** (option 2): not recommended at the default heap;
  71 GiB reserved a node at 256 PEs is the cost the design refused in §3.4, and
  the fallback hides the result from the user.

And one more, not asked: **option 3, `memfd` heaps with `MADV_HUGEPAGE` under
`shmem_enabled=advise`, is the change to design if a cluster shows the
need** - a random-access workload at 64 PEs a node or more that the mount
option is refused for, or page tables measured in the gigabytes on a lead. It
is a day or two, improves the leftovers' story, and the proposal above is
most of its design.

## Questions for the author

1. Remove `OSOE_AM_HUGEPAGES` now, as recommended, with the README's and the
   playbook's text about the mount option - or hold it until the cluster's
   first rung has run?
2. Should the playbook ask the site for `huge=within_size` on `/dev/shm` as a
   matter of course (A0's facts, C14's change), or only after a random-access
   rung shows the gap the VM measured?
3. Is option 3, `memfd` heaps, worth a proposal of its own now - the leftovers'
   story alone argues for it - or does it wait for a cluster's number?
4. The NUMA setting: written into C14 as designed here and built on the first
   two-socket node, as recommended, or built now blind?
5. The measurement patch is on this branch under `OSOE_HUGE_PAGES_PROBE`, three
   `madvise` lines. Revert it before any merge (recommended: the branch's record
   is the commit), or keep it as a test hook for the suite's huge-page test,
   should option 1 or 3 be built?
