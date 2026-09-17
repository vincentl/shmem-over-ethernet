# Q2 survey: UCX (UCP/UCT), OSSS-UCX, and Open MPI `oshmem` (spml/ucx)

## Sources and how claims are labelled

I cloned these sources shallowly into the scratchpad and read them. Every permalink below points at the exact commit I read.

| Tree | Commit | Date | Permalink base |
|---|---|---|---|
| UCX (`openucx/ucx`, develop, `configure.ac` says 1.24.0) | `9c85cdf1425441e4820e9d2ead9534291b5802f0` | 2026-09-16 | `https://github.com/openucx/ucx/blob/9c85cdf1425441e4820e9d2ead9534291b5802f0/` |
| Open MPI (`open-mpi/ompi`, main, `oshmem/` only) | `8caa67e74b7bb3144b4a995156bd6102baa44dcb` | 2026-09-15 | `https://github.com/open-mpi/ompi/blob/8caa67e74b7bb3144b4a995156bd6102baa44dcb/` |
| OSSS-UCX (`openshmem-org/osss-ucx`, `main`) | `cbbb91d4a6eb1f6ecd8ccd77b35c174afd2e1040` | 2023-05-19 | `https://github.com/openshmem-org/osss-ucx/blob/cbbb91d4a6eb1f6ecd8ccd77b35c174afd2e1040/` |

The paths below are relative to those bases: `UCX:`, `OMPI:`, `OSSS:`.

Each claim carries one of three labels:
- **[src]**: I observed it in the source (the file is named).
- **[doc]**: it is documented at a URL.
- **[recalled]**: it comes from memory and I have not verified it.

OSSS-UCX activity: `main` was last touched in May 2023, and `master` in 2021. OSSS-UCX's `configure.ac` still sets `spec_minor_version` to 4, yet the tree implements the 1.5/1.6 routines `put_signal` and `wait_until`, which suggests the code moved ahead of the version string **[src]** (`OSSS:configure.ac`). The Open MPI tree is the actively maintained one.

---

## 1. Primitives relevant to one-sided PGAS

**RMA.**
- `ucp_put_nbx` and `ucp_get_nbx` take an endpoint, a buffer, a remote address and a `ucp_rkey_h` **[src]** (`UCX:src/ucp/api/ucp.h`).
- Completion of a put only means the local buffer can be reused. The API says: *"completion of the operation on the remote address requires use of ucp_worker_flush_nbx() or ucp_ep_flush_nbx()"* **[src]** (ucp.h, around line 3853).
- A put or get can now carry a remote scatter-gather list, `ucp_dt_make_sgl()` with `ucp_request_param_t::remote`, giving a separate address and rkey per element. That is a vectored or batched RMA primitive **[src]** (ucp.h, around lines 871–896).
- Protocols per message size:
  - zero-copy RDMA (`put_offload.c`, `get_offload.c`)
  - rendezvous for RMA (`rma_rndv.c`)
  - an AM-emulated software path (`put_am.c`, `get_am.c`, `rma_sw.c`)

  All are in `UCX:src/ucp/rma/` **[src]**.
- With the software path, the target must run `ucp_worker_progress`:
  - `ucp_put_handler` unpacks the data to `puth->address` and sends a completion AM.
  - The get handler sends the reply as AM bcopy fragments.

  Source: `UCX:src/ucp/rma/rma_sw.c` lines 70–187 **[src]**.

**Atomics.**
- `ucp_atomic_op_nbx` supports ADD, AND, OR, XOR, SWAP and CSWAP on 32- and 64-bit values, with an optional reply buffer for the fetch variants **[src]** (`UCX:src/ucp/api/ucp.h`, `src/ucp/rma/amo_*.c`).
- There are two implementations:
  1. **Offload** (`amo_offload.c`) calls `uct_ep_atomic{32,64}_{post,fetch}` or `uct_ep_atomic_cswap*`. Only these transports provide it:
     - `rc_verbs`, `rc_mlx5` and `dc_mlx5`, using Mellanox atomic verbs or DEVX atomic HCA capabilities (`IBV_WR_ATOMIC_FETCH_AND_ADD`, `IBV_WR_ATOMIC_CMP_AND_SWP`)
     - ugni
     - shared memory (`self`, `mm`)

     Sources: `UCX:src/uct/ib/rc/base/rc_iface.c` 195–215, `src/uct/ib/mlx5/rc/rc_mlx5_common.c` 1145–1190, `src/uct/ib/mlx5/dv/ib_mlx5dv_md.c` 2550–2580, `src/uct/ugni/rdma/ugni_rdma_iface.c` **[src]**.

     `rc_verbs` supports only ADD offload; other opcodes return `UCS_ERR_UNSUPPORTED` (`rc_verbs_ep.c` 372–401) **[src]**.
  2. **Software emulation** (`amo_sw.c`) sends an `UCP_AM_ID_ATOMIC_REQ` active message. The target's AM handler then does a CPU-atomic op on the raw address (`ucs_atomic_add64`, `ucs_atomic_cswap64`, and so on) and sends `ATOMIC_REP` or a completion **[src]** (`amo_sw.c` 160–280). This needs target-side progress.
- There are two consistency domains:
  - `UCT_IFACE_FLAG_ATOMIC_CPU`: consistent with CPU operations.
  - `UCT_IFACE_FLAG_ATOMIC_DEVICE`: consistent only with other atomics on the same device **[src]** (`UCX:src/uct/api/uct.h` around line 387).
- `UCX_ATOMIC_MODE=cpu|device|guess` (default `guess`) picks the domain **[src]** (`ucp_context.c` 281–290).
- If a software AMO request arrives while device atomics are selected, UCX logs *"Unsupported"*, and a TODO in the code admits the mixed case is not handled **[src]** (`amo_sw.c` 221–245). Mixing domains is therefore a correctness hazard.

