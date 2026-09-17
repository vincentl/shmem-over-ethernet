# Q2 survey: NVSHMEM (NVIDIA)

Scope: NVSHMEM as a design for "OpenSHMEM-like PGAS on high-performance networks, with GPUs starting the communication". The survey focuses on the transports (IBRC proxy, IBGDA, UCX, libfabric, NVLink/P2P, and the new GPUNetIO), how per-peer resources grow at 10^5 PEs, and what NVSHMEM had to build in software or chose not to support.

## Sources and labelling

- **Source tree**: `github.com/NVIDIA/nvshmem`, cloned 2026-09-16, HEAD `b0d9d3dc08fc3ee0840fdb6f3a2c11932d85a2e2` (commit date 2026-08-27). The changelog's latest entry is **3.7.2**. Permalink base below: `https://github.com/NVIDIA/nvshmem/blob/b0d9d3dc08fc3ee0840fdb6f3a2c11932d85a2e2/` (written as `SRC/`).
- **Docs**: docs.nvidia.com/nvshmem (current "latest", 3.7.x era). I fetched these pages through a summarising fetch tool, so I report their wording only as paraphrase or short quotes.
- **Papers and blog posts**: the NVIDIA IBGDA blog (Nov 2022); "Demystifying NVSHMEM" (arXiv 2606.05951); "GPU-Initiated Networking for NCCL" (arXiv 2511.15076).
- **Labels**: *[src]* = seen in the source (file given); *[doc]* = documented (URL given); *[recalled]* = from memory, not checked this session; *[derived]* = my own arithmetic on [src] values.
- **Naming**: NVSHMEM's current code and docs use "CTA" (cooperative thread array), "SM" (streaming multiprocessor) and "warp" for GPU execution scopes. Older docs said "CUDA thread / block / device". I keep the code's names.
- **Not verified**: the "Scaling Graph … with NVSHMEM" paper and the SC/ISC NVSHMEM papers named in the brief. Nothing below depends on them.

---

## 1. Primitives relevant to one-sided PGAS

**RMA (put/get/p/g, blocking and nbi)**
- Callable from host, on-stream, and from GPU kernels. GPU kernels get `_block`/`_warp` scoped variants *[src]* `SRC/src/include/host/nvshmem_api.h`, `nvshmemx_api.h`.
- The docs list host-only APIs (init, memory management, kernel launch, stream ops), GPU-only APIs (block/warp scoped), and APIs available on both sides (RMA, AMO, ordering, point-to-point sync, collectives) *[doc]* https://docs.nvidia.com/nvshmem/api/latest/api/overview.html.

**Atomics (AMO)**
- Full integer set, including swap, cswap, fetch, and bitwise ops *[src]* `nvshmem_api.h` lines ~118–207.
- 3.7 adds non-standard floating-point `nvshmemx_{half,float,double}_atomic_{add,fetch_add}`, supported only over P2P and proxy-backed IBRC *[src]* `nvshmem_api.h:157-167`; `changelog` 3.7.0.
- Unsupported: `long long`/`int64_t`/`ptrdiff_t` for add, fetch_add, inc, fetch_inc *[doc]* overview.html. Atomic floating-point min/max *[doc]* https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/unsupported-operations.html.
- **The NIC does the atomics.** IBGDA writes mlx5 WQE segments for masked fetch-add and masked compare-swap (`ibgda_atomic_{32,64}_masked_{fa,cs}_seg_t`, 16 B each) plus a DUMP opcode *[src]* `SRC/src/include/non_abi/device/pt-to-pt/ibgda_device.cuh:140-190`. These are Mellanox/NVIDIA vendor extensions, not standard IB verbs *[recalled]*. IBRC registers memory with `IBV_ACCESS_REMOTE_ATOMIC` and sets `max_dest_rd_atomic` *[src]* `SRC/src/modules/transport/ibrc/ibrc.cpp:440,538`; `transport_ib_common.cpp:478-511`. The libfabric transport uses `fi_atomicmsg` with `FI_ATOMIC_READ/WRITE` *[src]* `libfabric.cpp:2341-2422`.

**Put-with-signal and signals**
- NVSHMEM had `nvshmem_put*_signal[_nbi]`, `nvshmem_signal_fetch` and `nvshmem_signal_wait_until` as device APIs. It also adds counted signals (`nvshmemx_signal_counted_*`) and `nvshmemx_signal_op` *[src]* `nvshmem_api.h:343-386`; symbol list in `nvshmemx_api.h`.
- The paper reports that from the host slow path, `put_signal` is available only through the on-stream proxy path *[doc]* arXiv 2606.05951 §V-B.

**Point-to-point synchronization**
- `wait_until`, `_all`, `_any`, `_some` and the `_vector` forms are declared `__device__` *[src]* `nvshmem_api.h:386-430`.

**Ordering and completion**
- `nvshmem_fence` orders operations to a destination but does not complete them. `nvshmem_quiet` completes them. There are QP-scoped `nvshmemx_qp_fence/quiet` and a new `nvshmemx_flush` (lets the source buffer be reused without promising remote visibility) *[src]* `changelog` 3.7.0; *[doc]* arXiv 2606.05951 §III-B6; https://docs.nvidia.com/nvshmem/api/latest/gen/mem-model.html.

