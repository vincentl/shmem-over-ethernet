# Q2: What OpenSHMEM 1.4–1.6 semantics cost on RDMA-class networks

Analysis agent report for the "minimal core API" study. It goes through every routine family in OpenSHMEM 1.4, 1.5 and 1.6. For each one it names the network or hardware property that decides the cost, and sorts the family into one of three classes:

- **CORE**: core fast path.
- **DERIVED**: can be built efficiently from core primitives.
- **SLOW**: a slow path is acceptable.

## 0. Sources, labels, caveats

**Spec text.** The spec is `scratchpad/spec16.txt`, a `pdftotext -layout` extraction of `docs/reference/OpenSHMEM-1.6.pdf` (1 Nov 2024).
- "§9.x.y" means a section of that document. "L1234" means a line in `spec16.txt`.
- Quotes from it are *documented (spec §)*.

**Implementation sources.** Local clones sit under `scratchpad/src*`. Each is cited as *observed in source* with repo, commit and file:

| Code | Repository | Commit |
|---|---|---|
| SOS | https://github.com/Sandia-OpenSHMEM/SOS | `4aa91d7fa9041785bd367fc9cefab99d313dd9df` (configure.ac says 1.5.3) |
| libfabric | https://github.com/ofiwg/libfabric | `0d5527ae694af929014da0385d409f638e81105e` |
| UCX | https://github.com/openucx/ucx | `9c85cdf1425441e4820e9d2ead9534291b5802f0` |
| osss-ucx | https://github.com/openshmem-org/osss-ucx | `cbbb91d4a6eb1f6ecd8ccd77b35c174afd2e1040` |
| OSHMPI | https://github.com/pmodels/oshmpi | `ee5cf110e673c098707257bb025404e17ac0a5fc` |
| NVSHMEM | https://github.com/NVIDIA/nvshmem | `b0d9d3dc08fc3ee0840fdb6f3a2c11932d85a2e2` (changelog 3.7.2) |

- Permalinks follow the form `https://github.com/<org>/<repo>/blob/<commit>/<path>`.

**Other documents.**
- *Portals 4.3*: SAND2022-8810, local `src-q2p/p43.txt`.
- *UEC 1.0.2* (Ultra Ethernet Specification v1.0.2, 28 Jan 2026): local `uec/uec.txt`.

**Caveats.**
- Web access was only partly used. One search for the Fujitsu uTofu guide succeeded. A fetch of the OpenSHMEM 1.5 PDF, meant to cross-check version provenance, was **declined by the user**. Version attributions below therefore rest on Annex F/G/H of the 1.6 document only.
- Several premises in the task brief are corrected by Annex G (see §1). In particular, `wait_until_all/any/some` and `put_signal` are **1.5**, not 1.6.

---

## 1. What changed in 1.4, 1.5 and 1.6 (Annex G, F, H)

*All documented, spec Annex G L10350–10600, Annex F L10027–10345, Annex H.*

**Added in 1.4 (G.3):**
- Contexts (`shmem_ctx_create/destroy`), plus ctx variants of RMA, AMO and memory-ordering routines.
- `shmem_sync_all`/`shmem_sync`, "PE synchronization without completing pending communication operations".
- `shmem_calloc`, `shmem_test`.
- `shmem_init_thread`/`shmem_query_thread` and the thread-safety semantics.
- The `shmem_atomic_*` namespace, with fetching and non-fetching bitwise AND/OR/XOR.
- Expanded RMA/AMO types, C11 generic `shmem_wait_until`.
- Item 20: "Clarified the semantics of shmem_quiet for a multithreaded OpenSHMEM PE."

**Deprecated in 1.4:** the old AMO names (`shmem_cswap`, `finc`, `fadd`, …), `shmem_wait`, long-typed `shmem_wait_until`, the entire Fortran API, SMA_* env vars, header dir `mpp`.

**Added in 1.5 (G.2):**
- Teams: `team_my_pe/n_pes/get_config/translate_pe/split_strided/split_2d/destroy`.
- `shmem_team_create_ctx`, `shmem_ctx_get_team`.
- Team-based collectives: sync, alltoall[s], broadcast, collect/fcollect, `{and,or,xor,max,min,sum,prod}_reduce`.
- **Nonblocking AMOs**.
- **Blocking and nonblocking put-with-signal**, `shmem_signal_fetch`.
- `shmem_malloc_with_hints` (hints `SHMEM_MALLOC_ATOMICS_REMOTE`, `SHMEM_MALLOC_SIGNAL_REMOTE`).
- **Multi-element and vector wait/test**: `wait_until_{all,any,some}[_vector]`, `test_{all,any,some}[_vector]`.
- The profiling interface, `SHMEM_CTX_INVALID`.
- Clarifications: "point-to-point synchronization routines preserve the atomicity of OpenSHMEM AMOs" (item 16). Also "symmetric variables used as ivar arguments ... must be updated using OpenSHMEM AMOs" (item 17).

**Deprecated in 1.5:** active-set collectives and their constants, the fixed-size `{alltoall,alltoalls,broadcast,collect,fcollect}{32,64}`, `TYPENAME_*_to_all`, `shmem_barrier` (replaced by "shmem_quiet + shmem_sync"), and short/ushort wait/test.

**Removed in 1.5:** the Fortran API and SHMEM_CACHE.

**Added in 1.6 (G.1):**
- `shmem_sum_inscan`/`shmem_sum_exscan`.
- Multiple init/finalize and `shmem_query_initialized`.
- **`shmem_ibput`/`shmem_ibget`**.
- **`shmem_signal_add`/`shmem_signal_set`**.
- `shmem_team_ptr`.
- Sessions: `shmem_ctx_session_start/stop`.
- **`shmem_pe_quiet`**.
- The memory-management routines are clarified as "collective operations on the world team" (item 10).

**Errata against 1.5 (Annex H.1):** "Clarified that Fence operations only guarantee ordering for operations that are performed on the same context" (G.1.18 / H.1.3).

**Active-set collectives are still not removed in 1.6.** Annex F lists them with "Last Version Supported: Current", so a conforming 1.6 library must still provide them. *Documented, Annex F table L10109–10131.*

**Gaps in the change history** (*documented by absence; unverified against the 1.4/1.5 PDFs*):
- No Annex G entry in 1.4–1.6 records *adding* `shmem_fence`, `shmem_quiet` or `shmem_signal_wait_until` (§9.11.15).
  - G.6 (1.1) item 1 mentions "completion semantics of memory synchronization interfaces, §9.12".
  - G.3 item 20 (1.4) clarifies `shmem_quiet`.
  - The 1.6 text treats fence and quiet as long-standing.
- I could not confirm from primary sources in which published version the C functions `shmem_fence`/`shmem_quiet` first appeared.
- The 1.6 document does not contain "memory spaces". Only OSHMPI provides them, as an extension (`shmemx_space_create/attach/malloc/create_ctx`). *Observed in source: oshmpi `include/shmemx.h.in` L12–44.*

---

## 2. Cross-cutting semantics that decide cost

### 2.1 A blocking put is not remotely complete on return
- **Quote.** "The routines return after the data has been copied out of the source array on the local PE. The delivery of data words into the data object on the destination PE may occur in any order. Furthermore, two successive put routines may deliver data out of order unless a call to shmem_fence is introduced between the two calls." (§9.6.1.1, L3564–3567)
- For `shmem_p`: "Use shmem_quiet to force completion of all remote Put transfers" (§9.6.1.2).
- **Consequence.** No put, blocking or not, needs a per-op round trip before returning. A put can be an inline or bounce-buffered send whose remote effect is observed only through quiet, barrier, fence or signal. The core must support **deferred completion accounting**, not per-op acknowledgment waits.