**Counters and GPU-initiated ops (new).** UCX now ships a device API, `UCP_FEATURE_DEVICE`, in `UCX:src/ucp/api/device/`:
- `ucp_device_put(src_mem_list, idx, off, dst_mem_list, idx, off, len, channel_id, flags, req)`
- `ucp_device_counter_inc(inc_value, mem_list, idx, offset, channel_id, …)`, which maps to `uct_device_ep_atomic_add`
- `ucp_device_counter_read` and `ucp_device_counter_write`
- `ucp_device_progress_req`

These are C++ templates meant to run on a GPU, using pre-exchanged "memory descriptor lists" (index-addressed rkeys) and "channels" **[src]** (`ucp_device_impl.h` 175–420, `ucp_host.h`). The DEVX GPU transport is `src/uct/ib/mlx5/gdaki` **[src: directory present]**.

This is the closest thing in UCX to a "put plus increment remote counter" core primitive. It is a good fit for `shmem_put_signal`, but neither OpenSHMEM implementation uses it.

**Tag matching, stream, AM.**
- `UCP_FEATURE_TAG`, `STREAM` and `AM` exist **[src]** (ucp.h 143–171). `ucp_am_send_nbx` provides eager/rendezvous active messages with user-registered handlers **[src]** (`ucp_am.c`).
- **Neither OpenSHMEM implementation requests TAG, STREAM or AM.** Both call `ucp_init` with `UCP_FEATURE_RMA | AMO32 | AMO64` only:
  - `OMPI:oshmem/mca/spml/ucx/spml_ucx_component.c` (spml_ucx_init)
  - `OSSS:src/shmemc/ucx/init.c` 417–420

  **[src]**
- Open MPI's `shmem` two-sided needs (`mca_spml_ucx_send`/`recv`) go through the MPI PML, not UCP AM **[src]** (`spml_ucx.c`, `mca_spml_ucx_recv`/`send`).

**Collective offload.** UCP has none. Collectives live in a separate library, UCC. Open MPI's `oshmem/mca/scoll/ucc` uses UCC, bootstrapped over an MPI-based OOB allgather **[src]** (`OMPI:oshmem/mca/scoll/ucc/scoll_ucc_module.c` 107–160). SHARP and in-network offload come through UCC, not UCX **[recalled]**.

---

## 2. Memory registration and remote keys

**Model.**
- `ucp_mem_map` produces a `ucp_mem_h` that may span several memory domains (MDs): IB HCA(s), shm, cuda, and so on.
- `ucp_rkey_pack` serialises the handle as: `md_map` (a 64-bit `ucp_md_map_t`), then per-MD packed TL rkeys, then sys_dev and distance info **[src]** (`UCX:src/ucp/core/ucp_rkey.c` around line 183, `ucp_rkey_pack_memh`).
- An IB MD's packed TL rkey is `sizeof(uint64_t)` **[src]** (`src/uct/ib/base/ib_md.h:20`).
- A packed rkey for a single IB HCA is therefore a few tens of bytes. It is larger with multiple HCAs or shm and cuda MDs **[src, size estimate derived]**.
- Open MPI aborts if a packed rkey is 0xffff bytes or longer **[src]** (`spml_ucx.c` `mca_spml_ucx_register`, around line 935).
- Unpacked form: a `ucp_rkey_t` holds a cache (flags, mem_type, ep_cfg_index, amo lane/proto, amo_rkey) plus `md_map`, plus one `ucp_tl_rkey_t` per remote MD. Each `ucp_tl_rkey_t` is 32 bytes on LP64 (`uct_rkey_bundle_t` of rkey, handle and type, plus a `cmpt` pointer) **[src]** (`ucp_rkey.h` 32–36 and 114–136, `uct.h` 1744).
- Rkeys with up to `UCX_RKEY_MPOOL_MAX_MD=2` MDs come from a memory pool; larger ones are malloc'd **[src]** (`ucp_context.c`).
- `ucp_ep_rkey_unpack` resolves the rkey against the endpoint's configuration: it does a protocol and lane resolve through a per-worker `rkey_config_hash`, `ucp_rkey_proto_resolve` **[src]** (`ucp_rkey.c` 865–900, 1036).

  **An unpacked rkey is therefore bound to a worker and its endpoint configurations, not just to a key value.**

**One key for the symmetric heap?**
- Yes, per segment. Both implementations register each symmetric segment with one `ucp_mem_map`:
  - Open MPI has `MCA_MEMHEAP_SEG_COUNT 2`: the heap plus the static data/bss segment **[src]** (`OMPI:oshmem/mca/memheap/base/base.h:47`). It optionally adds a device-NIC-memory segment **[src]** (`memheap_base_select.c` around line 274).
  - OSSS-UCX registers the globals segment plus N heaps, `proc.comms.nregions` **[src]** (`OSSS:src/shmemc/ucx/init.c` 145–215).
- **But the key differs per PE.** Each PE registers its own copy, so the receiver holds `n_segments × n_PEs` rkeys **per context** (see §3).

**Mitigations present in UCX and Open MPI:**
1. `UCP_MEM_MAP_SYMMETRIC_RKEY` is a *hint*. On mlx5 DEVX, the mkey index is allocated from a reserved "symmetric-key" range, so that the rkeys received from different peers are likely to be equal **[src]** (`UCX:src/ucp/api/ucp.h` 574–582, `src/uct/ib/mlx5/dv/ib_mlx5dv_md.c` 744–786).

   `ucp_rkey_compare()` then lets a consumer dedupe:
   - Open MPI keeps a sorted, refcounted `rkey_store` per context. Each unpacked rkey is binary-searched with `ucp_rkey_compare`, and on a match the duplicate is destroyed and the stored one reused.
   - This is enabled only when `spml_ucx_symmetric_rkey_max_count > 0` (default 0; the help text says *"typical use 5000"*).
   - It applies only to remote-node PEs.

   **[src]** (`OMPI:spml_ucx.c` 150–290, 405–435; `spml_ucx_component.c` 158–160). The saving is memory only: `ucp_ep_rkey_unpack` still runs once per PE per segment before the compare.