**Collectives**
- barrier, sync, alltoall, broadcast, fcollect, reduce, reducescatter, each in host, `_on_stream` and device (`_block`/`_warp`) forms *[src]* symbol list; `SRC/src/host/coll/*`, `SRC/src/host/stream/coll/*`.
- Reductions go through a dlopen'd NCCL function table (`nccl_ftable.AllReduce`) *[src]* `SRC/src/host/coll/rdxn/rdxn.h:32`.
- There is no network collective offload (e.g. SHARP) in the transport code I read. NCCL does any offload underneath *[recalled]*.

**Other primitives**
- No tag matching or active messages are exposed.
- IBRC uses internal send/recv on a per-device SRQ for control and send-based atomics *[src]* `ibrc.cpp:291,404-417,1183,1444`.

**Explicit QP handles (new since 3.5)**
- `nvshmemx_qp_create`, `nvshmemx_qp_signal_op`, `nvshmemx_qp_fence/quiet`, and a `qp_index` argument on RMA ops *[src]* `nvshmemx_api.h`; `ibgda_device.cuh:2917`; *[doc]* topic map → https://docs.nvidia.com/nvshmem/api/latest/gen/api/qp.html (gate 3.5).
- In effect this is NVSHMEM's replacement for `shmem_ctx_t` (see §8).

## 2. Memory registration model and remote keys

**Symmetric heap on CUDA VMM**
- `cuMemAddressReserve(p2p_npes * heap_size)` reserves VA for **every NVLink/P2P-reachable peer's heap**, so peer heaps sit at fixed offsets. Physical memory is committed on demand with `cuMemCreate` + `cuMemMap` *[src]* `SRC/src/host/mem/mem_heap.cpp:983,1486-1494`; *[doc]* arXiv 2606.05951 §IV-A/§IV-C.
- Defaults: `NVSHMEM_SYMMETRIC_SIZE` = 1 GiB; `NVSHMEM_MAX_MEMORY_PER_GPU` = 256 GiB; `NVSHMEM_CUMEM_GRANULARITY` = 2^29 (512 MiB) *[src]* `SRC/src/include/bootstrap_host_transport/env_defs_internal.h:37`; `SRC/src/include/host/env/env_defs.h:54,194,237`.
- With DMA-BUF (default unless `NVSHMEM_IB_DISABLE_DMABUF`) or nvidia-peermem, each committed chunk is registered with every selected NIC *[src]* `env_defs.h` (transport) line 61; `heap_registration.cpp:496`.

**Registration granularity: one key per 512 MiB chunk, per NIC, per peer**
- IBGDA builds a GPU-resident rkey table indexed `[chunk_idx * n_pes * num_devs + pe * num_devs + dev]`. Each entry is `nvshmemi_ibgda_device_key_t {__be32 key; uint64_t next_addr;}` (16 B with alignment) *[src]* `SRC/src/modules/transport/ibgda/ibgda.cpp:4704-4740`; `SRC/src/include/device_host_transport/nvshmem_common_ibgda.h:228-232`.
- **Constant memory holds only the first 64 rkeys, 64 lkeys and 128 DCT addresses.** Everything else sits in a GPU "globalmem" array *[src]* `nvshmem_common_ibgda.h:121-123, 260-290`. With the default 2-chunk heap, peers past about 32 fall out of the constant-memory fast path.
- On each new registration the whole rkey vector is resized and the GPU copy is freed and rebuilt (`cudaFree(ibgda_device_rkeys_d)` then re-copy). That is O(chunks × N) work per heap growth *[src]* `ibgda.cpp:4711-4745`.
- Every PE learns every other PE's rkeys through a collective handle exchange. The handle is `NVSHMEM_MEM_HANDLE_SIZE` = 512 B per PE per transport, and IBGDA supports at most 15 devices per PE for that reason *[src]* `SRC/src/include/internal/host_transport/nvshmemi_transport_defines.h:9`; `nvshmem_common_ibgda.h:126`.

**Can one key cover the heap?** No, not by design. Keys are per chunk (VMM granularity) and per NIC. The whole-heap VA is contiguous, so a single MR per NIC over the reserved range would be possible if the NIC supported lazily populated or on-demand-paging registration. NVSHMEM does not do this *[derived from src]*.

**Other transports**
- UCX: `ucp_mem_map` + packed rkey, unpacked per endpoint (`ucp_ep_rkey_unpack`) *[src]* `SRC/src/modules/transport/ucx/ucx.cpp:369,627,954,1299`.
- libfabric: requires `FI_MR_PROV_KEY` (provider-assigned keys, so each peer has distinct keys); adds `FI_MR_ENDPOINT` for CXI and `FI_MR_HMEM` for verbs *[src]* `libfabric.cpp:3487-3498,3575`.

**Rkey table size at scale** *[derived]*: entries = chunks × N × NICs, each 16 B.

| Heap per PE | Chunks | N = 10^4 | N = 10^5 | N = 10^6 |
|---|---|---|---|---|
| 1 GiB (default) | 2 | 305 KiB | 3.05 MiB | 30.5 MiB |
| 64 GiB | 128 | 19.5 MiB | 195 MiB | 1.9 GiB |
| 256 GiB (max) | 512 | 76 MiB | 763 MiB | 7.6 GiB |

