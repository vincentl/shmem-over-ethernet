# Q2 survey: MPI-3 RMA as a substrate for OpenSHMEM (OSHMPI, MPICH ch4, Open MPI osc)

Scope: whether MPI-3/4 one-sided communication (RMA) works as the "core" under OpenSHMEM 1.4–1.6 at 10^5+ PEs. The main evidence is OSHMPI, the only maintained OpenSHMEM-over-MPI library, plus the MPICH and Open MPI RMA code it runs on.

## Sources and labels

Labels on claims:
- **[src]** = observed in source.
- **[doc]** = documented in a paper, man page or README.
- **[recalled]** = from memory and not checked in this session.

Pinned sources read for this report:
- **OSHMPI** at commit `ee5cf110e673c098707257bb025404e17ac0a5fc` (2024-08-31, latest tag v2.1a1). Base URL: `https://github.com/pmodels/oshmpi/blob/ee5cf110e673c098707257bb025404e17ac0a5fc/` (below: `OSHMPI:`).
- **MPICH** tag `v5.0.1` (commit `f47d175fffa7ac9aecd912079be6bdb723c8e706`). Base URL: `https://github.com/pmodels/mpich/blob/v5.0.1/` (below: `MPICH:`).
- **Open MPI** `main` at `8caa67e74b7bb3144b4a995156bd6102baa44dcb` (2026-09-15). Base URL: `https://github.com/open-mpi/ompi/blob/8caa67e74b7bb3144b4a995156bd6102baa44dcb/` (below: `OMPI:`).
- **[Si21]** M. Si, H. Fu, J. R. Hammond, P. Balaji, "OpenSHMEM over MPI as a Performance Contender: Thorough Analysis and Optimizations", OpenSHMEM 2021 workshop (LNCS 13159, 2022). PDF: https://minsii.github.io/assets/pdf/openshmem2021-mpi.pdf. Springer: https://link.springer.com/chapter/10.1007/978-3-031-04888-3_3
- **[Ham14]** J. R. Hammond, S. Ghosh, B. Chapman, "Implementing OpenSHMEM using MPI-3 one-sided communication", OpenSHMEM 2014 (LNCS 8356). PDF: https://www.csm.ornl.gov/workshops/openshmem2013/documents/ImplementingOpenSHMEM%20UsingMPI-3.pdf
- **[BoF19]** M. Si, P. Balaji, "OpenSHMEM over MPI: Capabilities and Challenges", SC19 OpenSHMEM BoF slides: https://www.mcs.anl.gov/~minsi/pdf/oshmpi-shmem-bof-201911.pdf

I could not verify the MPI 4.1 / 5.0 RMA change lists from the standard text. One WebFetch of an MPI-5.0 report page returned an unrelated section. Items in §6/§10 about standard versions are therefore marked [recalled].

---

## 1. Primitives relevant to one-sided PGAS

**What MPI RMA offers** (MPI-3.0 and later):
- **Put and Get.** `MPI_Put`/`MPI_Get` are non-blocking-in-effect: completion is implied only by a flush or unlock. `MPI_Rput`/`MPI_Rget` return a request.
- **Accumulates.** `MPI_Accumulate`, `MPI_Get_accumulate`, `MPI_Fetch_and_op` and `MPI_Compare_and_swap`. These are atomic per element only if every concurrent accumulate on a target uses the same op (or the same op plus `MPI_NO_OP`). The info key `accumulate_ops=same_op|same_op_no_op` states this [doc, Ham14 §2.2; src, OSHMPI `src/internal/setup_impl.c` L40–116 and the configure help text for `--enable-amo`].
- **No triggered ops or counters.** There is no RMA-triggered notification (nothing like put_signal) and no counters.
- **Tag matching and collectives.** Two-sided tag matching is a separate API, with its own progress rules. There are no user-level active messages; OSHMPI builds AMs from Isend/Irecv (§8). Collectives are the full MPI set, with nonblocking variants.
- **Displacements, not addresses.** Operations use `(win, rank, disp, count, datatype)`. For a dynamic window, `disp` is the absolute address (relative to `MPI_BOTTOM`).
- **Datatypes add cost.** Every op carries a datatype handle, which must be decoded on the fast path unless IPO inlining folds it [doc, Si21 §5.1].

**How OSHMPI maps OpenSHMEM onto it** [src]:

| OpenSHMEM | OSHMPI mapping | File |
|---|---|---|
| `shmem_put` / `shmem_get` (blocking) | `MPI_Put`/`MPI_Get` + `MPI_Win_flush_local(pe)` | `src/internal/rma_impl.h` L13–32, L107–200; flush in `src/include/oshmpi_impl.h` L789–793 |
| `shmem_put_nbi` / `get_nbi` | plain `MPI_Put`/`MPI_Get`, no flush. **`MPI_Rput`/`MPI_Rget` are not used** (grep finds no uses) | `rma_impl.h` `OSHMPI_ctx_put_nbi` L107 |
| `iput` / `iget` (strided) | derived datatype (`MPI_Type_create_*`, cached) + `MPI_Put` | `rma_impl.h` `ctx_iput_nbi_impl`; `src/internal/strided_impl.h` |
| `fence` / `quiet` | `MPI_Win_flush_all` + `MPI_Win_sync` on every window with an outstanding op, then an AM flush round to every PE with AM traffic | `src/internal/order_impl.h` L23–98 |
| `cswap` | `MPI_Compare_and_swap` + `flush_local` (direct mode only) | `src/internal/amo_impl.h` L53 |
| `swap`, `fadd`, `finc`, `fetch` | `MPI_Fetch_and_op(REPLACE / SUM / NO_OP)` + `flush_local` | `amo_impl.h` L12 |
| `add`, `inc`, `set`, `and`, `or`, `xor` | `MPI_Accumulate(SUM / REPLACE / BAND / BOR / BXOR)` + `flush_local` | `amo_impl.h` L34 |
| `malloc` / `free` / `realloc` | dlmalloc mspace inside the preallocated window, then `OSHMPI_barrier_all()`: flush_all + win_sync + AM flush + `MPI_Barrier` | `src/internal/mem_impl.c`; `src/internal/coll_impl.h` L12–32 |
| collectives | `MPI_Bcast`, `MPI_Allgather(v)`, `MPI_Alltoall`, `MPI_Allreduce`, `MPI_Barrier`; the nonblocking variants plus polling when AM progress is needed | `src/internal/coll_impl.h`, `src/internal/am_progress_impl.h` |
| teams | `MPI_Comm_split` (color = selected / `MPI_UNDEFINED`); `translate_pe` = `MPI_Group_translate_ranks` | `src/internal/team_impl.c` L43–64; `src/shmem/team.c` |

**Collective offload** exists only as whatever the MPI library does internally. MPI gives no hardware-collective handle.

## 2. Memory registration model and key scaling

### MPI window flavors

- `MPI_Win_create(base,size)` registers user memory. Bases differ per rank, so the implementation must keep a per-peer base, key and disp_unit [doc, Ham14 §3].
- `MPI_Win_allocate` lets the library allocate the memory, in principle symmetrically, and use shm within a node.
- `MPI_Win_allocate_shared` is node-local only and allows load/store through `MPI_Win_shared_query`.
- `MPI_Win_create_dynamic` + `MPI_Win_attach`/`detach`: attach is a local call. Remote peers learn of the new region lazily, or never, unless the implementation has a hint.

### What OSHMPI does [src, `src/internal/setup_impl.c` L118–292; configure.ac `--enable-win-type`]

**Default (`win_creates`): two windows, both locked once with `MPI_Win_lock_all(MPI_MODE_NOCHECK)` and never unlocked until finalize.**
- **Heap:** `MPI_Win_allocate(true_size, disp_unit=1, info{alloc_shm=true, accumulate_ops, which_accumulate_ops, disable_shm_accumulate=true, accumulate_ordering=none})` over a dup of `COMM_WORLD`. The heap size comes from `SHMEM_SYMMETRIC_SIZE` (default 128 MiB).
- **Statics:** `MPI_Win_create(__data_start, _end - __data_start)`.
- **Displacement:** `addr - local_base`. Each RMA op issues `MPI_Get_address` twice plus `MPI_Aint_diff` (`oshmpi_impl.h` L636–667).

**Optional `dynamic_win` (recommended with MPICH/OFI ≥ 3.4b1):**
- One `MPI_Win_create_dynamic` window with the MPICH-specific hint `coll_attach=true`. Heap and statics are attached with `MPI_Win_attach` + `MPI_Barrier`.
- Operations use absolute displacements.
- OSHMPI tries to map the heap at the same virtual address on all PEs. Rank 0 picks a random address in `0x6000_0000_0000–0x6FFF_FFFF_FFFF` and broadcasts it; every PE `mmap`s at that address; an `MPI_Allreduce` checks for failure (`src/internal/util/symm_mem.c` L59–157).
- It then allgathers every PE's base (`OSHMPIU_check_symm_mem` L177). If the bases match, the object is flagged "symm" and the local address is used directly. Otherwise it keeps **`base_offsets[N]` (8 B × N) per non-symmetric object**, e.g. 800 KB per object at N=10^5 (`oshmpi_impl.h` L145, L641–656).

**Spaces** (`shmemx_space_*`, the memory-kinds/GPU extension) [src, `src/internal/space_impl.c` L17–24, L284–330]:
- Each attach does `MPI_Win_create` over `COMM_WORLD`, or `MPI_Win_attach` in dynamic mode.
- Each space context adds another `MPI_Win_create` over the same memory.

### How keys scale inside the MPI library [src]