### 2.2 Nonblocking operations have no request handles
- **Quote.** "The routines return after initiating the operation. The operation is considered complete after a subsequent call to shmem_quiet." (§9.6.2.1, L4102; §9.7.2.x has the same wording for nbi AMOs)
- There is no handle, test or wait per operation. Completion is observable only per context: `shmem_ctx_quiet`, `shmem_ctx_pe_quiet` (1.6), `shmem_barrier_all` (default ctx), and `shmem_ctx_destroy` ("performs an implicit quiet", §9.5.3).
- **Consequence.** The core needs per-context outstanding-operation counters, and per-(context, peer) counters for `pe_quiet`. It does not need per-op request objects. SOS does exactly this: `SHMEM_TRANSPORT_OFI_CNTR_INC(&ctx->pending_put_cntr)` in its put and put-signal paths. *Observed in source: SOS `src/transport_ofi.h` ~L703, L844.*

### 2.3 Buffers stay in use until completion
- **Quote.** "Buffers provided to OpenSHMEM routines are in-use until the corresponding OpenSHMEM operation has completed at the calling PE. Updates to a buffer that is in-use ... result in undefined behavior." (§4.2, L865–867)
- For nbi puts the source buffer must stay valid until quiet, so zero-copy large nbi puts are legal.
- The glossary defines *local completion* ("after which a write ... would not affect the value(s) read"). Blocking put returns at local completion, so it may copy into a bounce buffer and return, or send zero-copy and wait for local completion only.

### 2.4 Progress: the target is passive
- **Quote.** "Other PEs should be able to communicate (e.g., put, get, atomic, etc.) and complete communication operations with that computationally-bound PE without that PE issuing any explicit OpenSHMEM calls." (§4.1, L839–842)
- The Note to Implementers allows a progress thread but says high-performance implementations "are expected to leverage hardware offload" and the community "discourages releasing OpenSHMEM implementations that do not provide asynchronous one-sided operations".
- **Consequence.** Any feature whose target side needs CPU execution needs a progress thread. That covers software-emulated AMOs, signal atomics and team-translated addressing done at the target. This is the dominant cost cliff.

### 2.5 Atomicity is scoped narrowly
- **Quote.** "The atomic routines guarantee that concurrent accesses by any of these routines to the same location, using the same datatype ..., and using communication contexts ... in the same atomicity domain will be exclusive. Exclusivity is also guaranteed when the target PE performs a wait or test operation on the same location and with the same datatype as one or more atomic operations." (§3.2, L682–685)
- Undefined behavior covers: different atomicity domains, different datatypes, **"atomic and non-atomic OpenSHMEM operations ... used to access the same location concurrently"**, and AMOs mixed with local load/store (§3.2 items 1–4).
- `SHMEM_TEAM_WORLD` and `SHMEM_TEAM_SHARED` form **separate** atomicity domains (§3.2 footnote 4).
- **Consequences:**
  - (a) AMOs never need exclusivity against RMA put/get, so plain RMA stays lock-free.
  - (b) 32-bit and 64-bit AMOs on overlapping bytes need not exclude each other, so per-width NIC atomics are enough.
  - (c) The separate WORLD and SHARED domains let an implementation run SHARED-derived contexts on CPU `lock`-prefix atomics through shared memory, and WORLD-derived contexts on NIC atomics, with no coherence between the two. Read this way, the spec deliberately avoids requiring NIC/CPU atomic coherence *across domains*.
  - (d) The spec *does* require NIC/CPU coherence for the target's own `wait`/`test` reads.

### 2.6 PE numbers on a context are team-relative
- **Quote.** "A context's associated team specifies the set of PEs over which PE-specific routines that operate on a communication context ... are performed. All point-to-point routines that operate on this context will do so with respect to the team-relative PE numbering of the associated team." (§9.5, L2947–2949; §9.6 L3463–3465)
- **Consequence.** Every RMA or AMO on a team context translates team rank to world rank or fabric address (see §4.9 for the cost).

### 2.7 Symmetric addresses and no O(N) translation
- **Quote.** "the implementation must avoid costly address translation operations in the communication path, including O(N) memory translation tables, where N is the number of PEs. In order to avoid address translations, the implementation may re-map the allocated block of memory based on agreed virtual address." (§9.3 Note to Implementers, L2002–2006)
- Global and static C variables are symmetric: "These data objects must not be defined in a dynamic shared object (DSO)" (§3, L603–604).
- **Consequences.** The data segment must be registered at init. The ideal core addresses remote memory as (segment id, offset) or (key, VA), with one key per segment valid for all PEs. Per-(PE, segment) keys or bases are exactly the O(N) tables the note warns against.
- osss-ucx keeps a per-(region, PE) base table `proc.comms.regions[region].minfo[pe].base` and a per-(ctx, region, PE) rkey lookup. Every op does a linear region search, `lookup_region`. *Observed in source: osss-ucx `src/shmemc/ucx/comms.c` L64–80, L95–155.*

### 2.8 Collectives work on symmetric arrays with one-sided readiness
- **Quote.** "Before the local PE calls a shmem_alltoall routine ... • The dest array on all PEs in the team is ready to accept the result of the operation. • The source array at the local PE is ready to be read by any PE in the team. The application does not need to synchronize to ensure that the source array is ready across all PEs prior to calling this routine." (§9.10.5; the same wording is in §9.10.7–9.10.10)
- **Consequence.** Collectives may be built from RMA get (pull) and put (push) plus a rendezvous signal. No two-sided matching engine is required.

---

## 3. Properties that decide cost

Every family below is tagged with one or more of these properties:

| Tag | Property |
|---|---|
| **P-RMA** | One-sided contiguous write/read of registered memory with no target CPU |
| **P-KEY** | One registration/key covering a whole symmetric segment, valid for all peers; (segment, offset) addressing with no per-PE tables |
| **P-ADDR** | Cost of mapping a PE number to a fabric address (world rank index), plus team-to-world translation |
| **P-CMPL** | Per-context and per-(context, peer) counting of outstanding ops, with a blocking wait (quiet, pe_quiet) |
| **P-ORD** | Per-(context, peer) delivery ordering at a fence point (fence, put-with-signal) |
| **P-RMW** | Remote read-modify-write atomicity at the target on 4/8-byte words, ideally NIC-executed: CSWAP (masked), ADD, optional bitwise ops, optional fetch-back |
| **P-COH** | Coherence of NIC-executed RMW with CPU loads at the target, so wait/test reads never see torn values (Portals calls this `PTL_COHERENT_ATOMICS`) |
| **P-WAKE** | A target-side wakeup or counter when a remote write or RMW lands, so a local wait need not spin |
| **P-AGREE** | Collective agreement and matching across a team (ordering of collective calls) |
| **P-CTX** | Hardware and memory cost of a context: tx queue, CQ, lock elision |
| **P-SPRAY** | Behaviour under multipath packet spraying and replay (UEC RUD/RUDI, EFA SRD, adaptive routing) |

---

## 4. Family-by-family analysis

### 4.1 Blocking put/get: typed, sized (8–128), mem, p/g — **CORE**
- **Versions.** Present since before 1.0. 1.4 expanded types. 1.5 added team-relative PE numbering via ctx.
- **Semantics.** See §2.1 for put. For get: "The routines return after the data has been delivered to the dest array on the local PE" (§9.6.1.5, L3858–3860), so a blocking get needs the reply before returning.
- **Deciding properties.** P-RMA, P-KEY, P-ADDR. Small `shmem_p`/`shmem_g` are "very low latency" (§9.6.1.2), so an inline or immediate-data path matters.
- **Hard parts:**
  - Large puts need fragmentation to the provider message size. SOS loops `fi_writemsg` per `shmem_transport_ofi_max_msg_size` fragment (*observed: SOS `transport_ofi.h` ~L800–825*).
  - Byte order inside a put does not matter (§9.6.1.1).
- **Class.** CORE: one-sided write/read with deferred completion.

