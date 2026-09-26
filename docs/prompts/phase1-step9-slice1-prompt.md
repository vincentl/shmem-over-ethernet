# Implement phase 1, step 9, first slice: block-strided RMA

You are adding OpenSHMEM 1.6's block-strided RMA - `shmem_ibput` and
`shmem_ibget` (spec §9.6.1.4, §9.6.1.8) - to **osoe** (OpenSHMEM Over
Ethernet). Step 9 is the rest of 1.6's routines: this slice, then the prefix
scans (§9.10.10), which have a design (§5.8, "Prefix scans") and will get a
model before code.

*Opened on 2026-09-26 under the author's standing direction to press ahead on
whatever the Pi cluster does not block; nothing here is a new design decision:
every form follows one the library already has.*

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks.**

## Read first

1. `CLAUDE.md`: names are words; kebab-case files, snake_case identifiers.
2. The spec, `docs/reference/OpenSHMEM-1.6.pdf`, §9.6.1.4 and §9.6.1.8 whole -
   the arguments, the constraints on `dst`, `sst` and `bsize`, the PE numbering
   relative to the context's team - and §9.6's type tables.
3. `shmem/src/rma-typed.c` whole: how `iput`/`iget` are generated for every
   Table 5 type and every size, through `strided_put` and `strided_get`, and
   what its header comment says each costs. `rma.h`.
4. `shmem/include/shmem.h` and `pshmem.h`: the declarations and the C11
   generic selections for `iput`/`iget`, which dispatch on arity first (design
   §7.3); `shmem/src/export.h` (`OSOE_API`, `OSOE_FN`: the `pshmem_` strong /
   `shmem_` weak pairing); `cmake/CheckExports.cmake`.
5. `shmem/tests/test-typed-rma.c` and its registrations in
   `shmem/tests/CMakeLists.txt`.

## Decisions

1. **Every form the spec lists**: typed, for every Table 5 type, with and
   without a context; sized, for 8, 16, 32, 64 and 128, with and without a
   context; and the C11 generics, with and without a context - each defined as
   the library defines every routine, the context form the definition and the
   plain form calling it with `SHMEM_CTX_DEFAULT` (§7.3).
2. **What a call does**: block b, for b from 0 to `nblocks` - 1, copies `bsize`
   elements from `source + b × sst` to `dest + b × dst`, strides counted in
   elements. A stride below `bsize` is undefined in the spec: the job ends by
   name, as the library does for undefined arguments it can check for the price
   of a compare. `nblocks` or `bsize` of 0 transfers nothing.
3. **One contiguous transfer per block**, so a call costs O(`nblocks`)
   transfers and not O(`nblocks` × `bsize`): every block but the last carries
   `OSOE_TRANSPORT_MORE`, and an `ibget`'s blocks are nonblocking gets
   completed by one wait for the gets, as an `iget`'s elements are. Both
   strides equal to `bsize` is one transfer of the whole.
4. **Say what it costs** in `rma-typed.c`'s header comment, beside `iput`, and
   update the design notes and the README wherever they list what is built.

## Not in this slice

The prefix scans; any change to `iput`/`iget`; a remote-strided transport
operation (there is none to map onto, as `rma-typed.c` says).

## Tests

A tier-2 test at L1, L2 and L4, on `0,0,1,1` and `0,1,2,0,1,0`, and under
`OSOE_FORCE_REMOTE=1`: every Table 5 type through the typed forms and every size
through the sized ones, with and without a context; strides equal to `bsize`
and larger, on either side, source and destination differing; blocks larger
than an AM chunk (64 KiB a block, say); `nblocks` and `bsize` of 0; an `ibget`
into memory that is not symmetric; every element of the destination checked,
the gaps between blocks included, so a block written one stride off or a gap
written at all shows. The C11 generic forms from a translation unit compiled as
C11, as the existing generic tests are. A stride below `bsize` ending the job
by name, as the other refusals are tested. `shmem.exports` still passes.

## Verification

Debug and asan in the VM, foreground, under the lock; the main session runs
release, tsan, tier 3, mutations and the load loop.

## Stop and ask

If a form the spec lists cannot follow an existing pattern without changing
`rma.h`'s contract; or if the spec's text disagrees with a decision here.