**MPICH ch4:ofi** (`MPICH: src/mpid/ch4/netmod/ofi/ofi_win.c` L116–265; struct in `ofi_pre.h` L350–359):
- Every window creation runs an `MPIR_Allreduce` (registration success) and an `MPIR_Allgather` of `MPIDI_OFI_win_targetinfo_t {int32 disp_unit; uint64 mr_key; uintptr base}`. That is **24 B × comm size per window**: 2.4 MB per window per process at 10^5.
- With `FI_MR_SCALABLE`-style keys (no `MR_PROV_KEY`, no `MR_VIRT_ADDRESS`) and identical disp_units, the table is freed after creation. The key is then `NUM_OPTIMIZED_MEMORY_REGIONS + win_id`, O(1) per peer.
- With provider keys or virtual addressing (verbs;rxm, CXI with `MR_PROV_KEY`), the table is **retained for the window's lifetime**.
- One MR covers the whole window. For dynamic windows MPICH tries to register `[0, UINTPTR_MAX)` (`MPI_BOTTOM`) as one MR where the provider allows it (`MPIDI_OFI_ENABLE_MR_REGISTER_NULL`). CXI does not allow it (comment at L161–169).
- With `coll_attach`, **each `MPI_Win_attach` does an Allreduce plus an Allgather of `{key, base, size}` (24 B × N) and mallocs N AVL-tree nodes, one per remote process** (`MPIDI_OFI_mpi_win_attach_hook` L905–1010). Every later RMA op on a dynamic window does an AVL lookup in the target's tree.
- In short: one registration can cover the symmetric heap, but the per-window per-peer key table is O(N) unless the provider supports requested keys.

**MPICH ch4:ucx** (`MPICH: src/mpid/ch4/netmod/ucx/ucx_win.c` L17–165; `ucx_pre.h` L87–110):
- Window creation allgathers rkey sizes (8 B × N), then `Allgatherv`s the packed rkeys, then calls `ucp_ep_rkey_unpack` once per peer. That produces **N unpacked `ucp_rkey_h` objects per window**, plus a `{rkey, addr, disp}` table (24 B × N) and a `target_sync` array (4 B × N).
- An unpacked UCX rkey is a heap object of the order of 100 B or more [recalled]. At N=10^5 that is on the order of 10 MB or more per window per process [derived estimate, not measured].
- `MPI_Win_create_dynamic` in ch4:ucx goes straight to the generic AM implementation (L235–237): **no native RMA on dynamic windows over UCX**.

**Open MPI osc/rdma** (`OMPI: ompi/mca/osc/rdma/osc_rdma_component.c`, `osc_rdma_peer.c` L151–215):
- Peer info is not allgathered to every rank. Node leaders publish a rank→(node, local rank) array and per-node state regions. A process builds a peer object **on first use**, doing blocking RDMA reads of the leader's array (`ompi_osc_rdma_peer_setup`).
- Peers live in a hash table when `world_size > 256` and in a `calloc(world_size, 8 B)` pointer array otherwise (L1403, L1476–1484). Per-window cost is therefore closer to O(peers actually touched) plus O(nodes).
- Dynamic windows are capped at `osc_rdma_max_attach` regions, **default 64** (L229). Each window's state holds `max_attach × region_size` bytes (region_size includes the BTL registration-handle size) (L1513–1520).
- Remote attach changes are found by comparing `region_id`s and refreshing under a lock (`osc_rdma_dynamic.c` L385–425).

## 3. Per-peer state (10^5–10^6 PEs)

**Items that grow with N, per process** [src unless marked]:
- **Per window, from the MPI library.** MPICH OFI: 24 B × N (retained unless scalable keys). MPICH UCX: about (rkey object + 28 B) × N. MPICH generic (AM path): `MPIDIG_win_target_t` objects `{3 counters, sync struct, rank, UT_hash handle}`, created lazily per target that got an AM op. They are cached until `MPI_Win_free` unless `MPIR_CVAR_CH4_RMA_MEM_EFFICIENT=true` (`MPICH: src/mpid/ch4/include/mpidpre.h` L491–498; `src/mpid/ch4/src/mpidig_win.h` L14–25).
- **Per communicator.** MPI keeps a rank→address map per communicator. MPICH compresses the "direct/offset/stride" cases [doc, Si21 §5.2]. An arbitrary `MPI_Comm_split` result is O(N) [recalled]. OSHMPI teams are `MPI_Comm_split` results, so **every team costs one communicator plus one `MPI_Group`**.
- **Fixed OSHMPI state.** Four communicators are dup'd or split from `COMM_WORLD` (team_world, team_shared, AM comm, AM ack_comm) (`setup_impl.c` L798, L810; `src/internal/am_impl.c` init L57–58). `OSHMPI_am.outstanding_op_flags[N]` is an array of atomic flags, about 4 B × N (`am_impl.c` L61–64).
- **O(N) work on sync paths.** `OSHMPI_am_flush` loops over all N PEs on every `fence`/`quiet`/`malloc`/`free`, even with no AM traffic (it returns early only when direct AMO and direct RMA are fixed at configure time) (`src/internal/am_impl.h` L22–78). In MPICH, `MPIDIG_mpi_win_flush_all` calls `MPIDIG_win_check_all_targets_remote_completed`, which **iterates ranks 0..N-1 doing a hash find per rank** whenever any target object exists (`MPICH: src/mpid/ch4/src/ch4_impl.h` L730–752; `mpidig_win.h` L838–876).
- **Connections.** These are whatever the MPI netmod does: OFI RDM AV entries, UCX endpoints (often created lazily) [recalled]. MPI adds nothing per window on top of the communicator's endpoints, except the UCX rkeys above.

