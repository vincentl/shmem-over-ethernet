# Q2 survey: GASNet-EX, GPI-2 / GASPI, LCI — middleware lessons for a minimal OpenSHMEM core API

Survey date: 2026-09-16. Sources were shallow-cloned and read locally. Versions:

| Library | Repository | Commit / tag used |
|---|---|---|
| GASNet-EX | https://github.com/BerkeleyLab/gasnet (the Bitbucket repo now only holds a README saying the project moved to GitHub) | `19143e8b1b8965727555194110beeb81a57080b5` (main, 2026-09-12; after 2025.8.0) |
| GPI-2 | https://github.com/cc-hpc-itwm/GPI-2 | `3f5dbc68ef8c2eb89f92aa2514c49be6f1865aa3` (tag v1.7.0) |
| LCI | https://github.com/uiuc-hpc/lci | `663de28b9287eaae1313ccec826c0ca2ca9e0c2b` (main, LCI v2 line; latest tag v2.0.0-beta.1) |
| OpenSHMEM reference impl. (U. Houston/ORNL/SBU) | https://github.com/openshmem-org/openshmem | `5f91637b27690c148cbf81523f2bc0b0fb7829b4` (last commit 2017-07-12; OpenSHMEM 1.3-era) |

Permalink form for the files cited below: `https://github.com/<org>/<repo>/blob/<commit>/<path>`. Shorthand used: **[GEX]** = `BerkeleyLab/gasnet@19143e8b`, **[GPI]** = `cc-hpc-itwm/GPI-2@3f5dbc68`, **[LCI]** = `uiuc-hpc/lci@663de28b`, **[OSH]** = `openshmem-org/openshmem@5f91637b`.

Claim labels: *documented* (spec, README, paper), *observed in source* (file:line), *recalled* (from memory, not checked this session). **LCPC18** = Bonachea & Hargrove, "GASNet-EX: A High-Performance, Portable Communication Library for Exascale", LCPC'18, LBNL-2001174, https://gasnet.lbl.gov/pubs/gasnet-ex-lcpc18-tech.pdf (full text fetched and read).

---

## Part A — GASNet-EX

### A.0 Core/extended split (the model the study wants to copy)

- **The core is job management plus Active Messages. Everything else has a reference implementation built only on the core.** *Documented*, [GEX] `extended-ref/README`: "a reference implementation of the extended API, written solely in terms of the GASNet core API … once the core API is implemented, the extended-ref files provide working implementations of everything else." LCPC18 §1.1 says the same: a new conduit ports "a minimal core (consisting of a few job management routines and the AM interfaces)". Reference code in the tree (*observed*):
  - RMA over AM: `extended-ref/gasnet_extended_amref.c`.
  - Collectives: `extended-ref/coll/` (eager, rendezvous, put/get, trees, autotune, scratch).
  - Non-contiguous RMA ("VIS"): `extended-ref/vis/` (vector, indexed, strided).
  - Remote atomics: `extended-ref/ratomic/gasnet_refratomic.c`.
- **Conduits replace pieces piecemeal, as the hardware allows** (*observed*):
  - ibv: `gasnet_extended.c`, `gasnet_ratomic.c`, firehose.
  - ucx: `gasnet_extended.c`, `gasnet_ratomic.c`.
  - ofi and smp: `gasnet_extended.c`.
  - mpi and udp: core only.
- **Current conduits** (*documented*, [GEX] `ChangeLog` head): ibv, ofi, smp, udp, mpi, ucx (ucx is [EXPERIMENTAL]). **aries-conduit (uGNI) was removed in 2025.8.0** after deprecation in 2023.9.0. Cray XC users are sent to ofi+gni. Historical conduits included PAMI, DCMF, LAPI, Portals, PSM2, MXM, Gemini, elan, GM and SISCI (LCPC18 §1.1). The lesson: over 20 years, per-vendor native conduits were steadily replaced by ibv, ofi and ucx.
- **Spec status** (*documented*, [GEX] `docs/GASNet-EX.txt:5-6, 88-89`): "*not* a final normative document … beta documentation". GEX_SPEC_VERSION 0.20. Many flags are marked `[UNIMPLEMENTED]` (see A.2).

### A.1 Primitives relevant to one-sided PGAS

**RMA** (*documented*, `docs/GASNet-EX.txt` ~2540-2720):
- Put and Get come in three flavors each: `gex_RMA_{Put,Get}Blocking`, `…NBI` (implicit-event) and `…NB` (explicit `gex_Event_t`).
- Value-based variants exist: `gex_RMA_GetBlockingVal`, `gex_RMA_PutBlockingVal/NBIVal/NBVal`. They carry scalars up to `sizeof(gex_RMA_Value_t)`. These map directly onto `shmem_put64` / `shmem_get64`-style scalar calls.
- The peer is named by a `(gex_TM_t tm, gex_Rank_t rank)` pair, not a global node id.
- `lc_opt` separates local completion from remote completion. Choices are `GEX_EVENT_NOW`, `GEX_EVENT_DEFER`, `GEX_EVENT_GROUP`, or a pointer to a leaf event.
- `GEX_FLAG_IMMEDIATE` asks the call to return "no-op" instead of stalling on backpressure.
- `GEX_FLAG_PEER_NEVER_SELF` and `GEX_FLAG_PEER_NEVER_NBRHD` let the caller assert the peer is remote, so the library can skip locality checks (spec v0.14+).
- **Remote address rule**: "In all cases, the remote memory must be in the remote bound segment" (NOTE 4). The local address may lie outside the segment unless the EP is bound to a device segment (NOTE 3).

**NBI access regions and implicit events** (*documented*):
- `gex_NBI_BeginAccessRegion` / `gex_NBI_EndAccessRegion` group operations.
- `gex_NBI_Test` / `gex_NBI_Wait(event_mask)` complete by category: `GEX_EC_GET`, `GEX_EC_PUT`, `GEX_EC_AM`, `GEX_EC_LC`, `GEX_EC_RMW`.
- Together these are a natural fit for `shmem_ctx_sync` and `shmem_barrier`-style "wait for all my outstanding puts" semantics.