2. `UCX_GVA_ENABLE` (default off) registers the *whole* address space once per MD, as an implicit ODP-style MR: `uct_ib_reg_mr(md, NULL, SIZE_MAX, …)` in `uct_ib_mlx5_devx_mem_reg_gva`. Every `ucp_mem_map` then reuses that single MR, so each process has exactly one rkey per HCA **[src]** (`ucp_mm.c` 484–533; `ib_mlx5dv_md.c` 940–990). Combined with symmetric keys, this is the "one key for everything" end-state for a core API. It is mlx5-only and opt-in.
3. `UCP_MEM_MAP_NONBLOCK` gives on-demand paging registration. OSSS-UCX uses it for every heap (`init.c` 205). Open MPI uses it when `spml_ucx_heap_reg_nb=1`, and calls `ucp_mem_advise(WILLNEED)` from a memuse hook **[src]** (`spml_ucx.c` 830–858).
4. `ucp_rkey_ptr` gives shm-mapped direct pointers for `shmem_ptr` **[src]** (OMPI `mca_spml_ucx_rmkey_ptr`; OSSS `comms.c` around line 430).

---

## 3. Per-peer state and what grows with PE count

**Resource hierarchy.**
- `ucp_context` (per process): MDs, resources, registration cache.
- `ucp_worker`: progress engine. It owns UCT ifaces, meaning QPs, CQs and shm segments, and yields a worker address.
- `ucp_ep`: per (worker, peer). It holds one or more lanes, each a UCT ep: an RC QP, a DC ep that borrows a DCI from a pool, a UD ep, and so on.

Source: UCP API **[src]** (ucp.h). The worker address blob is tens to hundreds of bytes and grows with the number of devices and transports **[recalled]**.