**Estimate for OSHMPI (default build) at N=10^5 on MPICH/OFI with provider keys**, derived from the struct sizes above:
- 2 windows × 2.4 MB, plus 0.4 MB AM flags, plus communicator maps (compressible for dups).
- Total: roughly **5 MB/process just for RMA metadata**, i.e. about 500 GB across the job, before counting endpoints.
- Each extra space or context window adds 2.4 MB/process and a **collective Allgather over 10^5 ranks**.

## 4. Ordering and delivery guarantees

- **Put/Get have no ordering.** MPI orders nothing between Put/Get operations to the same target; ordering comes only from completion (flush). Accumulates are ordered by default (`accumulate_ordering=rar,war,raw,waw`) [doc, Ham14 §2.2].
- **OSHMPI turns accumulate ordering off.** It sets `accumulate_ordering=none` because default ordering can disable network atomics, and OpenSHMEM does not require AMO ordering without a fence (`setup_impl.c` L111–114).
- **MPICH OFI needs ordering off for optimized keys.** "Optimized" MR keys require `accumulate_ordering == 0` (`ofi_win.c` L126–131). When ordering is on, `MPIDI_OFI_check_acc_order_size` also caps atomic message sizes (`ofi_rma.h` L771).
- **OSHMPI's fence/quiet is the conservative "remote completion" interpretation:** `MPI_Win_flush_all` + `MPI_Win_sync` [doc, Ham14 §3; src, `order_impl.h` L23–34]. `shmem_fence` takes no PE, so OSHMPI flushes every target of every window. Ham14 notes that doing better would need O(N) state in the SHMEM layer.
- **AM path order is per (source, comm).** Ordering comes from MPI's non-overtaking rule on a single communicator plus a single posted `MPI_ANY_SOURCE` receive (`am_impl.c` L90–125). Ops in the AM path are therefore ordered per source→target. Across AM and RDMA paths there is no ordering until a flush.
- **Multipath and adaptive routing** are hidden below MPI. MPI promises only that an RMA op completes by flush/unlock and that two-sided messages don't overtake [recalled]. Nothing is exposed that would let a SHMEM layer relax this.

## 5. Completion and progress model

- **Passive target with one permanent lock-all.** OSHMPI's whole run is one passive-target epoch (`lock_all(NOCHECK)`). In MPICH generic, `NOCHECK` skips the N control messages. Without `NOCHECK`, `MPIDIG_mpi_win_lock_all` sends one `MPIDIG_WIN_LOCKALL` AM to every rank and waits for N acks (`mpidig_win.h` L878–918), and unlock_all does the same (L740–765).
- **Completion calls.** Local completion is `MPI_Win_flush_local(rank)`; remote completion is `MPI_Win_flush(rank)` or `flush_all`; load/store consistency is `MPI_Win_sync`.
- **"MPI full progress" on the target is required whenever any op may have fallen back to AM.** In a pure-RDMA case it is pure overhead [doc, Si21 §4, §5.4].
  - Si21 counts **333 instructions for `shmem_putmem` in OSHMPI/MPICH-ch4 vs 71 in SOS**, and **544 vs 91 for `shmem_quiet`**. Full progress and target traversal account for 81+59 and 81+130+267 of those.
  - After MPICH/OSHMPI changes, the counts fall to 93 and 44, and Put latency matches SOS on Omni-Path (about 1 µs → 0.6 µs, intranode 1 byte) [doc, Si21 Tables 1–2, §6].
  - MPICH's fix is heuristic: `MPIR_CVAR_CH4_RMA_ENABLE_DYNAMIC_AM_PROGRESS` (default false) polls AM progress only every `MPIR_CVAR_CH4_RMA_AM_PROGRESS_LOW_FREQ_INTERVAL` (default **100**) flushes until an RMA AM has arrived [src, `mpidig_win.h` L28–74; `ch4_impl.h` L992–1010].
  - OSHMPI turns this on through MPI_T at init (`setup_impl.c` `initialize_mpit` L723–732).
- **MPI progress can be skipped entirely by some calls.** OSHMPI's own comment: "some MPI routines may skip internal progress (e.g., MPICH CH3, fetch_and_op(self) + flush_local(self) in test and wait_until)". It therefore inserts `MPI_Iprobe(ANY_SOURCE)` purely to kick progress (`oshmpi_impl.h` L796–814).
- **Progress thread.** None by default; OSHMPI's AM layer progresses by polling in the caller. An optional async thread (`OSHMPI_ENABLE_ASYNC_THREAD`) forces `SHMEM_THREAD_MULTIPLE` and **busy-spins on `MPI_Test`** with no yield (`am_impl.c` L46–84, "/* THREAD_YIELD */" placeholder).

## 6. Thread and context model