**Remote atomics ("RAtomics")** (*documented*, `docs/GASNet-EX.txt:3080-3530`, header `gasnet_ratomic.h`):
- A collective `gex_AD_Create(&ad, tm, dt, ops, flags)` builds an "Atomic Domain" for one datatype and a **set of opcodes**.
- Operations are `gex_AD_OpNB_<DT>` / `gex_AD_OpNBI_<DT>(ad, result_p, tgt_rank, tgt_addr, opcode, op1, op2, flags)`.
- Opcodes (spec ~2994-3045): SET, GET, SWAP, CAS, FCAS, ADD/FADD, SUB, INC, DEC, MULT, MIN, MAX, AND, OR, XOR, each with fetching and non-fetching forms.
- Why the opcode set is declared up front: "the best possible implementation of a operation 'X' may not be compatible with operation 'Y' … a NIC may offload 'X' (but not 'Y') and use of a CPU-based implementation of 'Y' would not be coherent with the NIC performing a concurrent 'X'" (spec ~3113-3120).
- **Atomicity holds only among operations through the same AD.** Mixing with plain CPU access or RMA to the same bytes needs "atomic access phases" separated by a barrier. The spec marks this `[INCOMPLETE / OPEN ISSUE]`.
- `GEX_FLAG_AD_ACQ` / `GEX_FLAG_AD_REL` request C11-style memory fencing. The target location must lie in the target's bound segment.
- This matches OpenSHMEM atomics (`shmem_atomic_{set,fetch,swap,compare_swap,inc,fetch_inc,add,fetch_add,and,or,xor,fetch_*}`) almost one to one.
- **Mismatch 1: the atomicity domain.** OpenSHMEM requires atomicity against *all* atomic ops on that address, not just those through one domain object.
- **Mismatch 2: the fencing model.** OpenSHMEM requires ordering between atomics and blocking puts and gets from the same PE; GASNet-EX provides this only through the ACQ/REL fence flags.

**Non-contiguous RMA (VIS)** (*documented*): `gex_VIS_{Vector,Indexed,Strided}{Put,Get}{Blocking,NB,NBI}`. Nothing in OpenSHMEM needs it directly, but it could accelerate collectives over strided data.