These figures are per NIC and held in GPU memory. The host keeps a mirror (`std::vector`) of the same size.

## 3. Per-peer state (the main quantitative section)

All connection setup below is **eager and all-to-all at init**. The only exception is IBGDA's later calls to `connect_endpoints` for user-created QPs, which are also all-peers *[src]* `SRC/src/modules/transport/common/transport_ib_common.cpp:817-930`; `ibgda.cpp:3419-3470,4427-4514`.

### 3.1 IBRC (CPU proxy thread posts verbs)

- **QPs per PE**: `n_pes × (IB_NUM_RC_PER_DEVICE × n_selected_devs + 1)`. The +1 is a dedicated host endpoint. Default `IB_NUM_RC_PER_DEVICE` = 1, so with one NIC that is **2 RC QPs per peer** *[src]* `transport_ib_common.cpp:836-857`; `transport/common/env_defs.h:94`.
- **Queue sizes**: `QP_DEPTH` = 1024 send WRs. Receives use a per-device SRQ of depth `SRQ_DEPTH` = 16384 with a shared recv CQ *[src]* `env_defs.h:121-122`; `ibrc.cpp:404-417`.
- **Connection exchange**: `boot_handle->alltoall` of `nvshmemt_ib_common_ep_handle {u32 qpn; u16 lid; u64 spn; u64 iid;}` (24 B aligned) × QPs-per-peer *[src]* `transport_ib_common.cpp:912-916`; `transport_ib_common.h:99-105`.
  - At N = 10^5 with one NIC, each PE sends 2 × 24 × 10^5 = 4.8 MB, and the job's bootstrap exchange totals ~480 GB *[derived]*.
  - Each PE also performs 2×10^5 `ibv_create_qp` + `modify_qp` calls *[derived]*.
- **Memory** *[recalled for mlx5 per-QP numbers, derived total]*: a verbs RC QP with 1024 send WQEs at 64 B is ≈64 KiB of send queue, plus NIC-side QP context and driver objects. 2×10^5 QPs is therefore ≳12–15 GiB of pinned host and NIC memory per PE, before rkeys. At 10^6 PEs it is an order of magnitude more. **RC-per-peer IBRC does not work at 10^5–10^6 PEs.**

### 3.2 IBGDA (GPU rings the NIC doorbell; QP/CQ buffers in GPU memory)

**Two modes, set by `NVSHMEM_IBGDA_NUM_RC_PER_PE`** *[src]* `transport/common/env_defs.h:158-208`:

| Variable | Default | Meaning |
|---|---|---|
| `IBGDA_NUM_RC_PER_PE` | **2** | RC QPs per remote PE, per NIC. 0 disables RC. If RC is on, "DCI will be used for enforcing consistency only". Raised to **8** automatically when a data-direct NIC is detected *[src]* `ibgda.cpp:5261-5265`. |
| `IBGDA_NUM_DCT` | 2 | DC targets per PE, per NIC |
| `IBGDA_NUM_DCI` | 1 | Total DC initiators. ≤0 means auto: CTA or SM count (`mpc`), `mpc × warp_size` for warp mapping, or `num_DCT × n_pes` for DCT mapping, plus shared DCIs *[src]* `ibgda.cpp:3068-3125` |
| `IBGDA_NUM_SHARED_DCI` | 1 | Shared DCI pool size |
| `IBGDA_DCI_MAP_BY` / `RC_MAP_BY` | cta / none | How QPs are assigned to GPU execution scopes |
| `IBGDA_NUM_FETCH_SLOTS_PER_{DCI,RC}` | 1024 | Internal get buffer slots per QP |
| `IBGDA_NUM_REQUESTS_IN_BATCH` | 32 | WQEs batched before a doorbell |
| `QP_DEPTH` | 1024 | WQEs per QP (min 128, max 32768 in IBGDA) |

- The docs say RC gives the lowest latency. They say DCI saves memory but "each time the DCI switches to a different DCT, the connection must be renegotiated", and that a dedicated QP per remote PE "can be prohibitive for large-scale applications" *[doc]* https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/performance.html.
  - **The default is RC mode (2 per peer).** Note that the performance page's table, as summarised, lists DCI as the default; the code default is 2 RC QPs per peer.
- RC creation loops over all N peers except self: RC QP count = `IBGDA_NUM_RC_PER_PE × N × NICs`. Each RC QP gets its own CQ (`num_cq_handles += dci.num_eps + rc.num_eps_per_pe × n_pes`) *[src]* `ibgda.cpp:3419-3470,3692`.