- **OpenSHMEM contexts.** `shmem_ctx_create` and `shmem_team_create_ctx` **always return `SHMEM_NO_CTX`**. Comment: "We cannot support it in OSHMPI unless MPI exposes the communication resource management to users (e.g., endpoint)" [src, `OSHMPI: src/shmem/context.c` L9–37]. This is legal under 1.5 (failure is not an error), but it means no private contexts and no per-thread isolation.
- **The only real contexts** are `shmemx_space` contexts: one `MPI_Win_create` per context, collective over `COMM_WORLD`, created at `shmemx_space_attach` (`space_impl.c` L311–327).
  - A window is the only MPI object that can carry "isolation". A context therefore costs a collective window creation (Allgather of 24 B × N in MPICH OFI) plus a `lock_all`.
  - In MPICH ch4, windows map to VCIs (`MPIDI_WIN(win, am_vci)`). Whether a window gets its own hardware context depends on `MPIR_CVAR_CH4_NUM_VCIS` and `MPIR_CVAR_CH4_RESERVE_VCIS` [recalled; `am_vci` use observed in `ofi_win.c` L157–159].
- **Threads.** OSHMPI protects its own objects with per-object pthread mutexes when `SHMEM_THREAD_MULTIPLE` (`oshmpi_impl.h` L305–345). MPI must also be `MPI_THREAD_MULTIPLE`.
  - MPICH ch4 takes `MPID_THREAD_CS_ENTER(VCI, ...)` around every RMA sync call (`mpidig_win.h` L847, L884).
  - With one VCI, every thread serializes on that lock, and window counters become atomics (`MPL_atomic_uint64_t issued_cntr`, `ofi_pre.h` L367–378).
  - MPICH "per-VCI" / "per-object" threading exists to reduce contention, but it depends on build and runtime settings [recalled].
- **Teams are communicators.** `shmem_team_split_strided` → `MPI_Comm_split`, collective over the parent. `split_2d` → two `MPI_Comm_split`s. `team_translate_pe` → `MPI_Group_translate_ranks`, O(N) in the general case [src `team.c`; Ham14 notes O(N) root translation].
- **MPI 4 Sessions** (`MPI_Session_init`, `MPI_Group_from_session_pset`, `MPI_Comm_create_from_group`) would let a SHMEM layer build world and team communicators without `MPI_Init` [recalled]. OSHMPI does not use them (no `MPI_Session` in source).
- **Endpoints.** The MPI "endpoints" proposal was never standardized [recalled]. That is why OSHMPI cannot give contexts their own network resources.

## 7. GPU memory support

- **OSHMPI.** GPU memory is reachable only through `shmemx_space` with `SHMEMX_MEM_CUDA` or `SHMEMX_MEM_ZE` (`space_impl.c` L88–150). The runtime environment variable `OSHMPI_MPI_GPU_FEATURES` (`pt2pt,put,get,acc`) tells OSHMPI what the MPI library supports.
  - If GPU Put/Get is missing but GPU pt2pt is present, **every GPU RMA op becomes an Isend/Irecv active message** (`oshmpi_impl.h` L753–787; `rma_am_impl.h`).
  - With neither, OSHMPI aborts.
- **MPICH OFI** registers device memory for RMA only if `MPIDI_OFI_ENABLE_HMEM` and the pointer is a "strict" device pointer. Otherwise registration fails collectively and the **whole window falls back to AM** (`ofi_win.c` L175–190, L214–224).
- **MPICH dynamic windows:** "device buffers are not currently supported" in `coll_attach`, so the window falls back to AM (`ofi_win.c` L928–931).
- **MPICH per-op check:** `MPIDI_OFI_gpu_rma_enabled(origin_addr)` gates each op onto the AM path (`ofi_rma.h` L354–359, L618–621).
- **Open MPI osc/rdma** uses an "accelerated BTL" plus registration handles; osc/ucx relies on UCX memory types [recalled beyond the struct fields seen].

## 8. What OSHMPI had to emulate in software (primary evidence)

All items [src] at the OSHMPI commit above unless noted.

1. **Active messages built from MPI two-sided** (`src/internal/am_impl.c`, `am_impl.h`, `am_pre.h`, `am_progress_impl.h`).
   - MPI has no AM or RPC primitive. OSHMPI keeps a fixed-size packet union (`OSHMPI_am_pkt_t`), a single posted `MPI_Irecv(ANY_SOURCE, OSHMPI_AM_PKT_TAG)` on a dup'd communicator, and a callback table.
   - Replies go on a second dup'd "ack" communicator with a unique per-op tag: `OSHMPI_AM_PKT_PTAG_START + atomic_counter++`.
   - **The counter never wraps.** `OSHMPI_ASSERT(tag < MPI_TAG_UB)` (`am_impl.h` L15–20). MPI only guarantees `MPI_TAG_UB ≥ 32767` [recalled standard minimum], so a long AM-heavy run can hit the assertion. It aborts in debug builds; with `--enable-fast` the assertion is compiled out and tags could collide [src for the assert; the overflow risk is my inference].
   - Every blocking MPI call on this path (send, recv, waitall, barrier, bcast, allgather(v), alltoall, allreduce) is replaced by its nonblocking form plus a `MPI_Test` / `OSHMPI_am_cb_progress()` polling loop, so incoming AMs are served while blocked (`am_progress_impl.h` L16–175).
   - Progress handles one packet per poll (`OSHMPI_AM_CB_PROGRESS_POLL_NCNT 1`, `am_impl.c` L88).