### 4.2 Nonblocking put_nbi/get_nbi — **CORE** (same primitive), completion **DERIVED**
- **Versions.** Added in 1.3 (G.4 item 8).
- **Semantics.** See §2.2. For get_nbi, "At the completion of shmem_quiet, the data has been delivered to the dest array on the local PE" (§9.6.2.2).
- **Ordering.** Fence does *not* order get_nbi ("It does not guarantee order of delivery of nonblocking Get or values fetched by nonblocking AMO routines", §9.12.1; Table 14 has no fence mark for nonblocking Get).
- **Deciding properties.** P-RMA plus P-CMPL. At quiet time the initiator must drain completions, and for zero-copy get, handle the arriving read responses.
- **Class.** The data movement is CORE. `quiet` is DERIVED from counters plus progress.

### 4.3 Strided iput/iget; interleaved-block ibput/ibget (1.6) — **DERIVED**
- **Semantics.** `ibput` copies `nblocks` blocks of `bsize` elements. Source stride is `sst ≥ bsize` and dest stride is `dst ≥ bsize` (§9.6.1.4). "Calling shmem_ibput with a block size of 1 is equivalent to the shmem_iput API" (§9.6.1.4 Notes). Delivery order within the op is unconstrained (same wording as put).
- **Why it is not core.**
  - The destination is strided in *remote* memory. Packing into one contiguous RMA write only works when `dst == bsize`.
  - Otherwise there are two options, and the passive-target rule (§2.4) forbids the second:
    - N separate RMA writes, one header each.
    - Target-side scatter, which needs target CPU.
  - A core "vectored RMA" (one message with many (addr, len) segments under one key) would make ibput efficient. Libfabric has `rma_iov_count > 1` in `fi_msg_rma`, but providers commonly limit it (`rma_iov_limit`). *Recalled.*
- **Class.** DERIVED:
  - `dst == bsize`: a single put.
  - Few blocks: vectored RMA if the core offers it.
  - Otherwise: a loop of puts followed by nothing, since blocking-return semantics equal put's.
  - Large `nblocks` with a small `bsize` is an acceptable slow path.

### 4.4 Remote atomics (AMOs): **CORE** as one parameterized RMW primitive; the 20+ names are **DERIVED**
**Routines:**
- Non-fetching: `atomic_{set,inc,add,and,or,xor}`.
- Fetching: `atomic_{fetch,swap,compare_swap,fetch_inc,fetch_add,fetch_and,fetch_or,fetch_xor}`.
- `_nbi` variants of the fetching routines (1.5).
- The deprecated 1.4 names.

**Semantics** (*documented*):
- "The fetching routines return the original value of, and optionally update, the remote data object in a single atomic operation." (§9.7, L4180)
- "A call to a non-fetching atomic routine issues the atomic operation and may return before the operation executes on the target PE." (§9.7, L4186)
- CSWAP: "If cond and the remote dest are equal, then value is swapped into the remote dest; otherwise, the remote dest is unchanged. In either case, the old value of the remote dest is returned" (§9.7.1.3, L4408–4412).
- Types: at most 64-bit.
  - Standard AMO types (Table 6) are integers up to 64 bits: cswap, inc, add.
  - Extended types (Table 7) add float and double: fetch, set, swap.
  - Bitwise types (Table 8) are unsigned or exact-width integers.

**Deciding properties.** P-RMW, P-COH, P-SPRAY (an RMW is not idempotent, see §5).

**Network support:**
- **Portals 4.3.** `PtlAtomic`, `PtlFetchAtomic`, `PtlSwap` (the last "including compare-and-swap and swap under mask"). CSWAP/MSWAP are "restricted to the length of the longest native data type". Atomicity is "only atomic with respect to other calls to the Portals API on the same network interface" unless `PTL_COHERENT_ATOMICS` is returned, in which case it is also atomic "relative to processor-initiated atomic operations". *Documented: p43.txt §3.15, L4471–4490.*
- **HPE Slingshot (CXI/Cassini).** Hardware AMO opcodes `C_AMO_OP_{MIN,MAX,SUM,LOR,LAND,BOR,BAND,LXOR,BXOR,CSWAP}` with CSWAP comparisons `C_AMO_OP_CSWAP_{EQ,NE,LE,LT,GE,GT}`. A comment says "ATOMIC_WRITE is implemented as a CSWAP NE instead of SWAP". The provider can also issue some atomics as PCIe atomic ops (`FI_CXI_PCIE_AMO`). *Observed: libfabric `prov/cxi/src/cxip_atomic.c` L51–93; `man/fi_cxi.7.md` L1618–1636.*
- **NVIDIA ConnectX (mlx5) via IBGDA.** Extended atomic WQE opcodes `MLX5_OPCODE_ATOMIC_MASKED_CS` (swap, compare, swap_mask, compare_mask) and `MLX5_OPCODE_ATOMIC_MASKED_FA` (add, field_boundary), 4- and 8-byte (`IBGDA_{4,8}_BYTE_EXT_AMO_OPMOD`). NVSHMEM maps INC/FETCH_INC to masked FA, and SIGNAL, SIGNAL_SET, SWAP and SET to masked CS. *Observed: nvshmem `src/include/non_abi/device/pt-to-pt/ibgda_device.cuh` L127–186, L1255–1300; `externals/gpunetio/.../doca_gpunetio_verbs_def.h` L197 (`ATOMIC_MASKED_CS = 0x14`).* rdma-core's `mlx5dv.h` also defines `struct mlx5_wqe_atomic_seg` (*observed: `src/rdma-core/providers/mlx5/mlx5dv.h` L1347*). Standard IB verbs have no atomic verb. *Recalled.*
- **Fujitsu Tofu (uTofu).** ARMW "reads, calculates, and writes four-byte or eight-byte data atomically", with `utofu_armw4/8` and `utofu_cswap4/8`. *Documented, from a search summary of the "Development Studio uTofu User's Guide", https://software.fujitsu.com/jp/manual/manualfiles/m210007/j2ul2482/02enz003/j2ul-2482-02enz0.pdf. The PDF itself was not read; treat operand details as unverified.*
- **libfabric FI_ATOMIC** is often a software emulation on the target CPU:
  - EFA RDM executes `ofi_atomic_swap_handler` in provider progress on the target (*observed: `prov/efa/src/rdm/efa_rdm_pke_rta.c` L488, L540*).
  - rxm does the same (*observed: `prov/rxm/src/rxm_cq.c` L1306*).
  - These need target progress (§2.4).
  - SOS's OFI transport *requires* FI_ATOMIC and validates each datatype×op at init (*observed: SOS `src/transport_ofi.c` L1136–1244, L1479, L1684*).
- **UCX.** `amo_sw` protocol: the target worker executes `ucs_atomic_cswap##bits` on AM receipt, which needs target progress (*observed: ucx `src/ucp/rma/amo_sw.c` L162–251*). The UCP atomic API is deprecated in `ucp_compat.h`. osss-ucx still calls `ucp_atomic_fetch_nb(..., UCP_ATOMIC_FETCH_OP_CSWAP, ...)` (*observed: osss-ucx `src/shmemc/ucx/comms.c` L567–619*).
- **OSHMPI.** AMOs travel as MPI active-message packets (`MPI_Isend` + `MPI_Irecv` for the old value) (*observed: oshmpi `src/internal/amo_am_impl.h`*).

**So the task's premise needs refining.** Hardware CAS exists natively on Portals4-class NICs (Cassini), on ConnectX via extended mlx5 opcodes, and on Tofu. Libfabric exposes it portably but frequently emulates it in software.