**Transport choice by scale.**
- `ucp_is_scalable_transport(ctx, max_num_eps)` compares an iface's `max_num_eps` with `estimated_num_eps` **[src]** (`ucp_context.h:685`).
- RC sets `RC_MAX_NUM_EPS=256` by default **[src]** (`uct/ib/rc/base/rc_iface.c:111`).
- So once `estimated_num_eps` is above 256, UCP prefers DC or UD for the peer-to-peer lanes.
- Both OpenSHMEM implementations pass `estimated_num_eps = world size` and `estimated_num_ppn` **[src]** (`spml_ucx_component.c`; `OSSS init.c` 425–426).
- FAQ: *"RC transport will be used for small scale, and DC … for large scale. If DC is not available, UD will be used"* **[doc]** (https://openucx.readthedocs.io/en/master/faq.html).

**DC.**
- A DC interface has a pool of DC initiators: `DC_MLX5_NUM_DCI=32`, `TX_POLICY=dcs_quota` by default. Other policies are `dcs`, `rand`, `hw_dcs` (one HW DCS QP, channels assigned round-robin) and `dcs_hybrid`. `QUOTA=32` and `NUM_DCI_CHANNELS=32` **[src]** (`uct/ib/mlx5/dc/dc_mlx5.c` 63–143).
- There is one DC target (DCT) per iface.
- Connection state per peer is therefore O(1) small (address plus a DCI reference), and HW QPs are O(DCIs) per worker instead of O(peers) **[src + recalled for DCT count]**.
- DC handles cross-DCI ordering and fairness in software:
  - A DCI is released when it has no outstanding ops.
  - `dcs_quota` rotates DCIs, with `FC_HARD_REQ_TIMEOUT=5s` **[src]**.
- Open MPI flips `UCX_DC_MLX5_TX_POLICY=rand` via `setenv` for its auxiliary `put_all_nb` context **[src]** (`spml_ucx.c` 1577–1583). Configuring a transport through environment variables at runtime from an upper layer is a sign that the API lacks a per-worker knob.

**UD.** Software reliability with a retransmit timer (`UD_TIMER_TICK=10ms`, backoff 2.0), `UD_TIMEOUT=30s` and `LINGER_TIMEOUT=5m` **[src]** (`uct/ib/ud/base/ud_iface.c` 668–711). This needs a progress or async thread (§5).

**RC.** One QP per (worker, peer), with `RC_TIMEOUT=1.0s` and `RETRY_COUNT=7` **[src]** (`rc_iface.c` 35–56). Per-QP memory on ConnectX is on the order of kilobytes of host memory plus HCA context **[recalled]**. At 10^5 peers per worker per context, that is prohibitive, which is why DC exists.

**What the OpenSHMEM layers allocate per PE — the dominant scaling term:**
- Open MPI `add_procs`: an `allgather` of every worker address (with `default_ctx_ucp_workers` workers each), stored in `remote_addrs_tbl[w][n]`. It then **eagerly calls `ucp_ep_create` for every PE** on the default context **[src]** (`spml_ucx.c` 635–712).
- Open MPI `mca_memheap_modex_recv_all`: one allgatherv of every PE's packed mkeys for all segments, then `unpack_remote_mkeys` → `ucp_ep_rkey_unpack` for every (PE, segment) **[src]** (`memheap_base_mkey.c` 519–690).

  A lazy slow path also exists: `mca_memheap_base_get_cached_mkey_slow` does a per-PE request/response over MPI send/recv (`memheap_oob_get_mkeys`) **[src]** (lines 461–520, 699–725). It only runs if key exchange at init is disabled (`mca_memheap_base_key_exchange`).
- **Every `shmem_ctx_create` repeats the whole O(N) setup** in Open MPI:
  - a new `ucp_worker`
  - a `calloc(nprocs)` peer array
  - `ucp_ep_create` for all N PEs
  - `ucp_ep_rkey_unpack` for all N × n_segments keys

  Source: `mca_spml_ucx_ctx_create_common` **[src]** (`spml_ucx.c` 1030–1105). Destroyed contexts are parked in an idle array for reuse, not freed **[src]** (1158–1172).
- OSSS-UCX does the same per context: `shmemc_ucx_make_eps` calls `ucp_ep_create` and unpacks all regions' rkeys for all N PEs, followed by a `ucp_worker_flush_nbx` "wireup" **[src]** (`OSSS:src/shmemc/ucx/eps.c` 138–239). Worker addresses and packed rkeys are fetched with **one `PMIx_Get` per PE** (keys `r:%lu:%d` and so on) **[src]** (`OSSS:src/shmemc/ucx/pmix_client.c` 62–240). That is N lookups at init, served by PMIx's local datastore **[recalled]**.
- Put/get lookup cost:
  - Open MPI's `mca_spml_ucx_ctx_mkey_by_va` scans the peer's segment array linearly. That is O(segments) = 2 in practice.
  - OSSS-UCX does a region scan plus array index.

  Both are fine **[src]** (`spml_ucx.h:375`; `OSSS comms.c` 36–155).

**Rough per-context memory at N = 10^5** (derived from the struct sizes above and not measured, so treat it as an estimate):
- `ucp_peer_t` is 24 bytes, plus a `malloc`'d `spml_ucx_cached_mkey_t` per segment.
- Unpacked rkeys are about 50–100 bytes each × 2 segments.
- `ucp_ep_t` plus its extension is hundreds of bytes **[recalled]**.

That comes to about 0.5–1 KB per peer per context, so **roughly 50–100 MB per context per PE**, before any RC QPs. For 10^5 PEs × C contexts, this is the scaling wall, not the wire protocol.

**UCX endpoint laziness.**
- `ucp_ep_create` from a worker address is local and non-blocking: lanes are chosen from the address, and wireup messages flow when needed **[recalled]**.
- There is no UCP option that defers `ucp_ep_create` itself, and I found no `UCX_*` "lazy endpoint" variable in `ucp_context.c`'s config table **[src: negative search]**.
- Laziness must be implemented above UCP: create the ep on first access to a PE. Neither OpenSHMEM layer does this.
- Related knobs **[src]** (`ucp_context.c` 352–622):
  - `UCX_NUM_EPS`, `UCX_NUM_PPN`
  - `UCX_UNIFIED_MODE` (assume homogeneous devices, which shrinks addresses)
  - `UCX_CONNECT_ALL_TO_ALL`
  - `UCX_WIREUP_VIA_AM_LANE`
  - `UCX_RESOLVE_REMOTE_EP_ID`
  - `UCX_KEEPALIVE_INTERVAL=20s`
  - `UCX_MAX_PRIORITY_EPS=20`

---

## 4. Ordering and delivery

- UCP makes **no cross-operation ordering guarantee for RMA on an endpoint**. Different sizes can take different lanes and protocols (short, bcopy, zcopy, rndv, multi-rail), and RMA completion does not mean the remote write has landed (ucp.h put_nbx note above) **[src]**.
- `ucp_worker_fence()` *"guarantee[s] that operations issued on an endpoint prior to this call complete before any issued after"*, without waiting for completion. The modes are `UCX_FENCE_MODE=weak|strong|auto|ep_based` **[src]** (ucp.h around 4205–4223; `ucp_context.c` 413–421).
- `ucp_worker_flush_nbx` and `ucp_ep_flush_nbx` wait until remote memory is updated. `UCX_FLUSH_WORKER_EPS=y` flushes per endpoint so the flush is bounded **[src]**.
- **Multi-rail:** `UCX_MAX_RMA_RAILS=1` by default, so RMA uses one device unless configured otherwise **[src]** (`ucp_context.c` 237–243). Rendezvous defaults to 2 rails **[doc]** (FAQ). Splitting a large put across rails means partial writes land in arbitrary order. That is harmless for RMA semantics but matters for "put then signal" (§8).
- DC with `dcs_quota` can move an endpoint to a different DCI. Ordering across DCIs is kept because a DCI is only released once it has no outstanding ops **[src, dc_mlx5.c policy text]**.
- Adaptive routing and relaxed ordering: GVA and some registrations request `IBV_ACCESS_RELAXED_ORDERING` MRs (`uct_ib_mlx5_devx_mem_reg_gva`, `strict_order_mr`) **[src]**. Out-of-order packet placement within one RDMA write is allowed on hardware that supports it **[recalled]**.

The Open MPI layer adds its own ordering machinery (§8.4).

---

## 5. Completion and progress

- **Explicit progress:** `ucp_worker_progress(worker)` drives all ifaces of a worker. `UCX_ADAPTIVE_PROGRESS=y` polls only active ifaces **[src]** (`ucp_context.c` 316).
- Wakeup requires `UCP_FEATURE_WAKEUP`, then `ucp_worker_get_efd`, `ucp_worker_arm` (returns `UCS_ERR_BUSY` if events are pending) and `ucp_worker_wait`. The rule is to drain with progress before arming **[src]** (ucp.h around 2578–2660; `ucp_worker.c` 3421–3440).
- `ucp_worker_wait_mem(worker, addr)` sleeps until a local address changes:
  - On aarch64: `ldaxrb` exclusive-load plus `wfe`.
  - On x86_64: a generic fallback (`ucs_arch_generic_wait_mem`).

  **[src]** (`ucp_worker.c:3416`; `ucs/arch/aarch64/cpu.h:181`; `ucs/arch/x86_64/cpu.h:90`). It is the only UCX hook aimed at `shmem_wait_until`-style polling.
- **Neither OpenSHMEM layer enables `UCP_FEATURE_WAKEUP`** **[src: grep for WAKEUP/efd/arm, no hits in either tree]**. Both busy-poll.
- **Target-side progress is required** whenever an operation falls back to software:
  - AM-emulated put/get (`rma_sw.c`)
  - software atomics (`amo_sw.c`)
  - UD/DC software reliability and flow control, including the DC FC hard-request timeout
  - shm AM paths

  A PE blocked in compute and not calling into the library stalls its peers' operations **[src for handlers; implication derived]**.
- Open MPI works around this in three ways:
  - `opal_progress_register(spml_ucx_default_progress)` and `spml_ucx_ctx_progress` iterate the workers of every *active* context **[src]** (`spml_ucx.c` 667, 1150–1160; `spml_ucx_component.c` 205–216).
  - A progress-every-N-ops heuristic inside non-blocking put and get: `nb_put_progress_thresh=2048` and `nb_get_progress_thresh=4096` ops trigger up to `nb_ucp_worker_progress=32` `ucp_worker_progress` calls **[src]** (`spml_ucx_component.c` 175–189; `spml_ucx.c` `*_wprogress`).
  - An optional async progress thread (`spml_ucx_async_progress`, tick `async_tick_usec=3000`) that progresses only the auxiliary `put_all_nb` context under a spinlock **[src]** (`spml_ucx_component.c` 154–164, 219–247).
- OSSS-UCX calls `ucp_worker_progress` on the default context inside `shmem_test_lock` and `wait_until` loops and inside blocking waits **[src]** (`OSSS:src/shmemc/ucx/test.c` 14–29; `comms.c` 169–190, 379–395).

---

## 6. Thread and context model and hardware cost

- `ucp_worker_create` takes a `thread_mode` of SINGLE, SERIALIZED or MULTI. `ucp_params.mt_workers_shared` protects shared context resources, such as the registration cache, when multiple workers are used concurrently **[src]** (ucp.h; both SHMEM layers set it from the requested thread level).
- **OpenSHMEM ctx ↔ UCP worker, 1:1, in both implementations:**
  - Open MPI `mca_spml_ucx_ctx_create_common` maps `SHMEM_CTX_PRIVATE` or thread SINGLE/FUNNELED → `UCS_THREAD_MODE_SINGLE`, and `SERIALIZED` → `SERIALIZED`. Anything else is `MULTI` **[src]** (`spml_ucx.c` 1030–1060).
  - OSSS-UCX maps serialized → SERIALIZED, private → SINGLE, and otherwise MULTI **[src]** (`OSSS:src/shmemc/ucx/contexts.c` 23–45).
  - Open MPI's default context can own several workers (`default_ctx_ucp_workers`). New contexts connect round-robin to the peers' default workers (`cur_ucp_worker = ucp_worker_cnt++ % ucp_workers`) **[src]** (`spml_ucx.c` 1034, 1068).
- **Hardware cost per worker** **[src defaults; per-object memory recalled]**:
  - its own UCT ifaces: CQ(s), RX rings with `IB_RX_QUEUE_LEN=4096` × `SEG_SIZE=8192` bytes of receive buffers for AM-capable transports (`ib_iface.c` 103–152), and TX `TX_CQ_LEN=4096`
  - DC: 32 DCIs plus 1 DCT per worker
  - UD: one UD QP plus RX descriptors
  - shm: its own mm segment/FIFO

  So a context can easily cost ≥ 30–60 MB of pinned host memory for IB receive rings alone (4096 × 8 KB ≈ 32 MB per iface). That multiplies by contexts and by HCAs **[derived; UCX has some lazy-allocation knobs I did not verify]**.
- **Endpoints per worker:** endpoints are per worker, so N endpoints per context (§3).
- Teams and contexts:
  - Open MPI's `spml_ucx_team_create_ctx` returns `OSHMEM_ERR_NOT_IMPLEMENTED` **[src]** (`spml_ucx.c:1981`).
  - Teams are only (start, stride) arithmetic **[src]** (`spml_ucx.c` split_strided).
  - OSSS-UCX teams keep khash forward and reverse PE maps plus a list of contexts **[src]** (`OSSS:src/shmemc/ucx/pe.h`).

---

## 7. GPU memory

- **UCX:**
  - Memory types include HOST, CUDA, CUDA_MANAGED, ROCM, ZE (oneAPI), and RDMA (NIC on-device memory). Transports: `cuda_copy`, `cuda_ipc`, `gdr_copy`, `rocm_copy`, `rocm_ipc`, `ze`, and Gaudi **[src: `UCX:src/uct/{cuda,rocm,ze,gaudi}`]**.
  - Memory type is detected automatically when GPU transports are enabled.
  - Zero-copy RDMA of GPU memory needs a peer-memory driver or (from 1.14) dmabuf.
  - *"RMA and atomics have partial support only"* compared with tag, stream and AM **[doc]** (UCX FAQ).
  - RMA to GPU memory uses a rendezvous/pipeline path (`RMA_PPLN_ENABLE`, `RNDV_FRAG_MEM_TYPES`) **[src: config names in ucp_context.c]**.
  - Software AMO reply buffers support memory-type unpacking **[src]** (`amo_offload.c` `ucp_amo_memtype_unpack_reply_buffer`).
- **GPU-initiated:** the `UCP_FEATURE_DEVICE` API (§1), `ucp_device_put` and `ucp_device_counter_inc`, can be called from GPU code with DEVX/GDA-KI **[src]**. This is the NVSHMEM-style path, now in upstream UCX.
- **Open MPI oshmem:** the symmetric heap is host only. The `SHMEM_HINT_DEVICE_NIC_MEM` segment uses `UCS_MEMORY_TYPE_RDMA` (on-NIC memory). No CUDA or ROCm symmetric heap is supported **[src]** (`OMPI:oshmem/mca/sshmem/ucx/sshmem_ucx_module.c` 183–210).
- **OSSS-UCX:** host only. `ucp_mem_map` is called with `NONBLOCK|ALLOCATE` and no memory type; a grep for `memory_type` or `cuda` finds nothing **[src]** (`OSSS:src/shmemc/ucx/init.c` 185–215).

---

## 8. What the OpenSHMEM implementations had to emulate in software

This section carries the most weight in the survey. Each subsection states what the implementation needed, what UCX lacked, and what was built instead.

### 8.1 `put_signal` (OpenSHMEM 1.6)
- **Open MPI:** `mca_spml_ucx_put_signal` is `mca_spml_ucx_put` (a blocking put) + `mca_spml_ucx_fence(ctx)` + `MCA_ATOMIC_CALL(set|add)` on the signal address. The non-blocking version does `put_nb` + fence + atomic **[src]** (`OMPI:spml_ucx.c` 1667–1700). That is 2 wire operations plus a worker-wide fence per call.

  Because the signal is an AMO, it may take a different lane or protocol than the put. The fence exists for exactly that reason: to force the counter update to be ordered after the data.
- **OSSS-UCX:** `shmemc_ctx_put_signal` is `shmemc_ctx_put` (blocking) + `shmemc_ctx_fence` + `shmemc_ctx_set|add`. The `_nbi` variant is marked *"TODO HACK1: just use blocking implementation"* **[src]** (`OSSS:src/shmemc/ucx/comms.c` 1003–1066).
- **UCX has the right primitive, but only on the GPU path.** `ucp_device_put` followed by `ucp_device_counter_inc` on the same channel. There is no host-side UCP "put with remote counter increment" and no multi-op batch **[src]**.

### 8.2 `wait_until` / `test` on ivars (1.6)
- **Open MPI:** every `mca_spml_ucx_wait_until_*` aborts with `RUNTIME_SHMEM_NOT_IMPLEMENTED_API_ABORT`. `test_all` returns `OSHMEM_ERR_NOT_IMPLEMENTED`, and `test_any`/`test_some` abort **[src]** (`OMPI:spml_ucx.c` 1705–1800).
- **OSSS-UCX:** a local polling loop. `shmemc_ctx_test_<op>` reads `*var`; if false it calls `shmemc_progress()`, which is `ucp_worker_progress` on the **default** context regardless of the context passed. `wait_until` loops `test → ucp_worker_wait_mem(ch->w, var) → sched_yield()`. The `all`, `any` and `some` variants spin over the array with `yielder()` per element **[src]** (`OSSS:src/shmemc/ucx/waituntil.c` 1–120, `test.c` 14–29, `yielder.h`).

  So `wait_until` is "busy poll plus yield", with an aarch64 WFE hint. There is no network-level trigger.

### 8.3 Locks (`shmem_set_lock` / `clear_lock` / `test_lock`)
- **OSSS-UCX:** a distributed queue lock built from **remote CSWAP plus puts plus busy progress**.
  - The lock word `{int16 locked; int16 next}` sits at a hashed "owner" PE: `(addr>>3) % n_pes`, or PE n−1 for non-global addresses. Requests do `shmem_int_atomic_compare_swap` against the owner in a retry loop.
  - Waiters chain themselves in with `shmem_short_p` to the previous holder, then loop `shmemc_progress()` until their local `node.locked` is cleared by the releaser.
  - `clear_lock` first calls `shmemc_quiet()` (a flush), waits for a chainer, then `shmem_short_p`s the next PE.

  **[src]** (`OSSS:src/api/lock.c` 40–316)
- **Open MPI:** `oshmem/shmem/c/shmem_lock.c` (1057 lines, ticket/turn scheme with `lock_turn`, `lock_inform` and `lock_last_ticket` arrays). It uses `MCA_ATOMIC_CALL(cswap)` + puts/gets, and calls `opal_progress()` *"so that ompi will not deadlock"*. There is also `shmem_mcs_lock.c` (an MCS-style lock) **[src]** (`OMPI:oshmem/shmem/c/shmem_lock.c` 52–300; file list).

### 8.4 Atomics
- **Open MPI `atomic/ucx`:** maps directly to `ucp_atomic_op_nbx`.
  - `set` is implemented as `UCP_ATOMIC_OP_SWAP` without a reply buffer.
  - `cswap` uses a trick: it writes `value` into the reply buffer `prev` and passes `cond` as the operand, because UCP CSWAP takes both values that way.
  - Posted ops are fire-and-forget only if UCX ≥ 1.20 (`mca_atomic_ucx_nb_support`); older versions wait.
  - **All non-blocking fetch atomics (`fadd_nb`, `fand_nb`, `for_nb`, `fxor_nb`, `swap_nb`, `cswap_nb`) return `OSHMEM_ERR_NOT_IMPLEMENTED`.**

  **[src]** (`OMPI:oshmem/mca/atomic/ucx/atomic_ucx_module.c` 33–397, `atomic_ucx_cswap.c`)
- **Open MPI `atomic/basic`** (fallback component): a global lock (`atomic_lock_sync` char array of size `num_pe`, and `atomic_lock_turn`) around `get → compute → put` **[src]** (`OMPI:oshmem/mca/atomic/basic/atomic_basic_module.c` 23–123, `atomic_basic_cswap.c` 37–46). This is the "atomics via put/get with locks" emulation. It is O(N) memory and serialises all atomics job-wide.
- **OSSS-UCX:** with older UCX (no `HAVE_UCP_BITWISE_ATOMICS`), fetch-AND/OR/XOR are emulated as `do { ucp_get(old); new = old op v; ucp_atomic_cswap64(old→new) } while (ret != old)`. That is an optimistic retry loop of 2+ RTTs, livelock-prone under contention **[src]** (`OSSS:src/shmemc/ucx/comms.c` 710–780).

  With newer UCX it calls `ucp_atomic_fetch_nb` and `ucp_atomic_post`. The non-blocking fetch variants for bitwise ops call the blocking helper **[src]** (755–765).
- **Inside UCX:** unless the NIC supports device atomics (Mellanox-specific, ADD/CSWAP on verbs, extended ops only on DEVX with HCA atomic caps), **every AMO is emulated as an active message executed by the target's progress engine** (`amo_sw.c`) **[src]**. On Slingshot, EFA or generic RoCE, atomics therefore require target-side CPU progress.

### 8.5 Ordering / quiet / fence
Open MPI adds `strong_sync` modes and `synchronized_quiet`:
- It tracks, per context, a bitmap and index list of PEs written since the last fence. That is an O(N)-bit `opal_bitmap` plus an int array of N, allocated per context.
- At `shmem_fence` or `shmem_quiet` it then issues, *to each such PE*, one of:
  - a dummy 8-byte get from the peer's service segment (non-blocking or blocking), or
  - a `ucp_ep_flush_nbx`,

  and only then calls `ucp_worker_fence` or `opal_common_ucx_worker_flush` **[src]** (`OMPI:spml_ucx.h` 62–67, 412–426; `spml_ucx.c` 616–633, 1388–1490; params in `spml_ucx_component.c` 166–173).

This exists because UCX RMA completion does not imply remote placement, and plain worker flush was not deemed sufficient across transports.

### 8.6 `put_all_nb` (Open MPI extension used by its collectives)
- It loops over all N PEs: `put_nb` + `fence` + `atomic add(counter)` per PE, on an auxiliary context created with `UCX_DC_MLX5_TX_POLICY=rand` set in the environment. A `ucp_worker_flush_nb` callback decrements a refcount, and a timer-driven progress thread makes progress **[src]** (`OMPI:spml_ucx.c` 1566–1640).
- This is "N puts + N increments of a remote counter", i.e. put-with-completion-counter done by hand.

### 8.7 Collectives and teams
- **Open MPI:**
  - SPML team collectives (`team_alltoall`, `broadcast`, `reduce`, `collect`, `fcollect`, `alltoalls`), `team_sync`, `team_get` and `team_create_ctx` all return `NOT_IMPLEMENTED` **[src]** (`spml_ucx.c` 1950–2027).
  - Collectives come from `scoll` components: `basic` over SPML put/get, `mpi` (maps to MPI collectives over the PML), and `ucc`. UCC is bootstrapped with a hand-written OOB allgather built on MPI requests **[src]** (`OMPI:oshmem/mca/scoll/{basic,mpi,ucc}`).
  - Teams are only strided arithmetic; split_2d composes two of them **[src]**.
- **OSSS-UCX:** collectives come from SHCOLL (Stony Brook), implemented purely over OpenSHMEM puts/gets and puts with signals. For example, alltoall uses `shmem_putmem_nbi` with barrier, counter or `put_signal_nb` variants **[src]** (`OSSS:src/api/collectives/README.md`; `src/shcoll/src/alltoall.c` 45–200). No UCC or xpmem use was found **[src: grep]**.

### 8.8 Bootstrap and key distribution
- Open MPI uses MPI allgather/allgatherv (over its PML) for worker addresses and mkeys, plus MPI send/recv for lazy mkey fetch **[src]** (`spml_ucx.c` 520–600; `memheap_base_mkey.c`).
- OSSS-UCX uses per-PE `PMIx_Put` and `PMIx_Get` **[src]** (`pmix_client.c`).
- UCX provides no bootstrap or address directory service.

### 8.9 Other
- **Symmetric-address translation:** Open MPI translates va → rva per segment using each PE's base (`mkey_segment_init`) **[src]**. OSSS-UCX has an `ENABLE_ALIGNED_ADDRESSES` build option to skip exchanging heap bases **[src]** (`pmix_client.c`).
- **`shmem_ptr`:** `ucp_rkey_ptr`, which works only for shm-reachable peers **[src]**.
- **Memory fence for `shmem_fence` on-node:** OSSS-UCX uses compiler memory barriers (`__sync_synchronize`, `_mm_mfence`) **[src]** (`OSSS:src/shmemc/ucx/memfence.h`). Open MPI uses `opal_atomic_wmb()` **[src]**.

---

## 9. Known pathologies and scaling limits

1. **O(N × segments) rkey unpack per context.** `ucp_ep_rkey_unpack` is not a memcpy: it resolves protocol and lane config (`ucp_rkey_proto_resolve`, hash lookup) **[src]**. Open MPI's symmetric-rkey store only dedupes memory after the unpack, and it is off by default **[src]**. UCX issue #2408 records measurable time in rkey pack/unpack and asks for a way to avoid it **[doc]** (https://github.com/openucx/ucx/issues/2408; full thread not read). There are init-time unpack failure reports with OSHMEM at scale **[doc]** (https://github.com/openucx/ucx/issues/3731; not read in detail).
2. **Eager all-PE `ucp_ep_create` per context** in both implementations. That means O(N) memory and O(N) wireup per `shmem_ctx_create` **[src]**. With RC selected (for example forced by `UCX_TLS`, or when `estimated_num_eps` is wrong), that is O(N) QPs per context. RC's 256-ep threshold exists to avoid this **[src]**.
3. **Receive-buffer cost per worker** (4096 × 8 KB defaults) makes many contexts expensive **[src defaults, derived]**.
4. **UD/DC software timers need progress.** UD `TIMEOUT=30s` and DC `FC_HARD_REQ_TIMEOUT=5s` **[src]**. PEs that do not call progress (long compute, blocking in user code) can cause peer timeouts or `Endpoint timeout` errors. OpenSHMEM programs are especially prone because they often do one-sided work with no receive loop **[derived + recalled user reports]**.
5. **Software AMOs** turn every atomic, lock operation and `put_signal` into an AM round trip that depends on target CPU progress, unless the HCA has device atomics **[src]**. Device atomics are consistent only with other device atomics, which is a correctness trap when shm and network paths are mixed for on-node and off-node peers **[src]**.
6. **Ordering needs explicit fence or flush.** Open MPI's strong-sync issues a dummy get or flush per touched peer at each quiet or fence **[src]**.
7. **Busy polling in `wait_until`, locks and blocking ops,** with no wakeup-fd use **[src]**.
8. **Unimplemented 1.5/1.6 features in Open MPI:** wait_until and test, team_create_ctx, team collectives in SPML, and non-blocking fetch atomics **[src]**.
9. **Empirical scaling papers:**
   - OpenSHMEM-UCX on Cray XK: up to 40% better message rate than the vendor SHMEM **[doc: summary only]** (Baker et al., via https://www.osti.gov/servlets/purl/1265689 and https://www.cs.umd.edu/class/spring2021/cmsc714/readings/Shamis-UCX.pdf).
   - UCX for Tofu-D with OpenSHMEM-UCX on Fugaku **[doc: title/abstract only]** (https://www.researchgate.net/publication/367563945).

   I did not verify quantitative scale limits from these papers.

---

## 10. Build, portability, and maintenance

- **UCX** is large and actively developed: develop branch at 1.24.0, a commit dated today. It has many optional components (ib/mlx5/DEVX, efa/srd, gga, gdaki, cuda, rocm, ze, gaudi, ugni, tcp, sm, xpmem/knem/cma) that must be detected at configure time **[src: tree]**.
- Features come and go under `HAVE_DECL_*` guards. Open MPI's SPML carries three API generations for every call (`ucp_put`, `ucp_put_nb`, `ucp_put_nbx`) plus `#if` for rkey_compare, `SYMMETRIC_RKEY`, `MEMORY_TYPE_RDMA`, bitwise atomics, and runtime version checks (UCX ≥ 1.6 for random DCI, ≥ 1.20 for fire-and-forget AMO) **[src]**. OSSS-UCX has the same pattern (`HAVE_UCP_BITWISE_ATOMICS`, `HAVE_UCP_EP_CLOSE_NBX`) **[src]**.
- Hardware-specific features (DC, device atomics, symmetric rkeys, GVA, DEVX GPU ops) are mlx5-only **[src]**. On Slingshot/CXI, UCX has no native transport in this tree; there is no `uct/cxi` directory **[src: `ls src/uct`]**. So UCX-based OpenSHMEM on HPE Slingshot systems falls back to TCP or is not used **[recalled]**.
- **Open MPI OSHMEM cannot be built without UCX:** configure aborts with *"No spml found"* when `--with-ucx=no` **[doc]** (https://github.com/open-mpi/ompi/issues/12702). It also requires UCX ≥ 1.9 **[doc, same issue]**. There is a standing complaint that single-node OSHMEM should not need UCX **[doc]** (https://github.com/open-mpi/ompi/issues/9003).
- **OSSS-UCX** needs UCX + PMIx + PRRTE (launcher), with SHCOLL vendored. It is maintained by essentially one person (the bug-report contact in `configure.ac`) and `main` was idle since 2023 **[src]**.

---

## Implications for a core API

**Cheap on UCX-class substrates (map straight through):**
- Contiguous put and get to a (PE, segment, offset) with a pre-unpacked key: `ucp_put_nbx`/`get_nbx`, a zero-copy RDMA fast path on RC/DC, shm on-node.
- Vectored or batched RMA with per-element remote address and key (`ucp_dt_make_sgl`), and a worker-scoped fence/flush as the ordering primitive.
- A per-thread communication resource (worker = context) with SINGLE/SERIALIZED/MULTI modes.
- One registration per symmetric segment. On mlx5: a symmetric rkey hint, or GVA for a single whole-address-space key.
- GPU-side put and remote counter increment through the device API, on mlx5 with DEVX.

**Expensive or missing on UCX (the core should define these so they can be emulated centrally, with known cost):**
1. **Key-space scaling.** The core should expose *"one key per segment, possibly identical across PEs"* as a first-class notion:
   - a symmetric-key capability flag,
   - an index-addressed key table (as in `ucp_device_mem_list`),
   - key resolution done lazily and shared across contexts.

   UCX's `ucp_ep_rkey_unpack` binds keys to a worker's endpoint config, which forces O(N × segments × contexts) unpacks in current implementations.
2. **Lazy per-peer state.** The core must allow connect-on-first-use and must not require N endpoints per context. Contexts should share peer addressing and keys and own only a small transmit resource (DCI-like). UCX has no built-in lazy ep; both SHMEMs create N eps per context eagerly.
3. **Put + remote counter or signal as one ordered primitive.** Both implementations emulate `put_signal` as put + fence + AMO, and Open MPI's `put_all_nb` does the same N times. UCX exposes exactly this only in the GPU device API (`ucp_device_put` + `ucp_device_counter_inc` on a channel). This is a strong candidate for a core primitive.
4. **Atomic ops.** They are hardware-offloaded only on specific Mellanox HCAs and verbs; everywhere else they are an AM that needs target progress. A minimal core should offer at most *remote add* and *remote cswap* on 64-bit values, with a declared consistency domain (CPU vs device). Locks and the other fetch/bitwise atomics layer above (OSSS-UCX already does fetch-bitwise as get + cswap retry).
5. **Progress model.** The core must say explicitly whether target-side progress is needed per primitive (true for AMOs and any software RMA path in UCX), and must provide a wait/arm-fd primitive. Neither SHMEM layer used UCX wakeup; both busy-poll for `wait_until` and locks.
6. **Bootstrap and address directory** are outside UCX and should be a separate, lazily queryable core service, with per-PE lookup rather than an allgather of everything.
7. **Collectives** belong above the core. On UCX they come from UCC or from SHMEM-over-RMA libraries (SHCOLL), not from UCP.