2. **Atomics with mixed operations** (`src/internal/amo_am_impl.c`, `amo_am_impl.h`; configure `--enable-amo`; README `OSHMPI_AMO_OPS`).
   - MPI guarantees atomicity only for `same_op` / `same_op_no_op`, but OpenSHMEM programs mix cswap, add and set on one variable. **The default is therefore to implement every AMO as an AM** executed by the target CPU (`OSHMPI_AMO_OP_INT_IMPL` switch). Each round trip is an Isend of the header plus an Irecv of the old value.
   - Direct `MPI_Compare_and_swap` / `MPI_Fetch_and_op` / `MPI_Accumulate` are used only if the user promises, through `OSHMPI_AMO_OPS`, that one op (or one op plus fetch) is used program-wide (`setup_impl.c` L40–116).
   - OSHMPI also passes MPICH-specific `which_accumulate_ops`, and `disable_shm_accumulate=true` "otherwise no remote atomics can be offloaded to network" (L109).
   - Even in direct mode, MPICH ch4:ucx implements every accumulate, fetch_and_op and CAS as generic AMs (`MPICH: src/mpid/ch4/netmod/ucx/ucx_rma.h` L409–533). ch4:ofi goes native only when the provider has `FI_ATOMICS` for that type and op; verbs;rxm and tcp;rxm do not (`ofi_capability_sets.h`: `MPIDI_OFI_ENABLE_ATOMICS_RXM = OFF`).

3. **Distributed locks (`shmem_set_lock` / `test_lock` / `clear_lock`)** (`src/internal/lock_impl.h` L27–170).
   - This is a queue lock inside the user's `long`: `{int last; unsigned next}`.
   - The "last" word lives on **world rank 0** (`OSHMPI_LOCK_ROOT_WRANK 0`, "may change after using distributed lock"), so every acquire and release is at least one RMA atomic to rank 0.
   - **Acquire:** `MPI_Fetch_and_op(REPLACE)` of `last` at rank 0, then `MPI_Win_flush(0)`. If queued, `MPI_Accumulate(BOR)` sets the predecessor's `next`. Then a **busy-poll loop** of `MPI_Fetch_and_op(NO_OP)` on its *own* `next` word + `MPI_Win_flush(self)` + AM progress until the signal bit appears.
   - **Release:** `MPI_Compare_and_swap` at rank 0. If not last, busy-poll its own `next` word for the successor id, then `MPI_Accumulate(BOR)` of the signal bit on the successor.
   - Consequences: a single-rank hotspot, busy waiting, and it uses REPLACE, BOR, NO_OP and CAS on one window, **so it depends on mixed-op atomicity** (item 2).
   - It also assumes `sizeof(long) ≥ 8` (TODO comment).

4. **`shmem_wait_until` / `shmem_test`** (`src/internal/p2p_impl.h` L34–82).
   - **Emulated by polling:** loop { `MPI_Fetch_and_op(NO_OP)` on self + `MPI_Win_flush_local(self)`; compare; `OSHMPI_am_cb_progress()`; `MPI_Iprobe` to kick MPI progress }.
   - There is no wake-up primitive; the thread spins until the value changes.
   - The vector and multi-variable forms (`wait_until_all/any/some[_vector]`, `test_all/any/some`) are **`OSHMPI_ASSERT(0)` stubs** (`src/shmem/p2p_typed.c.tpl` L19–57).

5. **`shmem_put_signal` and `shmem_signal_wait_until` (1.6) are not implemented**: `OSHMPI_ASSERT(0)` (`src/shmem/signal_typed.c.tpl`, `signal_sized.c.tpl`, `src/shmem/p2p.c` L49–53). MPI RMA offers nothing to build a signal on besides the polling in item 4.

6. **Contexts: not emulated, refused.** `SHMEM_NO_CTX` always (§6). Space contexts are extra windows.

7. **Fence/quiet as "flush every window + AM flush round".**
   - `OSHMPI_am_flush` sends an `OSHMPI_AM_PKT_FLUSH` to each PE with an outstanding AM op and waits for acks. It **allocates `2 × N` `MPI_Request`s with `malloc` on every call where any PE is outstanding** (`am_impl.h` L22–78).
   - OSHMPI keeps a per-window `outstanding_op` atomic flag so it can skip `flush_all` / `win_sync` on idle windows (`--enable-op-tracking`, `order_impl.h` L10–33; motivated in Si21 §5.5).