**What must be core, and what derives from it:**
- Minimal primitive: **remote masked CSWAP on 4/8-byte words with optional fetch-back**, plus **remote ADD with optional fetch-back**. This is the intersection that ConnectX exposes.
  - `set` = non-fetching masked CS with compare_mask 0.
  - `swap` = fetching masked CS with compare_mask 0.
  - `inc` = add 1.
  - `fetch` = 8-byte get (see the P-COH note below).
- `and/or/xor[_fetch]`: native on Portals and CXI. On mlx5-class NICs they derive as a loop of fetch then CAS(old → old op v). That costs at least 2 RTTs and retries under contention. Acceptable, since bitwise AMOs are rarely hot.
- Floats for fetch/set/swap only: byte-wise CS with a full mask, so no FP semantics are needed at the NIC.

**P-COH caveat (target-side reads).** §3.2 requires exclusivity between AMOs and the target's own wait/test. If the NIC writes an 8-byte result by DMA while the CPU loads it, a torn read is possible unless DMA of ≤8 bytes lands as a single store. Portals makes this an explicit feature bit. Implementations use explicit synchronized loads: SOS `SYNC_LOAD` + `COMPILER_FENCE` in `shmem_synchronization.h`; NVSHMEM `ibgda_atomic_read`. The core should report whether NIC atomics are CPU-coherent.

**Ordering of AMOs relative to puts.**
- Table 14 lists blocking AMOs and nbi AMOs as fenced and quieted, but not the fetched values of nbi AMOs (footnote 10).
- Concurrent RMA and AMO on one location is undefined behavior (§3.2 item 3). Ordering therefore only matters for *sequential* issue from one initiator on one context. It falls under the fence discipline of §5.
- Non-fetching AMOs are nonidempotent, so a reliable exactly-once transport is required (UEC RUD is fine, RUDI is not; see §5).

**Allocation hint.** `SHMEM_MALLOC_ATOMICS_REMOTE` (§9.3.5) exists so implementations can place atomic targets in memory registered for NIC atomics. UCX NEWS mentions "remote access error when rcache memory is not registered with atomic access" (*observed: ucx `NEWS` L649*).

**Class.** CORE (one RMW primitive plus a coherence flag). Software fallback via target progress is a SLOW path.

### 4.5 Signals: put_signal[_nbi] and signal_fetch (1.5); signal_add/signal_set (1.6); signal_wait_until — **DERIVED** from core RMA + RMW + a per-op fence flag

**Exact semantics** (*documented*):
- "copying data ... to a data object on a specified PE and **subsequently** updating a remote flag to signal completion" (§9.8.3, L5542–5543).
- "The completion of signal update based on the sig_op signal operator using the signal flag on the remote PE **indicates the delivery of its corresponding dest data words** into the data object on the remote PE." (L5545–5548)
- So **data must be visible before the signal is**. The signal must be applied after every byte of its own put.
- Notes: "Without a memory-ordering operation, there is no implied ordering between the signal update of a put-with-signal routine and another data transfer. For example, the completion of the signal update in a sequence consisting of a put routine followed by a put-with-signal routine does not imply delivery of the put routine's data." (L5561–5565)
- The nbi variant adds: "two successive nonblocking put-with-signal routines, or a nonblocking put-with-signal routine with another data transfer may deliver data out of order unless a call to shmem_fence is introduced" (§9.8.4, L5680–5683).
- Atomicity (§9.8.1): the signal object is a `uint64_t`. Updates are atomic against:
  - other put-signals using the same operator;
  - signal_add when the operator is ADD, and signal_set when it is SET;
  - signal_fetch;
  - point-to-point synchronization routines accessing it.
- Operators: `SHMEM_SIGNAL_SET` is an atomic set; `SHMEM_SIGNAL_ADD` is an atomic add (§9.8.2).
- `signal_add`/`signal_set` (1.6) update the signal "without the associated data transfer" (§9.8 intro). Their text says "The update to the sig_addr signal object **at the calling PE** is expected to satisfy the atomicity guarantees" (§9.8.5 L5733–5735, §9.8.7 L5795–5797), although the target is PE `pe`. This looks like an editorial error. *Clarification needed; low risk.*
- `signal_fetch` reads the *local* signal object (§9.8.6). `signal_wait_until` blocks until the local signal object meets the condition (§9.11.15).

**How implementations do it** (*observed*):
- **SOS/OFI.** Fragmented `fi_writemsg(..., FI_DELIVERY_COMPLETE)` of the data. Then either `shmem_transport_fence(ctx)` (which, without total data ordering, is `put_quiet`: wait for completion), or the `FI_FENCE` flag on the signal op. The signal itself is `fi_atomicmsg` with `op = FI_SUM` or `FI_ATOMIC_WRITE` on `FI_UINT64` (SOS `src/transport_ofi.h` ~L795–870, L553–564).
- **SOS/UCX.** Put, then `shmem_transport_atomic`/`atomic_set` (`transport_ucx.h` L744–755).
- **NVSHMEM IBGDA.** RDMA-write WQEs followed by an atomic WQE (masked CS or FA). Put-signal WQEs are posted with `IBGDA_MLX5_FM_FENCE` in the WQE control segment. The atomic QP is chosen by hashing (pe, remote offset) (`ibgda_get_amo_qp`, "address-stable AMO") so operations on one address share a QP when multiple NICs are used (`ibgda_device.cuh` L1925–1950, L2321, L3228, L3287–3320).
- **UEC 1.0.2** names this use case: "ROD ... is designed for applications that require message ordering, e.g., MPI's match ordering or OpenSHMEM put-with-signal semantics." *Documented: uec.txt §3.2.2, L5787–5790.*

**Deciding properties.** P-RMW (the uint64 add/set), plus P-ORD within an operation: the signal RMW must be applied after the data. Under spraying this requires one of:
- (a) ordered delivery (UEC ROD);
- (b) completion-gated issue: the initiator waits for data ACKs before sending the signal, which costs one ACK RTT per put-signal;
- (c) a NIC-level fence on the signal op (`FI_FENCE`, Cassini `C_CMD_CQ_FENCE`, mlx5 FM bits).

**Class.** DERIVED:
- `put_signal` = RMA write(s) + fenced RMW{ADD or CS-mask-0}.
- `signal_add/set` = plain non-fetching AMOs on `uint64_t`.
- `signal_fetch` = local coherent load.
- `signal_wait_until` = see §4.6.
- It is *fast* only if the core exposes a **per-operation fence flag** (FI_FENCE semantics). A separate fence call per put-signal would force completion gating.

### 4.6 wait_until/test and the all/any/some[_vector] variants (1.4 test; 1.5 multi-element and vector) — local variable, remote writer: **DERIVED**, needs P-WAKE
- **Semantics.** "The shmem_wait and shmem_wait_until operations block until the value contained in the symmetric data object, ivar, **at the calling PE** satisfies the wait condition. The ivar object at the calling PE may be updated by an AMO performed by a thread located within the calling PE or within another PE." ... "Implementations must ensure that shmem_wait and shmem_wait_until do not return before the update of the memory indicated by ivar is fully complete." (§9.11.1, L7709–7720)
- The test variants never block. `test_all` of an empty set returns 1 (1.6 erratum). `*_any`/`*_some` must eventually report every satisfied index (fairness, §9.11.3/9.11.4).
- **There is no remote-variable wait in the spec.** Waiting on a remote value is left to the application: a polling fetch loop, or the target signalling back. Only "waiting on a local variable that a remote PE updates" is specified. (This corrects the task framing.)
- **Note to Implementers.** "Some platforms may allow wait operations to efficiently poll or block on an update to ivar. On others, an atomic read operation may be needed to observe updates ... implementations may be able to reduce the number of atomic reads performed by using non-atomic reads of ivar to wait for a change to occur, followed by an atomic read" (§9.11.1, L7729–7734).
- **Deciding properties.** P-WAKE and P-COH. Offload NICs do not interrupt the CPU when an RDMA write lands, so implementations poll or block on a counter.
  - SOS `SHMEM_WAIT_UNTIL_POLL`: spin with `shmem_transport_probe()`.
  - SOS `SHMEM_WAIT_UNTIL_BLOCK`: wait on `shmem_transport_received_cntr_wait(target_cntr + 1)`, a counter of received remote operations, with a `COMPILER_FENCE()` re-check.
  - *Observed: SOS `src/shmem_synchronization.h` L102–160.*
  - NVSHMEM mentions stream wait primitives (`cuStreamWaitValue`) for signal waits (*observed: nvshmem `changelog` L184*).
