# Q2 survey: future networks (Ultra Ethernet Transport; CXL / fabric-attached memory and OpenFAM)

Scope: (a) Ultra Ethernet Consortium specification and its UET transport, (b) CXL 2.0/3.x memory pooling and sharing, and the OpenFAM API and reference implementation, both judged as substrates for OpenSHMEM 1.4-1.6 at 10^5+ PEs.

## How claims are labelled

- *documented* (URL, section): read in a primary document during this survey.
- *observed in source* (URL + file): read in source code during this survey.
- *recalled*: from memory, not checked this session. Treat these as unverified.
- *inference*: my own reasoning from the documented facts.

## Primary sources used

- **[UEC]** Ultra Ethernet Specification v1.0.2, 28 Jan 2026 (CC BY-ND 4.0). https://ultraethernet.org/wp-content/uploads/sites/20/2026/01/UE-Specification-1.0.2-1.pdf. I downloaded it and read it as text. Section and table numbers below refer to this version. v1.0.1 (Oct 2025) is at …/2025/10/UE-Specification-1.0.1.pdf. The survey brief says "UEC 1.0, 2025"; 1.0.2 is the current errata release.
- **[libfabric]** ofiwg/libfabric main branch at commit 056b8ae (16 Sep 2026), sparse checkout. Also the `fi_msg(3)` man page: https://ofiwg.github.io/libfabric/main/man/fi_msg.3.html
- **[SOS]** Sandia-OpenSHMEM/SOS main branch at commit 4aa91d7 (27 Aug 2025). https://github.com/Sandia-OpenSHMEM/SOS
- **[uet-ref-prov]** https://github.com/ultraethernet/uet-ref-prov. README only.
- **[SUET]** https://github.com/spcl/suet. README only.
- **[CXL3WP]** CXL Consortium, "Compute Express Link 3.0" white paper, by Das Sharma and Agarwal. https://computeexpresslink.org/wp-content/uploads/2023/12/CXL_3.0_white-paper_FINAL.pdf. I did not have the CXL base specification itself; it is members-only.
- **[cxlalloc]** Ni, Sun, Zhu, Witchel, "Cxlalloc: Safe and Efficient Memory Allocation for a CXL Pod", ASPLOS '26. https://www.cs.utexas.edu/~witchel/pubs/ni26asplos-cxlalloc.pdf
- **[OpenFAM-CUG22]** Singhal et al., "OpenFAM: Programming Disaggregated Memory", CUG 2022. https://cug.org/proceedings/cug2022_proceedings/includes/files/pap102s2-file1.pdf
- **[OpenFAM-src]** OpenFAM/OpenFAM at commit 2dfb164 (16 Sep 2024), VERSION 3.2.0. https://github.com/OpenFAM/OpenFAM. The older API header OpenFAM/API `fam.h` (API v1.04) is at https://github.com/OpenFAM/API/blob/master/fam.h
- **[spec16]** OpenSHMEM 1.6 spec text, from the local briefing copy.

---

# Part A. Ultra Ethernet Transport (UET)

## A.0 Architecture in one paragraph

UET is a transport over IP/UDP (or directly over IP). It has four sublayers:

- **SES** (semantic sublayer): libfabric-shaped operations such as send, tagged send, RMA read/write, atomic, rendezvous and deferrable send.
- **PDS** (packet delivery sublayer): reliability and ordering.
- **CMS** (congestion management sublayer): UET-CC, NSCC or receiver-credit congestion control, and multipath entropy.
- **TSS** (transport security sublayer).

*documented* [UEC §3.2]. The software API is defined as a mapping onto libfabric v2.0 (`FI_PROTO_UET`, `FI_ADDR_UET`, endpoint type `FI_EP_RDM` or `FI_EP_DGRAM`) [UEC §2.2, Table 2-4]. There are three profiles, in increasing capability: **AI Base ⊂ AI Full**, and **AI Base ⊂ HPC**. HPC is not a superset of AI Full: deferrable send is the only AI Full capability HPC lacks [UEC §2.2.2.1]. Table 2-5 maps "SHMEM" to "Either AI Full or HPC" [UEC Table 2-5, documented].

## A.1 Primitives relevant to one-sided PGAS

### Profile feature matrix

*documented*, UEC Table 2-4 (fi_info caps), Tables 2-25/2-26 (RMA and atomic APIs), Table 2-20 (counters):

| Capability | AI Base | AI Full | HPC |
|---|---|---|---|
| FI_RMA, fi_write (UET_WRITE) | Y | Y | Y |
| fi_read (UET_READ), FI_REMOTE_READ | **no** | Y | Y |
| Non-fetching fi_atomic (UET_ATOMIC), bulk | Y | Y | Y |
| Fetching fi_fetch_atomic (UET_FETCHING_ATOMIC), 1 element only | **no** | Y | Y |
| fi_compare_atomic (CSWAP family) | no | no | **Y** |
| FI_TAGGED exact match | no | Y | Y |
| FI_TAGGED wildcard, FI_DIRECTED_RECV | no | no | Y |
| Deferrable send | no | Y | no (treated as send) |
| FI_FENCE | Y | Y | Y |
| FI_RMA_EVENT | Y | Y | Y |
| FI_HMEM | Y | Y | Y |
| Completion counters (fi_cntr) | none | SEND, RECV, READ, WRITE | also REMOTE_READ, REMOTE_WRITE |
| RUDI delivery mode | no | no | **Y** |
| max_order_{raw,war,waw}_size | 0 | 0 | 0 (no data ordering at all) |
| Max RMA message size | 1 GB ≤ x ≤ 4 GB-1 | same | same |

Consequences for OpenSHMEM:

- **AI Base has no native read.** `shmem_get` would have to be emulated with a send-based active message or a rendezvous "pull" built from sends (*inference*).
- **AI Base has no fetching atomics**, so it cannot do fetch, fetch_inc or compare_swap natively.
- **Only HPC has compare-and-swap** (`fi_compare_atomic`), which `shmem_atomic_compare_swap` needs.
- Fetching atomics are limited to "a single unit of the indicated data type" [UEC §2.2.5.4.4]. That matches OpenSHMEM, whose atomics are all single-element (*inference*).

### Atomic operations

The operation set is UET_AMO_MIN, MAX, SUM, DIFF, PROD, logical and bitwise AND/OR/XOR, READ, WRITE, CSWAP (plus NE, LE, LT, GE, GT variants), MSWAP, and INVAL (cache invalidate), plus vendor ops 0xE0-0xFE [UEC §3.4.6, lines ~9306-9331 of text, documented].

Every OpenSHMEM atomic has a direct opcode (*inference*):

| OpenSHMEM | UET |
|---|---|
| set | AMO_WRITE |
| fetch | AMO_READ (fetching) |
| inc, add | SUM |
| fetch_inc, fetch_add | fetching SUM |
| compare_swap | fetching CSWAP |
| and, or, xor | BAND, BOR, BXOR |
| fetch_and, fetch_or, fetch_xor | fetching BAND, BOR, BXOR |

Atomic consistency rules [UEC §3.4.1.5.4, §3.4.8.2, documented]:

- Atomicity is guaranteed at single-element granularity, scoped to one `{FA, JobID, PIDonFEP, Resource Index, Memory Key, Offset}`.
- Concurrent atomics from different FEPs must be consistent with each other.
- Consistency between atomics and plain send/write/read is **not** required: "Users needing write or read semantics that are consistent with atomic operations MUST use the atomic versions".
- Coherence with the target CPU's own load/store is optional, requested with `UET_AMO_CPU_COHERENT`; the target may return `RC_AMO_UNSUPPORTED_OP`.
- For OpenSHMEM, where a target PE may read the same symmetric variable locally while a remote PE does `shmem_atomic_inc`, CPU-coherent atomics are only vendor-optional (*inference*).

### Other primitives

- **Triggered operations and collectives.** `fi_trigger` is explicitly not required [UEC Table 2-27]. There is no collective offload beyond the libfabric collective API mention [UEC §2.2.5.4.5].
- **Active messages.** There is no AM primitive. Send, receive and tagged sends with header_data (64-bit CQ data) are the building blocks [UEC §3.4.1.9].

## A.2 Memory registration and remote keys

### Key format

UEC Table 2-13, *documented*. Keys are 64 bits:

| Bits | Field | Meaning |
|---|---|---|
| 63 | `IDEMPOTENT_SAFE` | The MR may be the target of idempotent (RUDI) transport; this implies no completion counters are bound to it |
| 62 | `OPTIMIZED` | The MR can be addressed by the small "optimized non-matching" header |
| 56:61 | reserved | |
| 48:55 | vendor | |
| 0:47 | RKEY | 48 bits |
| 0:11 | RKEY/INDEX | Used instead when OPTIMIZED=1; a **12-bit index, so at most 4096 MRs** in the optimized form |

Rules on keys [UEC §2.2.5.3.4, documented]:

- `mr_key_size ≤ 6` bytes.
- `FI_MR_ENDPOINT` is required.
- `mr_cnt ≥ 8`.
- A UET provider "SHOULD support user-assigned keys and therefore SHOULD NOT require FI_MR_PROV_KEY". The informative text notes that well-known keys let an application avoid exchanging keys.
- Binding a completion counter to an IDEMPOTENT_SAFE MR "MUST fail".

RKEY scope [UEC Table 2-17]:

- Optimized header, relative addressing: `{FA, JobID, PIDonFEP}`.
- Other headers: `{FA, JobID, PIDonFEP, Index}`.
- A key is local to the target process. With user-assigned keys, one well-known key per symmetric segment (data, heap, and maybe device heap) works for every PE. That gives **O(segments) keys, independent of PE count, with no key exchange** (*inference*, following the spec's informative text).
- One registration can cover a whole symmetric heap: `mr_iov_limit ≥ 1` and RMA messages up to 4 GB-1. The standard header carries a 64-bit `buffer_offset` [UEC Table 3-8].
- A symmetric heap larger than one MR requires the provider to accept large registrations. Nothing in the spec forbids that (*inference*).

### Header sizes

Standard SES request header fields [UEC Table 3-8, documented]:

| Field | Bits |
|---|---|
| opcode | 6 |
| message_id | 16 |
| ri_generation | 8 |
| JobID | 24 |
| PIDonFEP | 12 |
| resource_index | 12 |
| buffer_offset | 64 |
| initiator | 32 |
| match_bits / memory_key | 64 |
| header_data | 64 |
| request_length | 32 |

A smaller "small RMA" header and an "optimized non-matching" header are used when the operation is ≤ MTU and FI_REMOTE_CQ_DATA is not set [UEC Tables 2-14, 2-15]. The payload MTU is 1024, 2048, 4096 (mandatory, default) or 8192 [UEC §3.4.1.11].

## A.3 Per-peer state and scaling to 10^5-10^6 PEs

**Addressing.**
- Relative addressing is `{FA, JobID(24 b), PIDonFEP(12 b), Resource Index(12 b)}`. So there are **at most 4096 PIDonFEPs per FEP**, per job in relative mode [UEC §1, §3.4.1.3, Table 3-8].
- The informative text: a source needs a table of K-1 FEP entries and the destination a table of P PIDonFEPs. That is O(K+P) instead of O(K·P), with "K … in the tens of thousands, P in the hundreds" [UEC §1, Figure 1-9 text].
- The libfabric UET address struct is `{ver, flags, fep_cap(16), pid_on_fep(16), fa (IPv4 4 bytes or IPv6 16 bytes), start_resource_index(16), num_resource_indices(16), initiator_id(32)}` [UEC Figure 2-8]. That is about 36 bytes for IPv6 (*inference* from the struct).
- `FI_AV_TABLE` is required. The informative text suggests inserting addresses in rank order so that AV index = rank [UEC §2.2.5.3.8].
- The spec does not require `FI_AV_USER_ID` or a compressed AV. A naive AV is still O(N_PE) × about 36-40 bytes: about 4 MB at 10^5 PEs and 40 MB at 10^6 per process. A job-relative algorithmic AV (FA table plus a PIDonFEP computation) is permitted by the addressing model, but it is a provider implementation choice (*inference*).
- Overall claim: "supports up to millions of endpoints (NIC ports) with the ability to address up to billions of processes" [UEC §2 intro, documented].

**Connections: PDCs are ephemeral** [UEC §3.2.2, §3.5.8, documented].
- "Reliability state is required only between peers with ongoing communications."
- PDCs are created on demand with zero extra round trip. The SYN flag rides on the first data packet.
- They are closed on idle or resource pressure. All state is cleared on close except a stored "last PSN used" [UEC §3.5.8.2 informative].
- A PDC holds TX and RX state in both directions (PSNs, ACK bitmaps).
- **RUD PDCs may be shared** by every process pair between two FEPs. The mandatory initiator tuple is `{src FA, dst FA, TC, RUD/ROD}`, plus SDI when encryption is on.
- **ROD** forces one PDC per `{JobID, src PIDonFEP, dst PIDonFEP, RI, TC}`.
- A target "MUST support as many PDCs as the initiator attempts to create, up to an implementation-specific limit". No numeric limit is given.

**Delivery modes without PDCs.** RUDI and UUD use no PDC at all [UEC §3.5.6].
- RUDI "requires no PDS or SES state at the target". All state is at the initiator: an RTO timer per packet and a pkt_id [UEC §3.5.7.3, §3.5.22.1].

**Congestion control state.** A CCC is keyed by `{dst FA, DSCP}`. All RUD PDCs between an FEP pair in a TC share one CCC. Each ROD PDC gets its own CCC_ID [UEC §3.5.8.1].

**What grows with PE count** (*inference*):
- The AV, unless algorithmic.
- Nothing else in the transport. PDC and CCC count grows with *concurrently active FEP pairs*, and with active process pairs for ROD.
- With ROD for everything (see A.4), per-process-pair PDC state returns for every actively communicating pair. That is the main scaling hazard of using ROD for OpenSHMEM ordering.

## A.4 Ordering and delivery: which UET mode OpenSHMEM operations use

### The four PDS modes

*documented* [UEC §3.2.2, §3.5.7]:

| Mode | Guarantee | Target state | Loss recovery | Multipath | UET-CC |
|---|---|---|---|---|---|
| ROD | exactly once, in order | PDC | GoBackN | **single path** (single entropy value, changed only when idle or ≥ t_reroute ≥ 10 RTTs) | yes |
| RUD | exactly once, any order (dedup by PSN) | PDC | selective retransmit | sprayed (oblivious or path-aware, typically ≥ 256 EVs) | yes |
| RUDI | **at least once**, any order, no dedup | **none** | initiator RTO per packet, NACK on trim | sprayed | **no** |
| UUD | best effort | none | none | sprayed | no |

Multipath sources: [UEC §3.6.16, §3.6.16.1-2].

### Mode selection for the HPC profile

UEC Table 2-31, *documented*:

| Send ordering | R/W ordering | SEND/TSEND | RENDEZVOUS | **RMA** | **ATOMIC** |
|---|---|---|---|---|---|
| N | N | RUD | RUD | **RUD / RUDI** (vendor policy) | **RUD** |
| N | Y | RUD | RUD | ROD | ROD |
| Y | N | ROD | ROD eager, RUD remainder | RUD / RUDI | RUD |
| Y | Y | ROD | ROD eager, RUD remainder | ROD | ROD |

AI Base and AI Full use the same table without RUDI [Tables 2-29, 2-30].

"R/W ordering" means any of FI_ORDER_RAR, RAW, WAR, WAW, ATOMIC_*, RMA_*, SAR, SAW, WAS or RAS [UEC §2.2.6]. RUDI may be used for RMA only if all of these hold [UEC Table 2-16]:
1. the endpoint is not configured for R/W ordering;
2. the MR key has IDEMPOTENT_SAFE=1;
3. the destination FEP capability field advertises RUDI.

### Mapping OpenSHMEM operations

*inference*, from the tables above:

- **put / get / putmem / getmem / iput / iget, blocking and non-blocking:** fi_write and fi_read on **RUD**. Use **RUDI** only if the symmetric-heap MR has IDEMPOTENT_SAFE set, which forbids a remote-write counter on that MR, and the endpoint requests no RMA ordering.
- **All atomics (set, fetch, inc, add, cswap, and, or, xor, and the fetch variants):** always **RUD**, or ROD if R/W ordering is requested. **Never RUDI.** The spec says so directly: "fi_atomic() would be exposed to delivering the operation to the target twice" [UEC §3.4.1.13]. The RUD definition gives "atomic add" as its example non-idempotent operation [§3.2.2].
- **put_with_signal (OpenSHMEM 1.6):** a put on RUD or RUDI, then a non-fetching atomic (AMO_WRITE or SUM) on RUD, with `FI_FENCE` on the atomic (see below). The UEC text itself names "OpenSHMEM put-with-signal semantics" as a motivating use of ROD [UEC §3.2.2].
- **Collectives / AM / team bootstrap:** sends on RUD, or ROD if the layer above needs send-after-send ordering.

### Why RMA writes are idempotent but atomics are not

**Plain writes and reads.**
- A duplicated packet of a *plain* write re-deposits the same bytes at the same offset. A duplicated read re-reads.
- Both are idempotent "when they do not deliver a target completion" [UEC §3.4.1.13].
- A target completion, or a bound counter such as FI_REMOTE_WRITE, is a side effect that would double-count. Hence the IDEMPOTENT_SAFE rule that forbids counters on RUDI-eligible MRs [UEC §2.2.5.3.4.1].

**Atomics.**
- `SUM` or `fetch_add` applied twice adds twice.
- A *fetching* atomic's returned old value must come from exactly one application.
- `CSWAP` replayed after a third party restored the compared value would swap again.
- `AMO_WRITE` looks idempotent on its own. But its promised semantics are consistency (linearizability) with concurrent atomics from other FEPs [UEC §3.4.8.2], and a replay after another PE's write produces the history A, B, A, which no serial order explains.
- Sends and tagged sends consume a receive buffer, so they are not idempotent either [UEC §3.4.1.13].

**RUDI's idempotence window for writes is conditional.**
- The spec defines idempotence as "can be written into memory multiple times up until the final message completion is delivered at the initiator" [UEC §3.2.2].
- RUDI does no dedup, and the TSS section states that "replay protection at the transport layer is not possible and NOT provided by TSS" for RUDI [UEC §3.7.9, documented].
- So a *late* duplicate of an earlier write could land after the initiator has seen completion and issued a newer overlapping put to the same address (*inference*). PSN randomisation and epoch rejection exist only for PDC modes and TSS epochs [UEC §3.5.8.2, §3.7.10].
- The spec's intended RUDI pattern is "do lots of RMA Writes/RMA Reads to multiple destinations and then barrier" [UEC §3.5.7.3]. That pattern tolerates this hazard. A strict OpenSHMEM `shmem_fence` over RUDI does not strictly guarantee "later puts replace earlier ones", unless one of these holds:
  1. overlapping puts after a fence use RUD;
  2. a bounded network packet lifetime is assumed and waited out;
  3. TSS epoch-based rejection with a short `rxMaxEpochLifetime` is enabled [UEC §3.7.10].

  (*inference*; I found no spec text addressing late RUDI duplicates after completion.)

**RUDI trade-off at scale.** RUDI and UUD are **not under UET congestion control**. The spec says they SHOULD NOT share a traffic class with RUD/ROD for large transfers, and "it is the responsibility of the application to avoid creating congestion" [UEC §3.2.2, §3.5.7.3]. At 10^5 PEs doing bulk puts, RUDI is therefore only safe on a separate, provisioned traffic class, or for small puts (*inference*).

### How fence ordering can be provided

There are three documented mechanisms.

**1. ROD PDC.** Ask for R/W ordering (e.g. FI_ORDER_RMA_WAW or ATOMIC_WAW), which pushes RMA and atomics onto ROD [Table 2-31]. Costs:
- single-path (no spraying);
- GoBackN;
- one PDC per process pair;
- and even then WAR ordering needs target buffering, and ROD data ordering for atomics is an optional capability limited to ≤ 16-byte elements [UEC §3.4.1.7.2, §3.4.8.1.3].

*documented* costs. My assessment: this is the wrong default for OpenSHMEM puts.

**2. Initiator-side fencing over RUD or RUDI.** "after one operation is performed, the initiator waits for that operation to complete, and then the next operation is performed" [UEC §3.4.1.7.2]. For mixed RMA and messaging, "source-side fencing" is mandated when ROD is not used [UEC §3.4.1.7.1]. This is exactly SOS's software `shmem_transport_fence`:

```c
#if WANT_TOTAL_DATA_ORDERING == 0
    shmem_transport_put_quiet(ctx);
#endif
    shmem_transport_get_wait(ctx);
```

*observed in source*, SOS `src/transport_ofi.h` around line 553.

**3. The libfabric `FI_FENCE` flag.** It is required in all UET profiles: fi_writemsg, fi_atomicmsg and fi_sendmsg MUST support it [UEC Table 2-4, Tables 2-24/2-25/2-26]. Its libfabric semantics: the fenced operation "and any operation posted after the fenced operation will be deferred until all previous operations targeting the same peer endpoint have completed" [libfabric fi_msg(3), documented]. This makes the provider or NIC do per-peer initiator fencing without a host round trip. Existing implementations:

- **CXI (Slingshot/Cassini)** implements it in hardware by emitting a command-queue fence command before the RMA, AMO or IDC put. `cxi_cq_emit_cq_cmd(cmdq->dev_cmdq, C_CMD_CQ_FENCE)` appears in `prov/cxi/src/cxip_cmdq.c` (functions `cxip_cmdq_emit_idc_put` and `cxip_cmdq_emit_dma`). *observed in source*, [libfabric] 056b8ae.
- **sockets provider** holds the fenced entry until it is at the head of the TX list (`prov/sockets/src/sock_progress.c:1987`).
- **verbs** maps it only in the Windows NetworkDirect header (`IBV_SEND_FENCE`, `prov/verbs/include/windows/infiniband/verbs.h`). *observed in source*.
- **SOS** uses `FI_FENCE` for put-with-signal when configured `--enable-fi-fence`; the default is off, falling back to transport fence (`configure.ac:320`, `src/transport_ofi.h:829-836`, `src/transport_ofi.c:1483`). *observed in source*.
- **OpenFAM** implements `fam_fence` as a 10-byte `fi_writemsg(..., FI_FENCE)` to a special `FAM_FENCE_KEY` on every memory server (`src/common/fam_libfabric.cpp:1322-1352`). *observed in source*.

Ordering scope: UET message ordering is scoped per `{initiator FA, target FA, initiator PIDonFEP, JobID, target PIDonFEP, RI, TC}` [UEC §3.4.1.7.1]. That matches OpenSHMEM's per-destination-PE fence scope ("ordering of Put, AMO, and memory store operations … with respect to a specific destination PE", [spec16] §Synchronization) (*inference*).

## A.5 Completion and progress model

*documented* [UEC §2.2.5.3.7, §3.4.8.3, §3.5.22.1]:

- Completion queues are mandatory.
- `fi_cntr` completion counters (FI_CNTR_EVENTS_COMP and FI_CNTR_EVENTS_BYTES): none on AI Base; send/recv/read/write on AI Full; HPC adds remote read/write.
- Required op flags: FI_INJECT, FI_INJECT_COMPLETE, FI_TRANSMIT_COMPLETE and FI_DELIVERY_COMPLETE. The last is an optional stronger guarantee; the target may refuse DC with a semantic failure.
- Default global observability is "not guaranteed". The initiator completion means all packets ACKed plus the semantic response received.
- RUDI has **no message completion at the target**.

**SOS already uses these.**
- Put and get counters via `fi_cntr_read/fi_cntr_wait` on `ctx->put_cntr/get_cntr`.
- A *target* counter bound to the heap and data MRs with `fi_mr_bind(..., FI_REMOTE_WRITE)` under `ENABLE_TARGET_CNTR` (`src/transport_ofi.c:679-773`, `:822`).
- `FI_DELIVERY_COMPLETE` on puts and on put-with-signal (`src/transport_ofi.h:723-829`).

*observed in source*.

**Conflict:** a target counter bound to the symmetric heap MR makes that MR non-IDEMPOTENT_SAFE, and UET requires the bind to fail on an IDEMPOTENT_SAFE MR. So SOS's current target-counter build option is incompatible with RUDI puts (*inference* from [UEC §2.2.5.3.4.1] plus [SOS] source). A core API should not require target-side put counters on the heap.

**Progress thread.** UET is designed for full hardware offload of PDS/SES ("potentially fully hardware-accelerated", arXiv:2508.08906 abstract, documented). No progress thread is inherent. Software providers do need one or need polling:
- [uet-ref-prov] is user-space over raw sockets or AF_XDP with copying;
- [SUET] runs over UDP or verbs DGRAM.

*documented* READMEs.

## A.6 Thread and context model

*documented* [UEC Table 2-4, Table 2-27]:

- `FI_THREAD_SAFE` is the required threading level.
- `max_ep_tx_ctx ≥ 1` and `max_ep_rx_ctx ≥ 1`.
- **No shared TX/RX contexts** (`max_ep_stx_ctx = 0`, `max_ep_srx_ctx = 0`).
- `fi_scalable_ep` is not required.
- If scalable receive queues are supported, consecutive Resource Indices must be allocated.
- Each additional libfabric endpoint consumes Resource Index space (12 bits per PIDonFEP) and possibly a PIDonFEP (12 bits per FEP).

The hardware cost per context (queues, doorbells) is vendor-specific and not given in the spec. *inference*: an OpenSHMEM context maps to a libfabric endpoint (or TX context). How many a NIC can afford is unknown until products ship.

## A.7 GPU memory

`FI_HMEM` is required on all profiles [UEC Table 2-4]. `FI_HMEM_DEVICE_ONLY`, `FI_HMEM_HOST_ALLOC`, `FI_MR_DMABUF`, `fi_hmem_ze_device` and `FI_OPT_CUDA_API_PERMITTED` are not required [UEC Tables 2-27, 2-28]. *documented*. So dmabuf-based GPU registration is vendor-optional.

## A.8 What OpenSHMEM-over-UET would have to emulate

No OpenSHMEM implementation on UET exists. A search of the SOS repo and issues found none (*documented* search result, 16 Sep 2026). The evidence therefore comes from UET-compatible providers and from OFI-based SOS.

**By profile** (*inference* from Table 2-4 and 2-25/26):
- **AI Base:** get, fetch atomics and compare_swap would be emulated via send/AM.
- **AI Full:** compare_swap would be emulated.
- **HPC:** nothing needs emulation.

**Existing software UET providers:**
- [SUET]: "ROD" only, no RUD PDC, and SES send/recv, write and **non-fetching** fi_atomic only. So get, fetch atomics and cswap need emulation. It has been tested with OpenMPI and MPICH, but not OpenSHMEM. *documented* README.
- [uet-ref-prov]: all four PDS modes. SES has untagged and tagged messaging, deferrable send and RMA read/write. **No atomics are listed.** *documented* README; not verified in source.
- Linux kernel RFC (Enfabrica, Mar 2025): PDS in RUD mode only, experimental. https://lwn.net/Articles/1013363/, documented.

**SOS-over-OFI dependencies that UET preserves** (*observed in source*, SOS `src/transport_ofi.c:1478-1487`, `transport_ofi.h`):
- FI_RMA and FI_ATOMIC with `fi_atomicvalid` checks;
- FI_RMA_EVENT for target counters;
- FI_FENCE (optional);
- FI_DELIVERY_COMPLETE.

UET mandates FI_FENCE and FI_RMA_EVENT on every profile, and fetch atomics on AI Full and HPC. SOS also documents a CXI limitation: "CXI provider currently does not support fetch atomics with FI_DELIVERY_COMPLETE", so it uses FI_INJECT plus a non-blocking path (`transport_ofi.h:1062`).

**Emulation expected at scale** (*inference*):
- Per-PE key exchange can be avoided with user-assigned well-known keys.
- Collectives need a software layer (no offload).
- `shmem_team_*` and `shmem_ptr` (NULL off-node) need nothing special.

## A.9 Known pathologies and limits

Each item is *documented* unless marked.

1. **ROD pathologies.** ROD is single-path with GoBackN, and ROD PDC close forces new ROD messages to wait for the full close [UEC §3.5.7.2, §3.5.8.3]. Wildcard or untagged matching over ROD requires buffered unexpected messages. Buffer exhaustion is handled by disabling the target `{FA, JobID, PIDonFEP, RI}` tuple (RC_DISABLED) until software quiesces all senders, a barrier-like recovery [UEC §3.4.3.6.3.1].
2. **RUDI risks.** No congestion control and no replay protection [UEC §3.5.7.3, §3.7.9].
3. **Header overhead.** The standard SES header is about 44 bytes before PDS, UDP, IP and optional TSS [Table 3-8]. Small puts should use the small or optimized RMA headers, which need ≤ MTU and no remote CQ data.
4. **Fixed field widths.** 4096 PIDonFEP per FEP, 4096 Resource Indices, 4096 optimized MR indices, 24-bit JobID.
5. **Message size caps.** 4 GB-1 maximum message and RMA size, so larger puts must be split above SES. Packets of one message stay on one PDC. Distributing large data across PDCs requires splitting into multiple messages above SES [UEC §3.5.6].
6. **Immaturity.** There are no public NIC product performance numbers in my sources. Real performance is *unverified*.
7. **Third-party criticism.** Tom Herbert's review (https://medium.com/@tom_84912/…) returned HTTP 403 and was not read.

## A.10 Build, portability, maintenance

- The API is plain libfabric 2.x, so an OFI transport (SOS-style) is the natural path. New attributes such as `FI_PROTO_UET`, `FI_ADDR_UET` and fi_cntr are v2.0 additions [UEC §2.2].
- Vendors must provide on/off switches for features beyond a profile [UEC §2.2.2].
- Control plane: JobID assignment happens via a privileged process or provisioning system through a "UET Control API" (Linux netlink implementation) [UEC §2.2.4, §2.2.11]. This means launcher and RM integration is required for relative addressing (*inference*).
- Reference implementations: [uet-ref-prov] (MIT, user-space, clarity-first) and [SUET] (BSD/GPLv2, about 7.5 GB/s per core on verbs, per README).

---

# Part B. CXL fabric-attached / shared memory, and OpenFAM

## B.1 Primitives

### CXL itself: load/store memory, no messaging

- **Pooling (CXL 2.0).** Memory is a "fungible resource that can be flexibly allocated and deallocated to different servers". Each allocation is exclusive to one host [CXL3WP, documented].
- **Sharing (CXL 3.0).** "a given region of memory [can] be simultaneously accessible by more than one host and still guarantee that every host sees the most up to date data at that location, without the need for software-managed coordination", using "hardware coherency" [CXL3WP, documented].
- **Fabrics.** "up to 4096 nodes … using … Port Based Routing (PBR)". A **GFAM** device is a Type-3 device "accessed by multiple nodes (up to 4095)" [CXL3WP, documented].
- **Coherency mechanism.** CXL 3.0 Type-2/Type-3 devices can "back invalidate a cache line through the host processor" (BISnp, enhanced coherency) [CXL3WP, documented]. This is the mechanism behind inter-host cache coherence (HWcc).
- **Multi-headed devices and Dynamic Capacity Devices.** DCD shared or tagged extents are the sharing unit in the Linux model: extents "may be exclusive to a single host or shared across multiple hosts" and carry a UUID tag. As of v6.14 Linux had no formal DCD management interface; patches are still in flight in 2026. Sources: docs.kernel.org CXL device-types page and linux-cxl patch threads, from search snippets; **pages not read directly**, so partly unverified.
- **PBR details.** The 12-bit PBR IDs and exact Fabric Manager API commands (FM API, GFAM "GIM" in 3.1) are *recalled*, not verified; the base spec is members-only.

### OpenSHMEM mapped onto shared CXL memory

*inference*. The symmetric heap lives in a shared FAM extent mapped by all PEs.

| OpenSHMEM | On shared CXL memory |
|---|---|
| put / get | memcpy into or out of the mapped region: CPU load/store, no NIC, no registration, no keys |
| shmem_ptr | always non-NULL |
| atomics | CPU `lock cmpxchg`, `lock xadd` etc. on the shared location, **only if** inter-host cache coherence holds (see B.4) |
| fence / quiet | trivially satisfied, since stores are synchronous. Visibility still depends on coherence |
| wait / test (`shmem_*_wait_until`) | need cross-host notification. There is no cross-host futex, so polling or an interrupt/doorbell device is needed |
| collectives, teams | software above load/store |

### OpenFAM API

*documented* [OpenFAM-CUG22 §II; OpenFAM/API fam.h; openfam.github.io]:

- Two-level hierarchy: **regions** (with non-functional attributes: redundancy NONE, RAID1 or RAID5; volatile or non-volatile; interleaving across memory servers) and **data items** allocated in regions.
- Both are addressed by opaque `Fam_Descriptor` capabilities. In fam.h v1.04 the descriptor is `{regionId, offset}`.
- Both have POSIX-like `mode_t` permissions and names (`fam_lookup`, `fam_lookup_region`).
- Data path: `fam_get/put_{blocking,nonblocking}`, strided and indexed `fam_gather/scatter`, and `fam_copy` (FAM to FAM).
- Load/store: `fam_map`/`fam_unmap`. Cache coherence "is maintained among processors within a node (scale-up), but is not provided across nodes (scale-out) accessing FAM" [CUG22, documented].
- Atomics: `fam_set`, `fam_add`, `fam_subtract`, `fam_and/or/xor`, `fam_compare_swap` (int32, int64, uint32, uint64, int128), `fam_fetch_*`, "with memory side controllers ensuring atomicity".
- Ordering: `fam_fence`, `fam_quiet` ("semantics similar to those defined in OpenSHMEM"), `fam_barrier_all`.
- Extensions: contexts (`fam_context_open/close`), `fam_progress()` (count of pending ops), `fam_backup/restore`.

## B.2 Registration and keys

**CXL.** There is no registration. Access control is at the granularity of host physical address decoding: DCD extent or tag, switch and FM binding, then host page tables (*recalled/inference*). There are no per-PE keys, but also no per-PE protection inside a shared extent beyond each host's page tables.

**OpenFAM reference implementation** (*documented* CUG22 §III): "upon validation of permissions, FAM is mapped from the memory server, registered onto libfabric and the key shared with PEs". The descriptor carries per-memory-server keys and base addresses: `descriptor->get_keys()`, `get_base_address_list()`, `get_memserver_ids()` in `src/fam-api/fam_ops_libfabric.cpp`, *observed in source*. That is **O(memory servers) keys per data item, not O(PEs)**.

## B.3 Per-peer state

**CXL.** PE count is bounded by the fabric: **≤ 4096 PBR nodes, and ≤ 4095 hosts per GFAM** [CXL3WP]. That is two orders of magnitude below 10^5 PEs by node count, although each host may run many PEs. Beyond one CXL fabric, a network transport such as UET is required (*inference*). Address-space cost: mapping every PE's symmetric heap into every PE means `N_PE × heap_size` of virtual address space per process, unless heaps are shared in one extent (*inference*).

**OpenFAM reference implementation.**
- PEs connect to memory servers, not to each other.
- A CIS (client interface service) and a metadata service (radixtree KVS) handle allocation and lookup over gRPC or Thallium.
- PMIx is used for the PE group.

*documented* [CUG22 §III; OpenFAM README]. Per-PE state is O(memory servers).

## B.4 Consistency, coherence and cross-host atomics (the key question)

**What the white paper promises.** CXL 3.0 describes memory *sharing* as coherent across hosts [CXL3WP, documented]. With inter-host HWcc, CPU atomic instructions on a shared cache line would be linearizable across hosts, and several systems papers assume so:
- "cmpxchg atomic operations, which CXL makes coherent across host boundaries" (DAXFS, arXiv:2604.01620);
- "Accelerating Distributed Joins with CXL" (DOI 10.1145/3662010.3663449), which builds spinlocks from CAS.

Both come from search snippets only and were **not read in full**.

**What practice looks like.** [cxlalloc] (ASPLOS '26), *documented*:
- "Version 3 of the CXL specification defines an inter-host cache coherence protocol, but given implementation cost and complexity, it is unclear if HWcc will become widely supported in practice."
- "Two current hardware prototypes that support inter-host memory sharing do not support HWcc at all."
- Other work proposes limiting HWcc "to a small memory region".

Their workaround:
- **mCAS**: a near-memory-processing FPGA unit serialises CAS on a *device-biased* region that "must be marked uncachable (via /proc/mtrr, for example), as the NMP has no way of tracking or invalidating CPU caches".
- A thread starts an mCAS by writing 64 bytes (expected value, new value, address) to a special-write region, then reads 16 bytes back.
- For non-HWcc memory, "software cache coherence" (SWcc) "manually controls cache state by flushing and fencing" and requires threads pinned to cores.

My conclusion (*inference*): on CXL, cross-host OpenSHMEM atomics as bare CPU `lock cmpxchg` are **only safe if the device implements HWcc**. On most near-term devices a SHMEM layer would need one of:
1. a small HWcc or device-biased metadata region holding atomic targets, and mCAS-like device primitives (not standardised);
2. software coherence (flush plus fence) for put and get visibility;
3. falling back to network atomics.

Arbitrary symmetric variables would not be CAS-able in place, which conflicts with OpenSHMEM's "any symmetric integer" atomics.

**OpenFAM.**
- The memory-server model gives no cross-node coherence for `fam_map` [CUG22].
- `fam_map` is **not implemented** outside the shared-memory (SHM) model: `FAM_UNIMPLEMENTED_MEMSRVMODEL()` in `src/fam-api/fam.cpp` `fam::Impl_::fam_map`. *observed in source*.
- The fence and quiet comments state they "do NOT order load/store accesses by the processor to FAM enabled by fam_map()" (`fam.cpp` near line 5367). *observed in source*.
- Atomics in the memory-server model are **libfabric atomics sent to the memory server**: `fi_compare_atomicmsg` (FI_CSWAP) and `fi_fetch_atomicmsg` (FI_SUM) in `src/common/fam_libfabric.cpp:1581-1689` and `src/fam-api/fam_ops_libfabric.cpp` (`compare_swap`, `atomic_fetch_add`). *observed in source*. The default provider is `sockets` (`config/fam_pe_config.yaml`), whose atomics are software.
- In the SHM model, atomics are CPU atomics (`fam_atomic_64_compare_store`, `src/fam-api/fam_ops_shm.cpp:1432`), valid within one host.
- An atomic cannot cross an interleave-block boundary. The source throws "Atmoic operation can not be performed, size of the value goes beyond interleave block".

## B.5 Completion and progress

**CXL load/store.** Completion is synchronous; nothing is needed.

**CXL error handling** (*recalled*, unverified): uncorrectable memory errors or a device or link failure surface as machine checks or poison on *every* host that touches the memory.

**Failure isolation** (*documented* [cxlalloc] §2): partial failure (a crashed process holding a lock) is a first-class problem. cxlalloc "assume[s] our CXL device is reliable", achievable with independent power. Cross-host locks need recoverable lock protocols (*inference*).

**OpenFAM.** Non-blocking operations are tracked per context. `fam_quiet` waits using async futures over the default contexts (`Fam_Ops_Libfabric::quiet_context`), and `fam_progress()` returns the pending count. *observed in source* and *documented*.

## B.6 Threads and contexts

**CXL.** Contexts cost nothing in hardware.

**OpenFAM.** A context equals a libfabric endpoint context. CUG22 reports that the original single-context design forced `quiet()` over all of a PE's operations, which motivated `fam_context_open` [CUG22 §IV-B, documented].

## B.7 GPU memory

**CXL.** Type-2 accelerators with HDM and peer access to HDM are described [CXL3WP]. GPU access to *shared* GFAM is not described in the documents I read (*unverified*).

**OpenFAM.** No GPU support is mentioned in the README or CUG22 (*documented* absence in what I read).

## B.8 What FAM-based PGAS implementations had to emulate

- **OpenFAM (memory-server model)** (*observed in source*):
  - `fam_map` is unimplemented;
  - fence is a synthetic 10-byte RMA write with FI_FENCE to a reserved key on every memory server (`fam_libfabric.cpp:1322`);
  - atomics are delegated to the provider (software in `sockets`);
  - collectives other than barrier do not exist [CUG22].
- **cxlalloc on CXL** (*documented*): software cache coherence via flush and fence; a device-side mCAS where HWcc is missing; partial-failure recovery.
- **No OpenSHMEM-on-CXL-shared-memory implementation** was found this session. SOS and OSHMPI use shared-memory transports (XPMEM, CMA) intra-node only (*recalled*).

## B.9 Performance and limits

- **Latency and bandwidth** [cxlalloc §5, documented; Intel Agilex 7 CXL device on a CXL 1.1 host, measured with Intel MLC at a 3:1 read:write ratio]:

  | | CXL memory | Local DRAM |
  |---|---|---|
  | Read latency | 357 ns | 112 ns |
  | Bandwidth | 19.9 GB/s (2 channels) | 114 GB/s (4 channels) |

  Switched or multi-hop CXL 3.x latency: no measured data in my sources (*unverified*).
- **Scale:** ≤ 4096 fabric nodes, ≤ 4095 hosts per GFAM [CXL3WP].
- **OpenFAM over RDMA** [CUG22 §IV, documented]:
  - with 16 memory servers and an even workload, aggregate throughput was 179.6 GB/s (fam_get_blocking) to 193.1 GB/s (fam_put_nonblocking);
  - with a random, uneven workload, the maximum was 134.5 GB/s, attributed to incast at memory servers;
  - the software stack adds "slightly less than 1 µs" round trip over the raw fabric for 256-byte gets;
  - the implementation initially did not target performance, scale or fault tolerance (README).

## B.10 Build, portability, maintenance

**CXL.**
- Needs CXL 3.x switches, MHD or DCD devices, a Fabric Manager, and kernel DCD/extent support, which was still being merged in 2026 (search snippets).
- famfs provides a user-space shared-file view.
- HWcc availability is vendor-dependent.

Very low software burden for put/get; high hardware and ecosystem risk (*inference*).

**OpenFAM.**
- C++ with a C API.
- Depends on gRPC or Thallium, PMIx, libfabric, and HPE NVMM (gull, cloned from a `devel` branch in `third-party/download.sh`).
- The last commit was Sep 2024, so maintenance activity is low (*observed*).

---

# Implications for a core API

## Made cheap

**UET, HPC profile:**
- **One-sided put/get** as `fi_write/fi_read`: RUD by default, or stateless and sprayed RUDI when the heap MR is IDEMPOTENT_SAFE.
- **Single-element atomics, including CSWAP and fetching variants.** These must go on RUD (dedup), never RUDI.
- **Well-known user-assigned keys**: one key per symmetric segment, no exchange, key count independent of N.
- **Per-peer transport state proportional to active peers**, via ephemeral PDCs and a PDC shared per FEP pair for RUD.
- **Per-peer fence as a flag** (`FI_FENCE`, mandatory in all profiles; hardware-implemented in CXI).
- **Completion counters.**

**CXL, within one fabric:**
- **put/get as memcpy, and `shmem_ptr`.**
- **fence/quiet** are free.

## Expensive or hazardous

**On UET:**
1. **Ordering by transport.** R/W ordering forces ROD: single path, GoBackN, and a PDC per process pair. The core API should express ordering as *fence points* (an FI_FENCE-like flag or initiator-side quiesce per destination), not as a per-endpoint ordering mode.
2. **RUDI puts.**
   - They are incompatible with target-side counters or completions on the heap MR, so the core must not require target write counters on symmetric memory.
   - They are outside congestion control, so they need their own traffic class.
   - They have no replay protection. A late duplicate can violate strict fence semantics without an extra guard (epoch rejection, a packet-lifetime wait, or RUD for overlapping post-fence puts).
   - The core should expose a per-operation "idempotent/unordered OK" hint so bulk, non-overlapping puts can use RUDI while fenced or overlapping ones use RUD.
3. **Profile gaps.**
   - AI Base lacks read and fetching atomics; AI Full lacks compare_swap.
   - The core must allow get, fetch-AMO and CAS to be layered on send/AM, with known cost: an extra message and a target software handler.
4. **Address tables.** AV of O(N) entries at about 36-40 bytes each unless the provider implements an algorithmic, job-relative AV. The core should accept rank → (FA index, PIDonFEP) functions rather than per-peer address blobs.
5. **Payload limits.** A 4 GB-1 message cap and one-PDC-per-message rule mean large puts must be chunked above the transport to be multipathed across PDCs.

**On CXL / FAM:**
1. **Cross-host atomics are not reliably CPU-native.** HWcc is spec-defined but questionable in practice; prototypes lack it. The core API should keep atomics as an explicit primitive, implementable as CPU CAS (HWcc), device mCAS on a sequestered region, or a network atomic, rather than assuming load/store plus lock-prefix instructions.
2. **Visibility and fence** under software coherence need explicit flush points, so keep fence/quiet in the core.
3. **Scale** caps at about 4096 nodes. CXL is an intra-pod tier; a two-tier core ("local-mapped" vs "networked" peers, as GASNet-EX PSHM) is required for 10^5 PEs.
4. **Failure isolation.** A crashed PE holding a cross-host lock, or a memory error, affects every host. Locks must be recoverable.
5. **Wait/test** on remotely updated memory needs a notification primitive that load/store fabrics do not provide.