8. **Symmetric addresses** (`src/internal/util/symm_mem.c` L59–200).
   - MPI does not promise symmetric virtual addresses. OSHMPI emulates them with random-address `mmap` coordinated by Bcast + Allreduce.
   - If that fails, it falls back to per-object `base_offsets[N]` and an extra add per op.
   - In the default (non-dynamic) mode it instead converts every address to a displacement relative to the window base on each op.

9. **Address → window lookup on the fast path** (`oshmpi_impl.h` L669–717).
   - Every default-context op range-checks the heap, then statics, then **walks the linked list of spaces under a mutex** (`space_list.cs`) to find the window. Si21 counts 12 instructions for this translation before optimization.
   - Si21 needed a **non-standard `MPI_Put_abs` / `MPI_Get_abs` plus an `rma_abs` window hint** to drop the double address translation, where OSHMPI converts to a displacement and MPICH converts back to a virtual address (Si21 §5.3, Fig. 5).

10. **Collective memory management.**
    - `shmem_malloc`, `free`, `realloc`, `align`: local dlmalloc, then `OSHMPI_barrier_all()` (flush_all + sync on every window + AM flush over N + `MPI_Barrier`) (`mem_impl.c`, `coll_impl.h` L12–32).
    - **The heap cannot grow.** It is one fixed `MPI_Win_allocate` of `SHMEM_SYMMETRIC_SIZE`.
    - Growth would need dynamic windows, with MPICH's per-attach Allgather (§2) and Open MPI's 64-region cap.
    - `shmem_addr_accessible` only checks heap and statics; `shmem_ptr` returns non-NULL only for self (`src/shmem/setup.c` L66–83). Intranode load/store through `MPI_Win_shared_query` is not used, although Ham14's OSHMPI-1.0 used shared-memory windows for all-SMP runs (Ham14 §3, Fig. 1).

11. **Strided RMA and alltoalls** need MPI derived datatypes built per call. OSHMPI added a datatype cache (`--enable-strided-cache`, `src/internal/strided_impl.c`/`.h`).

12. **Active-set (deprecated) collectives** need a sub-communicator per `(start, stride, size)` triple, built with `MPI_Group_incl` + `MPI_Comm_create_group`. OSHMPI caches them in a linked list (`coll_activeset_impl.h`; `coll_impl.c`). Ham14 flagged the cache as needed.

13. **GPU RMA through AMs** when MPI lacks GPU-aware RMA (§7). These AM put/get paths are **blocking even for `_nbi`** ("TODO: nonblocking version of AM PUT", `rma_impl.h` `OSHMPI_ctx_put_nbi`).

14. **The spec level stopped at OpenSHMEM 1.4 plus teams.** `SHMEM_MAJOR/MINOR_VERSION = 1.4` (`include/shmem.h.in.tpl` L67–70). The 1.5 team, context and signal APIs exist as stubs or refusals, as noted above.

## 9. Known performance pathologies and scaling limits

- **Instruction overhead of the MPI path** [doc, Si21]: stock OSHMPI/MPICH-ch4 is 333 vs 71 instructions for putmem and 544 vs 91 for quiet.
  - The causes are MPI semantics: datatype decoding (22 instructions), rank→network address lookup through the window's communicator (17), displacement translation, full progress (81), AM-target traversal (59/130), and duplicated flush for the statics window (267).
  - Closing the gap needed MPICH internals changes (datatype and window attributes embedded in handles, IPO link-time inlining, a progress heuristic) plus a non-standard `MPI_Put_abs`.
  - On an older CH3/OFI stack, putmem+quiet latency was about 25 µs vs about 2 µs for SOS at small sizes [doc, BoF19 slide 3; values read from chart text, approximate].
- **Collective window creation** [src]. MPICH OFI does Allreduce + Allgather(24 B × N) per window and, with `coll_attach`, per attach. MPICH UCX does Allgather + Allgatherv + N rkey unpacks per window. At 10^5 PEs every context, space or team window is a job-wide collective. OSHMPI's design (window-per-context, window-per-space) makes this a scaling wall for 1.5 teams and contexts.
- **Dynamic windows** [src]. Open MPI osc/rdma limits attaches (default 64) and must refresh remote region tables when `region_id` changes. MPICH ch4:ucx has no native RMA on dynamic windows. MPICH ch4:ofi without `coll_attach` has no native RMA on dynamic windows, only AM (the attach hook exits early). With `coll_attach`, attach is collective and O(N) memory per attach.
- **O(N) sync loops** [src]. MPICH `flush_all` scans all N ranks' target hash once any AM target exists. OSHMPI `am_flush` scans N flags and mallocs 2N requests. Both run on every `shmem_quiet`, `shmem_malloc` and `shmem_free`.
- **Whole-window AM fallback** [src]. If any single rank fails to register (for example GPU memory without HMEM, or provider limits), MPICH forces AM for the entire window for all ranks (`ofi_win.c` L214–224, L968–974).
- **Lock-all without NOCHECK** is N control messages at lock and at unlock (§5). OSHMPI avoids this with NOCHECK.
- **THREAD_MULTIPLE contention** [src]. VCI critical sections around every RMA sync call plus OSHMPI per-object mutexes, including a mutex on the space-list lookup in every default-context op when spaces exist. Beyond these code paths I have no quantitative measurement [not verified].
- **Busy-polling** in `wait_until`, locks and the async progress thread (§8.3–8.4, §5). This burns a core per waiting PE.
- **Rank-0 hotspot** for every `shmem_set_lock` / `clear_lock` in the job (§8.3).
- **Tag exhaustion** in AM mode (§8.1).