- **Class.** DERIVED:
  - Fast enough with a core "target-side incoming-write counter or event per region or context" that can be blocked on (Portals counting events, libfabric `FI_RMA_EVENT`, `fi_cntr_wait` on EFA hardware counters — *observed: libfabric `man/fi_efa.7.md` ~L100–113*).
  - SLOW path without it (busy-poll, or a progress thread plus a futex).
  - Multi-element and vector forms are loops over the single-element check: SLOW path acceptable.

### 4.7 fence / quiet / pe_quiet (1.6) — ordering **CORE** (a guarantee, not an API); quiet and pe_quiet **DERIVED**
**fence** (*documented*):
- "All operations on symmetric data objects issued to a particular PE on the given context prior to the call to shmem_fence are guaranteed to be delivered before any subsequent operations on symmetric data objects to the same PE on the same context. shmem_fence guarantees order of delivery, not completion. It does not guarantee order of delivery of nonblocking Get or values fetched by nonblocking AMO routines." (§9.12.1, L8931–8935)
- "shmem_fence only provides per-PE ordering guarantees and does not guarantee completion of delivery" (Notes).
- Scope is per context (erratum H.1.3).
- Table 14 covers memory stores (footnote: depends on ctx options, i.e. `SHMEM_CTX_NOSTORE`), blocking put, blocking AMO, blocking and nbi put-with-signal, nbi put, and nbi AMO (not fetched values). Blocking get is marked for quiet only.
- The fence is also thread-scoped: "The shmem_fence routine can enforce memory store ordering only for the calling thread."

**quiet** (*documented*):
- "ensures completion of all operations on symmetric data objects issued by the calling PE on the given context ... guaranteed to be complete and visible to all PEs when shmem_quiet returns." (§9.12.2, L9010–9013)

**pe_quiet** (1.6, *documented*):
- Same as quiet, restricted to the target PE list.
- "On certain platforms, when shmem_pe_quiet is invoked on a set of PEs, the performance might be equivalent to shmem_quiet." (§9.12.3, L9114–9115) The spec explicitly allows quiet as the fallback.

**Deciding properties.** P-CMPL for quiet and pe_quiet. P-ORD and P-SPRAY for fence.

**Implementations** (*observed*):
- **SOS.** `shmem_transport_fence`: `#if WANT_TOTAL_DATA_ORDERING == 0` then `shmem_transport_put_quiet(ctx)`; always `shmem_transport_get_wait(ctx)` "to support nonblocking fetch-atomics". So fence equals quiet unless the transport guarantees total data ordering (`src/transport_ofi.h` L553–564).
- **OSHMPI.** `OSHMPI_ctx_fence` and `OSHMPI_ctx_quiet` are the same body (`MPI_Win_flush`/`MPI_Win_sync` plus an AM flush) (`src/internal/order_impl.h` L36–95).
- **osss-ucx.** `shmemc_ctx_fence` calls `ucp_worker_fence(ch->w)` unless the ctx is NOSTORE (`src/shmemc/ucx/comms.c` L219–231).
  - UCX documents `ucp_worker_fence`: "Communication operations issued on a particular endpoint ... prior to this call are guaranteed to be completed before any communication operations issued on the same endpoint after this call."
  - It has modes `weak|strong|auto|ep_based` (`src/ucp/api/ucp.h` ~L4205–4225; `src/ucp/core/ucp_context.c` L104–108, L414–420).
  - UCX's text says "completed", which is stronger than OpenSHMEM's "delivered".
- **Memory stores.** Local stores through `shmem_ptr` need CPU memory barriers. osss-ucx uses `_mm_mfence`/`__sync_synchronize` (`src/shmemc/ucx/memfence.h` L34–48).

**Class.**
- `quiet`: DERIVED from per-ctx outstanding counters plus progress.
- `pe_quiet`: DERIVED from per-(ctx, peer) counters, or SLOW (plain quiet, which the spec permits).
- `fence`: DERIVED as either a no-op (when the core guarantees per-(ctx, peer) order) or per-peer completion gating. A core per-op fence flag makes it cheap. See §5.
- Memory at 10^5 peers: dense per-peer counters cost 8 B × 10^5 = 0.8 MB per context (*arithmetic*). A sparse table of peers with in-flight ops is better.

### 4.8 Contexts and thread levels — **CORE** (context = independent queue + completion + ordering domain); options **DERIVED**
**Semantics** (*documented*):
- "The communication context defines an independent ordering and completion environment" (§9.5, L2929).
- Contexts are shareable by default.
- `SHMEM_CTX_SERIALIZED`: "not used by multiple threads concurrently".
- `SHMEM_CTX_PRIVATE`: "used only by the thread that created it".
- `SHMEM_CTX_NOSTORE`: "Quiet and fence operations performed on the given context are not required to enforce completion and ordering of memory store operations" (§9.5 table, L2960–2972).
- `shmem_ctx_create` is "not collective".
- "An unsuccessful context creation call is not treated as an error and the OpenSHMEM library remains in a correct state" (§9.5.1, L2997–3002). Implementations may therefore cap the number of contexts.
- Team config `num_contexts` is a best-effort reservation (§9.4.3).
- Destroy performs an implicit quiet (§9.5.3).
- Thread levels (§9.2): in THREAD_MULTIPLE "all OpenSHMEM calls are thread-safe"; "Blocking OpenSHMEM calls will only block the calling thread".

**THREAD_MULTIPLE cost.**
- Shareable contexts, including `SHMEM_CTX_DEFAULT`, need a lock or lock-free queue on every op.
- SOS takes a per-ctx mutex unless `PRIVATE|SERIALIZED` (*observed: SOS `transport_ofi.h` L348–352*).
- SOS shares STX (transmit contexts) among contexts and dedicates them for PRIVATE ones (*observed: `transport_ofi.c` L422–566*).

**Deciding properties.** P-CTX (a hardware queue or CQ per context), plus ensuring per-peer state is **not** duplicated per context. Connections × contexts at 10^5 PEs would be prohibitive, so address vectors must be shared across contexts.

**Class.** CORE: a context is a cheap object carrying (tx queue or shared queue, completion counters, ordering domain) over a shared per-peer address table. The option semantics (lock elision, NOSTORE skipping memory barriers) are DERIVED. Context creation is a SLOW path.

### 4.9 Teams (1.5): split_strided, split_2d, translate, get_config, destroy, team contexts — creation **SLOW**, translation **DERIVED** (O(1))
**Semantics** (*documented*):
- Team creation "is a collective operation on the parent team object" and must happen in the same order on all members (§9.4).
- `split_strided` takes a (start, stride, size) triplet. 1.6 makes wrap-around undefined, so a team is an arithmetic progression of parent ranks (§9.4.6).
- `split_2d`: "(x, y) = (pe mod xrange, ⌊pe ÷ xrange⌋)" (§9.4.7). X-teams are contiguous runs; y-teams have stride `xrange`, so both are arithmetic progressions.
- **Team-relative RMA exists**: contexts from `shmem_team_create_ctx` number PEs relative to their team (§9.5.2 L3048–3049; §9.6 L3463–3465).
- `shmem_team_destroy` is collective and destroys shareable contexts (§9.4.8).

