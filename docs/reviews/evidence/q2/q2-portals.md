# Q2 survey: Portals 4, Sandia OpenSHMEM's Portals transport, the Portals 4 reference implementation, BXI (and Cray SeaStar)

Labels: **[doc]** = documented (URL given), **[src]** = observed in source (URL + file + line), **[recalled]** = from memory, not verified here, **[inference]** = my own reasoning from the cited evidence.

## Sources used

| Short name | What | Where |
|---|---|---|
| P4.3 | *The Portals 4.3 Network Programming Interface*, SAND report (2022) | https://www.sandia.gov/app/uploads/sites/144/2023/03/portals43.pdf (PDF text extracted locally; section numbers cited) |
| SOS | Sandia OpenSHMEM, `main` @ `4aa91d7f` (last commit Aug 2025); checked against tag `v1.4.5` (`1f89a0f0`) | https://github.com/Sandia-OpenSHMEM/SOS/blob/4aa91d7fa9041785bd367fc9cefab99d313dd9df/src/transport_portals4.h and `.../src/transport_portals4.c` |
| P4RI | Portals 4 reference implementation, `master` @ `4e787c13` (Sep 2024) | https://github.com/Portals4/portals4/tree/4e787c1330d956f9d924b7aed21094eb40dfd83d |
| OFA12 | R. Brightwell, "Portals 4 Exascale", OFA Workshop, Mar 2012 (slides) | https://downloads.openfabrics.org/Media/Monterey_2012/2012_Workshop_Mon_Portals_4_Exascale.pdf |
| NET4EXA | Martinelli et al., "NET4EXA: Pioneering the Future of Interconnects for Supercomputing and AI", arXiv:2601.19413 (Jan 2026), co-authored by Eviden/BULL | https://arxiv.org/pdf/2601.19413 |
| NP24 | The Next Platform, "Eviden Mainstreams BXI Interconnect…", Dec 2024 (journalism, not primary) | https://www.nextplatform.com/2024/12/10/eviden-mainstreams-bxi-interconnect-thanks-to-ultra-ethernet-and-ai-boom/ |
| OMPI-UBCL | Open MPI `ompi/mca/osc/ubcl` and `pml/ubcl` (Bull SAS, added Oct 2025, commit `b2d103c3`) | https://github.com/open-mpi/ompi/tree/main/ompi/mca/osc/ubcl (README.md) |
| CUG06 | Brightwell et al., "An Accelerated Implementation of Portals on the Cray SeaStar", CUG 2006 | https://www.sandia.gov/app/uploads/sites/210/2022/11/p3-accel-cug06.pdf |
| HWCNT | Islam et al., "Minimizing the usage of hardware counters for collective communication using triggered operations" (EuroMPI'19 / ParCo 2020) | https://dl.acm.org/doi/abs/10.1145/3343211.3343222 (abstract via search result) |
| TRIG10 | Hemmert, Barrett, Underwood, "Using Triggered Operations to Offload Collective Communication Operations", EuroMPI 2010 | https://link.springer.com/chapter/10.1007/978-3-642-15646-5_26 (abstract only) |

Not reachable: the HOTI 2015 BXI paper ("The BXI Interconnect Architecture", Derradji et al., https://ieeexplore.ieee.org/document/7312662/). IEEE Xplore and Semantic Scholar gave empty pages or rate limits. So the hardware internals of BXIv1/v2 below (NIC tables, matching offload details, translation cache size) are **unverified** unless a claim cites NET4EXA or NP24.

Terminology fix: the brief says `PTL_NI_FLOW_CTRL`. The 4.3 spec calls the option **`PTL_PT_FLOWCTRL`** and sets it per portal table entry in `PtlPTAlloc` [doc P4.3 §2.8]. There is no NI-level flag of that name in 4.3 [doc P4.3, grep of spec text].

---

## 1. Primitives relevant to one-sided PGAS

**RMA.** `PtlPut` and `PtlGet` go through an MD (initiator memory) to a *portal table index* plus *remote offset* at the target. The target resolves this against the first matching entry on that PT's priority list [doc P4.3 §2.5, §3.15]. There is no rkey on the wire, and the target decides what is exposed. On a **non-matching** NI (`PTL_NI_NO_MATCHING`), the first LE always matches. P4.3 says this "allows fully pipelined operation for the non-matching address translation." A persistent LE at the list head gives much higher message rates, and the spec names OpenSHMEM as the beneficiary [doc P4.3 §2.5.1]. The spec also says: "In typical scenarios … OpenSHMEM uses the non-matching interface and lightweight counting events" [doc P4.3 §2.5].

**Atomics (native, target-side).** [doc P4.3 §3.15.4, Table 3-4]
- `PtlAtomic`: one-way `target = target OP src`. Ops are MIN, MAX, SUM, PROD, LOR, LAND, BOR, BAND, LXOR, BXOR, DIFF.
- `PtlFetchAtomic`: the same, plus the old value is returned in a reply.
- `PtlSwap`: SWAP, CSWAP, CSWAP_NE/LE/LT/GE/GT, MSWAP.
- Datatypes are fixed-width integers, float, double, long double and complex. MSWAP and the logical/bitwise ops are integer-only.
- Size limits come from the NI: `max_atomic_size` and `max_fetch_atomic_size`. CSWAP and MSWAP are limited to **one datatype item**.
- Atomicity holds only at datatype granularity, and only against other Portals calls on the same NI. It extends to host-side ops only if the NI reports `PTL_COHERENT_ATOMICS`. Otherwise the host must call `PtlAtomicSync()` before touching memory that atomics have modified [doc P4.3 §3.15.4, §3.15.8].
- OpenSHMEM's AMOs map almost 1:1: `atomic_add`/`inc` → `PtlAtomic(SUM)`, `fetch` variants → `PtlFetchAtomic`, `swap` → `PtlSwap(SWAP)`, `compare_and_swap` → `PtlSwap(CSWAP)`. Bitwise reductions map onto BOR/BAND/BXOR [src SOS `transport_portals4.h` L720–1100].
- History: these features were added to Portals 4 specifically for OpenSHMEM. See Barrett, Brightwell, Hemmert, Pedretti, Wheeler, Underwood, "Enhanced Support for OpenSHMEM Communication in Portals", HOTI 2011 [doc OFA12 slide "OpenSHMEM on Portals 4.0"; paper contents recalled].

**Counting events (CT).** A CT is a `{success, failure}` pair of `ptl_size_t` values. The implementation increments it on selected events (ACK, REPLY, SEND, and target-side COMM events), counting either events or bytes. The API is `PtlCTAlloc/Get/Wait/Poll/Set/Inc/Free` [doc P4.3 §3.14]. Implementation Note 14 asks that `PtlCTGet` stay "as close to a pair of loads in performance as possible" [doc P4.3 §3.14]. Full events (`ptl_event_t` in an EQ) still exist, but SHMEM does not need them on the fast path.

**Triggered operations.** `PtlTriggeredPut/Get/Atomic/FetchAtomic/Swap/CTInc/CTSet` (plus the `PtlTriggeredME*` family in the header) run when a CT's success count reaches a threshold. The payload is read *at trigger time*. Operations release in threshold order, and `PtlCTCancelTriggered` cancels pending ones [doc P4.3 §3.16]. The motivating example is offloading a tree reduction [doc P4.3 §3.16; TRIG10]. `PtlStartBundle/EndBundle` hints group calls [doc P4.3 §3.17].

**Tag matching.** The matching NI uses 64-bit match/ignore bits, source rank or NID/PID, priority and overflow lists, unexpected headers, locally managed offsets, `min_free`, and `PtlMESearch`. This is MPI's substrate [doc P4.3 §2.5.2, §3.12]. OpenSHMEM does not need it (see §8).

**Active messages.** None natively. An AM can be built from a matching or non-matching LE with locally managed offsets plus full events. That requires a receive-side handler loop, which Portals avoids for RMA ("application bypass") [doc P4.3 §1.6] [inference].

**Collective offload.** Nothing is packaged. The building blocks are triggered ops plus CTs. Hardware CTs are scarce: "if the number of required counters exceeds the number of hardware counters, a collective needs to stall… if the HFI has a counter cache, utilizing a large number of counters can cause cache thrashing" [doc HWCNT abstract].

**4.3 additions.** [doc P4.3 App. C.1]
- `PTL_MD_UNRELIABLE`: put with no ACK, droppable.
- `PtlAbort`.
- `PTL_PT_ALLOC_DISABLED`.
- `PTL_ME_LOCAL_INC_UH_RLENGTH`.

## 2. Memory registration model and keys

- **No remote keys exist in the API.** Protection comes from the target's LE: `uid` and the `PTL_LE_OP_PUT/GET` permissions, plus bounds, where out-of-range access truncates to 0 bytes [doc P4.3 §3.11]. The initiator names memory by (rank, PT index, offset). Key material per PE pair is therefore **zero bytes**, and per target it is one LE per exposed region.
- **Whole address space with one LE.** If the NI reports `PTL_TARGET_BIND_INACCESSIBLE`, an LE may be `start=NULL, length=PTL_SIZE_MAX`, "even though the entire region is not currently allocated". Touching unmapped memory either SEGVs the process or fails the op with `PTL_NI_SEGV` [doc P4.3 §3.11]. Implementation Note 9 puts registration on the implementation. It may use "lightweight kernels or NIC hardware translation caching", or "a software thread on the target to implement a remote registration caching scheme like Firehose" [doc P4.3 §3.11].
- **Initiator side.** An MD also carries no key. SOS binds one MD with `start=0, length=PTL_SIZE_MAX` per role, which covers all of the initiator's memory, so no per-buffer registration is needed [src SOS `transport_portals4.c` L203–253, L750–763].
- **What SOS does.** Details in §8.
  - Default build: two LEs on two PTs, one over the data/BSS segment and one over the symmetric heap. The initiator computes offset = addr − base [src SOS `.h` L165–240, `.c` L713–739].
  - `--enable-remote-virtual-addressing` build: one LE with `start=NULL, length=PTL_SIZE_MAX` on PT 8. The **target virtual address is used as the offset** [src SOS `.c` L699–711; `.h` L201–206]. This needs `PTL_TARGET_BIND_INACCESSIBLE` and aborts without it [src `.c` L490–498].
  - The upshot: one registration covers the whole symmetric heap and data segment, and nothing scales with PE count.
- **The reference implementation over IB** still registers memory. LE/MD append calls `mr_lookup_app()` unless `BIND_INACCESSIBLE` is in effect [src P4RI `src/ib/ptl_le.c` L213–219]. Data movement looks up MRs per operation in a registration cache (`ptl_mem.c` L91, `ptl_init.c` L922). **ummunotify** invalidates the cache: "Not using ummunotify may result in program incorrectness" [doc P4RI README; src `ptl_mr.c`].
- **BXI.** "User processes issue their commands using virtual-address arguments, and the NIC translates them into physical addresses" [doc NET4EXA §I.A]. NP24 reports that BXIv3 "can register up to 32 million potential reception buffers" and manages "over 2 million contexts in the NIC" [doc NP24, journalism]. Whether BXI pins or faults, and how large its translation cache is, is **unverified**.
- **SeaStar (Portals 3.3).** The NIC had "no hardware support for virtual-to-physical address mapping". NIC-based mode worked only under Catamount, which gives each process a physically contiguous address space [doc CUG06 §III].

## 3. Per-peer state at 10^5–10^6 PEs

- **API level.** A logical NI (`PTL_NI_LOGICAL`) needs `PtlSetMap(ni, map_size, ptl_process_t[])`. `ptl_process_t` is an 8-byte union: `rank:uint32`, or `{nid:uint32, pid:uint32}` [src P4RI `include/portals4.h` L53–58, L98–103]. That is **8 B/PE**, or 8 MB at 10^6 PEs, as input. The map is local and may differ between processes. It may return `PTL_IGNORED` when the runtime owns the map [doc P4.3 §3.6.6]. The spec calls Portals "connectionless", with no explicit connection setup [doc P4.3 §1.4].
- **SOS.** It builds `pe_map` of `num_pes × sizeof(ptl_process_t)` from **one runtime KVS `get` per PE** ("portals4-procid") and then calls `PtlSetMap` [src SOS `.c` L590–611]. Startup is O(N) KVS lookups per PE, O(N²) in total, unless the PMI caches the data. At runtime SOS keeps **no** per-peer state: `peer.rank = pe` goes straight into every call [src `.h` L371–395]. Symmetric-address checking in the virtual-addressing build compares bases only with `(me+1) % npes`, which is O(1) [src `.c` L559–587].
- **Reference implementation.** On `PtlSetMap` it `calloc`s a rank table of `entry_t {rank, main_rank, nid, pid : 4 B each; struct conn *}`, about 24 B/rank. It also **eagerly `conn_alloc`s a `conn_t` for every rank** [src P4RI `src/ib/ptl_ni.c` L178–211; `ptl_ni.h` L18–24]. A `conn_t` holds a pthread mutex, a `sockaddr_in`, transport ops, an rdma_cm id, and several atomics [src `ptl_conn.h` L87–145]. Its size was not measured; my estimate is a few hundred bytes. The RC QP comes up lazily through rdma_cm (`rdma_create_qp` in `ptl_conn.c` L427/494/760), and an SRQ is shared for receives (`ptl_iface_ib.c` L293). At 10^6 PEs that is hundreds of MB of eager host state, plus one RC QP per active peer [inference].
- **BXI.**
  - Scale: BXIv2 "scaled up to 64k nodes" and BXIv3 targets "8M interconnected endpoints". Elsewhere the same paper says "up to 64k nodes (NICs) per Cluster and up to 128 federated clusters" [doc NET4EXA §I, §I.C.2]. NP24 gives 64,000 endpoints for v1/v2.
  - Routing state: "routing is fixed within a conversation — i.e. the combination of a process and an MPI communicator — for the in-order delivery of messages" [doc NET4EXA §I.C.2]. So the NIC or switch tracks something per conversation.
  - Whether rank→NID translation lives in NIC tables or in host memory is **unverified**. My recollection is that it is NIC-side.

## 4. Ordering and delivery

- **Defaults.** "By default, Portals provides reliable, ordered delivery of messages between pairs of processes" [doc P4.3 §1.4].
- **Short-message data ordering.**
  - Write-after-write and read-after-write ordering is per address, on the same LE, from the same initiator, for messages ≤ `max_waw_ordered_size`, which must be **at least 64 B**.
  - Write-after-read ordering covers messages ≤ `max_war_ordered_size`, which must be at least 8 B [doc P4.3 §2.7.1, §3.6.1].
  - Non-overlapping bytes of two messages are ordered only with the `PTL_TOTAL_DATA_ORDERING` feature on both ends [doc P4.3 §2.7.1].
- **Long messages** only guarantee *match order*. Bodies may land in any order, which allows partial retransmission and adaptive routing [doc P4.3 §2.7.2].
- **Fence.** The spec says Portals ordering is sufficient for `shmem_fence()` to be a no-op only with TOTAL_DATA_ORDERING and both ops < `max_waw_ordered_size` [doc P4.3 §2.7.2 Discussion].
- **Relaxing order.** Request `max_waw/war_ordered_size = 0`, or set `PTL_MD_UNORDERED`, which turns off message and data ordering for that MD [doc P4.3 §2.7.5]. SOS sets `PTL_MD_UNORDERED` on every MD unless total data ordering is on [src SOS `.c` L206, L223, L244, L753].
- **Replies and ACKs** never need ordering [doc P4.3 §2.7.1].
- **Reference implementation defaults.** `PTL_LIM_MAX_WAW_ORDERED_SIZE` and `…WAR…` default to **8** with min 8 [src P4RI `src/ib/ptl_param.c` L126–137]. That is below the spec 4.3 minimum of 64 for waw. The discrepancy is observed, not diagnosed. It probably reflects an older spec version [inference].
- **BXI.** Routes are fixed per (process, communicator) for ordering. "Minimal or non-minimal adaptive routing is guaranteed for out-of-order tolerant packets" [doc NET4EXA §I.C.2]. So ordering costs path diversity on BXI, and PGAS traffic that declares itself unordered can use adaptive routing [inference]. BXIv3 adds link-level retransmission, per-packet ACK, and transport retransmission on CRC error or timeout [doc NET4EXA §I.C.2].
- **Flow control.**
  - Without `PTL_PT_FLOWCTRL`, a message that matches nothing is dropped and `PTL_SR_DROP_COUNT` is incremented. If the EQ is full, the message is still delivered and ACKed, but the target event is lost.
  - With `PTL_PT_FLOWCTRL`, the PT is disabled, a `PTL_EVENT_PT_DISABLED` event is raised, and later messages fail with `PTL_NI_PT_DISABLED` in the initiator's ACK/REPLY until `PtlPTEnable` [doc P4.3 §2.8]. The initiator learns about flow control only if it requested ACKs [doc P4.3 §2.8 Discussion].
  - SOS does not set `PTL_PT_FLOWCTRL` (its `PtlPTAlloc` options are 0) [src SOS `.c` L650–677]. With one persistent LE covering the address space nothing is ever unmatched, so flow control is moot for RMA [inference].

## 5. Completion and progress model

- **Application bypass** is a design goal: "Portals does not require activity on the part of the application to ensure progress" [doc P4.3 §1.6]. The implementation may use hardware, a driver, or "a progress thread running in the application process" [doc P4.3 §2.1].
- **SOS completion is almost entirely counter-based.**
  - Each context has a `pending_put_cntr` and `pending_get_cntr`, which are host integers incremented *before* issuing an op (see the NOTE-MT comment), and CTs `put_ct` and `get_ct` fed by `PTL_MD_EVENT_CT_ACK` or `…CT_REPLY` [src SOS `.h` L131–147; `.c` L196–253].
  - `shmem_transport_get_wait` / blocking get calls `PtlCTWait(get_ct, pending)` [src `.h` L~680–700].
  - `shmem_transport_quiet` calls `PtlCTWait(put_ct, pending)`, looping while other threads add ops [src `.h` L253–294].
  - Success full events are disabled (`PTL_MD_EVENT_SUCCESS_DISABLE`). The single EQ holds only SEND events for bounce-buffered puts and errors [src `.c` L205, `.h` L317–366].
  - This matches the design shown in OFA12 (Put MD/Put CT, Get MD/Get CT, Target CT, Err EQ) [doc OFA12].
  - SOS needs **no progress thread**. A Sandia page describes Portals SHMEM as not requiring "a SHMEM-level progress thread or function" [doc https://www.sandia.gov/portals/projects-using-portals-4-0/ via search snippet].
- **Target-side wait.** SOS attaches a *target CT* to the heap and data LEs with `PTL_LE_EVENT_CT_COMM`, which counts incoming puts and gets [src `.c` L680–698]. `SHMEM_WAIT` / `shmem_wait_until` then blocks with `received_cntr_get` → re-check → `PtlCTWait(target_ct, n+1)` instead of spinning [src SOS `src/shmem_synchronization.h` L133–175; `.h` L1290–1330]. This applies only with `SHMEM_THREAD_SINGLE`; multithreaded builds and `--enable-hard-polling` poll instead [src `shmem_synchronization.h` L180–198]. The result is a blocking wait on "any remote write to me" with no host interrupt per message beyond the CT wakeup.
- **Reference implementation.** Every process gets a progress thread (`pthread_create(&ni->catcher, …progress_thread…)` [src P4RI `src/ib/ptl_recv.c` L1065]), plus a libev event-loop thread [src `ptl_fat_lib.c` L257]. The README says this "leads to the use of too many cores on a machine". The alternative is the PPE daemon, which shares one progress thread per host and needs XPMEM [doc P4RI README]. Target-side atomics, gets, and matching run in that software thread [inference from structure: `ptl_tgt.c`, `ptl_atomic.c`].
- **BXI.** It "permits a complete offload of communication primitives in hardware, thus enabling independent progress of computation and communication" [doc, abstract of the BXI papers quoted in search results for https://dl.acm.org/doi/10.1109/HOTI.2015.15]. But Bull's new Open MPI OSC says that for passive-target locks "the target process still needs to call `ubcl_progress` to actively handle the request to establish the *lock*" [doc OMPI-UBCL README "Passive sync"]. So BXI's *software* stack (UBCL) has host-driven progress paths for some semantics.

## 6. Thread and context model; hardware cost of a context

- **Thread safety.** All Portals calls are thread-safe and atomic per call. Handles belong to processes, not threads. The API has no thread IDs [doc P4.3 §2.9, §3.15.1 Discussion]. Contexts are not an API concept: the unit is NI + MD + CT + EQ.
- **SOS context = 3 MDs + 2 CTs on one shared NI, one shared EQ, and shared PTs/LEs** [src SOS `.c` L181–270]:
  - `put_md`
  - `put_volatile_md`
  - `get_md`
  - `put_ct`
  - `get_ct`

  Global objects on top of that: 1 EQ with 2048 slots, 2 PTs, 2 LEs (1 LE with virtual addressing), 1 target CT, 1 bounce-put MD, and 1 bounce-put CT [src `.c` L126–153, L640–763].
  - SOS requests `max_mds = max_cts = max_eqs = max_entries = 1024` and `max_pt_index = 64` [src `.c` L456–471]. So **≈340 contexts** fit before `PtlMDBind` fails at the requested limits [inference: 1024 / 3 minus globals]. Real limits come from `ni_limits`.
  - Context options (`SHMEM_CTX_PRIVATE`/`SERIALIZED`) are ignored; there is no grep hit in the transport. All contexts share process-global mutexes: `ptl4_ctx`, `ptl4_pt_state`, `ptl4_frag`, `ptl4_event_slots` [src `.c` L168–173]. The Portals transport therefore gives *no* per-context hardware isolation. Contexts only separate completion accounting.
- **SOS team and counter objects (`shmemx_ct_*`).** Each one allocates a **distinct PT index**, or two without virtual addressing, from a fixed 32-entry table, 22 of which are initially free. Each gets its own LE over heap and data, bound to a user CT [src SOS `.h` L47, L1127–1215; `.c` L87–124]. That caps the number of counter objects at roughly 11–22, and each allocation must happen collectively in the same order on all PEs [src comment at `.h` L1140–1146].
- **BXI.** NP24 reports "over 2 million contexts in the NIC" for BXIv3 (journalism, definition of "context" unknown). NET4EXA says the BXIv3 NIC has 8 VCs and the switch has 16 [doc NET4EXA §I.C.1]. Hardware counters are finite on offload NICs [doc HWCNT].

## 7. GPU memory support

- **Portals 4.3 spec.** No mention of GPU, accelerator, or device memory (grep of the spec text) [doc P4.3]. With `BIND_INACCESSIBLE` an LE covers virtual addresses, so GPU memory would depend entirely on how the implementation translates addresses [inference].
- **SOS Portals transport.** No GPU support [src, no references].
- **Reference implementation.** None [src].
- **BXIv3.** "in addition to virtual-to-physical address translation, the BXIv3 NIC will facilitate direct interactions with accelerators such as GPUs from NVIDIA, AMD, and Intel… zero-copy… direct access to GPU memory buffers" [doc NET4EXA §I.C.1]. The work package plans "a new Portals transport layer, enabling GPU-to-GPU communications across major GPU vendors, exploring GPU-triggered communications" and "triggered operation extensions" [doc NET4EXA §II.B]. These are plans (TRL-8 target), not shipped features.

## 8. What OpenSHMEM implementations built on Portals had to emulate in software (most important)

Primary evidence is SOS `src/transport_portals4.{c,h}`. The file dates from a 2012 split ("Start split of Portals4 from SHMEM core", commit `391b91fc`) and is essentially the same in v1.4.5 and on current `main`: `git diff --stat v1.4.5 HEAD` shows 14 insertions and 2 deletions. Line numbers below refer to `main` @ `4aa91d7f`.

### 8a. Mapped natively (no emulation)

| OpenSHMEM | Portals primitive | Source |
|---|---|---|
| Symmetric heap + data exposure | 1 LE (`NULL`, `PTL_SIZE_MAX`) with `BIND_INACCESSIBLE`, else 2 LEs on 2 PTs; non-matching logical NI (`PTL_NI_NO_MATCHING \| PTL_NI_LOGICAL`) | `.c` L479–484, L699–739 |
| Addressing a PE | `ptl_process_t.rank = pe`, no lookup | `.h` L371–395 |
| Small blocking put (≤ `max_volatile_size`) | `PtlPut` on the `PTL_MD_VOLATILE` MD, ACK counted in a CT, no copy by SOS | `.h` L371–395 |
| Put / get, large | `PtlPut` / `PtlGet` zero-copy; completion by `PtlCTWait` | `.h` L520–700 |
| `atomic_add/inc`, bitwise AMOs, `atomicv` ≤ volatile size | `PtlAtomic(SUM/BOR/BAND/BXOR…)` | `.h` L862–893 |
| `atomic_fetch_add/inc/…` | `PtlFetchAtomic` | `.h` L1022–1058 |
| `atomic_swap` / `compare_and_swap` / masked swap | `PtlSwap(PTL_SWAP / PTL_CSWAP / PTL_MSWAP)` | `.h` L720–860 |
| `atomic_set` / `atomic_fetch` | Plain `PtlPut` / `PtlGet`, relying on ordering and datatype-granular atomicity | `.h` L1062–1090 |
| `shmem_quiet` / local and remote completion | Host pending counter vs `PtlCTWait` on ACK/REPLY CTs | `.h` L253–294 |
| `shmem_wait_until` blocking | Target CT on LE (`PTL_LE_EVENT_CT_COMM`) + `PtlCTWait` | `.c` L680–698; `shmem_synchronization.h` L133–175 |
| Datatypes, including complex and long double | Direct table to `PTL_*` datatypes | `.c` L35–85 |
| Locks (`shmem_set_lock`/`clear_lock`) | Distributed queue lock built entirely from remote `swap`/`cswap`/`mswap` + `atomic_fetch` + `SHMEM_WAIT` | `src/shmem_lock.h` L40–130 |
| Reductions (linear and tree algorithms) | Children issue remote `PtlAtomic` (`atomicv`) straight into the root's buffer; `shmem_transport_atomic_supported()` returns 1 for every op/type | `src/collectives.c` L582–640, L833, L1086; `.h` L1102–1110 |

### 8b. Emulated or worked around in software

1. **Buffering for nonblocking puts beyond `max_volatile_size`.** `PTL_MD_VOLATILE` covers only lengths ≤ `max_volatile_size`, and the reference implementation's default is **8 B** (`ptl_param.c`, `PTL_LIM_MAX_VOLATILE_SIZE .val = 8`). SOS therefore copies puts up to `SHMEM_BOUNCE_SIZE` into a bounce buffer from a free list and sends them via a separate `put_event_md` with a full SEND event so the buffer can be freed [src `.h` L401–470]. SOS also rations **EQ slots by hand**: a global counter starts at 2048, and it drains the EQ when the counter is exhausted [src `.c` L153; `.h` L317–366, L436–441]. Longer puts use a "long frag" object carrying a user completion pointer and reference count [src `.h` L472–515]. *The core API did not offer an initiator-side "copy and forget" for arbitrary sizes, or completion without host EQ traffic.*
2. **Chunking atomics beyond `max_atomic_size`.** `shmem_transport_atomicv` splits a vector reduction into multiple `PtlAtomic` calls of at most `max_atomic_size` each, tracked with a long frag. It holds a mutex across the whole loop "to prevent another thread from seeing the incomplete frag" [src `.h` L966–1010]. The reference implementation's `max_atomic_size` / `max_fetch_atomic_size` default and cap is **512 B** [src P4RI `ptl_param.c` L112–125]. Fetch-atomic and swap assert `len ≤ sizeof(long double _Complex)` and ≤ `max_volatile_size` [src `.h` L735–737, L1036–1037]. At startup SOS aborts if any of volatile, atomic, or fetch-atomic size is below `sizeof(long double _Complex)` [src `.c` L624–638].
3. **`shmem_fence` becomes `shmem_quiet`** unless the NI provides `PTL_TOTAL_DATA_ORDERING`. Even then SOS still quiets after any "long" (non-volatile) put [src `.h` L298–314]. The configure option `--enable-total-data-ordering=yes|no|check` exists for this [doc SOS README]. Fence is therefore a full ACK round-trip wait on most implementations.
4. **Put-with-signal (1.5).** It is implemented as `put_nbi` → `fence` (so quiet) → `PtlAtomic(SUM)` or `put_scalar` of the signal. The source says `FIXME: Need to optimize non-blocking put with signal for Portals… keeps the "fence" in between data and signal put` [src `.h` L1113–1125]. *There was no compound "write data, then atomically update a flag" primitive.* A triggered op keyed on the data put's ACK CT could have expressed it [inference].
5. **Remote virtual addressing checks.** The one-LE mode requires the heap and data bases to be *identical* virtual addresses on all PEs. SOS verifies this only against one neighbour through the runtime KVS and aborts on mismatch [src `.c` L530–587]. Without it, every op computes a PT and offset by comparing against the heap base, which is an extra branch and subtraction per op [src `.h` L165–240].
6. **Nonblocking get and AMO completion granularity.** No per-op handle exists. `get_nbi`, `swap_nbi`, `cswap_nbi`, and `fetch_atomic_nbi` all reuse the blocking-issue path, and completion is "all gets on this ctx" via the counter [src `.h` L760–775, L1062–1075]. `shmem_pe_quiet` (1.6) has no Portals-specific implementation (not found in transport).
7. **Counter objects (`shmemx_ct_*`) and target-side notification.** One PT index per counter from a 32-entry, collectively allocated table [src `.h` L1127–1215]. Portals has no cheap way to attach many CTs to sub-ranges of one LE.
8. **Error handling.** Any `ct.failure != 0` or `ni_fail_type != PTL_OK` raises a fatal error (`RAISE_ERROR`, with "TODO: Handle failures gracefully") [src `.h` L271–277, L1246–1250]. There is no retry, and no handling of `PTL_NI_PT_DISABLED` or `PTL_NI_SEGV`.
9. **Atomic cache visibility.** `shmem_transport_syncmem()` → `PtlAtomicSync()` runs at lock acquisition and at finalize, because the NI may not provide `PTL_COHERENT_ATOMICS` [src `.h` L1352–1355; `shmem_lock.h` L117–120, L144; `.c` L780–781]. The reference implementation's `_PtlAtomicSync` body is `/* TODO */` [src P4RI `src/ib/ptl_move.c` L925–933]. Bull's UBCL OSC documents the same issue: "When the data modifications are not fully handled by the NIC, some counterproductive caches must be cleaned at the start of one-sided exposure epochs" [doc OMPI-UBCL README "Sync"]. **Any core API that offloads AMOs to a NIC needs this sync point.** OpenSHMEM 1.5 also says `shmem_wait_until` variables "must be updated using OpenSHMEM AMOs" [doc spec16.txt Annex G.2 item 17].
10. **Everything collective, team, and sync-related is portable software** above put/get/atomic. SOS uses no triggered ops, bundles, MEs, flow control, EQ polling, or `PtlAbort` (grep over `src/` finds none).
    - Broadcast, alltoall, gather, and team split are generic.
    - Reductions use network atomics for linear and tree algorithms, and software recursive doubling or ring otherwise.
    - Barrier-like handshakes use `pSync` scalars plus `SHMEM_WAIT` [src `collectives.c` L582–640].
    - Triggered-op collective offload was researched separately (TRIG10; Underwood et al., HOTI 2011 [doc OFA12]) but never landed in SOS.
11. **Intranode** optimisation (XPMEM, CMA) is a separate SOS layer. The reference implementation has its own shmem/KNEM/XPMEM paths [doc SOS README; P4RI README].
12. **Other Portals-based OpenSHMEMs.**
    - Open MPI OSHMEM ships only `spml/ucx` today [src https://github.com/open-mpi/ompi/tree/main/oshmem/mca/spml, API listing].
    - Open MPI still carries `mtl/portals4`, `osc/portals4`, `coll/portals4`, and `btl/portals4` [src API listing of `ompi/mca/*`, `opal/mca/btl`].
    - Whether OSHMEM ever ran over `btl/portals4` via the removed `yoda` SPML is [recalled, unverified].
    - GASNet 1.20.2+ had Portals 4 core support [doc Sandia projects page]. Its later removal from GASNet-EX is [recalled].
    - Cray SHMEM on SeaStar/Portals 3.3 is [recalled].
    - Eviden's own OpenSHMEM story on BXI is **unverified**. No source found. The 2025 upstream contribution is MPI-only (`pml/ubcl`, `osc/ubcl`) [doc OMPI-UBCL].

Summary of item 8 [inference]: with a non-matching logical NI, one address-space LE, and ACK/REPLY counters, the performance-critical path of OpenSHMEM needs **no software emulation** for scalars, bulk put/get, or AMOs. That includes CAS and masked swap, which nearly every other network emulates with an AM or a lock. The software additions all come from **size limits** (volatile, atomic, and fetch-atomic sizes), **completion granularity** (per-context counters rather than per-op handles), **ordering** (fence = quiet), **compound ops** (put-with-signal), **the atomic sync point**, and **scarce indexable resources** (PT indices, MDs per context, EQ slots).

## 9. Known performance pathologies and scaling limits

- **Reference implementation defaults are tiny.** `max_volatile_size = 8`, `max_waw/war_ordered_size = 8`, `max_atomic_size = 512` (cap 512), `max_pt_index = 63` (cap 511). This forces SOS onto bounce-buffer and quiet paths for anything bigger than a scalar [src P4RI `src/ib/ptl_param.c` L70–143].
- **Reference implementation progress.** One progress thread per process plus an event-loop thread. The README says it uses "too many cores". IB transport correctness depends on the out-of-tree **ummunotify** module. Only one NID per program lifetime is allowed [doc P4RI README]. Every rank gets a `conn_t` eagerly at `PtlSetMap` [src `ptl_ni.c` L178–211]. UDP/rUDP is "experimental" [doc P4RI README].
- **Offload moves the heavy work off the host, but results are only visible after `PtlAtomicSync`** unless atomics are coherent [doc P4.3 §3.15.4].
- **Hardware counter scarcity** limits triggered-op collectives. When counters run out, collectives stall, and a counter cache can thrash [doc HWCNT].
- **SOS on Portals.**
  - Fence is a quiet.
  - One global EQ with 2048 slots, drained under a global mutex, sits on the bounce path.
  - All contexts share one NI and four global mutexes, so thread scaling is limited by the host.
  - Roughly 340 contexts fit at the requested `max_mds = 1024`.
  - About 11–22 `shmemx_ct` objects fit.
  - Startup does O(N) KVS gets per PE [src, sections above].
  - Message rate on shared memory was ~7.2 M msg/s for both "Portals LE/CT" and SHMEM on a 3.33 GHz Westmere. Half round-trip latency was 0.43 µs for raw Portals vs 0.39 µs for SHMEM [doc OFA12 "Results" slides]. SHMEM adds essentially no overhead over raw Portals.
- **Ordering vs adaptive routing on BXI.** Ordered traffic is pinned to one route per conversation. Only out-of-order-tolerant packets get adaptive routing [doc NET4EXA §I.C.2].
- **Scale caps.** BXIv1/v2 cap at 64k nodes [doc NET4EXA; NP24]. BXIv3 aims for 8M endpoints via federation of up to 128 clusters of 64k NICs [doc NET4EXA].
- **Red Storm lessons.**
  - Posting receives costs a host↔NIC round trip.
  - Matching was too heavy for PGAS, so Portals 4 added the non-matching interface and lightweight events and ACKs [doc P4.3 §1.3].
  - SeaStar's host-based mode took an interrupt per message.
  - Its NIC-based mode ran on a 500 MHz PowerPC 440 with 384 KB of scratch memory and no V→P translation, so it worked only on Catamount [doc CUG06 §II–III].
- **Flow control.** Recovery is by design "slow, but must be possible". Local operations and responses may overflow the EQ and drop events [doc P4.3 §2.8].

## 10. Build, portability, maintenance burden

- **Spec.** It is healthy but low-churn: 4.0 (2012) → 4.1 → 4.2 → 4.3 (2022) [doc OSTI entries in search results; P4.3 App. C]. 4.3 added unreliable puts and `PtlAbort` [doc P4.3 App. C.1].
- **Reference implementation.** Last commit Sep 2024 (`PtlAbort`) [src P4RI git log]. It needs libev, the ummunotify kernel module for IB correctness, XPMEM for the PPE mode, and optionally KNEM [doc P4RI README]. The fat library and light (PPE) library share the same soname and cannot coexist [doc P4RI README]. It is effectively a research vehicle.
- **SOS Portals transport.**
  - CI: Travis Portals builds disabled Feb 2019 (`432390ed`), and the GitHub workflow Portals row is commented out [src SOS `.github/workflows/*.yml` L316–317].
  - Last Portals-specific functional commits: put-signal (2018/2020) and an issue-1052 fix (Apr 2022) [src git log].
  - The transport is still in the tree and builds exclusively; configure refuses more than one network transport [src SOS `configure.ac` L457–495].
  - About 2.2 kLOC (798 + 1431 lines) [src `wc -l`]. It is the smallest network transport in SOS, which reflects how directly Portals semantics fit OpenSHMEM [inference].
- **BXI software.** Proprietary drivers and firmware. Bull upstreamed `pml/ubcl` and `osc/ubcl` into Open MPI in Oct 2025, built with `--with-ubcl` against an external UBCL library [doc OMPI-UBCL]. BXI's MPI path now goes through UBCL rather than the generic Portals 4 MTL/OSC components [inference]. BXIv3's NIC is FPGA-based, and the ASIC is planned for v4 [doc NET4EXA §I.B]. The NET4EXA text calls the BXIv3 NIC "a full hardware implementation of the Portals 3 interface over Ethernet" in one place and "Like previous BXI versions, the NIC will implement the Portals 4 API" in another. The first is likely a typo [doc NET4EXA §I.C.1, §I.C.3; inference].
- **Cray SeaStar** used Portals 3.3 and is historical [doc CUG06; P4.3 §1.3]. Cray's later Gemini/Aries used uGNI/DMAPP, and HPE Slingshot/Cassini uses libfabric CXI with Portals-like hardware matching and LE semantics [recalled].

---

## Implications for a core API

**Cheap on this substrate:**
- **Keyless, connectionless RMA addressed by (rank, offset).** Remote state is O(0) per PE pair. An address-space-wide or symmetric-segment-wide exposure object costs one LE per process, and the initiator side is one MD over all memory. The core API should let one *region handle* be implicit, derived from rank plus virtual address, with no per-PE keys [src SOS; doc P4.3 §3.11].
- **Remote atomics executed at the target**, including CAS and masked swap, with a datatype argument. They carry OpenSHMEM AMOs, locks, and reductions with no host involvement at the target [src SOS `shmem_lock.h`, `collectives.c`]. They belong in the core, as `atomic(op, dtype)`, `fetch_atomic`, and `swap(kind)`, with a queryable max size and a documented `sync()` visibility point.
- **Counter-based completion.** `{success, failure}` counters bumped by ACK/REPLY, with wait-until-count. This gives quiet, blocking get, and pending counts without an event per op. A **target-side counter on the exposure region** gives a blocking `shmem_wait_until` with no polling thread [src SOS].
- **Unordered-by-default sends** (`PTL_MD_UNORDERED`) that let hardware spray adaptively, with ordering as an explicit opt-in [doc P4.3 §2.7.5; NET4EXA].
- **Initiator "volatile" small puts.** The NIC or library copies before return, so small blocking puts need no user completion tracking [doc P4.3 §3.10].

**Expensive or awkward:**
- **Size ceilings.** Volatile, atomic, and ordered sizes are implementation-defined and can be tiny (8 B / 512 B in the reference implementation). The layer above then needs bounce buffers, chunking, and EQ-slot rationing. A core API should *expose* these limits and let the extended layer fall back [src SOS, P4RI].
- **Fence and ordering.** Without total data ordering, fence needs a quiet (round-trip wait). If the core API offers an ordering mode, it constrains routing on real hardware such as BXI [src SOS; doc P4.3 §2.7.2; NET4EXA].
- **Per-operation completion handles.** These are absent. Contexts are MD+CT bundles on one shared NI and EQ, so they give no hardware isolation and only a few hundred fit at the requested limits [src SOS]. A core API needs a per-context hardware queue concept that Portals 4 lacks.
- **Compound ops such as put-with-signal.** Triggered ops can express them, but no SHMEM used them, and hardware CTs are scarce [src SOS FIXME; doc HWCNT]. It is better to put a narrow "put + atomic-on-ACK" primitive in the core than general triggers.
- **Address-space LEs need `BIND_INACCESSIBLE`, identical virtual addresses across PEs, and NIC translation or an ODP-like mechanism.** Otherwise registration caches (ummunotify) or per-segment LEs are needed [doc P4.3 Impl. Note 9; src P4RI].
- **Rank maps.** 8 B/PE in the API, but eager per-rank connection objects in software implementations and O(N) KVS setup [src P4RI, SOS]. The core API should accept a runtime-provided map lazily or by callback.
- **Failure semantics.** Collapsing to success/failure counts loses *which* op failed. SOS aborts on any failure [src SOS].
- **GPU memory.** Not in the Portals 4 spec. It is only planned for BXIv3 [doc NET4EXA].