## 10. Build, portability, and maintenance burden

- **Build and portability.** OSHMPI is small: about 5.5k lines in `src/internal` + `src/include`, plus templated typed front ends. It is autotools-based and needs MPI-3 plus OpenPA for threads [src, `wc -l`; README; CHANGES].
  - Portability rests on MPI, but **OSHMPI relies on MPICH-specific hints and CVARs** (`alloc_shm`, `which_accumulate_ops`, `disable_shm_accumulate`, `coll_attach`, `perf_preference`, `MPIR_CVAR_CH4_RMA_ENABLE_DYNAMIC_AM_PROGRESS`). Without them it still works but is slower or falls back to AM (configure help: "rely on non-standard MPI hint coll_attach").
  - It also relies on per-OS linker symbols to find statics (`__data_start`/`_end` on Linux, `get_etext`/`get_end` on macOS) (`setup_impl.c` L11–35).
- **Maintenance activity.** Last release tag v2.1a1. The HEAD commit is from Aug 2024 (a small fix) [src, git log]. 1.5/1.6 features remain stubs (§8).
- **MPI standard evolution.** MPI-4.0 added Sessions and large-count (`_c`) bindings [recalled]. MPI-4.1 [recalled]:
  - extended `MPI_Win_shared_query` to all window flavors (mpi-forum issue #23, https://github.com/mpi-forum/mpi-issues/issues/23, documented as a proposal; adoption in 4.1 is recalled);
  - added the info assertion `mpi_assert_memory_alloc_kinds` / memory-allocation-kinds info;
  - added `mpi_accumulate_granularity` (4.0).

  MPI-5.0 (2025) centered on a standard ABI [recalled]. **None of these add endpoints/contexts, absolute-address RMA, mixed-op atomicity, RMA-triggered notifications or non-collective window creation** [recalled; not verified against the standard text in this session].
- **GPU-aware RMA** stays implementation- and provider-specific: HMEM in OFI, memory types in UCX (§7).

---

## Implications for a core API

**Made cheap by this substrate, when the MPI implementation is good:**
- **Contiguous Put/Get on a pre-registered symmetric region, using one permanently open passive-target epoch.** On OFI with scalable keys this reaches native latency after about 90 instructions [Si21]. Point for the core: expose **one registration covering the symmetric heap, with an O(1) key**, and **absolute-address** RMA. Do not repeat MPI's displacement + datatype + communicator-rank indirection.
- **Local completion vs remote completion** as separate calls (flush_local vs flush). The core should keep this split with a cheap, counter-based local completion check.
- **Same-op atomics offloaded to the NIC** (`fi_atomic`, `fi_compare_atomic`) where providers have them.
- **Collectives, teams and PE translation.** MPI collectives and communicators serve as a portable, slower layer above the core.

**Made expensive or impossible (the core must provide these directly):**
1. **Communication contexts that don't require a collective window.** MPI has no endpoint abstraction, so OSHMPI refuses `shmem_ctx_create`. The core needs locally created contexts (TX resources) usable against the *existing* registrations, with O(1) per-peer cost.
2. **Mixed-op atomicity on one address** (cswap / add / set / fetch together). MPI forces a target-CPU AM. The core needs either a general atomic RMA (CAS + add + swap on 8-byte words, type-agnostic) or an AM facility with guaranteed target progress. Locks sit on top of this, so it is performance-critical for `shmem_lock`.
3. **An active-message / RPC primitive with bounded handler IDs and reply matching.** Tag-based emulation risks tag exhaustion, needs ANY_SOURCE receives and polling (§8.1).
4. **Progress without polling.** A target-side progress guarantee for AM fallbacks (a progress thread or NIC-driven) that costs nothing on the pure-RDMA fast path. MPI makes every flush pay for "maybe an AM arrived" [Si21 §5.4].
5. **A wake-up or notification primitive** for `wait_until` and `put_signal` (1.6), e.g. a NIC-generated event on writes to a registered "signal" region, or a triggered op. Absent that, the core must document that wait_until is a spin.
6. **Non-collective, O(1)-per-peer registration growth** (heap growth, spaces, memory kinds, GPU). MPI dynamic windows are either unsupported natively (ch4:ucx), collective with O(N) per attach (MPICH `coll_attach`), or capped (Open MPI, 64 regions).
7. **Batched synchronization that is O(peers with outstanding ops), not O(N)** for fence/quiet. MPI `flush_all` implementations and OSHMPI's AM flush scan all N ranks.
8. **Symmetric virtual addresses** as a core guarantee (or a key+offset scheme needing no per-peer base table). Otherwise every object carries 8 B × N offsets.