**Translation cost.**
- Any team derived from WORLD by strided or 2D splits (even recursively) maps team rank i to world rank `a + b·i`, because affine maps compose. Translation is one multiply and one add, with O(1) memory per team (*derived by composition*).
- `SHMEM_TEAM_SHARED` (node-local PEs) is not an arithmetic progression in general. It needs a table of local-node size, and teams split from SHARED inherit that table.
- **Implementations:**
  - SOS stores `start, stride, size` per team (*observed: `src/shmem_team.h` L20–29*). Yet its RMA path passes `pe` straight to `GET_DEST(pe)` with no team translation (`data_c.c4` `SHMEM_DEF_PUT` L327–344; `transport_ofi.h` L275–277, L649, L689). This looks like **nonconformance for team contexts** (*observed; medium confidence — a translation elsewhere could have been missed*).
  - osss-ucx translates with khash forward/reverse maps (`src/shmemc/teams.c` L241–261), which is O(team size) memory. Its `comms.c` RMA path shows no team translation either (*observed; medium confidence*).
- **Deciding properties.** P-ADDR (translation per op) and P-AGREE (creation).

**Class.**
- Translation: DERIVED in software above a core that addresses world ranks. Cost is negligible for strided and 2D teams; SHARED teams need a small local table.
- Split and destroy: SLOW (collective agreement).
- `team_my_pe/n_pes/get_config/translate_pe`: local.

### 4.10 shmem_ptr / team_ptr (1.6) / addr_accessible / pe_accessible — **outside the network core** (local queries; on-node shared mapping)
- **Semantics.** `shmem_ptr` returns a local pointer "when it can be accessed using memory loads and stores. Otherwise, a null pointer is returned" (§9.1.9). `team_ptr` does the same with a team-relative PE (§9.1.10).
- `SHMEM_TEAM_SHARED` is defined as the PEs that "would mutually return a non-null address from a call to shmem_ptr or shmem_team_ptr for all symmetric heap objects" (§7, L1167–1177).
- `pe_accessible` means "running from the same executable file". `addr_accessible` means the address is symmetric and accessible from that PE (§9.1.7–9.1.8).
- **Deciding property.** An OS-level mapping of peer heaps (XPMEM, CMA, shm), not the fabric.
- **Interaction.** Stores through `shmem_ptr` fall under fence and quiet (Table 14, "Memory Store") unless NOSTORE, which requires CPU memory barriers.
- **Class.** Not in the network core. `pe/addr_accessible` are local registry lookups: SLOW is fine.

### 4.11 Distributed locks: set_lock/test_lock/clear_lock — **DERIVED** from CAS/swap AMOs + wait + quiet; **SLOW** is acceptable
- **Semantics.** "Waiting PEs are guaranteed to set the lock in a first-come, first-served manner." `shmem_clear_lock` releases "after performing a quiet operation on the default context". The lock is a "non-reentrant mutex". The lock object is a symmetric `long` initialised to 0, and any other access while in use is undefined behavior (§9.13.1, L9327–9346).
- **SOS.** A distributed FIFO queue:
  - `swap` on `lock->last` at PE 0 to enqueue;
  - masked swap (`shmem_internal_mswap`) on the predecessor's `data` to link;
  - `cswap` in clear to release if last;
  - `atomic_fetch` of the local word, then `SHMEM_WAIT` for the signal bit.
  - The two 32-bit fields `last`/`data` are packed into the user's `long`.
  - *Observed: SOS `src/shmem_lock.h` L1–110.*
- **OSHMPI.** Uses `MPI_Fetch_and_op` and `MPI_Compare_and_swap` (*observed: `src/internal/lock_impl.h` L56–158*).
- **Deciding properties.** P-RMW (swap, masked swap, CAS), P-WAKE, P-CMPL (the quiet in clear).
- **Class.** DERIVED and SLOW. Masked swap is directly the mlx5/Portals MSWAP shape, another argument for making the core CS primitive *masked*.

### 4.12 Symmetric memory: malloc/free/realloc/align/calloc/malloc_with_hints; symmetric statics — **SLOW** allocation, with a **CORE** registration requirement
**Semantics** (*documented*):
- World-collective (1.6 G.1 item 10), with identical arguments on all PEs.
- The implicit barrier "semantically equivalent to shmem_barrier_all", which quiets the default context: on exit for malloc, align, calloc and malloc_with_hints; on entry for free; entry and/or exit for realloc (§9.3, §9.3.1–9.3.6).
- Zero size means no action and no barrier (1.5 G.2.25).
- `shmem_realloc` "returns the symmetric address of the allocated space (which may have moved); otherwise, all PEs return a null pointer"; it preserves hints and does not preserve `shmem_align` alignment (§9.3.3).
- The user must ensure no pending operations on *other* contexts before free/realloc.
- Hints: `SHMEM_MALLOC_ATOMICS_REMOTE` should be used "when the majority of operations performed on this memory are atomic operations, and origin and target PEs ... do not share a memory domain" (§9.3.5).
- Statics are symmetric, and DSO statics are excluded (§3).

**Deciding properties.**
- P-AGREE: a barrier over the world. At 10^5 PEs a tree barrier is about 17 levels, i.e. O(log N) latency.
- P-KEY: if each allocation needs a new registration and key exchange with 10^5 peers, it becomes catastrophic. The fix is to reserve a large VA range per PE at init with an agreed layout, register it once with one key (or an implicit or ODP registration), and have `shmem_malloc` only run a deterministic allocator plus a barrier.
- For the data segment, PIE/ASLR moves it per process, so either a base offset per PE (the forbidden O(N) table) or disabled ASLR or same-address remapping is needed. osss-ucx assumes globals (region 0) have identical addresses and translates only heaps through a per-PE base table (*observed: `comms.c` L109–122*).

**Class.** The allocation calls are SLOW (collective). The core must provide cheap large-region registration with one key valid for all peers, plus an "atomic-capable" registration attribute.

### 4.13 Collectives — **DERIVED** (software over RMA + AMO add + wait), **SLOW** acceptable except barrier/sync latency
**Team-based (1.5).**
- Routines: `shmem_team_sync`, alltoall[s], broadcast (updates root dest too, 1.5 G.2.4), collect/fcollect, `{and,or,xor,max,min,sum,prod}_reduce` (in-place allowed), and `sum_{in,ex}scan` (1.6).
- "All OpenSHMEM teams-based collective operations are blocking routines", must happen "in the same program order across all PEs in a team", and "may not be called simultaneously by multiple threads on a given team" (§9.10, L6085–6089).
- `shmem_barrier_all` "is equivalent to a call to shmem_ctx_quiet on the default context followed by a call to shmem_team_sync on the world team" (§9.10 L6149–6151, §9.10.1).
- sync "only ensures completion and visibility of previously issued memory stores and does not ensure completion of remote memory updates" (§9.10.3–9.10.4).

**Active-set (deprecated 1.5, still required in 1.6).**
- `shmem_barrier`, active-set `shmem_sync`, `{alltoall,alltoalls,broadcast,collect,fcollect}{32,64}`, `TYPENAME_{and,...,prod}_to_all`.
- Parameters: `PE_start`, `logPE_stride` (stride = 2^logPE_stride), `PE_size`.
- `pSync` is a *symmetric* work array initialized to `SHMEM_SYNC_VALUE` and restored on return (§9.10, L6110–6126). Active-set broadcast does *not* update the root's dest (§9.10.7).

**Implementation evidence.** SOS builds collectives from `shmem_internal_put_scalar` onto the symmetric `pSync`, `SHMEM_WAIT_UNTIL(pSync, ...)`, and `shmem_internal_atomic(... SUM ...)` (*observed: SOS `src/collectives.c` L271–404; 157 uses of put/get/atomic/wait in that file*). This confirms collectives are derivable from RMA + AMO add + target wakeup.