**Per-RC-QP memory** *[src constants; derived sums]* (`ibgda.cpp:57,78,1438-1439,2186-2187,2280-2285`; `nvshmem_common_ibgda.h:118-120,203,154`). Buffers default to GPU memory (`IBGDA_FORCE_NIC_BUF_MEMTYPE=gpumem`):
- WQ: `QP_DEPTH` (1024) × `MLX5_SEND_WQE_BB` (64 B *[recalled]*) = 65,536 B
- CQ: 1024 × 64 B (`NVSHMEMI_IBGDA_CQE_SIZE`) = 65,536 B
- Internal buffer ("ibuf"): `IBUF_SLOT_SIZE` (256) × (1024 + 1) = 262,400 B, allocated per endpoint *[src]* `ibgda_rc_create_internal_buffer(ep…)`
- GPU descriptors: `nvshmemi_ibgda_device_qp_t` 184 B + `cq` 72 B
- Doorbell record: a few bytes; NIC-side QPC/CQC in firmware is not quantified
- **Total ≈ 394 KiB per RC QP.** With `QP_DEPTH`=128 and fetch slots at their minimum (the warp size), it drops to ≈17–20 KiB.

**IBGDA RC mode, default settings, one NIC** *[derived]*:

| N | RC QPs per PE | GPU memory per PE | Tuned-minimum (~18 KiB/QP) |
|---|---|---|---|
| 10^3 | 2,000 | 0.75 GiB | 34 MiB |
| 10^4 | 20,000 | 7.5 GiB | 343 MiB |
| 10^5 | 200,000 | **75 GiB** (exceeds most GPUs) | 3.4 GiB |
| 10^6 | 2,000,000 | 750 GiB | 34 GiB |

On top of this: NIC firmware QP contexts, and the bootstrap all-to-all of `ibgda_rc_handle` (24 B × 2 × N² in total).

