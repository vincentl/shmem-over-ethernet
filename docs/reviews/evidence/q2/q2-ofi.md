# Q2 survey: libfabric/OFI (cxi, verbs+rxm, efa, opx) and Sandia OpenSHMEM's OFI transport

## Sources and labels

Sources were read from shallow clones made on 2026-09-16:

- **libfabric** `main` @ `0d5527ae694af929014da0385d409f638e81105e` (`configure.ac` says `2.7.0rc1`; the newest tags are v2.6.0 and v2.7.0rc2). Links below use the prefix `LF/` = `https://github.com/ofiwg/libfabric/blob/0d5527ae694af929014da0385d409f638e81105e/`.
- **Sandia OpenSHMEM (SOS)** `main` @ `4aa91d7fa9041785bd367fc9cefab99d313dd9df` (2025-08-27; newest tag v1.5.3; `mpp/shmem-def.h.in` says `SHMEM_MINOR_VERSION 5`). Prefix `SOS/` = `https://github.com/Sandia-OpenSHMEM/SOS/blob/4aa91d7fa9041785bd367fc9cefab99d313dd9df/`.
- **rdma-core** `master` @ `00357df3a344b3515a9a4d345cf0b8c8efb7f116`. Prefix `RC/` = `https://github.com/linux-rdma/rdma-core/blob/00357df3a344b3515a9a4d345cf0b8c8efb7f116/`.

Every claim carries one of these labels:

- **[D]**: documented. A man page or doc in the tree, or a web page. The URL is given.
- **[S]**: observed in source. File and line are given.
- **[R]**: recalled from memory and not verified here.
- **[I]**: my inference from [D] or [S] evidence.

The libfabric man pages come from `main` and may describe behaviour newer than the 2.6.0 release.

---

## 0. Executive summary for the core-API question