**Deciding properties.**
- P-AGREE.
- P-WAKE: rendezvous at the waiting PE.
- P-RMA for the bulk data.
- Optional collective offload (SHARP, Portals triggered ops) is an accelerator, not a requirement.

**Scaling notes.**
- `collect` (variable `nelems`) needs a size agreement first (two phases).
- `alltoall` is O(N²) data by definition.
- Scans need prefix algorithms in team rank order.
- Team-based collectives on different teams may run in parallel, so each team needs its own match state (SOS uses `psync_idx` per team, *observed `shmem_team.h`*).

**Class.**
- DERIVED: barrier, sync, broadcast, reduce, scan.
- SLOW acceptable: alltoall[s], collect.
- Active-set routines: DERIVED by creating or caching an internal strided team with stride = 2^logPE_stride, plus the pSync/root differences. SLOW.

### 4.14 Sessions (1.6): ctx_session_start/stop, BATCH, total_ops — **DERIVED** (hints; a no-op is conformant)
- **Semantics.** "These options serve only as hints to the library; it is up to the implementation whether or not to apply any optimizations within a session." "Starting and stopping OpenSHMEM sessions should not affect the completion or ordering semantics of any OpenSHMEM routines" (§9.9, L5811–5817).
- BATCH candidates are "non-blocking RMAs, non-blocking AMOs, non-blocking put-with-signals, blocking scalar puts, small blocking puts, and blocking non-fetching AMOs". "large blocking puts, all blocking gets, blocking fetching AMOs, and the memory ordering routines might require the library to enforce completions" (Table 9, L5950–5956).
- The example calls `shmem_ctx_quiet` after stop: "shmem_ctx_session_stop() does not quiet the context" (L6043).
- **Deciding properties.** Doorbell and packet coalescing (libfabric `FI_MORE`, supported by CXI per `man/fi_cxi.7.md` L101–110). Batching is legal because blocking puts need only local completion (§2.1).
- **Class.** DERIVED and SLOW. The core only needs an optional "more coming" or deferred-doorbell flag.

### 4.15 shmem_global_exit — **outside the data core** (runtime control plane)
- **Semantics.** "non-collective ... When any PE calls shmem_global_exit, it results in the immediate notification to all PEs to terminate" (§9.1.6, L1500–1505).
- **Deciding property.** Out-of-band job control (PMI/PMIx abort, launcher), not RDMA.
- **Class.** SLOW, via the runtime.

### 4.16 init/finalize (multiple init in 1.6), query_initialized, info, profiling — **SLOW / not network**
- Finalize is collective. The last call "performs an implicit global barrier" and destroys teams and shareable contexts (§9.1.4, L1406–1420).
- Profiling requires `pshmem_` name-shifted entry points (§10). This costs nothing on the data path, but it rules out routines that exist only as inline macros without a `pshmem_` twin.

### 4.17 SHMEM_TEAM_SHARED — **SLOW** (init-time locality agreement)
- Defined by mutual `shmem_ptr` accessibility (§7). It is a distinct atomicity domain (§3.2).
- It needs locality discovery at init (hostname or NUMA exchange), which is a collective, and a local translation table (§4.9).

### 4.18 Memory spaces — **not part of 1.4–1.6**
- No occurrence in the spec text. Only OSHMPI's `shmemx_space_*` extension exists (§1). Out of scope for conformance.

---

## 5. Fence ordering under packet spraying (UEC RUD/RUDI, EFA SRD, Slingshot adaptive routing)

### 5.1 What OpenSHMEM actually requires
- **R1 (fence).** For each (context, target PE): operations issued before a fence are delivered before operations issued after it (§9.12.1). This covers put, put_nbi, AMO (not fetched values), put-with-signal, and, unless NOSTORE, local stores.
- **R2 (no ordering within an interval).** Between fences, delivery order is unconstrained, even for the words of a single put (§9.6.1.1). Different contexts and different target PEs are never mutually ordered (H.1.3).
- **R3 (signal after data).** Inside one put-with-signal, the signal update implies that its own data is delivered (§9.8.3). This is an intra-op fence.
- **R4 (quiet).** Completion and visibility per context; per (context, PE set) for pe_quiet.
- **R5 (exactly-once for AMOs).** Non-fetching and fetching AMOs are not idempotent, so replay is not allowed.
- Get_nbi and nbi-AMO fetched values are excluded from R1.

### 5.2 How spraying breaks naive implementations
- **UEC RUD** ("Reliable, unordered delivery ... can tolerate reordering in the network (e.g., atomic add operations)") gives exactly-once but unordered delivery. Two puts to the same address issued in sequence may land in either order. That satisfies R2 and R5 but not R1 or R3 without extra mechanism. *Documented: uec.txt §3.2.2, L5781–5786.*
- **UEC RUDI** allows packets to be "delivered more than once to the semantic sublayer and in any order". Data "can be written into memory multiple times up until the final message completion is delivered at the initiator". RUDI RMA also requires the endpoint not be configured for R/W message ordering and the memory key to be `IDEMPOTENT_SAFE` (Table 2-16). *Documented: L5796–5803, L4139–4151.*
  - Consequence 1: a replayed packet of put #1 can overwrite put #2 after #2 has landed, **even if #2 was issued after a fence**, unless #2 is released only after #1's completion reached the initiator.
  - Consequence 2: RUDI is never valid for AMOs or signals (R5).
  - Consequence 3: for puts, fence must be completion-gated.
- **UEC ROD** delivers "once and only once in the order the packets are sent" on "a single network path (i.e., using a single entropy value)" with GoBackN (§3.5.7.2, L10510–10519). A fence is free if all of a context's operations to a peer share one ROD PDC, but spraying is lost and GoBackN is costly under loss.
- **EFA SRD** is unordered (*documented: `man/fi_efa.7.md` L35*). The EFA device's default strict-ordering MR mode guarantees "that the TX completion of an operation guarantees that subsequent operations to the same endpoint appear at the target after it"; `FI_EFA_MR_RELAXED_ORDERING` removes that (*documented: `man/fi_efa.7.md` L288–298*). EFA ordering is therefore **completion-gated**: a natural fence implementation.
- **Slingshot/Cassini.** libfabric `FI_FENCE`: "the requested operation, also known as the fenced operation, and any operation posted after the fenced operation will be deferred until all previous operations targeting the same peer endpoint have completed" (*documented: `man/fi_msg.3.md` L329–340, `man/fi_atomic.3.md` L675–686*). CXI lists `FI_FENCE` and implements it with a hardware command-queue command `C_CMD_CQ_FENCE` (*observed: `prov/cxi/src/cxip_cmdq.c` L368–369*). Requesting it has a cost: "Requesting FI_FENCE prevents the use PCIe RO for RMA" (*observed: `prov/cxi/src/cxip_info.c` L1987–1992*).
- **Portals 4.3.** Ordering is guaranteed only for messages below `max_waw_ordered_size`. Long message bodies may be adaptively routed. "Portals only guarantees ordering semantics sufficient for shmem_fence() to be a no-op when PTL_TOTAL_DATA_ORDERING is returned ... and the operations are both shorter than max_waw_ordered_size." *Documented: p43.txt §2.7.1–2.7.2, L1010–1052.*
- **ConnectX (mlx5).** WQE control segments carry fence-mode bits `NO_FENCE`, `INITIATOR_SMALL_FENCE`, `FENCE`, `STRONG_ORDERING`, `FENCE_AND_INITIATOR_SMALL_FENCE`. NVSHMEM sets `FM_FENCE` on put-signal and AMO WQEs. *Observed: nvshmem `ibgda_device.cuh` L130–136, L672, L2321, L2535, L2879, L3228.* The exact hardware semantics of these modes are not documented publicly in anything I read (*unverified; the names suggest per-QP fence and strong-ordering modes for adaptive-routing and out-of-order RDMA*).
- **Multi-rail.** NVSHMEM's "address-stable AMO" hashes (pe, remote offset) to a QP so operations on the same address stay on one QP across several NICs (*observed: `ibgda_device.cuh` L1925–1950*). Ordering domains must follow (context, peer), or at least (context, peer, address), not the rail.

