# Implement phase 1, step 6, first slice: symmetric statics

You are starting **§7.1 step 6** of **osoe** (OpenSHMEM Over Ethernet): the
symmetric statics and the compiler wrapper that step 5 was ahead of. Global and
static variables are symmetric objects in OpenSHMEM - the spec's own examples
declare `static long pSync[...]` and `static int count` - and osoe refuses them
today, by name ("is not within the symmetric heap (symmetric statics are not yet
supported)").

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

*Written by Claude on 2026-09-24. The corpus survey of that day found symmetric
statics the one gap that blocks most of the corpus: ISx and all of hclib's copies
of it, NPB IS, bale's default mode (one `static char roothost[256]` in convey),
about thirty openshmem-examples programs, and most of hclib's apps. The decisions
below are Claude's, for the author's review.*

## Read first

1. `CLAUDE.md`.
2. Design §4.3 (the staging: phase 1.5, the agent applies statics operations;
   phase 2, a wrapper-generated linker script; both rejected alternatives), §4.1
   (heaps: one object per PE, node-mates map each other's), §4.2 (addressing),
   §4.10 (the boundary addresses by `(pe, region, offset)`; `OSOE_REGION_STATICS`
   is reserved in `transport.h`), §6.2 (the `/dev/shm` capacity check, names
   removed early).
3. `tools/wrapper/` (oshcc), `symmetric.c` (address to offset), `node.c` (the
   heaps' objects and mappings), `transport-local.c` and `progress-host.c` (region
   resolution on the two paths), `watch.c`.

## Decisions

1. **Phase 2 directly; phase 1.5 is skipped.** The agent-applied path of phase
   1.5 would be slow by design and would be thrown away; the linker script is
   what the wrapper exists to do, and the wrapper now exists.
2. **Statics are a region, addressed by offset - and the wrapper still agrees
   their addresses.** The boundary addresses everything as `(pe, region,
   offset)` (§4.10), and each PE resolves an offset against its own mapping of
   the target, so no SHMEM operation needs a static at the same address on
   every PE. The wrapper passes §4.3's `-fno-pie` and `-no-pie` all the same:
   a program that sends a static's address to another PE as data, and has it
   used there, needs the address to agree, and under ASLR it would name another
   variable, or none. With both, every PE's statics sit where every other's do;
   a program built without them - a build system that insists on PIE - still
   works, but for that one pattern.
3. **What is shared: all of the executable's writable data, in two page-aligned
   output sections.** The wrapper's linker script gathers the executable's
   `.data` and `.bss` input sections - thread-local sections excluded - into
   `.osoe_statics_data` and `.osoe_statics_bss`, each aligned to the linker's
   maximum page size and bracketed by symbols. "All", not "the user's objects":
   a crt object's or a static archive's data in the region is harmless, since
   each PE's region is its own object that node-mates only read and write
   through SHMEM operations naming it, and gathering by exclusion of nothing
   is what makes the script independent of how the program was compiled - one
   command or many, archives or not. Libraries linked dynamically keep their
   data in their own mappings, outside. (§4.3's rejection of runtime discovery
   stands: the bracket symbols are the linker's, not a guess at the segment.)
4. **At `shmem_init`**, if the bracket symbols are there (they are weak, so a
   program not built with `oshcc` has none, and statics stay refused, now with
   the fix named): each PE creates its statics object beside its heap, copies
   the region's current contents in, and maps it over the region with
   `MAP_FIXED | MAP_SHARED`, before any thread of the library's exists and with
   signals blocked; node-mates map it, as they map the heaps, and the names are
   removed as early. `.bss`'s part is created zero and needs no copy of its
   zeros. *(An application thread started before `shmem_init` is not stopped
   for it: the review of the slice named the window's remaining limits, which
   §4.3 lists.)*
5. **Every symmetric address becomes `(region, offset)`.** `symmetric.c` finds
   the region an address is in; RMA, AMOs, waits and signals pass the region
   through; the collectives' intranode phase reads a member's source through
   its region's base; `shmem_ptr` and `shmem_addr_accessible` answer for
   statics as for the heap.
6. **The capacity check counts statics**, and `osoe-stat` shows the region's
   size per PE.
7. **The wrapper comes first, in this slice.** `oshcc` is a POSIX `sh` script
   the build configures twice - for the build tree and for the installed tree -
   so that build systems find the name they expect (PRK's `SHMEMCC`, bale's
   `CC=oshcc`, ISx's and the NAS makefiles). Statics then add `-fno-pie` to
   every call, and `-no-pie` and the script to every link of a program; a
   shared library (`-shared`) is linked as asked.

## Order of work

Part A - the linker script, proved in the VM before anything depends on it:
gathered sections, bracket symbols, PIE, a static archive, a one-command
compile-and-link, a program with TLS, huge `.bss` without file bloat. *(Done
first, in `build/scratch/statics-lab/`: every case as wanted, and `libosoe.so`'s
weak references see the program's brackets once they have default
visibility.)*
Part B - the region at init, and its resolution on both transport paths.
Part C - `(region, offset)` through the API. Part D - tests (ISx's pattern:
a static reduction target and a global atomic target; `pSync`, `pWrk`; a
static broadcast; waits on a static; `shmem_ptr` on a static), the corpus
rehearsal (ISx, NPB IS, the examples), notes.

## Not in this slice

Statics of shared libraries the program loads; a region that grows (a program
cannot grow its statics); statics under a RoCE backend's registration (the
region is registered like the heap when there is one).