**IBGDA DC mode** (`NUM_RC_PER_PE=0`):
- QPs per PE per NIC are **fixed**: `NUM_DCI` (1, or #CTAs/SMs + 1 on auto) + `NUM_DCT` (2). Each DCI carries a WQ, CQ and ibuf of ≈394 KiB by default.
- What grows with N:
  - the DCT address table: `NUM_DCT × N` × `mlx5_wqe_av` (48 B *[recalled]*; IBGDA can use a 16 B "half AV" segment in the WQE *[src]* `ibgda_device.cuh:147-155`). That is **9.2 MiB at 10^5 and 92 MiB at 10^6** *[derived]*;
  - the rkey table in §2 (3 MiB to 763 MiB at 10^5, depending on heap);
  - the host-side handle arrays (`dct_handles` = `num_eps × n_pes`) *[src]* `ibgda.cpp:2924,3010`.
- **DC is the only IBGDA mode that fits 10^5+ PEs.** The cost is reconnect latency when a DCI changes target DCT, and serialization through few DCIs. The DCT map-by mode (`num_DCT × N` DCIs) removes renegotiation but brings back O(N) QPs *[doc]* performance page; *[src]* `ibgda.cpp:3101-3103`.

**Other per-PE limits**: `NVSHMEMI_IBGDA_MAX_DEVICES_PER_PE` = 15 *[src]* `nvshmem_common_ibgda.h:126`. `nvshmemi_ibgda_device_state_t` is fixed at 8384 B in GPU constant memory *[src]* `nvshmem_common_ibgda.h` static_assert.

### 3.3 UCX

- Eager `ucp_ep_create` for all `n_pes × ep_count` endpoints; addresses exchanged via bootstrap *[src]* `ucx.cpp:486-530`.
- What that costs depends on the UCX transport. With `UCX_TLS` including dc_mlx5 it is a UCP ep object per peer, roughly 100s of B to a few KB *[recalled]*. With rc it is RC QPs, as in 3.1.
- rkeys are unpacked per (ep, registration) *[src]*.
- GPU kernels reach UCX only through the CPU proxy.

### 3.4 libfabric (Slingshot/CXI default, also EFA and verbs)

- `NVSHMEM_LIBFABRIC_PROVIDER` defaults to `cxi`. Up to 16 NICs per PE *[src]* `transport/common/env_defs.h:143-146`.
- RDM endpoints, `FI_AV_TABLE`, endpoint names allgathered, then **all N × eps-per-PE names inserted into an AV per domain** *[src]* `libfabric.cpp:3041,3198,3300-3320`.
  - So per-peer state is one AV entry, typically ~16–32 B in the provider plus fi_addr *[recalled]*, and no connections. At 10^6 PEs that is tens of MB per domain *[derived, approximate]*.
- The data path uses `FI_DELIVERY_COMPLETE`, `FI_PROGRESS_AUTO` + `FI_THREAD_COMPLETION`, falling back to `FI_PROGRESS_MANUAL` + `FI_THREAD_SAFE` *[src]* `libfabric.cpp:3506-3529`.
- The hints request an `FI_FENCE` capability for one provider path *[src]* `libfabric.cpp:3494`. That flag is not in upstream libfabric as far as I know *[recalled]*. It points to a CXI-specific ordering extension; unverified.
- Proxy batching: `LIBFABRIC_PROXY_REQUEST_BATCH_MAX` = 32. Signal waits spin 1024 iterations before sleeping *[src]* `env_defs.h:148-156`.

### 3.5 NVLink/P2P intranode

- Every P2P-reachable peer's heap is mapped into this process's VA (§2). Puts and gets are load/store operations run directly by the GPU kernel.
- VA reservation = `p2p_npes × heap_size`: 72 NVL GPUs × 256 GiB = 18 TiB of VA, which is fine on 48-bit GPU VA *[derived]*.
- 3.7 adds TMA-backed NVLink put/get and shared-memory registration (`nvshmemx_{ask,give,release}_smem`) *[src]* `changelog`.

### 3.6 GPUNetIO (new in 3.7)

- A DOCA-based GDAKI transport with `GPUNETIO_NUM_RC_PER_PE_{GPU,CPU}` = 2 each, i.e. the same RC-per-peer scaling as IBGDA RC mode *[src]* `env_defs.h:215-243`; `changelog` 3.7.0.

### 3.7 Proxy channel wire format limits PE numbers

- The GPU→CPU proxy ring is a host-pinned buffer of `CHANNEL_BUF_SIZE` = 2^22 B (4 MiB). Requests are packed 8-byte words *[src]* `SRC/src/include/device_host_transport/nvshmem_constants.h:18-19`; `SRC/src/include/device_host/nvshmem_proxy_channel.h`.
- Several request types carry **`uint16_t pe`** (QP sync, put_dma_request_2, put_inline_request_0, and others at lines 55, 88, 99, 132, 219). That implies **at most 65,536 PEs addressable on the proxy (IBRC, UCX, libfabric) path** *[src; implication derived]*.
- I found no guard against `n_pes > 65535` in `src/host/proxy`, `src/host/init` or `device_host` (grep only; not exhaustive).
- Base requests use a 40-bit remote offset (`roffset_high:32 + roffset_low:8`), so remote heap offsets above 1 TiB would overflow on that path *[src; derived]*.

## 4. Ordering and delivery guarantees

**Semantics**
- NVSHMEM deliberately relaxes OpenSHMEM ordering. Blocking read operations are expected to see prior operations in order. For non-blocking and device-initiated operations, programs must call `nvshmem_fence` (ordering) or `nvshmem_quiet` (completion) *[doc]* https://docs.nvidia.com/nvshmem/api/latest/gen/mem-model.html.
- The doc gives an example where a sequence of fetch operations need not observe earlier writes. On mixed NVLink+IB systems, synchronization operations "only guarantee visibility of updates to the local PE's symmetric objects"; on NVLink-only systems visibility is global *[doc]* mem-model.html.
- **Atomicity** holds only for concurrent AMOs with the same datatype on the same location. It does not hold across mixed datatypes, mixed AMO and plain RMA, or against non-NVSHMEM access *[doc]* mem-model.html.

**Mechanism in IBGDA: the consistency op ("CST")**
- Each QP tracks `get_head` (last fetch-type WQE) and `get_tail`. For a blocking get, or at quiet, the kernel posts an mlx5 **DUMP** WQE (the NIC reads 1 B of local GPU memory) on a DCI and waits for its completion. That gives a point where earlier work on that path has been processed. The comment reads "For CST, it is cheaper than RDMA READ" *[src]* `ibgda_device.cuh:2124-2195`.
- RC has no loopback QP, so CST always borrows a DCI to self: "We don't have RC loopback to self. So, we grab a DCI for CST" *[src]* `ibgda_device.cuh:2178-2186, 3796-3820`. This is why DCIs exist even in RC mode (the env text "DCI used for enforcing consistency only").
- CST can be skipped (`may_skip_cst`) only in single-device configurations. "Multiple devices break our CST optimizations" *[src]* `ibgda.cpp:4491-4494`.
- **Multiple QPs per peer** (2 RC by default, round-robin with `RC_MAP_BY=none`) mean operations to the same peer can complete out of issue order *[src; derived]*.
  - AMOs defend against this with **address-stable QP selection**: SplitMix64 over (pe, heap offset) picks the QP/NIC, so every AMO on the same word takes the same QP *[src]* `ibgda_device.cuh:1916-1960`. `NVSHMEM_IB_ATOMIC_POLICY` = AUTO|SINGLE|MULTI controls this *[src]* `transport/common/env_defs.h:84-87`.
- Proxy-path ordering needs `__threadfence_system()` so that GPU stores to the issue counter are not reordered before loads of the ack counter *[src]* `proxy_device.cuh` ~line 88; `ibgda_device.cuh:3813-3818`.

**Wire-level assumptions**
- RC and DC are reliable, in-order transports per QP *[recalled]*. IB adaptive routing with RC relies on NIC out-of-order packet placement, not on NVSHMEM *[recalled]*.
- PCIe relaxed ordering is on by default (`IB_ENABLE_RELAXED_ORDERING=true`). That is TLP ordering for DMA, not message ordering *[src]* `env_defs.h` ~line 91.
- QP ack timeout 20 and retry count 7 are exposed *[src]* `env_defs.h:67-70`.
- libfabric uses `FI_DELIVERY_COMPLETE` *[src]*. I saw no `FI_ORDER_*` request in the hints lines I read (not exhaustively verified).

## 5. Completion and progress model

**IBGDA**
- The GPU kernel polls the CQ, which lives in GPU memory (`ibgda_poll_cq`; loops at `ibgda_device.cuh:545-592`). There are no interrupts.
- Doorbells are rung by GPU SMs (`IBGDA_NIC_HANDLER=gpu`) or, as a fallback, by a CPU with a GDRCopy mapping (`cpu`, `cpu_host_memory`); `auto` tries GPU first *[src]* `env_defs.h:202-208`.
- `NUM_REQUESTS_IN_BATCH`=32 coalesces WQEs before a doorbell.

**IBRC, UCX, libfabric: CPU proxy thread**
- GPU kernels reserve bytes in the pinned ring with an atomic add (`nvshmemi_proxy_channel_reserve`) and write 8-byte request words. `pthread_create(nvshmemi_proxy_progress)` polls the channels and executes PUT, GET, G, P, AMO, FENCE, QUIET_QP, PUT_SIGNAL and REGION_END *[src]* `SRC/src/host/proxy/proxy.cpp:415,489,1415-1520`; `proxy_device.cuh:66-110`.
- When the ring is full, the GPU spins until `proxy_channels_complete` advances *[src]* `proxy_device.cuh` `check_channel_availability`.
- IBRC also needs target-side receive progress for send-based atomics (`progress_recv`, SRQ) *[src]* `ibrc.cpp:1285-1371`. Without NIC-native atomics, **target CPUs must make progress**.

**A proxy thread exists even with no network**: on NVLink-only systems it handles device-side global exit and timeout polling. `NVSHMEM_DISABLE_LOCAL_ONLY_PROXY` turns it off *[src]* `env_defs.h:124-129`; `proxy.cpp:415` (minimal progress thread).

## 6. Thread and context model; hardware cost

- There are **no OpenSHMEM contexts**. `nvshmemi_team_create_ctx` / `nvshmemi_ctx_get_team` are commented out *[src]* `SRC/src/include/internal/host/nvshmemi_team.h:50-52`. No `ctx` symbol appears in the public headers (grep).
- What NVSHMEM uses instead:
  - **GPU execution scopes** (thread, CTA/"block", SM, "warp") pick QPs through `DCI_MAP_BY`/`RC_MAP_BY`. Docs characterise the CTA mapping as "Low Resource, High Synchronization Overhead" and warp as "High Resource, Low Synchronization Overhead" *[doc]* performance page.
  - **Explicit QP handles** (`nvshmemx_qp_create`, 3.5+). Each handle creates N more QPs per NIC through `connect_endpoints` (`total_eps = n_pes * num_qps`) *[src]* `transport_ib_common.cpp:858-874`; `ibgda.cpp:4435-4437`. **A context costs O(N) QPs on RC transports**, while with DC it is O(1) DCIs *[derived]*.
- Concurrency inside a QP from thousands of GPU threads is handled without locks:
  - WQE slots are reserved by CAS on `resv_head`/`ready_head`, and ibuf slots by CAS on `ibuf.tail` *[src]* `ibgda_device.cuh:1659-1666,2066-2092`;
  - a CAS **spinlock** (`ibgda_lock_acquire`: `while (atomicCAS(lock,0,1)==1);`) guards the doorbell post (`post_send_lock`) *[src]* `ibgda_device.cuh:416-418,1567-1584`.
- Host threading: `nvshmem_init_thread` / `query_thread`. The libfabric path uses `FI_THREAD_COMPLETION` with host-EP submissions separated from proxy-thread submissions *[src]* `libfabric.cpp:360,1263`.

## 7. GPU memory support

- GPU memory is the native case: the heap is GPU VMM memory, registered with the NIC through DMA-BUF or peermem.
- The **device cannot target host memory** for device-initiated communication. Local host buffers must be registered with `nvshmemx_buffer_register` *[doc]* unsupported-operations page.
- IBGDA places WQ, CQ and doorbell-record buffers in GPU memory for SM access via GPUDirect RDMA *[doc]* IBGDA blog.
- Measured gains *[doc]* https://developer.nvidia.com/blog/improving-network-performance-of-hpc-systems-using-nvidia-magnum-io-nvshmem-and-gpudirect-async/ (Nov 2022, NVSHMEM 2.6–2.8, 4× DGX-A100, ConnectX-6 200 Gb/s):
  - up to **9.5×** block-put throughput under 1 KiB;
  - **~180 MOPS vs ~1.7 MOPS** put rate for IBGDA vs IBRC, against a ~215 MOPS NIC limit;
  - all-to-all latency ~64 µs vs 128–256 µs.
- GPU-kernel constraints *[src]*:
  - **no blocking**: `wait_until`, quiet and ring-full waits are busy spin loops, optionally with a timeout (`TIMEOUT_NCYCLES` 1e10) read from a global timer *[src]* `SRC/src/include/non_abi/device/wait/nvshmemi_wait_until_apis.cuh`;
  - **no mutexes**, only CAS spinlocks;
  - **no allocation, init or team creation** on the device; those are host-only and collective *[doc]* overview.html; arXiv 2606.05951 §III-B2;
  - no public grid-scoped (multi-CTA) collectives, because synchronising across CTAs inside a kernel "is more expensive" *[doc]* arXiv 2606.05951 §VI-D.

## 8. What NVSHMEM had to build in software, or dropped (most important)

### 8a. Not implemented, or implemented only partly

| OpenSHMEM feature | NVSHMEM status | Evidence / reason |
|---|---|---|
| `shmem_lock` family | Unsupported | *[doc]* overview.html list. No lock symbols *[src]* grep. A distributed lock would need blocking and waiting on the GPU. |
| Contexts (`shmem_ctx_*`, `team_create_ctx`) | Absent; replaced by execution scopes and QP handles | *[src]* `nvshmemi_team.h:50-52` commented out. On RC transports a context costs O(N) QPs (§6). |
| `shmem_realloc`, `pe_accessible`, `addr_accessible`, `collect`, `alltoalls`, Fortran | Unsupported | *[doc]* overview.html |
| `shmem_iput`/`iget` (strided) | Not over remote transports | *[doc]* overview.html; paper: "Remote strided RMA and host-side `g` operations are unsupported" *[doc]* arXiv 2606.05951 §V-B |
| 64-bit add/inc AMOs; float min/max AMOs | Unsupported | *[doc]* overview.html; unsupported-operations page (emulate with reduce) |
| 1.5 `malloc_with_hints`; 1.6 `ibput`/`ibget`, `sum_inscan`/`exscan`, sessions, `signal_add`/`set`, `team_ptr`, `pe_quiet` | No matching symbols in public headers | *[src]* grep of `src/include/host`, `src/include/device` (name-based; may miss macro-generated names) |
| Device-side host-memory targets | Unsupported | *[doc]* unsupported-operations page |
| Strict OpenSHMEM ordering/visibility | Relaxed; user must fence or quiet | *[doc]* mem-model.html |

### 8b. Emulated or layered in software above the network

1. **Ordering and consistency points (CST)**: a DUMP WQE posted on a DCI loopback plus `get_head`/`get_tail` bookkeeping per QP (§4). It exists because (a) there are several QPs per peer, (b) completions are polled lazily by the GPU, and (c) the GPU must know a blocking `get`'s data has landed. It costs an extra WQE, a doorbell and a CQ poll per blocking get or quiet, and is disabled on multi-NIC setups where skipping would be unsafe *[src]* `ibgda_device.cuh:2124-2240,3796`; `ibgda.cpp:4491`.
2. **Blocking scalar get from GPU**: RDMA READ into a per-QP internal buffer (256 B slots × 1024), then a copy. The paper measures scalar `g` capped below 9 GB/s because each load needs its value before the next can proceed *[src]* `ibgda.cpp:2280-2285`; *[doc]* arXiv 2606.05951 §VII-B.
3. **GPU→CPU proxy for every network op on non-GDA transports**: a lock-free ring of 8-byte request words plus a host progress thread (§5). This is the IBRC "put_signal-like and blocking ops" path. The blog measured it at ~1.7 MOPS *[src]* `proxy.cpp`, `proxy_device.cuh`; *[doc]* blog.
4. **Atomics without NIC-native support**: IBRC falls back to **send-based atomics**. The target's progress thread receives the op on an SRQ and applies it with a CPU mapping of GPU memory (GDRCopy or DMA-BUF mmap); counters `atomics_received/processed/acked` track this *[src]* `ibrc.cpp:165-175,697-706,1285-1371`. IBGDA avoids it only through vendor mlx5 masked atomic WQE segments (§1). Keeping the same word on the same QP across NICs also needed software address hashing (§4).
5. **Signals and `wait_until`**: GPU busy-polls a symmetric word. On libfabric, signal delivery is a spin-then-sleep loop on the CPU *[src]* `nvshmemi_wait_until_apis.cuh`; `env_defs.h:153-156`. The proxy thread handles timeouts for `WAIT_UNTIL_{GE,EQ,NE}` and barrier call sites *[src]* `proxy.cpp` `progress_timeout_polling`.
6. **Collectives**: on-stream and host paths through NCCL (`nccl_ftable.AllReduce`). Device collectives are built from RMA and proxy calls, and barrier forces quiet or `__threadfence_system` *[src]* `SRC/src/host/coll/rdxn/rdxn.h:32`; *[doc]* arXiv 2606.05951 §VI-E. The paper finds inter-node collectives weak (<0.20 GB/s in its test) because optimisation has focused on intranode NVLS *[doc]* §VII-C.
7. **Symmetric addressing**: VA reserved for all P2P peers, chunked registration, and per-peer rkey tables rebuilt on growth (§2). Remote address = `peer_heap_base[pe] + (addr − heap_base)` *[doc]* arXiv 2606.05951 §IV-C.
8. **Team-based bootstrap**: 3.7 added teams-based OpenSHMEM bootstrap with a fallback to active-set collectives. Bootstrap plugins are PMI (default), PMIx, MPI, UID and SHMEM *[src]* `changelog` 3.7.0; `SRC/src/modules/bootstrap/{mpi,uid,pmi,pmix,shmem}`; `SRC/src/include/host/env/env_defs.h:86`.

## 9. Known performance pathologies and scaling limits

- **RC-per-peer memory** (IBRC, IBGDA default, GPUNetIO): O(N) QPs. About 75 GiB of GPU memory at 10^5 PEs with IBGDA defaults (§3.2) *[derived]*. NVIDIA's own docs call per-peer QPs potentially "prohibitive" at large scale *[doc]*.
- **DC renegotiation** when DCIs are shared across many targets *[doc]* performance page. The auto DCI count grows with GPU scope count, and the DCT map-by mode brings back O(N) QPs *[src]*.
- **Eager all-to-all connection setup** through the bootstrap alltoall: O(N²) total bytes and O(N) QP creations per PE at init (§3.1) *[src; derived]*.
- **Rkey tables** O(chunks × N × NICs), fully rebuilt on each registration, and outside constant memory beyond 64 entries (§2) *[src]*.
- **16-bit PE fields and 40-bit offsets** in the proxy channel format (§3.7) *[src]*.
- **Proxy throughput ceiling**: ~1.7 MOPS vs ~180 MOPS for IBGDA *[doc]* blog.
- **Scalar gets and CST**: each blocking get needs a consistency point; scalar `g` below 9 GB/s *[doc]* paper.
- **Busy-wait GPU kernels**: `wait_until` and quiet burn SM cycles. Quiet and fence latency grows with resource contention *[doc]* performance page; *[src]* wait APIs.
- **Multi-NIC** disables CST skipping and needs address-hash AMO routing *[src]*.
- **Tuning evidence**: the NCCL GIN paper reports DeepEP needing 8–24 QPs per kernel set and GIN spreading them across communicators, 4 contexts each *[doc]* arXiv 2511.15076. The NVSHMEM analysis paper tunes `IBGDA_NUM_RC_PER_PE=64` to expose concurrency *[doc]* arXiv 2606.05951 §VII-A. At that setting the RC scaling above gets 32× worse *[derived]*.

## 10. Build, portability and maintenance burden

- **Size of the transport code** (lines, `wc -l`) *[src]*:
  - IBGDA host side 5,348 + device kernels 3,823;
  - `mlx5_ifc.h` 5,542 (raw DevX PRM layouts);
  - libfabric 3,831 + 959;
  - GPUNetIO 3,141 + GDAKI device 2,174;
  - IBDEVX 2,259; IBRC 2,107; UCX 1,595;
  - common IB 1,847.
- IBGDA writes mlx5 WQEs and doorbells directly (DevX, PRM structures). It is **ConnectX/BlueField-only**, needs DMA-BUF or peermem, and depends on vendor atomic opcodes *[src]*. GPUNetIO (3.7) moves this onto DOCA *[src]* `changelog`.
- **GPU ABI stability**: `static_assert`s pin device-visible struct sizes (qp 184 B, cq 72 B, state 8384 B, request words 8 B) and carry version fields *[src]* `nvshmem_common_ibgda.h`, `nvshmem_proxy_channel.h`. Device code is shipped as LTO-IR/bitcode per architecture *[src]* `changelog` 3.7.0.
- libfabric is tuned for CXI, with a possibly nonstandard `FI_FENCE` capability (§3.4). EFA and verbs are secondary.
- Compilers: GNU/nvcc or Clang only *[doc]* unsupported-operations page.

---

## Implications for a core API

**Made cheap by this substrate**
- **One-sided RDMA write/read into a pre-registered heap with a (chunk, peer, NIC) → key lookup**, posted with no locks by many concurrent initiators. Reservation is CAS-based (`resv_head`/`ready_head`), one doorbell covers a batch, and completion is polled. A core `put`/`get` primitive should take the key lookup out of the call path (a precomputed per-peer handle) and allow batched submission with a single "flush/doorbell".
- **NIC-native atomics** (masked fetch-add, compare-swap) on NVIDIA NICs. A core AMO primitive maps onto them at zero software cost *there only*.
- **Connectionless reliable initiators (DC, or RDM on CXI/EFA)** keep per-peer state at one address entry (16–48 B) plus rkeys. That is the only model shown here to reach 10^5–10^6 PEs.

**Made expensive, or pushed into software**
- **Per-peer connected QPs**: about 394 KiB per RC QP with IBGDA defaults; ×2 RC per peer, ×NICs, ×user QP handles. The core must not assume per-peer connections, and **contexts must not cost O(N) hardware**. NVSHMEM's QP-handle API does exactly that on RC.
- **Ordering and visibility**: several QPs per peer plus lazy polling force a software consistency mechanism (DUMP-WQE CST, fence/quiet). A core should expose an explicit, cheap "ordering point / completion barrier per (context, peer)" and not promise implicit OpenSHMEM ordering on the fast path. OpenSHMEM 1.6 already narrows fence to per-context ordering (Annex G.1 item 18).
- **Blocking gets and scalar reads** need a round trip plus a consistency point. Returning values synchronously from a GPU kernel is the slow path.
- **Atomics without NIC support** need target-side CPU progress (send-based AMO): a hidden progress-thread requirement. The core should mark AMO as "native if available, else AM-emulated with target progress".
- **Locks, `wait_until`, signals and collectives on accelerators** turn into spin-polling or proxy/NCCL. Keep them above the core.
- **Key distribution**: per-chunk × per-peer × per-NIC rkeys give O(N) tables that are rebuilt on growth. A core should favour **one key (or a few) per heap per NIC**, e.g. a whole-VA registration with lazy population, or provider/global keys such as CXI's, so key state does not grow with N × chunks.
- **Wire and handle formats** must not bake in 16-bit PE numbers or 40-bit offsets, as NVSHMEM's proxy channel does.
- **Connection setup** must be lazy or connectionless. An eager bootstrap all-to-all of QP handles is O(N²).