### 5.3 The minimal guarantee a core must give
To serve R1–R5 at 10^5 PEs over sprayed fabrics without forcing total order, the core should provide:

1. **Exactly-once RMW** (R5). A core that only offers idempotent (RUDI-like) delivery cannot host AMOs or signals except through a software target.
2. **A per-operation fence flag scoped to (queue, peer)**, with `FI_FENCE` semantics: the flagged op is applied at the target only after every earlier op on the same queue to the same peer has completed. This gives:
   - R3 directly: flag the signal RMW.
   - R1 lazily: set the flag on the *first* op after an `shmem_fence` call, so fence itself does no work and costs nothing if no op follows.
3. **A per-(queue, peer) in-flight counter with a blocking wait**. This lets software implement the flag by completion gating when hardware cannot (SOS and OSHMPI already do), and it serves pe_quiet (R4). If the count is zero, the fence is free, so completion gating only stalls peers with in-flight operations.
4. **No guarantee required**:
   - ordering across queues (contexts) or across peers;
   - ordering of bytes within an operation;
   - ordering of get replies or fetched values;
   - total data ordering.

   A core should *not* promise ROD-style ordering by default, because that forfeits spraying.
5. **A declared idempotence contract**:
   - Plain RMA writes may use RUDI-class delivery only if fence is completion-gated (point 3).
   - Any provider that may replay stale writes after a later write's completion must report it, so the layer above never treats a fence as a no-op.

---

## 6. Summary classification

| Family | Versions (per Annex G) | Cost-deciding property | Class |
|---|---|---|---|
| put/get, p/g, sized, mem (blocking) | ≤1.3, ctx 1.4, team-rel 1.5 | P-RMA, P-KEY, P-ADDR; put returns before remote completion | CORE |
| put_nbi/get_nbi | 1.3 | P-RMA + P-CMPL (no handles; quiet only) | CORE data path; completion DERIVED |
| iput/iget | ≤1.3 | P-RMA; strided remote dest means N ops or vectored RMA | DERIVED |
| ibput/ibget | **1.6** | same as iput; bsize=1 is iput | DERIVED (large nblocks SLOW) |
| AMO non-fetching set/inc/add/and/or/xor | 1.3 (set), 1.4 names/bitwise | P-RMW exactly-once; P-COH | CORE primitive (masked CS + ADD); names DERIVED; bitwise via fetch+CAS loop where not native |
| AMO fetching fetch/swap/cswap/fetch_* | ≤1.4 | P-RMW with fetch-back; P-COH for fetch vs NIC writes | CORE (same primitive with fetch) |
| AMO _nbi | 1.5 | P-RMW + P-CMPL; fetched values not fenced | CORE + DERIVED completion |
| put_signal[_nbi], signal_fetch | 1.5 | P-RMW + intra-op P-ORD (signal after data) + P-SPRAY | DERIVED; fast only with per-op fence flag |
| signal_add/signal_set | **1.6** | P-RMW on uint64 | DERIVED (plain AMO) |
| wait_until/test (single) | test 1.4 | P-WAKE, P-COH (local var, remote writer) | DERIVED (counter/event) or SLOW (poll) |
| wait/test all/any/some[_vector] | **1.5** | as above, loops | SLOW acceptable |
| signal_wait_until | not recorded in Annex G | P-WAKE | DERIVED |
| fence | pre-1.4 (per-ctx clarified by 1.5 erratum) | P-ORD, P-SPRAY | DERIVED from per-op fence flag or completion gating |
| quiet | pre-1.4 | P-CMPL | DERIVED |
| pe_quiet | **1.6** | per-(ctx, peer) P-CMPL; spec allows quiet fallback | DERIVED / SLOW |
| contexts + options, thread levels | 1.4 (team ctx 1.5) | P-CTX; shared per-peer state; lock elision | CORE object; options DERIVED; create SLOW |
| teams split/destroy | 1.5 | P-AGREE | SLOW |
| team-relative PE on ctx | 1.5 | P-ADDR: affine O(1) for strided/2D teams; table for SHARED | DERIVED |
| shmem_ptr / team_ptr (1.6) / *_accessible | ptr ≤1.2; team_ptr 1.6 | on-node mapping; memory barriers for fence/quiet | outside network core |
| locks | ≤1.2 (clarified 1.5) | P-RMW (swap, masked swap, CAS), P-WAKE, P-CMPL | DERIVED, SLOW |
| malloc/free/realloc/align/calloc/with_hints | calloc 1.4, hints 1.5, world-collective 1.6 | P-AGREE (barrier), P-KEY (one registration) | SLOW calls; CORE needs cheap big-region single-key registration |
| symmetric statics | always | data-segment registration; ASLR base | CORE registration at init |
| barrier_all / sync / sync_all | sync 1.4 | P-CMPL + P-AGREE | DERIVED |
| broadcast / reduce / collect / fcollect / alltoall[s] | team 1.5 | P-AGREE, P-RMA, P-WAKE | DERIVED / SLOW |
| inscan/exscan | **1.6** | P-AGREE, prefix order | DERIVED / SLOW |
| active-set collectives (*_to_all, barrier, 32/64 variants) | deprecated 1.5, **still required in 1.6** | P-AGREE; symmetric pSync | DERIVED via internal strided team, SLOW |
| sessions | **1.6** | doorbell/packet coalescing hint | DERIVED (no-op conformant) |
| global_exit | 1.2 | out-of-band runtime | outside core |
| init/finalize/query_initialized (1.6) | | collective barrier | SLOW |
| memory spaces | not in spec | — | n/a (OSHMPI extension only) |

---

## 7. Implications for a core API
**What must be in the core** (each item follows from the semantics above):
1. **One-sided RMA write and read** addressed by (world rank, segment key, offset), with deferred completion. Optional vectored segments help ibput/iput.
2. **One registration per symmetric segment** (heap reservation and data segment), valid for all peers, with an "atomic-capable" attribute.
3. **Remote RMW on 4/8-byte words**: masked compare-and-swap and add, each with optional fetch-back and exactly-once delivery. Also a capability bit for native AND/OR/XOR and one for CPU coherence (`PTL_COHERENT_ATOMICS`-like).
4. **Contexts** as cheap (queue, completion-counter, ordering-domain) objects over shared per-peer addressing, with a single-threaded or no-lock mode.
5. **Per-(context, peer) and per-context in-flight counters** with blocking wait. Together with (6) these serve quiet, pe_quiet and fence-by-gating.
6. **A per-operation fence flag** with (queue, peer) scope (`FI_FENCE` semantics) and no stronger ordering promise.
7. *(Optional but important for waits, locks and collectives)* **a target-side incoming-operation counter or event** that can be blocked on.

**Everything else sits above the core:**
- typed and sized API variants, p/g;
- ibput/iput;
- all AMO names;
- signals;
- team translation, which is O(1) affine for strided and 2D teams;
- locks, collectives, active-set shims, sessions.

**Slow paths that are acceptable:**
- allocation and team collectives;
- context creation;
- multi-element and vector waits;
- active-set collectives;
- global_exit;
- software AMO fallback through a progress thread, where hardware RMW is absent (EFA/rxm/UCX-style emulation).

**Findings worth flagging to the design effort:**
- SOS, and apparently osss-ucx, do not translate team-relative PE numbers in their RMA paths. *Observed; medium confidence.*
- The signal_add/set text in §9.8.5/§9.8.7 ("at the calling PE") looks editorial.
- A RUDI-class idempotent transport is incompatible with a zero-cost fence and with any AMO or signal traffic.