**Active Messages** (*documented*, `docs/GASNet-EX.txt` ~1640-2250):
- **Categories**: Short (arguments only), Medium (payload copied into a library buffer at the target, then passed to the handler), Long (payload delivered straight to a client-chosen address in the target's segment). Each can be a Request or a Reply. Only one Reply may be sent from a Request handler, and all other communication is forbidden in handler context (spec lines 170-197).
- **Handler table**: `gex_AM_Index_t` is `uint8_t`. The client range is `[GEX_AM_INDEX_BASE .. 255]`, with the base guaranteed ≤128. Registration is per EP (`gex_EP_RegisterHandlers`) and is **not** collective. Arguments are `int32_t gex_AM_Arg_t`, at most `gex_AM_MaxArgs()`; GASNet-1 guaranteed at least 16 (*recalled*).
- **Payload limits**: `gex_AM_LUB{Request,Reply}{Medium,Long}()` is guaranteed to be at least 512 bytes (spec 1812, 1874). The exact `gex_AM_Max…(tm, rank, lc_opt, flags, nargs)` query depends on the pair of endpoints and is symmetric. In ibv-conduit, AM Mediums default to 64 KiB buffers, configurable from 1024 to 262144 bytes (*documented*, `ibv-conduit/README` ~170).
- **NPAM (negotiated-payload AM)**: a Prepare → Commit/Cancel split. The library can hand the client a network buffer to write into, avoiding the copy that concatenates header and payload. The client can also defer choosing the handler, arguments and (for Long) the destination address until Commit. LCPC18 §2.4 measured up to **14%** higher ping-pong bandwidth than fixed-payload AMs.
- **Immediate-mode AM injection**: LCPC18 §2.3 reports up to **97%** less running time on a synthetic "inattentive peer" benchmark, by adapting the send schedule to backpressure.

**Collectives** (*documented*, `docs/GASNet-EX.txt:3715-4192`):
- **Only `gex_Coll_BarrierNB`, `gex_Coll_BroadcastNB`, `gex_Coll_ReduceToOneNB` and `gex_Coll_ReduceToAllNB` exist.** There is no alltoall(v), gather/allgather(v), scatter or reduce-scatter.
- For OpenSHMEM 1.5/1.6 teams collectives (alltoall, alltoalls, allgather/collect, reduce), a GASNet-EX client must build most collectives itself.
- Collectives are always non-blocking, and reductions are typed so they can be offloaded (LCPC18 §2.7).
- A team needs "scratch space" in its bound segment unless it is created with `GEX_FLAG_TM_NO_SCRATCH`, in which case collectives on that team are prohibited (spec 906-955, 1045-1048).

**Tag matching**: none. GASNet has no two-sided tagged messaging; AMs serve that role.

**Counters and triggered operations**: none as primitives. `GEX_EC_*` implicit-event counters exist inside the library: an iop has "initiation and completion counters for puts and gets that are incremented atomically" (*documented*, `extended-ref/README`, Design Overview).

### A.2 Memory registration, segments, keys

- **Segment modes are a configure-time choice** (*documented*, `configure.in:1135-1139`; `ibv-conduit/README` Firehose section ~1075-1110):
  - **FAST**: the segment is pinned at attach. Out-of-segment local buffers use firehose dynamic registration or bounce buffers.
  - **LARGE / EVERYTHING**: nothing is pre-registered. Remote pinning is managed by the firehose cache, with a 75% main / 25% victim pool split.
  - **Offloaded atomics and memory kinds work only in FAST builds** (`ibv-conduit/README` "Remote Atomics"; `docs/memory_kinds_implementation.md` ~105-118).
- **`gex_Segment_Attach(&seg, tm, size)`** is collective, **at most one call per process**, and must use the primordial team. It includes an implicit barrier and publishes the segment. **"gex_Segment_Attach() does not provide alignment of segments across ranks"**: `--enable-aligned-segments` applies only to legacy `gasnet_attach()` (*documented*, spec 839-847). Aligned segments are **incompatible with PSHM** (*observed*, `configure.in:2240`: "--enable-aligned-segments is incompatible with PSHM support"). A symmetric-address client therefore cannot rely on VA alignment on current multicore nodes, and must translate addresses (base table or offsets).
- **Additional segments**: `gex_Segment_Create` + `gex_EP_BindSegment` + `gex_EP_PublishBoundSegment(tm, eps[], n, flags)` (collective; exchanges "RMA credentials"). `gex_EP_QueryBoundSegmentNB(tm, rank, &owneraddr, &localaddr, &size, flags)` fetches one peer's segment information and **may communicate**, which allows lazy discovery and caching. `GEX_FLAG_IMMEDIATE` makes it return `GEX_EVENT_NO_OP` if resolving would require communication (*documented*, spec 1263-1395).
- **Keys per peer**:
  - **ibv**: one `uint32_t rkey` **per peer per HCA** for the attached segment (`hca->rkeys`, "RKeys registered at attach time"; *observed*, `ibv-conduit/gasnet_core_internal.h:560`). Each connection endpoint caches a copy: `uint32_t rkey; // Copy of hca->rkeys[gasnetc_epid2node(epid)]` (:689). Cost is 4 bytes × N × HCAs, about 400 KB per process per HCA at 10^5 PEs, plus the per-peer segment base/size needed for address translation.
  - **ofi**: supports `FI_MR_PROV_KEY`, `FI_MR_VIRT_ADDR` and `FI_MR_ENDPOINT` (*documented*, `ofi-conduit/README:121-131, 210-240`).
  - **One registration covers the whole segment**, so a symmetric heap placed in the segment costs one key per peer.
- **Offset addressing** (`GEX_FLAG_PEER_SEG_OFFSET`) is specified to "eliminate non-scalable base address tables" (LCPC18 §2.8) but is still **[UNIMPLEMENTED]** (spec 571-578). That scalability claim is not realized in the current release.

### A.3 Per-peer state at scale

- **ibv**:
  - `GASNET_CONNECT_STATIC` defaults to TRUE and "a node will connect at startup to … ALL peers" unless a connect file is given. `GASNET_CONNECT_DYNAMIC` (default TRUE) only takes effect when static connection is restricted. A separate thread listens for connection requests (*documented*, `ibv-conduit/README:150-165, 455-480`). **Out of the box this is all-to-all RC.**
  - Mitigations: SRQ and XRC. `GASNET_USE_SRQ=-1` uses SRQ only when it reduces memory; XRC is used whenever SRQ is (README ~870-890). On-demand connections are available via connect files.
  - Per-connection record `gasnetc_cep_t`: AM credit counters, an `am_rem` semaphore padded to a cache line in threaded builds, and the rkey copy (*observed*, `gasnet_core_internal.h:626-690`). That is at least one cache line per peer per QP.
  - `hca->qps` = "qps per peer" (:584). The default is `GASNET_NUM_QPS` (*recalled*: 1 per port).
- **ucx**: "every process creates a UCX worker, participates in the exchange of worker addresses (with Allgather communication pattern), and creates UCP endpoints representing connections to all other processes" (*documented*, `ucx-conduit/README` Design Overview ~290-297). The README recommends DC transport at large scale (:54-55).
- **ofi**: address vector type FI_AV_TABLE (the default) or FI_AV_MAP (*documented*, `ofi-conduit/README:128, 248-255`). One AV entry per peer per EP.
- **API-level scalability fixes over GASNet-1** (*documented*, LCPC18 §2.8): per-peer segment queries replace the all-peers `gasnet_getSegmentInfo` array. Rank-translation queries keep N-sized tables out of the client. Neighbourhood/host queries are also scalable.

### A.4 Ordering and delivery

- Delivery is reliable. No ordering is promised between distinct operations, even to the same peer, **except** what the ACQ/REL fence flags buy for atomics (*documented*, spec "Memory Ordering/Fencing/Consistency", ~3180).
- **ibv multi-rail**: `GASNET_USE_FENCED_PUTS` "controls the use of atomic operations to provide for correct remote completion detection in the presence of multiple HCAs" (Bug 3447) (*documented*, `ibv-conduit/README` ~912-915). Striping RDMA across rails breaks naive remote-completion detection.
- **Offloaded atomics under multi-rail**: they must use **a single QP on the first HCA** for correctness (*documented*, README "Remote Atomics vs multi-rail"; *observed*, `ibv-conduit/gasnet_ratomic.c:22-35`). Ordering and coherence of NIC atomics depends on staying on one QP/HCA.

### A.5 Completion and progress model

- **Events**: root and leaf `gex_Event_t`, with `gex_Event_{Test,Wait}{,Some,All}` and `gex_Event_QueryLeaf`. "Test" no longer polls implicitly the way GASNet-1's "try" did (spec 2720-2760).
- **Progress is by polling inside library calls.** Conduits may also run a receive thread (may run AM handlers) and/or a send thread (`GASNET_RCV_THREAD`, `GASNET_SND_THREAD`). `GEX_FLAG_DEFER_THREADS` plus `gex_System_QueryProgressThreads` let the client launch these threads itself for affinity control. `gex_System_QueryHiddenAMConcurrencyLevel()` reports whether handlers can run on a hidden thread (spec 1450-1640).
- **Passive-target RMA** (the target is outside the library) needs no host progress on RDMA conduits. **AM-based emulation needs the target to poll**, the "inattentive peer" issue measured in LCPC18 §2.3.

### A.6 Threads, contexts, endpoints

- **Object model**: Client → Endpoints (EP) → one bound Segment per EP. Teams (TM) name ordered sets of EPs (spec 659-722). Several EPs per process are allowed, up to `GASNET_MAXEPS` ("optimistic compile-time constant", 676-680). An EP is identified by `(jobrank, ep_index)`. `gex_TM_Pair(local_ep, remote_ep_index)` builds an ad-hoc TM-pair for point-to-point calls without any collective team (1146-1201).
- **Restriction**: "The current implementation only supports creation of teams composed entirely of primordial endpoints" (1065-1067). Non-primordial EPs are reached only via TM-pairs.
- **Capabilities**: EPs can be created with `GEX_EP_CAPABILITY_AM` (and, per the flag family, RMA) (*documented*, spec ~1757). An RMA-only EP is a natural map for an OpenSHMEM context with no AM receive resources.
- **Thread modes**: SEQ, PAR and PARSYNC builds. `gex_System_QueryMaxThreads()` caps live client threads per process (spec 291-311). In PAR builds all endpoints are thread-safe (spec text in the AD section ~3300).
- **Hardware cost per EP**: in ibv, extra EPs map onto QP/CQ sets (*recalled*, not verified this session).

### A.7 GPU memory

- "Memory Kinds" are **off by default** and enabled with `--enable-memory-kinds`. Supported kinds are CUDA_UVA (NVIDIA), HIP (AMD ROCm) and ZE (Intel, experimental). `gex_Segment_Create` with a non-HOST kind produces device segments; an EP bound to a device segment then uses device addresses. Supported on ibv, ucx and ofi in **FAST mode only** (*documented*, `docs/memory_kinds_implementation.md:1-118`; `docs/memory_kinds.pdf`).
- Collective scratch space must be HOST memory, so teams over device-bound EPs must use `GEX_FLAG_TM_NO_SCRATCH` (spec 1049-1053).

### A.8 What clients had to build in software

#### A.8.1 OpenSHMEM reference implementation over GASNet(-1) — [OSH] `src/comms/gasnet/comms-inline.h` (3161 lines)

This is the primary evidence. The implementation targets GASNet-1 APIs (`gasnet_put_nbi_bulk`, `gasnet_AMRequestMedium0`, `gasnet_hsl_*`) and OpenSHMEM ~1.3: no contexts, no teams. *Observed in source* unless stated.

1. **Every atomic operation is an AM round trip executed under a CPU lock at the target.**
   - Covered ops: swap, compare-swap, fetch-add, fetch-inc, add, inc, fetch, set, xor.
   - Each is a Medium request/reply pair (`handler_<op>_out_<type>` / `_bak_<type>`, registered in handler table ~2650-2712).
   - The target handler takes a per-type handler-safe lock `gasnet_hsl_lock(&amo_lock_<type>)`, updates `*(pp->r_symm_addr)`, then `gasnet_AMReplyMedium0` with the old value (`AMO_SWAP_BAK_EMIT` at ~921-945; others at 1051, 1157, 1257, 1352, 1442, 1531, 1627, 1731).
   - The initiator `malloc`s a payload per call and blocks in `WAIT_ON_COMPLETION(p->completed)`, i.e. `GASNET_BLOCKUNTIL` polling (~985-1010).
   - A source comment admits the cost: "This, as in all the atomic handlers, is where the opportunity gap is … split this out into a post and wait/poll pair" (~970-980).
   - Consequences: atomics cost 2 AMs plus a malloc and a lock, and the target CPU must service AMs.
2. **Its own progress thread per host.**
   - `start_service()` loops `gasnet_AMPoll(); pthread_yield(); nanosleep(1000 ns)` (~284-300).
   - Only the first PE on each host starts it (`shmemi_thread_starter`, ~163-178). Under mpi-conduit every PE starts one (~395-400).
   - Wait mode is `GASNET_WAIT_SPINBLOCK`, or `SPIN` for PSM (~318-330).
   - The code that deferred to the ibv conduit's receive thread is disabled with `#if 0` ("Problems with IBV conduit thread if all PEs on one physical node", ~340-385).
   - Reason: AM-emulated operations (atomics, globals, locks) otherwise stall when the target PE is computing.
3. **Global and static variables (`.data`/`.bss`) are served by AM plus memcpy in FAST/LARGE builds.**
   - Globals are symmetric but not in the GASNet segment. `shmemi_comms_put/get` check `shmemi_symmetric_is_globalvar(dst)` and send `shmemi_comms_globalvar_{put,get}_request` (~2173-2280).
   - Each request is a Medium AM carrying the data after a header, chunked to `gasnet_AMMaxMedium()` (~2018, 2129), with a malloc'd page-aligned buffer and a blocking wait (~1985-2010).
   - Source TODO: "locking feels too coarse-grained with static (single) buffer" (~1893).
   - Only in SEGMENT_EVERYTHING builds can globals be targeted directly by RDMA.
4. **In SEGMENT_EVERYTHING builds, the symmetric heap is allocated and advertised by hand.**
   - `posix_memalign` allocates the heap, then the PE sends **N-1 AM Mediums** carrying `{addr,size}` and waits for N-1 replies (~800-835).
   - Otherwise `gasnet_getSegmentInfo(seginfo_table, npes)` fills an **N-entry table** (~790-795).
   - Address translation is `seginfo_table[pe].addr + (dest - seginfo_table[me].addr)` (`shmemi_symmetric_addr_lookup`, ~560-590). Segments are **not** assumed aligned: O(N) base table, O(N) startup messages.
5. **Distributed locks (`shmem_set_lock` and related) are built from swap atomics**, therefore from AMs.
   - The lock word lives on an owner PE chosen by `((uintptr_t)lock >> 3) % npes`.
   - Acquire does `shmem_int_swap` to that owner, then `shmem_short_p` to link into the predecessor's `l_next`.
   - It then waits with `GASNET_BLOCKUNTIL(!node->l_locked)` (~3026-3075).
6. **Collectives are software algorithms over put/get plus barrier.**
   - Directories: `src/broadcast/{linear,tree}`, `src/reduce/reduce-op.c`, `src/collect/collect-linear.c`, `src/alltoall/alltoall-linear.c`, `src/barrier/{linear,tree}`. They call `shmem_getmem`, `shmem_put*` and `shmem_barrier` (e.g. `broadcast-linear.c:75-78`, `reduce-op.c:230-266`).
   - The global barrier uses `gasnet_barrier_notify/wait` (~483-493).
7. **Nonblocking handles and fence/quiet are tracked by the client.** GASNet NB handles go into a uthash table (`nb_table_add`). The fence waits on a put counter, `GASNET_WAIT_PUTS()` and `nb_table_wait()` (~2420-2480).
8. **`shmem_wait/wait_until`** is `GASNET_BLOCKUNTIL(var <op> value)` polling (~866-905).

Takeaway for the core API:
- Put and get went straight to RDMA.
- **Atomics, out-of-segment symmetric data, locks, heap setup and progress on idle targets all fell back to AM plus target CPU.**
- Collectives, heap address tables and handle tracking were written in the client.
- GASNet-EX RAtomics (with NIC offload) and per-peer segment queries were created largely to remove items 1 and 4. The reference implementation predates them.

#### A.8.2 Other GASNet-EX clients (brief)

- **Named clients** (*documented*, LCPC18 §1.1): UPC++, Berkeley UPC, GCC/UPC, Clang UPC, Chapel, Legion, Titanium, Rice and OpenUH CAF, OpenCoarrays, OpenSHMEM reference impl., Omni XcalableMP.
- **AM-heavy clients** (*documented*, LCPC18 §2.4): "Cray Chapel and Stanford Legion make heavy use of GASNet AMs for moving computation to data."
- **UPC++** (*recalled*): `upcxx::rpc` is built on AM Medium/Long plus NPAM. `upcxx::copy` and GPU transfers go through memory kinds and RMA. Atomics are **not** a UPC++ feature; its remote atomics are user-level RPC.
- **Berkeley UPC** (*recalled*): uses GASNet-EX RMA and AMs. UPC 1.3 atomics were the design source for gex_AD, per LCPC18 §2.5 (*documented*). Its shared-heap management and locks use AMs.
- **Chapel** (*recalled*): the `CHPL_COMM=gasnet` path uses SEGMENT_FAST or EVERYTHING and runs on-statements via AMs. Chapel now prefers `CHPL_COMM=ofi` on HPE Slingshot.

#### A.8.3 OpenSHMEM over GASPI

**No OpenSHMEM implementation layered on GASPI/GPI-2 was found** (web search this session returned nothing). *Unverified absence.*

### A.9 Known performance pathologies and limits

- **Offloaded atomics are narrow** (*documented*):
  - **ibv**: only (F)CAS/(F)ADD/(F)SUB/(F)INC/(F)DEC/GET on **I64/U64**, plus GET/CAS on DBL. No SET, SWAP, AND/OR/XOR or 32-bit types (`ibv-conduit/README` "Remote Atomics" table). It is built on the verbs atomic work requests `IBV_WR_ATOMIC_FETCH_AND_ADD` / `IBV_WR_ATOMIC_CMP_AND_SWP` and `atomic_cap` (*observed*, `ibv-conduit/gasnet_ratomic.c:16-21, 205-340, 654-670`).
  - **"NVIDIA/Mellanox no longer supports the experimental API version needed to access the masked atomics"** (*observed*, `gasnet_ratomic.c:50-71`). The masked forms would have allowed SET, SWAP and AND/OR/XOR on 32-bit types.
  - **ucx**: offloads SET, GET, SWAP, (F)CAS, (F)ADD/SUB/INC/DEC and (F)AND/OR/XOR on **I32/U32/I64/U64**; no floating point; MULT/MIN/MAX go to AM (*documented*, `ucx-conduit/README` ~310-338).
  - **Fallback**: any AD whose op set is not fully offloadable, and all SEGMENT_LARGE/EVERYTHING ibv builds, use the AM reference (`gasnet_refratomic.c:287-476`, `gex_AM_RequestMedium` + `gex_AM_ReplyMedium`).
  - **Speedup when offloaded**: Aries offload cut 64-bit fetch-and-add latency by **70%** versus the AM reference (LCPC18 §2.5). That conduit is now removed.
- **RMA vs MPI-3 RMA** (LCPC18 Table 1, 8-byte round-trip latency, GEX / MPI):
  - Aries Haswell: 1.07 / 1.20 µs put, 1.43 / 1.57 µs get.
  - EDR IB MVAPICH2: 1.41 / 1.51 µs put, 1.82 / 1.91 µs get.
  - EDR IB Spectrum MPI (Summitdev): 1.61 / 8.10 µs put.
  - Flood bandwidth saturates by 4-8 KiB on 3 of 4 systems (§3.2).
- **Pinning limits**: `GASNET_PHYSMEM_MAX` defaults to about 2/3 of physical memory, divided across processes on a host. The firehose cache is susceptible to stale registrations (Bugs 495/955) (*documented*, `ibv-conduit/README` ~716-771, ~1460).
- **Local completion**: stalling on local completion cost up to **32%** of Put bandwidth on Aries (LCPC18 §2.2).
- **RDMA Read responders**: the HCA parameter `MAX_QP_OUS_RD_ATOM` (default 8) limits concurrent RDMA Read responders per QP; raising it to 16 gave about 30% more RDMA-GET bandwidth on one system (*documented*, `ibv-conduit/README` ~1270-1275).

### A.10 Build, portability, maintenance

- Autoconf. SEQ/PAR/PARSYNC and segment mode are **compile-time** choices, producing separate libraries (*documented*, `configure.in`). Per-conduit spawners are ssh, mpi and pmi.
- The spec is still beta after about 8 years. Several scalability features remain `[UNIMPLEMENTED]`: offset addressing, segment-disposition flags, LC_COPY.
- Vendor native conduits kept dying (Aries removed 2025; PAMI, PSM2, MXM before). The maintained set is now ibv, ofi, ucx plus portable fallbacks.

---

## Part B — GPI-2 / GASPI

### B.1 Primitives

- **API surface** (*observed*, [GPI] `src/include/GASPI.h`, full list extracted):
  - One-sided: `gaspi_write`, `gaspi_read`, `gaspi_write_list`, `gaspi_read_list`, and the notify forms `gaspi_write_notify`, `gaspi_read_notify`, `gaspi_write_list_notify`, `gaspi_read_list_notify`.
  - Notifications: `gaspi_notify`, `gaspi_notify_waitsome`, `gaspi_notify_reset`.
  - Passive two-sided: `gaspi_passive_send`, `gaspi_passive_receive`.
  - Atomics: `gaspi_atomic_fetch_add`, `gaspi_atomic_compare_swap`.
  - Collectives: `gaspi_barrier`, `gaspi_allreduce`, `gaspi_allreduce_user`.
  - Groups, segments (`create`, `alloc`, `register`, `bind`, `use`, `delete`, `ptr`, `list`), queues (`create`, `delete`, `purge`, `gaspi_wait`), `gaspi_connect`/`disconnect`, `gaspi_state_vec_get`, `gaspi_proc_kill`.
- **RMA addressing**: `(segment_id, offset, rank)`. There are no raw virtual addresses. Every communication call names a **queue** and a **timeout**.
- **Queue backpressure** (*observed*):
  - `gaspi_write` / `gaspi_notify` post to `qpC[queue][rank]`.
  - They return `GASPI_QUEUE_FULL` when `ne_count_c[queue] == queue_size_max` (`src/devices/ib/GPI2_IB_IO.c:380-384`).
  - Completion is collected by `gaspi_wait(queue, timeout)`, which covers the whole queue with no per-operation handle.
- **Notifications (a triggered-counter-like mechanism)** (*observed*):
  - `gaspi_notify(seg, rank, id, value, queue)` is **an RDMA WRITE of one `gaspi_notification_t` into the target segment's "notification space"** at index `id`, using a second rkey `rkey[1]` (`GPI2_IB_IO.c:371-419`).
  - `gaspi_write_notify` is a data write followed by a notification write.
  - **`gaspi_notify_waitsome` is a host busy-poll**: it scans `p[begin..begin+num]` for a non-zero entry, looping with `GASPI_DELAY()` (`src/GPI2_IO.c:687-800`).
  - `gaspi_notify_reset` atomically swaps the entry back to 0 and returns the old value (*recalled*).
  - Default notification space is **65536 IDs per segment** (`GASPI_DEFAULT_MAX_NOTIFICATION`, `src/GPI2_Types.h:38`).
  - Net effect: "RDMA write + remote-visible flag array + poll". No NIC-side trigger, no counter offload.
- **Atomics** (*observed*):
  - Operations: fetch-add and compare-swap on `gaspi_atomic_value_t` (u64), max `0xffff…ff` (`src/GPI2_ATOMIC.c`).
  - **IB device**: offloads with `IBV_WR_ATOMIC_FETCH_AND_ADD` / `CMP_AND_SWP` via `ibv_post_send` (or `ibv_exp_post_send` with `GPI2_EXP_VERBS`) on the **group QP**. It then **busy-polls the group CQ synchronously** (`src/devices/ib/GPI2_IB_ATOMIC.c`).
  - **Serialization**: the whole call is under the global group lock `lock_gaspi_tout(&gctx->groups[0].gl, timeout)` (`GPI2_ATOMIC.c` ~75-80). Atomics are therefore **serialized process-wide** and blocking.
  - **OFI device**: uses `fi_atomic` on a dedicated atomic CQ (`src/devices/ofi/GPI2_OFI_ATOMIC.c`).
  - **TCP device**: emulates atomics in its software device (`GPI2_TCP_ATOMIC.c`).
- **Collectives** (*observed*):
  - **Only barrier and allreduce.** Allreduce is limited to `GASPI_DEFAULT_ALLREDUCE_ELEM_MAX = 255` elements (`src/GPI2_CONFIG.c:30`) and uses its own buffer.
  - Groups are capped at `GASPI_MAX_GROUPS = 32` (`GPI2_CONFIG.c:24`).
  - The spec's rationale that other collectives belong in libraries is *recalled*.
- **Passive communication** (*observed*):
  - Two-sided send/receive on a per-peer passive QP attached to an SRQ (`GPI2_IB.c` ~968-980).
  - Receive blocks on a verbs completion channel `ibv_get_cq_event` (`GPI2_IB_PASSIVE.c:165`).
  - Maximum passive message is 65535 bytes (`GASPI_MAX_TSIZE_P`, `GPI2_CONFIG.c:28`). Maximum one-sided transfer is 1 GiB (`GASPI_MAX_TSIZE_C`).
- **Tag matching and AM**: none. Passive receive matches any sender.

### B.2 Registration and keys

- **Segments** (*observed*):
  - Up to **255 segments** (`GASPI_MAX_MSEGS`, `GPI2_CONFIG.c:25`), each registered whole.
  - Each segment has two MRs, data and notification space: `mr[2]`, `rkey[2]` (`src/GPI2_Types.h:44-70`).
- **Per-peer metadata** (*observed*):
  - `gctx->rrmd[segment_id]` is `calloc(gctx->tnc, sizeof(gaspi_rc_mseg_t))`: one descriptor **per rank per segment** (`src/GPI2_SEG.c:195-206`).
  - Each descriptor holds data addr, notification addr, 2 MR pointers, 2 rkeys, size and flags, roughly 64+ bytes (layout observed; exact size not computed).
  - At 10^5 ranks that is ≥6.4 MB per segment per process, allocated whether or not the peer is used.
- **Key exchange** (*observed*): `gaspi_segment_register(seg, rank, timeout)` exchanges keys with one rank at a time over the TCP "SN" side channel (`GPI2_SEG.c:408-412`). `gaspi_segment_create` does it for a whole group.

### B.3 Per-peer state and connection model

- **Topology setting** (*observed*, `src/include/GASPI_types.h:228-229`; `src/GPI2.c:362-390`): `GASPI_TOPOLOGY_STATIC` = "Statically connect everyone (all-to-all)", `GASPI_TOPOLOGY_DYNAMIC` = "connect peers (as needed)".
  - **Static is the default** (`GPI2_CONFIG.c` default struct). Init loops `pgaspi_connect(i)` over all lower ranks.
  - In dynamic mode, `gaspi_write` connects on first use (`GPI2_IO.c` ~310-316; `GPI2_ATOMIC.c` ~80-88).
- **QPs per peer on IB** (*observed*, `pgaspi_dev_create_endpoint`, `GPI2_IB.c:925-980`), all RC:
  - one group/collective QP (`qpGroups[i]`),
  - `queue_num` IO QPs (`qpC[c][i]`; **default 8**, `GPI2_CONFIG.c` default struct; **max 16**, `GASPI_MAX_QP`, `GPI2_Types.h:32`),
  - one passive QP (`qpP[i]`).
  - Default total is **10 RC QPs per peer**. Fully connected, 10^5 PEs means about 10^6 QPs per process. Each has queue depth `GASPI_DEFAULT_QSIZE = 1024` (`GPI2_CONFIG.c:29`).
  - Memory per RC QP on mlx5 is several KB (*recalled*), so static topology is infeasible at 10^5.
- **Side channel**: the TCP "SN" thread with epoll (`src/GPI2_SN.c:1426`, `pthread_create` at `GPI2.c:288`) handles connection setup, segment registration and group commits. Every process keeps TCP-level reachability to peers it connects with.

### B.4 Ordering and delivery

RC reliability applies per QP. **Nothing is ordered across queues**: separate queues are separate QPs (*observed* structure). The ordering claim is *recalled* from the spec. Failures appear as timeouts or `GASPI_ERROR` plus a per-rank state vector (`gaspi_state_vec_get`, HEALTHY/CORRUPT; *documented*, arXiv:1505.04628 abstract via search).

### B.5 Completion and progress

- There are no events or handles. Completion is per queue (`gaspi_wait`), per notification (`notify_waitsome`, polled), or synchronous (atomics, passive).
- **Every potentially blocking call takes `timeout_ms`**, with `GASPI_BLOCK` or `GASPI_TEST` as special values (55 `timeout_ms` parameters in `GASPI.h`). Internal locks are acquired with timeouts (`lock_gaspi_tout`).
- One-sided traffic needs no progress thread. The SN thread handles management only.

### B.6 Threads and contexts

- **Queues are the parallelism unit** (default 8, maximum 16). `gaspi_queue_create` / `gaspi_queue_delete` exist (`GPI2_IO.c:149-170`), capped at `GASPI_MAX_QP`.
- Each queue costs one CQ plus one QP per connected peer.
- `GASPI_MAX_THREADS = 1024` (`src/GPI2_Threads.c:36`).
- **Contexts**: an OpenSHMEM context maps naturally to a GASPI queue. The ceiling of 16 is below what OpenSHMEM thread-per-context use at scale wants, and each extra queue multiplies per-peer QPs.

### B.7 GPU

"No special GASPI/GPI-2 functions … to use GPUs and/or GPUdirectRDMA". The user allocates segment memory with CUDA and relies on nv_peer_mem; ConnectX-4+ is required (*documented*, [GPI] `README.md` §GPU/CUDA interoperability).

### B.8 Emulation evidence

- **No OpenSHMEM-on-GASPI exists** (see A.8.3).
- **Inside GPI-2 itself**: waitsome is CPU busy-polling of RDMA-written flags; allreduce is a software algorithm over the group QP; atomics serialize under a global lock; the TCP device emulates RDMA and atomics in software (`src/devices/tcp/`).
- **Implied cost for an OpenSHMEM port**:
  - Symmetric addresses must be translated to `(segment, offset)`.
  - Missing collectives (broadcast, alltoall, gather, reduce-to-root) must be written in software.
  - Atomics beyond fetch-add and CAS (set, swap, xor, and 32-bit types) need CAS loops or AMs, but there is no AM facility; passive send/recv plus a service thread would be needed.
  - Locks could use CAS.

### B.9 Pathologies and limits (all from source constants unless marked)

- Static all-to-all with 10 QPs per peer by default.
- Per-segment arrays sized by the number of ranks.
- 255 segments, 32 groups, 16 queues.
- Allreduce ≤255 elements; passive messages ≤65535 bytes.
- `notify_waitsome` burns a core while waiting.
- Atomics are globally locked and synchronous.
- **Performance at scale is unverified**: GPI-2 scaling papers were not fetched this session.

### B.10 Build and maintenance

- Autotools, GPL-3.
- Devices: IB verbs (primary), libfabric (EFA, Slingshot, Omni-Path; "not yet completely tested on all the different providers"), and Ethernet/TCP ("with less focus on performance") (*documented*, `README.md` §DEVICES).
- Launch via ssh, Slurm or LoadLeveler. Optional MPI interop.

---

## Part C — LCI (Lightweight Communication Interface, UIUC)

### C.1 Primitives

- **Communication operations** (*observed*, [LCI] `src/binding/input/comm.py`):
  - A generic `post_comm(direction, rank, buf, size, local_comp, …, rmr, remote_disp, tag, remote_comp, matching_policy, allow_retry …)`.
  - Specializations: `post_send`, `post_recv`, `post_am`, `post_put`, `post_get`.
  - Plus `progress`, `test_drained`, `wait_drained`.
- **Put with notification**: a put or AM can name a `remote_comp` (`rcomp_t`), a registered remote completion object at the target. This yields "RMA put/get (with/without notification)" (*documented*, `README.md` Overview). Metadata rides inline when `rcomp` fits in immediate bits (`src/core/communicate.cpp:208-256`).
- **Tag matching**: a matching engine (`resource_matching_engine`, `basic.py:145`) with `matching_policy_t::rank_tag` and other policies.
- **Collectives**: implemented in software over p2p in `src/collective/`: allreduce, alltoall, barrier, broadcast, gather, reduce, reduce_scatter (*observed*, directory listing).
- **Missing**: no atomics, no counters on remote memory, no collective offload.

### C.2 Registration and keys

- `register_memory(device, addr, size) → mr_t` and `get_rmr(mr) → rmr_t`.
- `rmr_t = {uintptr_t base; uintptr_t mr_base; uint64_t opaque_rkey}`, **24 bytes**, which the user must exchange out of band (*observed*, `src/api/lci.hpp:367-374`).
- Put and get address remote memory as `rmr` plus `remote_disp`, so an offset-based symmetric heap fits: one registration and one `rmr_t` per peer.
- **Registration cache**: optional (`use_reg_cache`).
- **IBV ODP**: explicit or implicit strategy (`network.py` net_context attributes).
- **OFI MR mode**: `FI_MR_VIRT_ADDR|FI_MR_ALLOCATED|FI_MR_PROV_KEY|FI_MR_LOCAL|FI_MR_ENDPOINT` (*observed*, `src/network/ofi/backend_ofi.cpp:89-90`).

### C.3 Per-peer state

- **ibv**: each **endpoint** eagerly creates **one RC QP per rank** in its constructor (`ib_qps.resize(get_rank_n())`, loop to `ibv_create_qp`, *observed* `src/network/ibv/backend_ibv.cpp:383-450`). Supporting per-peer state:
  - One padded atomic send-slot counter per rank (`qp_remaining_slots`).
  - With the default `ibv_td_strategy=per_qp`, **one thread domain and one parent PD per peer** (`ibv_alloc_td`, `ibv_alloc_parent_domain`, :390-425).
  - QPs share the device's single CQ and SRQ.
- **ofi**: `fi_av_insert` of all N peers at endpoint creation after a bootstrap allgather (`backend_ofi.cpp:287-327`).
- **Scaling**: no on-demand connection path was seen in the IBV backend. At 10^5 ranks this is 10^5 RC QPs and 10^5 TDs per endpoint per process.

### C.4 Ordering

Not examined in detail. RC per peer gives per-QP order. Multi-device use gives no cross-device order (*recalled*).

### C.5 Completion and progress

- **Completion objects** (*observed*, `src/binding/input/comp.py`; `src/comp/{sync,counter,cq,handler,graph}.hpp`):
  - `alloc_sync` / `sync_test` / `sync_wait`
  - `alloc_counter` / `counter_get` / `counter_set`
  - `alloc_cq` / `cq_pop`
  - `alloc_handler` (callback)
  - `alloc_graph` / `graph_add_node` / `graph_add_edge` / `graph_start` / `graph_test`
  - `register_rcomp` / `reserve_rcomps` for remote completions
- **Progress is explicit**: the user calls `lci::progress()` per device, and there is an optional separate "progress endpoint" (`network.py` device attributes). `net_poll_cq` exposes raw completions. No internal progress thread was found (*observed*, no `std::thread` in `src/`).
- **Backpressure**: statuses are done / posted / retry (`allow_retry`), analogous to GASNet-EX IMMEDIATE.

### C.6 Thread and context model

- The resource hierarchy is runtime → net_context (backend, provider) → device (CQ, SRQ, packet pool, send/recv/CQE depths: `net_max_sends`, `net_max_recvs`, `net_max_cqes`) → endpoint (QP set).
- The design targets one device per thread with lock-free structures. The SC'25 paper claims multithreaded communication "catches up with multi-process communication" (*documented*, `README.md`; paper arXiv:2505.01864 **not fetched**).
- **Cost per context**: one device means one CQ, an SRQ and a packet pool; each endpoint adds N QPs.

### C.7 GPU

`src/accelerator/accelerator_{cuda,hip}.cpp`, a `use_dmabuf` attribute for CUDA registration, and `docs/gpu_direct_rdma.md` (*observed*).

### C.8 Emulation evidence

- No OpenSHMEM or PGAS-over-LCI found. Known clients are HPX and PaRSEC (*documented*, README publication list).
- Inside LCI, AM, rendezvous, matching and collectives are all software above verbs/OFI send/recv/RDMA.

### C.9 Limits

- Eager all-to-all RC per endpoint, plus per-peer thread domains.
- Backends are ibv and ofi only on this branch. CMake still has UCX hooks (`CMakeLists.txt:160`, `lci-ucx` target) but there is no `src/network/ucx`. LCI v1 had a UCX backend (*recalled*).
- Shared memory is "experimental".

### C.10 Build

CMake, C++ with generated bindings (`src/binding/generate_binding.py`), NCSA license. The v2 API is still beta (`v2.0.0-beta.1`).

---

## Implications for a core API

**What these substrates make cheap (put in the core, pass straight to hardware):**
1. **Contiguous put/get into a pre-registered segment named by (endpoint, peer, offset).**
   - One registration covers the heap. All three libraries converge on this: GASNet segments, GASPI `(segment, offset)`, LCI `rmr + remote_disp`.
   - Include blocking, scalar-value and non-blocking forms.
   - Offset addressing removes the N-entry base table. GASNet-EX specified it for that reason and still has not implemented it. Aligned VAs are unreliable because GASNet's aligned mode is incompatible with PSHM.
2. **Separate local completion from remote completion, plus a group/implicit completion counter per context** (GASNet `lc_opt`, NBI access regions, `GEX_EC_*` counters). This is what `shmem_ctx_sync`, fence/quiet and blocking puts need. LCPC18 measured 32% on Aries.
3. **Immediate/retry injection** (GEX IMMEDIATE, LCI retry, GASPI QUEUE_FULL). This is cheap and needed by NB/NBI contexts under backpressure.
4. **An RMA-only context/endpoint** (GEX EP capabilities, LCI endpoint, GASPI queue). A context must not carry AM receive buffers or tag-matching state.

**What these substrates make expensive, and how to state the cost:**
5. **Atomics.**
   - Without NIC support, every OpenSHMEM atomic becomes AM request + reply + lock + target polling. [OSH] did this for all ten atomic ops and had to run a progress thread per host.
   - NIC offload exists but is narrow and fragile:
     - ibv verbs offers only 64-bit FADD/FCAS; the masked extensions are gone.
     - GPI-2 IB atomics serialize under a global lock.
     - UCX offers integer set/swap/cas/add/and/or/xor.
     - Aries offload gave 70% but that conduit is gone.
   - **Design lesson from GASNet-EX: expose atomics as a domain (datatype × op-set) so the implementation picks all-NIC or all-CPU coherently.** OpenSHMEM's "all atomics on an address are mutually atomic" rule forces the op-set to be the union actually used. The core should therefore offer (a) an 8-byte FADD/FCAS primitive where hardware has it, and (b) a guaranteed AM-plus-lock fallback with known cost of about 2 messages plus target CPU.
6. **Target-side progress.** Anything AM-based needs the target to poll. GASNet-EX gives optional receive threads and immediate-mode AMs; LCI gives explicit progress; GASPI busy-polls. The core must specify who drives progress for AM-backed operations: a progress thread per host (the [OSH] choice) or polling inside every API call.
7. **Per-peer connection state is the 10^5-PE killer in all three defaults.**
   - GASNet ibv connects all-to-all by default (SRQ/XRC/connect-file mitigations).
   - UCX conduit creates endpoints to all peers (DC recommended).
   - GPI-2 static topology uses 10 RC QPs per peer.
   - LCI eagerly creates 1 RC QP plus 1 thread domain per peer per endpoint.
   - **The core must allow connectionless or on-demand peer addressing** (DC, UD-based, or FI_RDM with AV) and bound per-peer metadata to a few tens of bytes: rkey plus base (or offset only), plus a lazily filled address.
   - Per-peer segment metadata is O(N) in every design: GPI-2 `rrmd[seg][rank]` ≥64 B × N × segments; GASNet ibv 4 B rkey × N × HCAs plus the base table. Allow lazy per-peer query-and-cache, as with `gex_EP_QueryBoundSegmentNB` + IMMEDIATE.
8. **Collectives.** GASNet-EX gives only barrier, broadcast and reduce; GASPI only barrier and a ≤255-element allreduce; LCI software p2p algorithms. None has alltoall or gather offload. OpenSHMEM 1.5/1.6 team collectives will live above the core in software. Keep in the core only typed reduce and barrier hooks for possible offload, plus a team-scoped scratch region (GASNet-EX requires one per team).
9. **Out-of-segment symmetric data** (`.data`/`.bss`, or a heap not in the registered segment) forces AM plus copy ([OSH] globalvar path with a coarse single-buffer lock). The core should require that everything addressable be in registered segments, allow multiple segments per context (GASPI: 255; GASNet-EX: one attached plus published segments), and make "register globals as a segment" a supported pattern.
10. **Triggered operations and counters.** GASPI notifications are only RDMA-write-into-flag-array plus host polling, and no surveyed substrate offers NIC-side triggers. A "notify" primitive is cheap to build over put, but waiting on it costs a CPU (busy-poll) unless the network provides a completion event.
11. **Device memory.** GASNet-EX memory kinds (CUDA_UVA, HIP, ZE) work only in FAST segment mode and cannot use host scratch for team collectives. GPI-2 needs no API change, just GPUDirect registration. LCI uses dmabuf. The core should treat device segments as ordinary segments with a kind tag and forbid AM or bounce-buffer fallbacks for them.

**Verification gaps** (not checked this session):
- GASPI spec text (notification and queue semantics are taken from GPI-2 source, not the standard).
- LCI SC'25 paper contents.
- GPI-2 and LCI scaling measurements.
- Exact per-QP memory on current NICs.
- `gex_EP_Create` signature and EP capability details, which are in `docs/implementation_defined.md` (not read).