1. **SOS needs only a small set of libfabric features.** It requests `FI_RMA | FI_ATOMIC` on `FI_EP_RDM`, plus `FI_RMA_EVENT` for target counters, `FI_DELIVERY_COMPLETE` on puts, and `inject_size >= sizeof(long double)` (16 B) (SOS/src/transport_ofi.c#L1464-L1545 [S]). Its whole completion model uses `fi_cntr` counters, not CQ entries (put and get counters per context, and one target counter). The CQ is used only to recycle bounce buffers (SOS/src/transport_ofi.c#L599-L640; SOS/src/transport_ofi.h#L420-L452, #L484-L540 [S]).
2. **Native vs emulated atomics by provider:**
   - **cxi:** native. One element per operation, NIC-executed (Cassini AMO).
   - **efa:** emulated as two-sided messages that the target CPU applies. `efa-direct` has no atomics at all.
   - **verbs;ofi_rxm:** emulated in rxm on the target CPU.
   - **raw verbs MSG endpoints:** now expose single-element 64-bit `ATOMIC_WRITE`, fetch-`SUM`/`READ` and `CSWAP` through IBTA atomic opcodes. rdma-core implements these only in the `rxe` and `ionic` user providers; I did not see mlx5 support. rxm does not use them.
   - **opx:** RMA and atomics are software-onloaded on the HFI.
3. **What SOS emulates in software above OFI:**
   - all collectives;
   - locks, built from cswap, mswap, fetch and wait;
   - `shmem_wait_until` and signal-wait: CPU polling, or a blocking wait on a target counter;
   - put-with-signal: put, then a fence, then an atomic write/sum;
   - `ibput`/`ibget`: loops over puts;
   - teams;
   - fetch-atomic on CXI, rewritten as fetch-SUM(0);
   - bounce-buffered puts;
   - STX sharing policy;
   - context locking;
   - nonblocking atomics, by counting outstanding operations;
   - reductions on types the provider does not support, via software fallback.

   SOS **does not** use `fi_collective`, `fi_trigger` or deferred work queues (grep of `src/` finds no hits [S]). It does not support counting-team (CT) operations over OFI (`RAISE_ERROR_STR("OFI transport does not currently support CT operations")`, SOS/src/transport_ofi.h#L1460-L1504 [S]). Its `syncmem` is a no-op (SOS/src/transport_ofi.h#L1535 [S]).
4. **Scaling-relevant design:**
   - **Keys:** with `FI_MR_SCALABLE`-style client keys, a PE needs **zero per-peer key state** (keys 0 and 1 are the same on every PE). With provider keys (`FI_MR_PROV_KEY`), SOS stores 2 keys + 2 base addresses = **32 B per PE** (3.2 MB at 1e5 PEs; 32 MB at 1e6) [S/I].
   - **Addresses:** SOS inserts **all N addresses eagerly** at startup, fetched one by one from the PMI KVS [S].
   - **Endpoints:** each PE has **one target endpoint**, and all contexts are initiator-only endpoints that share one AV. So the remote peer needs only one address per PE [S].
5. **Hardware offload that libfabric can express but only CXI provides:**
   - counters offloaded to hardware,
   - triggered operations and deferred work queues (DWQ),
   - hardware multicast collectives (`fi_allreduce` limited to 32-byte payloads).

   These map onto "put-with-signal", `shmem_wait_until` and small reductions, but SOS uses none of them except counters.

---

## 1. Primitives relevant to one-sided PGAS

### 1.1 libfabric API surface

- **`fi_rma`:** `fi_read`/`fi_write`/`fi_writemsg`, plus `fi_inject_write` (buffered, no completion) and `fi_writedata`. Flags include `FI_INJECT`, `FI_DELIVERY_COMPLETE`, `FI_MORE` and `FI_FENCE` (LF/man/fi_rma.3.md [D]). `FI_FENCE` is capability bit `1<<21` (LF/include/rdma/fabric.h#L150 [S]).
- **`fi_atomic`:**
  - Entry points: `fi_atomic`, `fi_inject_atomic`, `fi_fetch_atomic`, `fi_compare_atomic` and their `*msg` forms.
  - Validity checks: `fi_atomicvalid`/`fi_query_atomic` report the maximum element count per operation.
  - Operations: MIN, MAX, SUM, PROD, LOR/LAND/LXOR, BOR/BAND/BXOR, READ, WRITE, and CSWAP{,_NE,_LE,_LT,_GE,_GT}, MSWAP (LF/man/fi_atomic.3.md [D]).
- **`fi_cntr`:**
  - Calls: `fi_cntr_open/read/readerr/add/set/wait`.
  - Event types: `FI_CNTR_EVENTS_COMP` or `FI_CNTR_EVENTS_BYTES`.
  - Binding: a counter binds to an endpoint (initiator) or an MR (target).
  - Waiting: `fi_cntr_wait(threshold, timeout)` needs a wait object other than `FI_WAIT_NONE`.
  - XPU: new `FI_XPU` counters for device-side reads (`fi_cntr_export_xpu`) (LF/man/fi_cntr.3.md [D]).
- **`fi_trigger`:**
  - `FI_TRIGGER_THRESHOLD` fires one operation when a counter reaches a threshold, in threshold order.
  - Deferred work queues (`FI_QUEUE_WORK` via `fi_control` on the domain) take a trigger counter, a threshold, a completion counter, and one of msg/tagged/rma/atomic/fetch_atomic/compare_atomic/cntr operations.
  - Deferred work generates no CQ entries by default (LF/man/fi_trigger.3.md [D]).
  - Header has `FI_TRIGGER_XPU` (LF/include/rdma/fi_trigger.h#L48 [S]).
  - Implemented only by **cxi**, sockets and psm3: `FI_QUEUE_WORK` handling exists in prov/cxi and prov/sockets, and FI_TRIGGER in psm3/sockets [S grep].
- **`fi_collective`:**
  - Calls: `fi_join_collective` over an `fi_av_set`, plus barrier, broadcast, alltoall, allreduce, allgather, reduce_scatter, reduce, scatter, gather.
  - Collectives "map to atomic operations" (LF/man/fi_collective.3.md [D]).
  - Maturity: the v1.9 man page labelled the API **experimental** (https://ofiwg.github.io/libfabric/v1.9.0/man/fi_collective.3.html [D]). The 2.7.0rc1 page no longer contains the word (grep [S]), but the cxi man page still calls it "(experimental) OFI collectives API" (LF/man/fi_cxi.7.md §Collectives [D]).
  - The software utility provider `prov/coll` implements barrier, broadcast, allreduce, allgather and scatter. alltoall, reduce, reduce_scatter and gather are `fi_coll_no_*` (LF/prov/coll/src/coll_ep.c#L69-L79 [S]).
- **Scalable endpoints and shared contexts:** `fi_scalable_ep` with `fi_tx_context`/`fi_rx_context`, and `fi_stx_context`/`fi_srx_context` (LF/man/fi_endpoint.3.md#SCALABLE ENDPOINTS, #SHARED CONTEXTS [D]). Few providers support them; see §6.
- **`FI_ORDER_*`:** per-type ordering bits for a single source/destination endpoint pair. There are RMA_{RAR,RAW,WAR,WAW}, ATOMIC_* and the combined RAR/RAW/WAR/WAW/SAS/... (LF/man/fi_endpoint.3.md §msg_order [D]).

### 1.2 Per provider

| Primitive | cxi (Slingshot 11) | verbs;ofi_rxm | efa (`efa` / `efa-direct`) | opx (Omni-Path/CN5000) |
|---|---|---|---|---|
| RMA read/write | Native NIC DMA [D fi_cxi.7] | Native RC RDMA; rxm hides connection setup [D fi_rxm.7] | `efa`: device RDMA when available (≤ ~1 GiB per op), else emulated over SRD send/recv; `efa-direct`: device only, 1 IOV [D efa_fabric_comparison.md] | Onloaded packet protocol (eager, or RTS/CTS with SDMA/TID for large); software target processing [S LF/prov/opx/src/fi_opx_rma.c; R] |
| Atomics | **Native** Cassini AMO: MIN/MAX/SUM/L*/B*/WRITE/READ/CSWAP*/MSWAP; exactly 1 element (`CXIP_AMO_MAX_PACKED_IOV 1`, `_vector_valid` requires count==1) [S LF/prov/cxi/src/cxip_atomic.c#L33,#L100-L150,#L475-L495]. Optional "PCIe fetch-add" remap [D] | rxm: **emulated**. Target CPU runs `ofi_atomic_*_handler` [S LF/prov/rxm/src/rxm_cq.c#L1297-L1357]. Raw verbs MSG EP: `ATOMIC_WRITE`, fetch `SUM`/`READ`, `CSWAP` only, 1 element, INT64/UINT64/(DOUBLE/FLOAT), no `FI_INJECT` [S LF/prov/verbs/src/verbs_ep.c#L1949-L2049] via `IBV_WR_ATOMIC_{CMP_AND_SWP,FETCH_AND_ADD}` [S RC/libibverbs/verbs.h#L1141-L1155], implemented in rdma-core `rxe` and `ionic` [S RC/providers/rxe/rxe.c#L799, RC/providers/ionic/ionic_verbs.c#L2083] | `efa`: **emulated** via WRITE_RTA/FETCH_RTA/COMPARE_RTA + ATOMRSP packets, up to MTU size [D LF/prov/efa/docs/efa_rdm_protocol_v4.md §3.3, §1.1]; `efa-direct`: **none** [D] | FI_ATOMIC listed [D fi_opx.7]; onloaded [R] |
| Counters | Hardware "light-weight counting events". RMA/AMO source and target counts always offloaded; sends offloaded below the rendezvous threshold; receives never [D fi_cxi.7 §Counters]. MMIO/writeback extension [D] | rxm: software `util_cntr`, advanced by CQ progress [S LF/prov/rxm/src/rxm_domain.c#L296-L330]; verbs: `fi_no_cntr_open` [S LF/prov/verbs/src/verbs_domain.c#L295] | Both fabrics have counters; `efa-direct` also has MSI-X hardware counters via the GDA `cntr_open_ext` [D fi_efa.7] | Software counters [S LF/prov/opx/src/fi_opx_cntr.c] |
| Target events (`FI_RMA_EVENT`) | Yes [D] | No (rxm, verbs) [D] | **No** [D fi_efa.7 §RMA] | **No** [D fi_opx.7] |
| Triggered ops / DWQ | Yes, hardware-backed; SEND, TSEND, READ, WRITE, ATOMIC, FETCH_ATOMIC, COMPARE_ATOMIC; RECV only with threshold 0 [D fi_cxi.7 §Triggered Operation] | No [D fi_verbs.7, fi_rxm.7] | No [I, not in feature matrix] | No [D] |
| Tag matching | Hardware, falling back to hybrid/software [D] | rxm software over RC | `efa`: software; `efa-direct`: none [D] | Software (onload) [R] |
| Collective offload | Hardware multicast reductions, ≤ 32-byte payload, 8 concurrent per group; needs Fabric Manager REST and libcurl [D fi_cxi.7 §Collectives] | None | None | None |
| `FI_FENCE` | Yes [D] | No (rxm) [D] | No [D] | Not listed [I] |

---

## 2. Memory registration model and remote-key scaling

- **libfabric modes** (LF/man/fi_mr.3.md#L167-L391 [D]):
  - `FI_MR_BASIC` / `FI_MR_SCALABLE` are 1.x compatibility names; `FI_MR_SCALABLE` has been deprecated since 1.5.
  - Modern bits are `FI_MR_VIRT_ADDR`, `FI_MR_ALLOCATED`, `FI_MR_PROV_KEY`, `FI_MR_ENDPOINT`, `FI_MR_RMA_EVENT`, `FI_MR_HMEM`, `FI_MR_LOCAL`.
  - Scaling hinges on two things. **Client-chosen keys** (no `FI_MR_PROV_KEY`) mean every PE can use the same key, so no per-peer key table is needed. **Offset addressing** (no `FI_MR_VIRT_ADDR`) means no per-peer base-address table is needed.
- **SOS** (SOS/src/transport_ofi.c#L702-L800, #L1497-L1520; SOS/src/transport_ofi.h#L177-L230 [S]):
  - **Default `--enable-ofi-mr=scalable`** (SOS/configure.ac#L243-L257 [S]) sets `mr_mode=0`, optionally with `FI_MR_RMA_EVENT` or `FI_MR_ENDPOINT` added. It registers two MRs: data segment key 0 and heap key 1. RMA addresses are offsets from the local base, so there is **no per-peer state**.
  - With `--enable-remote-virtual-addressing`, SOS registers **one MR covering `[0, UINT64_MAX)`** with key 0 and sends absolute virtual addresses. This needs identical layouts (the ASLR check) and a provider that can register the whole VA space (ODP or ATS) [S; I].
  - **Basic/prov-key mode** (`FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY`) publishes `fi_heap_key`, `fi_data_key`, `fi_heap_addr` and `fi_data_addr` to the PMI KVS. Every PE then does N gets per key into `uint64_t[npes]` and `uint8_t*[npes]` tables: **32 B/PE**, plus 16 B/PE for an FI_HMEM external heap (SOS/src/transport_ofi.c#L889-L1080 [S]).
  - At 1e5 PEs that is 3.2–4.8 MB of tables and 4e5–6e5 KVS lookups per PE (4e10–6e10 in total) at startup [I].
- **cxi:**
  - Requires `FI_MR_ENDPOINT`; also needs `FI_MR_ALLOCATED` unless ODP is used.
  - Keys: 32-bit client keys by default; 64-bit provider keys with `FI_MR_PROV_KEY`.
  - Keys **0–99 are "optimized MRs"** (higher write rate); ≥100 are standard MRs.
  - Capacity is "64k standard MRs or 2k optimized MRs" in theory, with much lower practical limits.
  - **Optimized (restricted, low-overhead) RMA needs:** an optimized MR, **no `FI_RMA_EVENT`**, and no `FI_ORDER_RMA_WAW`. To avoid counting events, do not bind a counter to the MR (LF/man/fi_cxi.7.md §Memory registration modes, §Optimized MRs, §Optimized RMA [D]).
  - In source, `key.events = mr->count_events || mr->rma_events || mr->cntr` tells the source to use unrestricted operations (LF/prov/cxi/src/cxip_dom.c#L387-L390 [S]).
  - Domain `mr_cnt = 100`, `cntr_cnt = 16`, `ep_cnt = 128` (LF/prov/cxi/src/cxip_info.c#L30-L60 [S]).
  - **SOS on cxi:**
    - SOS keys 0 and 1 land in the optimized range [I].
    - In any build without `--enable-hard-polling`, SOS binds a target counter to both MRs and enables them (SOS/src/transport_ofi.c#L719-L790 [S]). By the provider's own rule this **forces unrestricted RMA on every put** [I].
    - `--enable-mr-endpoint` and `--enable-ofi-manual-progress` are the configure flags labelled for cxi (SOS/configure.ac#L259-L269 [S]).
- **verbs:** "Only FI_MR_BASIC mode is supported ... No support for binding memory regions to a counter" (LF/man/fi_verbs.7.md §LIMITATIONS [D]). rxm needs VIRT_ADDR, ALLOCATED and PROV_KEY and does not support FI_MR_SCALABLE (LF/man/fi_rxm.7.md [D]). So SOS needs 32 B/PE of key tables [I]. One registration can cover the heap [R: ibv_reg_mr on a contiguous region].
- **efa:** both fabrics require `FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR`; `FI_MR_SCALABLE` is ❌ (LF/prov/efa/docs/efa_fabric_comparison.md §MR Modes [D]). This means per-peer key tables.
- **opx:** "Only FI_MR_SCALABLE is supported" (LF/man/fi_opx.7.md [D]), so there is no per-peer key state.

---

## 3. Per-peer state at 10^5–10^6 PEs

- **libfabric AV:** `FI_AV_TABLE` lets `fi_addr_t` be the rank index, so the application keeps no map; `FI_AV_MAP` hands back opaque handles (LF/man/fi_av.3.md [D]). **EFA** has deprecated `FI_AV_MAP` in 2.x and silently switches to TABLE [D fi_efa.7]. **cxi** uses one implementation for both [D].
- **SOS** (SOS/src/transport_ofi.c#L1254-L1310, #L1345-L1352 [S]):
  - Each PE publishes one name, the **target endpoint's** name, under the KVS key `fi_epname`.
  - Each PE gets all N names into a temporary `npes*addrlen` buffer and calls one `fi_av_insert` for all of them.
  - AV_TABLE is the default; `--enable-av-map` adds an `fi_addr_t[npes]` (8 B/PE).
  - All contexts share one AV and target the remote PE's single target endpoint (the `GET_DEST(pe)` macro). So contexts do **not** multiply remote address state [S/I].
  - Startup cost is O(N) KVS gets per PE [I].
- **cxi:**
  - The address is `struct cxip_addr` {pid : C_DFA_PID_BITS_MAX, nic : C_DFA_NIC_BITS, pad : 3, vni uint16} (LF/prov/cxi/include/cxip.h#L387-L392 [S]), about 8 B [I].
  - Each AV entry holds a use count, a `UT_hash_handle`, the address, an `fi_addr_t` and an auth-key pointer (cxip.h#L2791-L2797 [S]). I estimate **~90 B/peer** (UT_hash_handle ≈ 56 B [R]), so ~9 MB at 1e5 and ~90 MB at 1e6 [I].
  - `FI_SYMMETRIC` with AV_TABLE lets the rank index stand in for the network address and avoids reverse lookups for `FI_SOURCE` [D].
  - The network is connectionless for RMA, but "Cassini implements a connection-based reliability model to support reliable execution of AMOs" [D fi_cxi.7 §Unreliable AMOs]. That state lives in NIC/driver resources; I could not quantify it [unverified].
  - PIDs range over 0–510 per NIC per VNI, with a default PID granule of 256 [D/S]. This bounds endpoints per NIC [I].
- **verbs;ofi_rxm:**
  - rxm creates one RC connection per peer **on demand**; the first sends return `-FI_EAGAIN` while it connects (LF/man/fi_rxm.7.md [D]).
  - Per-peer software state is `struct rxm_conn` plus `FI_OFI_RXM_NUM_MSG_EPS` MSG endpoints (RC QPs) (LF/prov/rxm/src/rxm.h#L241-L265 [S]).
  - An RC QP typically costs kernel, NIC and pinned-buffer memory in the kilobytes to tens of kilobytes, plus rxm's receive buffers (default buffer size ~16 KiB) [R, D for 16k]. At 1e5 peers, full connectivity is impractical; one-sided PGAS with all-to-all access patterns will eventually connect to everyone [I]. XRC exists in the verbs provider (`VERBS_HAVE_XRC`) [S], but its scalability was not verified here.
- **efa:**
  - No connections: SRD is a reliable datagram. But `struct efa_rdm_peer` carries a reorder buffer (`robuf`), a message-ID counter, handshake state, RNR back-off, and TX/RX lists (LF/prov/efa/src/rdm/efa_rdm_peer.h#L88-L137 [S]).
  - Each peer needs a handshake before extra features, unless `FI_OPT_EFA_HOMOGENEOUS_PEERS` is set [D].
  - **An RMA target must already have the initiator in its AV** (`efa-direct`, or `efa` with homogeneous peers) [D]. That forces a full AV on every target [I].
- **opx:** per-flow reliability/replay state, with ≤ 7 MB default (8 KB–144 MB) of un-ACKed bytes per one-way flow (`FI_OPX_RELIABILITY_SERVICE_MAX_OUTSTANDING_BYTES`) (LF/man/fi_opx.7.md [D]). Replay-buffer memory therefore grows with the number of active peers [I].

---

## 4. Ordering and delivery guarantees

- **Semantics:** ordering is defined per source/destination endpoint pair only. "Relaxed message order ... may improve performance by utilizing multiple paths" [D fi_endpoint.3].
- **cxi:**
  - Supports SAS, WAW, RMA_WAW, RMA_RAR, ATOMIC_WAW and ATOMIC_RAR. The man page says `*_WAR/RAW` are not supported, with max_order_raw/war_size forced to 0 and max_order_waw_size = −1 (unbounded) [D fi_cxi.7 §Message Ordering].
  - The source macro `CXIP_MSG_ORDER` also lists ATOMIC_WAR and ATOMIC_RAW (LF/prov/cxi/include/cxip.h#L148-L155 [S]). The doc and code disagree.
  - "Relaxing the target ordering may result in improved performance" [D].
  - **High Rate Put** (`FI_CXI_HRP`) is ACKed by the fabric egress port, "does not guarantee global ordering", and needs a following fenced put to restore order [D].
  - "Unreliable AMOs are not ordered" [D].
  - Default completions are **transmit-complete** (target NIC ACK). **`FI_DELIVERY_COMPLETE` makes the target do an unconditional zero-byte flushing read, "higher latency"** [D fi_cxi.7 §Operation flags].
  - SOS sets `FI_DELIVERY_COMPLETE` as the default tx op flag and on bounce-buffered and large puts (SOS/src/transport_ofi.c#L1535, #L1756; transport_ofi.h#L685-L880 [S]). On cxi, SOS therefore pays the flush on those paths [I]. SOS's own comment says cxi "does not support fetch atomics with FI_DELIVERY_COMPLETE" (SOS/src/transport_ofi.h#L1062, #L1335, #L1410 [S]).
- **verbs RC:** RAR, RAW, RAS, WAW, WAS, SAW and SAS [D fi_verbs.7]. **rxm with FI_ATOMIC loses RAR/RAW/WAR/WAW/SAR/SAW and all max_order sizes become 0** (LF/man/fi_rxm.7.md §FI_ATOMIC limitations [D]; LF/prov/rxm/src/rxm_init.c#L501-L512 [S]). With `FI_OFI_RXM_NUM_MSG_EPS>1` (multipath over several QPs), only SAS and ATOMIC_* remain [D].
- **efa:** the device (SRD) gives **no ordering**, since it sprays packets over multiple paths.
  - The `efa` fabric restores SAS and ATOMIC_{RAR,RAW,WAR,WAW} in software with msg_id and a reorder buffer.
  - **`efa-direct` provides no ordering at all** [D efa_fabric_comparison.md; efa_rdm_protocol_v4.md §1.1].
  - Opt-in `FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES` [D].
  - SRD paper: Shalev et al., IEEE Micro 2020, https://ieeexplore.ieee.org/document/9167399 [D link from docs; content R].
- **opx:** the default msg_order is ATOMIC_{RAR,RAW,WAR,WAW} only (LF/prov/opx/include/rdma/opx/fi_opx.h#L331 [S]). Reliability is a software replay protocol with PING/ACK [D fi_opx.7].
- **How SOS copes:**
  - `shmem_fence` becomes `shmem_transport_put_quiet` (wait for all outstanding puts) whenever `WANT_TOTAL_DATA_ORDERING == 0` (SOS/src/transport_ofi.h#L553-L566 [S]). Unordered transports turn fence into a full quiet [S].
  - Put-with-signal either uses that transport-level fence or the optional `FI_FENCE` flag (`--enable-ofi-fence`) on the signal atomic (SOS/src/transport_ofi.h#L734-L876 [S]).
  - OpenSHMEM 1.6 clarified that fence ordering applies only within the same context (spec Annex G.1 item 18 [D spec16.txt]).

---

## 5. Completion and progress model

- **libfabric:** a CQ (with `FI_SELECTIVE_COMPLETION` to suppress events) or counters. Progress is `FI_PROGRESS_AUTO` or `FI_PROGRESS_MANUAL` [D fi_domain.3, fi_cntr.3].
- **SOS:**
  - *Counters:* every context has `put_cntr` (bound FI_WRITE) and `get_cntr` (bound FI_READ). Operations increment a local `pending_*` counter, and waits compare it with `fi_cntr_read` (SOS/src/transport_ofi.c#L599-L616; transport_ofi.h#L484-L540, #L958-L1005 [S]).
  - *Waiting:* a poll up to `OFI_TX/RX_POLL_LIMIT`, then `fi_cntr_wait(cntr, pending, -1)`. In THREAD_MULTIPLE builds with context locks, SOS forces a poll limit of −1, meaning spin-poll forever, and `FI_WAIT_NONE` (SOS/src/transport_ofi.c#L1902-L1916, #L1726-L1745 [S]).
  - *Target counter:* one `FI_CNTR_EVENTS_COMP` counter bound to the MRs with FI_REMOTE_WRITE, used only by blocking `shmem_wait*` in THREAD_SINGLE builds (`shmem_transport_received_cntr_wait`) (SOS/src/transport_ofi.h#L1506-L1532 [S]).
  - *Manual progress:* `--enable-ofi-manual-progress` sets `FI_PROGRESS_MANUAL`, and `shmem_transport_probe()` does `fi_cq_read` on the target CQ (SOS/src/transport_ofi.h#L384-L404 [S]). The configure comment says this "is currently required for cxi provider" (SOS/configure.ac#L265-L269 [S]).
  - *Hard polling:* `ENABLE_HARD_POLLING` removes the target counter and turns all waits into spin-polling with `probe()` (SOS/src/transport_ofi.h#L38-L40; shmem_synchronization.h#L178-L197 [S]). Hard polling is forced when CMA or XPMEM on-node transports are used (SOS/configure.ac#L535-L541 [S]).
  - SOS never creates a progress thread [S].
- **cxi:**
  - `FI_PROGRESS_MANUAL` **only** [D].
  - Hardware counters and hardware rendezvous-get mean most RMA and AMO completion needs no software progress: "Software progress is required to update counters unless the criteria for offloading are met" [D].
  - Wait objects: FD/POLLFD, with a documented race and a recommendation for short epoll timeouts [D].
- **rxm:** manual or auto; auto starts a listener thread and forces FI_THREAD_SAFE (LF/prov/rxm/src/rxm_init.c#L470-L476 [S]). Emulated atomics and SAR need **target-side CQ progress** [I from rxm_cq.c handling atomics in the receive path].
- **efa:** the `efa` fabric is **`FI_PROGRESS_MANUAL`**, and emulated RMA, atomics and even ordered SAS need **target-side polling** [D]. Hardware counters in `efa-direct` do not poll the CQ internally; the application must call `fi_cq_read` when it sees `-FI_EAGAIN` [D].
- **opx:** manual or auto; auto means one thread per CQ [D]. The target is always onloaded, so the target CPU must make progress for RMA [I].

---

## 6. Thread and context model, and hardware cost per context

- **libfabric options:** scalable endpoints (one address, many TX/RX contexts), STX (a transmit queue shared by many endpoints; endpoints keep their own CQs and counters), or plain endpoints (each with its own address and queues) [D fi_endpoint.3].
- **Provider support** [S grep of `max_ep_stx_ctx`]:
  - **STX:** only psm2, psm3, sockets and mrail. cxi = 0 (LF/prov/cxi/src/cxip_info.c#L47), opx = 0, verbs/efa unset (default 0).
  - **Scalable endpoints:** "not enabled in the CXI provider. Future releases ... will re-introduce" [D fi_cxi.7]; not supported by verbs [D] or rxm [D]; opx has `fi_opx_sep.c` [S]; `CXIP_EP_MAX_CTX_BITS 0`, i.e. one TX context per endpoint (cxip.h#L77-L79 [S]).
  - **cxi transmit alias endpoints** (`fi_ep_alias`) are lightweight but share ordering and resources with the original endpoint [D].
- **SOS contexts** (SOS/src/transport_ofi.c#L420-L597, #L1725-L1830, #L1851-L1990 [S]):
  - Each `shmem_ctx` gets its own `fi_endpoint`, CQ, 2 counters, optional bounce-buffer free list, and a lock (skipped for PRIVATE or SERIALIZED contexts).
  - *STX binding:* if the domain supports STX, the context binds one from a pool of `OFI_STX_MAX` (default 1). The allocator (round-robin or random, with a threshold) prefers a private STX per thread id for `SHMEM_CTX_PRIVATE` and falls back to sharing. `OFI_STX_AUTO` divides the domain's `tx_ctx_cnt` evenly among PEs on the node.
  - *Without STX* (cxi, verbs, efa, opx), **every context costs a full hardware endpoint**. On cxi that means TXQ/RXQ and event queues out of a domain `ep_cnt = 128` and a CQ out of `cq_cnt = 32` [S cxip_info.c; I].
  - *Single-endpoint optimisation:* by default the default context and the target endpoint are merged (`OFI_DISABLE_SINGLE_EP=false`) [S shmem_env_defs.h].
  - *Threading level:* `FI_THREAD_DOMAIN` for single/funneled; `FI_THREAD_SAFE` or `FI_THREAD_COMPLETION` for MULTIPLE [S L1516-L1529].
  - *Evidence on the STX design:* Grant et al. evaluated SOS contexts over the libfabric GNI provider on Aries and found low overhead, with contexts helping multithreaded programs ("Implementation and Evaluation of OpenSHMEM Contexts Using OFI Libfabric", OpenSHMEM 2017, https://link.springer.com/chapter/10.1007/978-3-319-73814-7_2 [D abstract via search]). A related OPA/PSM2 paper studies STX sharing [D search snippet, not read].
- **cxi triggered operations** use a hardware budget per CXI service. Example: 512 per service, 8 reserved by the provider, leaving 504 to divide among the N processes on the NIC. Exhaustion is "indeterminate behavior" unless the application uses `get_dwq_depth` or the semaphore-based `FI_CXI_ENABLE_TRIG_OP_LIMIT` [D fi_cxi.7].

---

## 7. GPU memory support

- **libfabric:** `FI_HMEM` with `FI_MR_HMEM` covers CUDA, ROCr, ZE, Neuron, SynapseAI and others [D fi_mr.3].
- **New `FI_XPU` API:** endpoints, CQs and counters exported for device-initiated data paths; `fi_xpu_ctx`, `max_xpu_ctx_cnt`, and `fi_mr_get_xpu_desc`/`fi_av_lookup2` for device-side descriptors (LF/man/fi_xpu.3.md [D]). **No provider in the tree implements it:** grep for `fi_xpu_ctx`/`max_xpu_ctx_cnt` under `prov/` finds nothing [S].
- **cxi:** CUDA, ROCR and ZE; DMABUF enabled by default; ZE requires implicit scaling to be disabled [D].
- **efa:** HMEM on both fabrics. The `efa` fabric emulates keys and bounce-buffers when there is no p2p. `efa-direct` has a "GPU Direct Async" domain extension that exposes QP, CQ and address attributes and hardware counters in GPU memory; host-side `fi_cntr_read`/`wait` are unsupported on GPU-backed counters [D fi_efa.7, comparison doc].
- **verbs/rxm:** HMEM through rxm (`rxm_hmem.c`) [S file exists]; details not verified.
- **SOS:** `--enable-ofi-hmem` registers an extra "external heap" MR with `FI_HMEM_CUDA` or `FI_HMEM_ZE` and publishes its key and address per PE (+16 B/PE) (SOS/src/transport_ofi.c#L650-L699, #L849-L887 [S]). NEWS v1.5.3rc1 says "extension support for GPU RDMA and external heap creation" for Intel SHMEM (SOS/NEWS [S]). SOS has no GPU-initiated path [S].

---

## 8. What SOS had to emulate in software (most important)

All paths below are in SOS/src at commit 4aa91d7.

| OpenSHMEM feature | How SOS implements it over OFI | Evidence |
|---|---|---|
| **Remote atomics** (add, inc, set, fetch, cswap, swap, bitwise) | Delegated to `fi_atomic` / `fi_inject_atomic` / `fi_fetch_atomic*` / `fi_compare_atomic*`. **SOS does no software emulation of point-to-point atomics.** At init it probes `fi_atomicvalid` for every (op, datatype) pair and **fails** (or only warns, with `OFI_ATOMIC_CHECKS_WARN`) if one is missing. Emulation is therefore pushed *into the provider* (efa `efa`, rxm, opx) [I]. Atomics over the inject size go through bounce buffers, then fall back to chunked `fi_atomic` with `max_atomic_size` from `fi_atomicvalid` | transport_ofi.c#L1180-L1250; transport_ofi.h#L1142-L1270 |
| **Nonblocking atomic fetch** (1.5 nbi AMOs) | Issued with `FI_INJECT` and tracked through the get counter; completed in `get_wait`/`fence` | transport_ofi.h#L1283-L1330, #L553-L566 |
| **`shmem_atomic_fetch` on cxi** | cxi "does not support fetch atomics with FI_DELIVERY_COMPLETE" and does not support FI_ATOMIC_READ. SOS therefore issues **fetch-SUM with a 0 operand** under `ENABLE_MR_ENDPOINT` | transport_ofi.h#L1405-L1421 |
| **Non-fetching AMOs replaced by fetching** | Configure option `--disable-nonfetch-amo`: "SOS will fetch values for any AMO" | configure.ac#L304-L308 |
| **Reductions** (`shmem_*_reduce`, `*_to_all`) | Always **software**: linear, ring, tree, and software recursive doubling (`op_to_all_recdbl_sw`) built from puts/gets. `shmem_transport_atomic_supported()` asks `fi_atomicvalid` whether a type/op can be done as a one-sided atomic, otherwise SOS uses software reductions (e.g. complex product). **`fi_collective` / cxi hardware allreduce are not used** | collectives.c#L582-L990; transport_ofi.h#L1436-L1456; grep finds no `fi_allreduce`/`fi_join_collective` in src |
| **All other collectives** (barrier/sync, broadcast, collect, fcollect, alltoall[s], inscan/exscan) | Software: k-ary trees, rings, recursive doubling, linear loops over puts/gets | collectives.c#L48-L1500 |
| **Teams** (split_strided, split_2d, contexts per team) | Pure software bookkeeping plus software collectives | shmem_team.c |
| **Locks** (`shmem_set_lock` etc.) | Software distributed queue lock built from `cswap` on `lock->last`, `atomic_fetch` on `lock->data`, `mswap` to set a signal bit, and `SHMEM_WAIT` loops | shmem_lock.h#L40-L80 |
| **`shmem_wait` / `wait_until` / `signal_wait_until`** | No network primitive. The PE rereads its own local memory in a loop. **Polling** means `shmem_transport_probe()` plus a spinlock pause. **Blocking** (THREAD_SINGLE, no hard polling, no manual progress) means reading the target counter, re-checking, then `fi_cntr_wait(target_cntr, prev+1)`. Any remote write wakes the waiter whatever the address. cxi (manual progress) and any build with CMA/XPMEM fall back to polling | shmem_synchronization.h#L100-L218; transport_ofi.h#L1506-L1532 |
| **Put-with-signal** (1.5) and signal add/set (1.6) | 1) Puts: `fi_writemsg` with `FI_INJECT` for small, fragmented `FI_DELIVERY_COMPLETE` for large. 2) Ordering: **software fence** (`put_quiet` over all outstanding puts) or `FI_FENCE` on the signal. 3) Signal: `fi_atomicmsg` of `FI_SUM` (add) or `FI_ATOMIC_WRITE` (set) on a `uint64_t`, with `FI_INJECT` | transport_ofi.h#L734-L876 |
| **`shmem_fence` / `shmem_quiet`** | Software: wait until the put and get counters reach the pending counts (poll, then `fi_cntr_wait`) and drain bounce buffers. Unordered builds turn fence into quiet. `syncmem` is a no-op because libfabric has "no analog to PtlAtomicSync()" | transport_ofi.h#L484-L566, #L1535-L1541 |
| **Bounce-buffered puts** | Per-context free list (`BOUNCE_SIZE`, `MAX_BOUNCE_BUFFERS=128`). Puts above `inject_size` but within the bounce size are copied and sent with `FI_COMPLETION`. The CQ is drained to recycle buffers; this needs `FI_CONTEXT` semantics and is disabled if the provider requires FI_CONTEXT/FI_CONTEXT2 | transport_ofi.h#L420-L482; transport_ofi.c#L1887-L1900; shmem_env_defs.h#L47-L49 |
| **Messages larger than `max_msg_size`** | SOS fragments them in a software loop | transport_ofi.h#L645-L680, #L800-L825 |
| **Retry on `-FI_EAGAIN`** | `try_again()` drains the CQ, calls `probe()`, and aborts after `OFI_MAX_POLL` retries | transport_ofi.h#L571-L612 |
| **Contexts, including SHARED and SERIALIZED** | Software mutex per context, plus a software STX pool and allocator (private STX per thread id, shared round-robin/random with a threshold, auto-partitioning of `tx_ctx_cnt` across node PEs). Without STX support, each context is a full endpoint | transport_ofi.c#L420-L597, #L1851-L1990; transport_ofi.h#L347-L380 |
| **Performance counters** (`shmemx_pcntr`) | Software pending counters plus `fi_cntr_read` | transport_ofi.h#L1544-L1637 |
| **`ibput`/`ibget`** (1.6, `shmemx_` in SOS) and `iput`/`iget` | Loops of scalar or nonblocking puts/gets, then `put_wait` | data_c.c4#L480-L530 |
| **Counting-team operations** | **Not implemented** over OFI (raise error) | transport_ofi.h#L1460-L1504 |
| **Address and key distribution** | PMI KVS, eager, O(N) gets per PE | transport_ofi.c#L889-L1080, #L1254-L1310 |
| **On-node transfers** | Separate CMA/XPMEM transports, which force hard polling | configure.ac#L495-L541 |
| **1.6 sessions, `pe_quiet`** | Session start/stop not present; `SHMEM_MINOR_VERSION 5`. grep finds `team_ptr`/`pe_quiet`-like names only in shmemx headers and remote_pointer_c.c | mpp/shmem-def.h.in#L61-L63; grep |

**Key reading:** SOS's OFI transport is a thin "counter-driven RMA plus atomics" layer. It emulates *synchronization* (wait, lock, fence) and *collectives* entirely in software. It relies on the *provider* for atomics, and provider-side emulation (efa, rxm, opx) silently turns SHMEM atomics into two-sided operations that need target progress [I]. Features the fabric could offload are left unused:

- cxi triggered and deferred work (wait_until wake-ups, signal),
- cxi hardware reductions,
- `FI_TRIGGER` in general,
- remote-event CQs (`FI_RMA_EVENT` with CQ data; SOS uses only a counter).

---

## 9. Known performance pathologies and scaling limits

### cxi
- **Hardware matching exhaustion.** When hardware match resources run out, the provider triggers flow control, drops and retransmits, "impacting performance significantly" or moves to hybrid software matching.
- **Rendezvous ID limits.** FI_MSG rendezvous has 256 IDs and FI_TAGGED has 32K−256. Exceeding them can cause "software deadlock" [D fi_cxi.7 §Messaging].
- **Trigger resources.** Triggered-operation exhaustion is "indeterminate behavior" [D].
- **Optimized MRs.** At most ~2k in theory, fewer in practice. Binding counters or `FI_RMA_EVENT` disables restricted fast-path packets. `FI_DELIVERY_COMPLETE` adds a flushing read [D].
- **Consequence for SOS:** default SOS builds hit both costs, the counter bound to the MR and FI_DELIVERY_COMPLETE on large and bounced puts [I].
- **ODP.** Network page faults "significantly impact application performance" [D]. Whole-VA registration (remote virtual addressing) would rely on ODP or ATS [I].
- **Hardware collectives.** They need the Fabric Manager REST API, multicast addresses are limited per system, there are at most 8 concurrent reductions, and payloads are ≤ 32 B [D].
- **CXI provider for MPI.** Open MPI developers cite "significant performance issues encountered using the vendor's OFI libfabric provider - CXI" and built the LINKx provider (cxi+shm) (https://www.osti.gov/servlets/purl/2438730 [D search snippet]).
- **Slingshot analysis:** De Sensi et al., SC20 (https://arxiv.org/pdf/2008.08886 [D link; content R]).

### verbs;rxm
- On-demand RC connections return `EAGAIN` on the first operations [D]. Per-QP memory grows with peers [R].
- Enabling FI_ATOMIC removes RMA ordering [D].
- Emulated atomics need target progress [I].
- Provider "cannot detect" CQ overrun [D fi_verbs.7].

### efa
- `efa` fabric: every emulated protocol, and ordering, needs target CPU progress (`FI_PROGRESS_MANUAL`) [D].
- `FI_RM_ENABLED` is only partly honoured: no CQ overrun protection [D].
- No `FI_RMA_EVENT`, so SOS would need hard polling [D/I].
- `efa-direct` lacks FI_ATOMIC, so SOS cannot select it: SOS hints always request FI_ATOMIC [S/I].
- Atomics are limited to MTU size (~8 KiB) [D].

### opx
- Reliability PING/ACK timers and a per-flow in-flight byte cap [D].
- Onloaded RMA uses target CPU [I].
- `FI_PROGRESS_AUTO` costs one thread per CQ [D].
- The SOS wiki notes PSM2/OPA HFI contexts default to one per physical core, with up to "80 contexts" configurable (https://github.com/Sandia-OpenSHMEM/SOS/wiki/Performance-Tuning [D]). This is PSM2-era; OPX limits were not verified.

### SOS at scale
- **Startup:** O(N) KVS gets per PE for addresses (and for keys in prov-key mode), and a temporary buffer of N×addrlen [S/I].
- **Collectives:** software only, so there is no offload at 1e5 PEs [S].
- **Wait semantics:** blocking wait wakes on *any* remote write (a global target counter), which risks thundering-herd spin at scale. Multithreaded builds spin-poll [S/I].

---

## 10. Build, portability and maintenance burden

- **libfabric:** 20+ providers in tree (`prov/`: coll, cxi, efa, hook, lnx, lpp, mrail, opx, psm2, psm3, rxd, rxm, shm, sm2, sockets, tcp, ucx, udp, usnic, util, verbs) [S].
- **Provider build dependencies:**
  - **cxi** needs libCXI and Cassini headers, and libcurl for collectives [D].
  - **opx** needs libuuid, libnuma and kernel headers, plus optional hfisvc/hfi1dv; it has a build-time `OPX_AV=table|map|runtime` choice [D fi_opx.7; S].
  - **verbs** needs libibverbs and librdmacm [D].
- **Per-provider semantic drift is large.** Examples:
  - mr_mode sets are disjoint: cxi needs ENDPOINT, opx only SCALABLE, efa and verbs need PROV_KEY and VIRT_ADDR.
  - STX and SEP availability varies.
  - Progress modes differ: cxi and `efa` are manual only.
  - The same `FI_ATOMIC` flag may mean hardware or emulated.
  - Doc and code can disagree (cxi ATOMIC_WAR/RAW ordering).

  [D/S]
- **SOS carries these differences as configure-time choices** (`--enable-ofi-mr=basic|scalable|rma-event`, `--enable-mr-endpoint`, `--enable-ofi-manual-progress`, `--enable-hard-polling`, `--enable-av-map`, `--enable-ofi-fence`, `--disable-ofi-inject`, `--disable-nonfetch-amo`, `--enable-remote-virtual-addressing`, `--enable-thread-completion`, `--enable-ofi-hmem`) and provider-name string checks (the sockets re-enable quirk, SOS/src/transport_ofi.c#L630-L640) [S]. One SOS binary is not portable across cxi, efa and verbs [I].
- **SOS release cadence:** last tag v1.5.3; main at 1.5 plus shmemx extensions [S].

---

## Implications for a core API

**Primitives this substrate makes cheap (at least on cxi, the one "complete" provider):**
- **One-sided put/get to a symmetric region with client-chosen, rank-invariant keys and offset addressing.** Zero per-peer key state on cxi (keys 0–99 optimized) and opx. The core API should *require* rank-invariant keys plus offsets, with a key-table fallback costing 16 B per region per peer [D/S/I].
- **Counter-based completion.** Aggregate completed/error counts per context and direction, with threshold wait, instead of per-operation CQ entries. SOS proves this is sufficient for put, get, quiet, fence and nbi atomics, and cxi offloads it [S/D]. The core should expose `cntr_read` and `cntr_wait(threshold)`.
- **Single-element remote atomics:** write, sum, cswap, mswap and fetch-variants on 1–8-byte scalars. Native on cxi (exactly 1 element) and on IBTA atomic-capable RC verbs (64-bit only, rxe and ionic) [S]. SHMEM atomics are single-element, so a core "scalar AMO" primitive of one element up to 8 (or 16) bytes matches the hardware exactly.
- **Inject (buffered, fire-and-forget) small writes and atomics** up to `inject_size` [D/S].
- **Batching doorbells** (`FI_MORE` on cxi) [D].

**Primitives it makes expensive or non-portable (design these as optional or extended, with known costs):**
- **Remote atomics on efa, rxm and opx** become two-sided and need target CPU progress. A core API should report "atomic is offloaded vs requires target progress" as a capability, because it changes whether the target must poll [D/S].
- **Ordering.** Ordered RMA plus atomics conflicts with multipath (efa SRD, rxm multi-EP, cxi HRP, unreliable AMOs). The core should make ordering *opt-in per context*, with an explicit cheap "fence/barrier" primitive (cxi `FI_FENCE`) and a documented software fallback (quiet = wait on counters) [D/S].
- **Target notification** (`FI_RMA_EVENT`/target counters) is absent on efa, verbs and opx. On cxi, enabling it disables the fast RMA path. So `shmem_wait_until` should stay in the extended layer, polling or blocking on an *optional* target counter scoped to a small signal region. Do not bind counters to the whole heap [D/S/I].
- **Delivery-complete vs transmit-complete.** Visibility-at-target costs an extra flush on cxi. The core should default to transmit-complete and make delivery-complete an explicit sync point [D/I].
- **Contexts.** STX and scalable endpoints are essentially gone from high-end providers (cxi, efa, verbs/rxm, opx has SEP only). A context generally costs a full endpoint plus CQ (cxi: ≤128 endpoints and 32 CQs per domain). The core should expose "transmit queue" as the scarce resource and let the upper layer multiplex contexts [S/D].
- **Triggered/deferred operations and collective offload** exist only on cxi, with small hard resource pools (e.g. 504 triggered ops per service; 8 concurrent ≤32-byte reductions). They are valuable for signal/wait_until and small reductions, but must be optional extensions with software fallbacks [D].
- **Per-peer state.** AV entries are ~90 B/peer on cxi (estimate), efa keeps rich per-peer protocol state plus handshakes, and verbs keeps RC connections per peer. The core should support lazy, index-based addressing (rank → address resolved on first use, `FI_AV_TABLE`/`FI_SYMMETRIC` style) and avoid O(N) eager KVS exchange at startup [S/D/I].
- **Collectives** in libfabric are experimental in heritage, partial in software (`prov/coll` lacks alltoall/reduce/gather), and hardware-limited on cxi. Keep all OpenSHMEM collectives above the core, with an optional "small allreduce offload" hook [D/S].
