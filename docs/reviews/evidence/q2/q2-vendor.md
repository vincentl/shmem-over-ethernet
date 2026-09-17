# Q2 survey: vendor and historical substrates — Cray DMAPP/uGNI (Gemini/Aries), Fujitsu uTofu (Tofu-D), AWS EFA/SRD

Scope: what these vendor networks show about a minimal core API for OpenSHMEM 1.4–1.6 at 10^5+ PEs. Each claim is labelled:
- **[doc]**: documented, with a URL.
- **[src]**: observed in source code, with a URL and file path.
- **[recalled]**: from memory and not checked here. Treat it as unverified.
- **[arith]**: my own arithmetic on sourced numbers.

## Source-access notes

- **Read in full:** the DMAPP CUG 2010 paper; the Pritchard CUG 2011 uGNI/MPICH2 paper; the Gutierrez/Hjelm HOTI 2012 Open MPI on Cray XE paper; the SRD IEEE Micro 2020 paper; the libfabric `prov/efa` source and docs; the SOS source; the Open MPI v4.1.6 `btl/ugni` source; the AWS EFA user guide; and the Cray OpenSHMEMX public docs repo.
- **Commits and tags used for source citations:**
  - libfabric `main` at `0d5527ae694af929014da0385d409f638e81105e`, cited as `LF/<path>` = `https://github.com/ofiwg/libfabric/blob/0d5527ae694af929014da0385d409f638e81105e/<path>`
  - SOS `main` at `4aa91d7fa9041785bd367fc9cefab99d313dd9df`, cited as `SOS/<path>` = `https://github.com/Sandia-OpenSHMEM/SOS/blob/4aa91d7fa9041785bd367fc9cefab99d313dd9df/<path>`
  - Open MPI tag `v4.1.6`, cited as `OMPI/<path>` = `https://github.com/open-mpi/ompi/blob/v4.1.6/<path>`
- **Could not read (Fujitsu uTofu User's Guide):** the guide (J2UL-2482, https://software.fujitsu.com/jp/manual/manualfiles/m210007/j2ul2482/02enz003/j2ul-2482-02enz0.pdf) now redirects (HTTP 301) to a generic Fujitsu documents portal. I did not read it in full. The uTofu claims below come from search-engine excerpts of that guide and from the abstracts of the Fugaku UCX papers. Anything more detailed is **[recalled]** and must be checked before it drives a design decision.
- **Closed source:** Cray SHMEM, Cray OpenSHMEMX (DMAPP layer), DMAPP and uGNI were never open source. For Cray, "what was emulated" therefore comes from papers, man pages and open-source peers on the same NIC (Open MPI `btl/ugni`, libfabric `gni`), not from Cray SHMEM source.

---

# Part A. Cray DMAPP and uGNI (Gemini: XE/XK; Aries: XC)

## A1. Primitives relevant to one-sided PGAS

**DMAPP** is Cray's API built for one-sided models. It was designed for Cray SHMEM, UPC, CAF and Chapel. Source: ten Bruggencate & Roweth, "DMAPP – An API for One-sided Program Models on Baker Systems", CUG 2010, https://cug.org/5-publications/proceedings_attendee_lists/CUG10CD/pages/1-program/final_program/CUG10_Proceedings/pages/authors/01-5Monday/03B-tenBruggencate-Paper-2.pdf (hereafter *DMAPP10*).

- **Remote address triple [doc DMAPP10 §2.2.2]:** every RMA call names the remote location as (virtual address, segment descriptor, PE).
- **Point-to-point PUT/GET [doc DMAPP10 §2.2.2]:** `dmapp_put`, `dmapp_get`. Each comes in three completion flavours:
  - blocking;
  - `_nb` (explicit non-blocking, returns a `syncid`);
  - `_nbi` (implicit non-blocking, no handle).
- **Strided and indexed variants [doc DMAPP10 §2.2.2]:** `dmapp_iput`/`dmapp_iget` (separate source and target strides); `dmapp_ixput`/`dmapp_ixget` (index arrays, i.e. scatter/gather within one remote PE). Strided PUT rate "is of particular importance to the Co-Array Fortran compiler".
- **PE-strided (one source, many target PEs) [doc DMAPP10 §2.2.3]:** `dmapp_put_ixpe`, `dmapp_scatter_ixpe`, `dmapp_gather_ixpe`, each with `_nb`/`_nbi`. The remote address must be symmetric. The paper says explicitly that "none of these functions are collective operations". They are one-sided fan-out and fan-in, intended for small data.
- **AMOs (atomic memory operations) [doc DMAPP10 §2.2.4, §3.1]:**
  - Single-operand: `dmapp_<op>_qw` with op ∈ {AADD, AAND, AOR, AXOR} (PUT semantics) and {AFADD, AFAND, AFOR, AFXOR} (fetch variants).
  - Two-operand: AFAX (atomic fetch and exclusive or) and ACSWAP (compare and swap).
  - All AMOs operate **only on 64-bit** quantities. They are "modelled on the set of AMOs provided on Cray X2" and run in NIC hardware via FMA.
  - The Gemini NIC "caches up to 64 AMOs". Targets outside that cache pay extra HyperTransport fetch/store cost (Fig. 6).
- **Synchronization [doc DMAPP10 §2.2.5]:**
  - `dmapp_syncid_test/wait` retires one explicit request.
  - `dmapp_gsync_test/wait` retires *all* outstanding implicit requests.
  - "only the initiating process has visibility of completion".
  - This is almost exactly OpenSHMEM's `shmem_put_nbi` + `shmem_ctx_sync` model, and it predates OpenSHMEM.
- **Collective offload:**
  - Cray MPICH could use DMAPP collectives (`MPICH_USE_DMAPP_COLL=1`) for MPI_Allreduce, MPI_Bcast and MPI_Barrier on Aries [doc, ALCF slides: https://www.alcf.anl.gov/sites/default/files/2020-05/Chunduri_MPI_Theta_CPW_May_5_2020_final.pdf, via search excerpt].
  - The DMAPP entry points were a `dmapp_c_pset_*` family (process-set create/export, then small-message allreduce offloaded to Aries). **[recalled]**
- **uGNI** is the lower layer [doc Pritchard, "A uGNI-Based MPICH2 Nemesis Network Module for Cray XE", CUG 2011, https://cug.org/5-publications/proceedings_attendee_lists/CUG11CD/pages/1-program/final_program/Wednesday/13C-Pritchard-Paper.pdf, hereafter *GNI11*]:
  - FMA (Fast Memory Access) does RDMA read, write and AMOs as a low-overhead programmed-I/O path.
  - BTE (Block Transfer Engine) offloads bulk RDMA read/write.
  - SMSG and MSGQ are messaging services built on Gemini's "RDMA write with remote notification".
  - `GNI_POST_CQWRITE` writes a small value directly into a remote CQ (completion queue). The source comment says it carries "up to 48 bytes" [src OMPI/opal/mca/btl/ugni/btl_ugni_rdma.h].
  - GNI11 states plainly that GNI "is also not optimal for middleware which supports applications like PGAS compilers and other applications requiring high performance for very fine-grain remote memory access". That is why DMAPP exists above it.
- **Tag matching:** neither DMAPP nor uGNI provides it. MPI did matching in software above SMSG [doc GNI11 §4.3].
- **Active messages:** not provided. SMSG with a per-message 8-bit tag (`GNI_SmsgSendWTag`) is the closest thing [doc GNI11 §4.3; Gutierrez & Hjelm, HOTI 2012, https://www.cs.unm.edu/~samuel/publications/2012/perf-eval-ompi-cray-xexk.pdf §V.B].

## A2. Memory registration and remote keys

- **Symmetric segments need no keys [doc DMAPP10 §3.2, §2.1].**
  - At `dmapp_init` DMAPP creates a Communication Domain and CQ and "pre-register[s] symmetric memory regions, i.e. the statically linked data segment and the symmetric heap". Pre-registration "eliminate[s] the need for expensive memory registration of these segments during end-user application execution".
  - Objects in symmetric segments "can be accessed … by other PEs in the job using **locally generated address information**".
  - Result: **no per-PE key exchange and no per-PE key storage** for symmetric memory. The segment descriptor is job-wide and identical on every PE. This is the ideal model for OpenSHMEM at 10^5 PEs.
  - The symmetric heap uses 2 MB large pages by default.
  - DMAPP "mmaps the symmetric heap directly … to the hugetlbfs file system" [doc, HPE CPE intro_hugepages via search excerpt, https://cpe.ext.hpe.com/docs/24.03/craype/intro_hugepages.html].
- **What makes keyless access work.** Gemini/Aries protection uses a job-wide Protection Tag (ptag) that ALPS assigns per job [doc GNI11 §2.1], plus an NIC-side translation of each segment. A remote NIC can therefore validate "PE i, segment k, offset" without a per-initiator rkey. The "cookie"/segment descriptor is identical across PEs because the launcher and DMAPP made it so. **[recalled for the MDD/NTT (memory domain descriptor / node translation table) mechanics; ptag per job is doc]**
- **Symmetric heap allocation is the caller's job [doc DMAPP10 §2.2.6].** `dmapp_sheap_malloc/realloc/free` are a region allocator inside DMAPP. "DMAPP applications are responsible for preserving the symmetry", i.e. all PEs make the same calls in the same order. There is no collective agreement inside DMAPP.
- **Dynamic memory needs out-of-band exchange [doc DMAPP10 §2.2.7].** `dmapp_mem_register` produces "non-symmetric" segments that can only be addressed with a descriptor communicated out of band. DMAPP keeps a registration cache.
- **uGNI [doc GNI11 §2.5]:** `GNI_MemRegister` returns an opaque Memory Handle (MH) that the peer must hold. Plain uGNI therefore needs per-region handle exchange. MPICH shares DMAPP's registrations when DMAPP is loaded, "to share the same memory registration resources" [doc GNI11 §4.7].
- **Hard NIC registration limits:**
  - libfabric `gni` provider: "a maximum of **4K memory registrations per node** when using scalable memory registration". Default user limit 192, provider limit 64 [doc fi_gni(7) v1.15.2, https://ofiwg.github.io/libfabric/v1.15.2/man/fi_gni.7.html].
  - MPICH chose large pages for mailboxes to reduce "pressure on the NICs registration resources used for registration of 4KB pages" [doc GNI11 §4.7].
  - Implication: a per-node registration budget, not just a per-process one, is a real constraint.

## A3. Per-peer state (template item 3, priority)

| Resource | Scaling | Numbers |
|---|---|---|
| DMAPP symmetric RMA | O(1) per PE: no per-peer connection, no per-peer key | [doc DMAPP10] |
| uGNI End Point (EP) | Software object bound to (NIC address, 32-bit inst id). Rebinding is "lightweight … does not involve communication … or system calls" | [doc GNI11 §2.3] |
| **SMSG mailbox** | **Linear in connected peers** per process. "memory usage … grows linearly with the number of peer-to-peer connections" | [doc GNI11 §2.8] |
| MSGQ | Per node pair, not per process pair. Max message 128 bytes | "about 74 KB/node for each inter-node connection … for a job spanning 10,000 nodes, about 740 MB is required on each node" [doc GNI11 §4.3] |
| Address table | NIC address + inst id per PE, obtained from PMI KVS (key-value store) | [doc GNI11 §4.2] |

**SMSG mailbox size vs job size in Cray MPICH [doc GNI11 Table 1]:**

| Job size (ranks) | Max SMSG message | Mailbox bytes per channel |
|---|---|---|
| ≤ 1024 | 1024 | 4672 |
| ≤ 16384 | 512 | 2624 |
| > 16384 | 256 | 1088 |

**Open MPI `btl/ugni` auto-shrinks the SMSG limit by job size [src OMPI/opal/mca/btl/ugni/btl_ugni_add_procs.c `mca_btl_ugni_smsg_setup`]:**

| nprocs | SMSG limit (bytes) |
|---|---|
| ≤ 512 | 8192 |
| ≤ 1024 | 2048 |
| ≤ 8192 | 1024 |
| ≤ 16384 | 512 |
| larger | 256 |

The mailbox size itself comes from `GNI_SmsgBufferSizeNeeded`, given `smsg_max_credits = 32` [src OMPI/…/btl_ugni_component.c].

**Worked cost at 10^5 PEs [arith].** With 1088 bytes per mailbox fully connected, each process needs about 109 MB. At 32 PEs per node that is about 3.5 GB of registered memory per node, before counting the peer's side. This is the "famous" uGNI scaling problem.

**Mitigations the stacks used:**
1. **Lazy connections** by default: a mailbox is allocated on first send, via wildcard datagrams through kGNI session management [doc GNI11 §4.2; HOTI12 §V.A]. With lazy connection disabled, "startup time at scale may increase significantly" [doc GNI11 §4.2].
2. **MSGQ** (per node pair) instead of SMSG [doc GNI11 §2.8].
3. **Shrinking the maximum eager message** as the job grows (tables above).
4. **Open MPI's "Eager GET"**: a small shared pool of pre-registered buffers plus an RDMA GET, instead of large per-peer mailboxes [doc HOTI12 §V.D].

**Lesson.** On Gemini/Aries, RMA to symmetric memory was O(1) per peer. Everything that needed *messages* (control, collectives, rendezvous, error recovery notifications) was where per-peer memory grew.

## A4. Ordering and delivery under adaptive routing

- **Delivery modes are chosen per post descriptor [src OMPI/opal/mca/btl/ugni/btl_ugni_rdma.h].**
  - `GNI_DLVMODE_PERFORMANCE` (adaptive routing) when no ordering is needed.
  - `GNI_DLVMODE_NO_ADAPT` when the BTL requests an order.
  - `GNI_DLVMODE_IN_ORDER` for `GNI_POST_CQWRITE`.
  - So in-order RDMA on Aries was bought by *turning off adaptive routing* for that transaction.
- **SMSG and MSGQ:** "messages are delivered in order". Reliability comes from a sliding-window protocol driven by TX CQE (completion queue entry) error codes [doc GNI11 §2.8].
- **Raw FMA/BTE RDMA gets no retry from GNI.** "GNI does not deal with failed FMA or BTE initiated RDMA transactions". The initiator must inspect CQEs and replay [doc GNI11 §4.6].
- **Atomics cannot simply be replayed.** MPICH used adaptive routing for RDMA and sent notifications only over reliable SMSG/MSGQ. It avoided AMOs "as it is difficult to implement algorithms which allow for recovery when such [AMO transactions fail]" [doc GNI11 §4.6].
  - A non-idempotent remote atomic (ADD, FETCH-ADD, CSWAP) cannot be blindly replayed after a transient network error.
  - Any core atomic primitive therefore needs exactly-once semantics from the transport, or must be restricted to idempotent forms.

## A5. Completion and progress model

- **CQs [doc GNI11 §2.6; DMAPP10 §3.1–3.2]:**
  - CQs live in host memory and are polled or blocked on.
  - Usually one TX CQ (RDMA and non-fetching AMO delivered, fetching results placed) and one RX CQ (SMSG arrivals).
  - DMAPP's sync functions just poll the CQ through GHAL (the Gemini Hardware Abstraction Layer) macros [doc DMAPP10 §3.2].
- **Target side:** passive for FMA/BTE RMA and AMOs. The NIC does everything, so an RMA target needs **no progress thread** (DMAPP10 definition of one-sided access, §1).
- **Initiator side:** must retire CQEs to avoid TX CQ overflow. MPICH used a transaction-descriptor freelist to cap outstanding operations [doc GNI11 §4.1]. DMAPP exposes "maximum number of outstanding non-blocking operations" via `dmapp_get_rma_attrs` [doc DMAPP10 §2.2.1].
- **RX CQ overrun:** the receiver must scan all mailboxes, which is O(peers) [doc GNI11 §4.3].
- **Asynchronous error notification** was a separate interface [doc GNI11 §2.9].

## A6. Threads and contexts

- **2010:** "The Gemini NIC provides each process with its own FMA hardware". Multiple FMA descriptors per process was future work, to "enable threads to issue RMA operations independently" [doc DMAPP10 §5]. FMA descriptor updates were "relatively expensive and should be minimized" [doc DMAPP10 §5].
- **Aries uGNI CDM (Communication Domain) modes** include `GNI_CDM_MODE_FMA_SHARED/FMA_DEDICATED`, `MDD_SHARED/MDD_DEDICATED`, `FMA_SMALL_WINDOW`, `CACHED_AMO_ENABLED` and `CQ_NIC_LOCAL_PLACEMENT`. Open MPI's default is FMA_SHARED | MDD_SHARED | FMA_SMALL_WINDOW [src OMPI/…/btl_ugni_component.c].
- **Open MPI "virtual devices":** up to 128 CDM/NIC handles per process for thread scalability. The parameter is described as "Higher numbers may result in better performance when using threads… max: 128" [src OMPI/…/btl_ugni_component.c].
- **Cray OpenSHMEMX `SHMEM_MAX_CTX`:** for THREAD_MULTIPLE the default is the "maximum number of available network resource available per PE in the node", which "depends on the total number of PEs per node" [doc https://github.com/PE-Cray/cray-openshmemx/blob/master/doc/source/intro_shmem.rst]. **Contexts are a per-node NIC resource divided among PEs.** An implementation cannot promise unlimited hardware-backed contexts.

## A7. GPU memory

No DMAPP or uGNI GPU-memory registration is documented in the sources read. **[recalled: Cray XK7 GPU RDMA went through Cray MPICH/CUDA-aware paths, not DMAPP SHMEM. Unverified.]**

## A8. What Cray SHMEM / OpenSHMEMX and peers emulated in software (priority)

Cray SHMEM source is closed. The evidence is Cray's docs plus open implementations on the same hardware.

1. **Symmetric-heap allocator and symmetry discipline** lived above the NIC, in the DMAPP library region allocator [doc DMAPP10 §2.2.6, §3.2]. Later it moved to Cray DSMML ("Cray DSMML is used by default" as "the symmetric heap memory management layer") [doc cray-openshmemx intro_shmem.rst `SHMEM_USE_DSMML`].
2. **Every collective was built in software from nonblocking PUTs**, except any DMAPP-offloaded reductions.
   - `shmem_alltoall` is strip-mined into blocks: `SHMEM_ALLTOALL_BLOCK_SIZE` "for Cray XE systems … between 256 and 1536".
   - It issues non-blocking PUTs with a sync frequency: `SHMEM_ALLTOALL_SYNC_FREQ` "for Gemini systems … between 1 and 256".
   - The SMP-aware alltoall "requires (number_of_nodes * number_of_PEs_per_node^2 * len_of_msg) extra bytes of symmetric memory", 4 MB by default.
   - Source for all three: [doc cray-openshmemx intro_shmem.rst]. The primitive underneath is **batched nb-PUT with a completion high-water mark**, i.e. DMAPP's `_nbi` + `gsync`.
3. **Team collective state is preallocated per team.** `SHMEM_NUM_TEAM_PSYNC` defaults to 512 concurrent teams per process. Exceeding it "will run out-of internal memory space to maintain psync arrays" [doc cray-openshmemx intro_shmem.rst]. psync arrays are symmetric per-team scratch space that SOS also uses.
4. **Thread-safe contexts over limited NIC resources** (A6). Cray also exposed non-standard thread registration (`shmemx_thread_register`, `shmemx_thread_fence`, `shmemx_thread_quiet`), sessions (`shmemx_session_start/stop`) and **handle-returning nonblocking ops** (`shmemx_put_nb`, `shmemx_get_nb`, `shmemx_add_nb`, `shmemx_cswap_nb`, `shmemx_local_complete`) [src, file list of https://github.com/PE-Cray/cray-openshmemx/tree/master/doc/source]. These are DMAPP's explicit `_nb`/syncid model surfaced to users, because OpenSHMEM's `_nbi` has no handles.
5. **Short-integer atomics.** `shmemx_short_add/cswap/fadd/finc/inc/swap` exist [src, same file list], but DMAPP AMOs are 64-bit only [doc DMAPP10 §2.2.4]. Non-64-bit atomics (and 32-bit and OpenSHMEM 1.6's wider type set) must be emulated by widening, lock-based, or messaging. **[inference; the shmemx_short_* bodies were not read]**
6. **Messaging for anything not RMA** (bootstrap, rendezvous, error notifications) went over SMSG/MSGQ, with the per-peer costs in A3 [doc GNI11; HOTI12].
7. **Registration cache** (uDREG, a kernel MMU-notifier driver) for non-symmetric memory [doc GNI11 §4.7].
8. **Transient-error replay** for RDMA, and avoidance of AMOs where replay is unsafe [doc GNI11 §4.6].
9. **Current Cray OpenSHMEMX drops DMAPP entirely.** "The current version support[s] the following transport layers: SMP-OFI"; "On Cray Shasta systems, Cray OpenSHMEMX uses libfabric (OFI)" [doc cray-openshmemx intro_shmem.rst]. A `dmapp` debug-log category still exists in the docs, a vestige of the XC layer.

## A9. Performance pathologies and scaling limits

- **SMSG per-peer memory** (A3) and startup time with eager connection setup [doc GNI11 §4.2].
- **The FMA→BTE switch** at DMAPP's default 4 KB threshold visibly steps bandwidth. The threshold is a latency-vs-CPU trade the program model must tune [doc DMAPP10 §3.2, Fig. 3].
- **FMA descriptor updates are expensive.** Strided and indexed operations suffer [doc DMAPP10 §5].
- **The AMO NIC cache holds 64 entries.** Random-target AMO rate drops once targets exceed it (Fig. 6: 1 vs 8192 AMOs) [doc DMAPP10 §4].
- **Per-node registration limit** of 4K registrations with scalable MR [doc fi_gni(7)].
- **Ordering vs adaptive routing:** in-order requires `NO_ADAPT`/`IN_ORDER` modes [src OMPI btl_ugni_rdma.h].
- **Buffer alignment:** GET via BTE needs "a multiple of 4 and 4-byte aligned" buffers, else fall back to PUT [doc HOTI12 §V.C].

## A10. Build, portability, maintenance

- Proprietary libraries (libdmapp, libugni) tied to ALPS-issued ptags and the CLE kernel (kGNI) [doc GNI11 §2.1].
- The line ended with Slingshot. Cray OpenSHMEMX is OFI-only today (A8.9). The libfabric `gni` provider last appears in the v1.15 docs used here. **[recalled: removed from libfabric around 1.16–1.17]**
- Code written to DMAPP (Cray SHMEM, Cray UPC/CAF runtime) had to be rewritten for Shasta. This is a cautionary tale for a core API that mirrors one vendor's API too closely.

---

# Part B. Fujitsu uTofu (Tofu Interconnect D, Fugaku)

## B0. Evidence quality

The primary uTofu User's Guide could not be fetched (see source notes). The items marked **[doc-excerpt]** come from search-engine excerpts of that guide (J2UL-2482). The rest is from the Fugaku UCX paper abstracts or **[recalled]**.

## B1. Primitives

- **Hardware per node [doc-excerpt, abstract of Tagawa et al., "Design and performance evaluation of UCX for the Tofu Interconnect D on Fugaku…", J. Supercomputing 2024, https://link.springer.com/article/10.1007/s11227-024-06201-x]:** the A64FX has an on-die TofuD controller with **six TNIs** (Tofu Network Interfaces) and **ten network ports** (6D mesh/torus). Fugaku has **158,976 nodes** [doc-excerpt, https://en.wikipedia.org/wiki/Fugaku_(supercomputer)].
- **One-sided RDMA put/get** between VCQs (virtual control queues), addressed by a remote **STADD** (start address) plus offset [doc-excerpt, uTofu guide]. "STADD values are managed for each VCQ" [doc-excerpt].
- **ARMW (atomic read-modify-write)** "is executed by the Tofu interconnect". `utofu_armw8` is in the function reference [doc-excerpt]. **[recalled]:**
  - 8-byte and 4-byte variants (`utofu_armw8`, `utofu_armw4`);
  - operations CSWAP, SWAP, ADD, XOR, AND, OR, with the old value returned to the initiator;
  - this is the closest hardware match to OpenSHMEM atomics among all surveyed networks, alongside DMAPP AMOs.
- **Send with piggyback:** small messages carried in the control queue entry. **[recalled: ~32 bytes; unverified]**
- **Collective offload** ("Tofu barrier gate" / Tofu Barrier Interface) for barrier and small allreduce/reduce on specific ports, used by Fujitsu MPI. **[recalled; unverified]**
- **Tag matching and active messages:** none in uTofu. **[recalled]**

## B2. Registration and keys

- Memory is registered per VCQ (`utofu_reg_mem`) and yields a STADD. `utofu_reg_mem_with_stag` lets the caller choose an **STAG** (steering tag) [doc-excerpt: function name appears in guide index].
- **[recalled]:**
  - With a caller-chosen STAG, every process can register its symmetric heap under the **same STAG number**.
  - A remote STADD can then be *computed* from (remote VCQ ID, STAG), with no per-PE key exchange. This is analogous to DMAPP's locally generated segment descriptor.
  - Reserved STAG count and maximum registrations per VCQ are limited (queryable via `utofu_query_onesided_caps`).
  - Verify both points against the guide.
- **Catch: STADDs are per VCQ, not per process [doc-excerpt].** If each OpenSHMEM context (VCQ) must register the heap separately, registration cost multiplies by contexts.

## B3. Per-peer state (priority)

- **Peer addressing:** a peer is addressed by a **VCQ ID** (64-bit, encodes Tofu coordinates, TNI, CQ and component id). No connection setup is needed. **[recalled]** The RIKEN QCD library stores exactly `target_vcq_id` (uint64) per peer plus a STADD per remote buffer [src https://github.com/RIKEN-LQCD/qws/blob/master/rdma_utofu_comlib.h `rdma_comlib_communicator`, `rdma_comlib_data`].
- **Per-peer cost is O(1) small integers:** VCQ ID (8 bytes) plus remote STADD(s) (8 bytes each), unless STAG-computed addressing removes the STADD. At 10^5 PEs × 2 STADDs + 1 VCQ ID that is 2.4 MB per process in the worst case [arith].
- **The scarce resource is VCQs per node.** "Users can use up to nine CQs per TNI and can create up to eight VCQs per CQ" [doc-excerpt, uTofu guide].
  - That gives 6 TNIs × 9 CQs = 54 hardware CQs per node for (on Fugaku) 48 cores [arith].
  - VCQs multiplex 8:1 over a CQ. **[recalled: VCQs sharing a CQ share its hardware queue, so throughput is not multiplied]**
- **Path selection:** routes are chosen by a path ID (`utofu_get_path_id`) per destination. **[recalled]** Any per-peer path cache is O(N).

## B4. Ordering and delivery

**[recalled; unverified]**
- Tofu-D uses per-packet choice among a small number of routes (the "Tofu coordinates" plus path ID). Delivery is reliable in hardware.
- uTofu does not promise ordering across different path IDs. Ordered delivery between a VCQ pair holds only if the path is fixed.
- MRQ (message receive queue) notifications arrive in completion order.

## B5. Completion and progress

- **Poll-driven:** initiators poll the TCQ (transmit completion queue) and receivers poll the MRQ (message receive queue). The QCD library tracks "number of mrq to poll (0 or 1)" [src qws rdma_utofu_comlib.h field `remaining_remote_mrq`]. **[recalled]** RDMA put/get can optionally generate a remote MRQ notification; without it the target is passive.
- **One-sided RMA and ARMW need no target progress thread** because they are hardware-executed [doc-excerpt: "executed by the Tofu interconnect"].

## B6. Threads and contexts

- VCQ handles are the unit of thread independence. The QCD library duplicates communicators "with the same vcq" [src qws `rdma_comlib_new_duplicate`].
- The UCX work targeted multithreaded RDMA. UCX on uTofu showed "about 32.8 times lower latency than Fujitsu MPI with 24 threads" and "about 37.8 times higher update rate" in GUPs on 256 nodes [doc-excerpt, J. Supercomputing 2024 abstract]. Fujitsu MPI's multithreaded path was the bottleneck; uTofu itself was not.
- Hard cap: 54 CQs × 8 VCQs = 432 VCQs per node [arith from doc-excerpt]. At 48 PEs per node that is about 9 VCQs per PE, so **shmem_ctx_create must fail or share** beyond that.

## B7. GPU memory

Not applicable to Fugaku (A64FX, no GPUs). uTofu GPU registration is not documented in the sources read.

## B8. What OpenSHMEM or PGAS on Tofu emulated (priority)

- **OpenSHMEM-UCX on Tofu-D exists as research.** "Design and Performance Evaluation of UCX for Tofu-D Interconnect with OpenSHMEM-UCX on Fugaku" (HPC Asia 2023 workshop; title documented at https://www.researchgate.net/publication/367563945). The body was not read.
  - The UCX layering implies software emulation of tag matching, eager/rendezvous, and UCX rkey packing on top of STADDs.
  - Whether UCX's atomics (`ucp_atomic_*` was removed from UCX, **[recalled]**) used ARMW is **unverified**.
- **MPICH on Fugaku** used libfabric with a uTofu-based provider on MPICH 3.4, with MPICH modifications [doc-excerpt, search result summarizing RIKEN R-CCS software, https://riken-rccs.github.io/]. Provider source was not located. A libfabric Tofu provider would emulate FI_TAGGED and FI_MSG ordering in software. **[inference]**
- **No SOS-on-uTofu or OSHMPI-on-Fugaku source or paper was found.** OSHMPI on Fugaku would sit on Fujitsu MPI RMA. **[unverified]**
- **Big-picture lesson [inference from B1–B6]:** Tofu offers RMA, 4/8-byte ARMW and hardware barrier offload, but no matching. An OpenSHMEM would need software for:
  - symmetric heap and STAG discipline;
  - teams and collectives beyond barrier/small reductions;
  - non-4/8-byte atomics;
  - context-to-VCQ multiplexing once VCQs run out.

## B9. Pathologies and limits

- **VCQ/CQ count per node** (B6) caps contexts.
- **Registration is per VCQ** (B2), so it multiplies with contexts.
- **Fujitsu MPI multithreaded communication** was 30–40× worse than direct uTofu/UCX in the cited benchmarks [doc-excerpt].

## B10. Build and maintenance

Proprietary (Fujitsu Technical Computing Suite), available only on Fujitsu Tofu systems (Fugaku, FX1000/FX700). Documentation access is fragile: the canonical guide URL now redirects. The UCX `utofu` transport is research code. **[recalled: not in upstream UCX]**

---

# Part C. AWS EFA / SRD

## C1. Primitives

- **SRD device semantics [doc Shalev et al., "A Cloud-Optimized Transport Protocol for Elastic and Scalable HPC", IEEE Micro 40(6) 2020, https://ieeexplore.ieee.org/document/9167399, PDF https://saeed.github.io/CS8803_DNS_Spring2024/assets/srd.pdf]:**
  - Reliable, **out-of-order**, message (not stream) datagrams.
  - Message size limited to the MTU, no segmentation.
  - Kernel driver doc: "Currently only Send operation is supported, but nothing precludes RDMA operations support in future (with weak memory consistency)" [doc https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/efa/SRD.txt]. That text predates device RDMA.
- **Current device capabilities [doc LF/prov/efa/docs/efa_rdm_protocol_v4.md §1.1]:**
  1. send/recv up to the MTU;
  2. "RDMA read of a memory buffer up to 1GB";
  3. "RDMA write of a memory buffer up to 1GB".
  - Send/recv has no ordering guarantee.
- **RDMA availability by instance type [doc https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/efa.html]:**
  - "RDMA write on most supported instance types that have Nitro version 4 and later. RDMA read is supported on all instances with Nitro version 4 and later."
  - Nitro v3 (c5n, m5n, g5, p3dn…) has **no** RDMA read or write.
  - p4d/p4de: read only.
  - c7gn and hpc7g: **RDMA read yes, RDMA write no**.
  - "RDMA write is not supported with all instance types."
- **libfabric `efa` fabric [doc LF/prov/efa/docs/efa_fabric_comparison.md; fi_efa(7) https://ofiwg.github.io/libfabric/main/man/fi_efa.7.html]:**
  - Features: FI_MSG, FI_TAGGED, FI_RMA, FI_ATOMIC, FI_DIRECTED_RECV, FI_MULTI_RECV, FI_HMEM.
  - "FI_TAGGED: efa provides support through software emulation".
  - "FI_ATOMICS: efa provides support through software emulation".
  - RMA is unlimited in size (segmented above the 1 GB device max).
- **libfabric `efa-direct` fabric [doc same]:** only FI_MSG (≤ ~8 KiB MTU), FI_RMA (≤ max_rdma_size "~1 GB", 1 IOV) and FI_HMEM. **No FI_ATOMIC, no FI_TAGGED**, "message ordering ❌".
- **Atomics are never offloaded.** The wire protocol has WRITE_RTA, FETCH_RTA and COMPARE_RTA packets ("Request To Atomic"), handled by the target's provider. Atomics are limited to "up to MTU size" [doc efa_rdm_protocol_v4.md §1.1, packet table]. Queryable via `FI_OPT_EFA_EMULATED_READ/WRITE/ATOMICS` [doc fi_efa(7)].
- **Counters:** "Both fabrics support local and remote operation counters" (fi_counter) [doc efa_fabric_comparison.md].
- **Collective offload:** none.

## C2. Registration and keys

- **Required MR modes [doc efa_fabric_comparison.md MR table]:** both fabrics require `FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR`. efa-direct also requires `FI_MR_LOCAL`. `FI_MR_SCALABLE` is not supported.
  - So keys are **provider-chosen** and must be exchanged with every peer.
  - A single MR can cover a whole symmetric heap (up to device limits), but its rkey must be distributed.
- **SOS under PROV_KEY [src SOS/src/transport_ofi.c ~L1645]:** "Heap and data use different MR keys, need at least 1 byte of key space if using provider selected keys". The key exchange goes through the runtime KVS at init.
  - Result: 2 keys × N PEs stored on each PE, i.e. O(N) state and an O(N²) bootstrap KVS at 10^5 PEs [arith: 2 × 8 B × 10^5 = 1.6 MB/PE of keys alone].
- **HMEM without p2p [doc efa_fabric_comparison.md]:** "efa fabric will emulate a lkey/rkey generated by Libfabric and uses internal MR map to verify the key on the target side", with bounce-buffer copies.

## C3. Per-peer state (priority)

**Device and verbs level:**
- Each remote *endpoint GID* needs an **address handle (AH)** created with `ibv_create_ah`. This is a kernel verb and a round trip [src LF/prov/efa/src/efa_ah.c `efa_ah_base_construct`]. AHs are ref-counted per GID, so they scale with remote *nodes* rather than remote processes.
- "Each AH is implicitly associated with an SRD context" on the NIC, which holds reliability state per remote node [doc SRD.txt §1].
- **QPs:** RC needs N·p·p QPs per node, RD needs p. SRD keeps RD's O(p) QPs without RD's "single outstanding message per EE context" limit [doc SRD.txt §1; IEEE Micro 2020]. **This is SRD's central scalability win.**
- AH creation fails with EINVAL across Availability Zones [src efa_ah.c warning text]. "EFA traffic can't cross Availability Zones or VPCs" and "is not routable" [doc AWS EFA guide].

**libfabric `efa` (RDM) per-peer structure `struct efa_rdm_peer` [src LF/prov/efa/src/rdm/efa_rdm_peer.h]:**
- reorder buffer `robuf`, default `EFA_RDM_PEER_DEFAULT_REORDER_BUFFER_SIZE (16)` entries, tunable by `FI_EFA_RECVWIN_SIZE` [src LF/prov/efa/src/efa_env.c];
- `next_msg_id`;
- handshake flags and `extra_info[]` feature bits;
- outstanding-TX lists;
- receiver-not-ready (RNR) backoff state;
- txe/rxe lists;
- an overflow packet list;
- the peer's user_recv_qp;
- an `rxe_map` hash map.

In addition, the address vector (FI_AV_TABLE) holds an `efa_av_entry` per peer [src LF/prov/efa/src/efa_av.h].

**Handshake before RMA:**
- Before the first RMA to a peer, the provider **forces a handshake** unless `FI_OPT_EFA_HOMOGENEOUS_PEERS` is set: "A handshake is required to choose the correct protocol (whether to use device read)" [src LF/prov/efa/src/rdm/efa_rdm_rma.c `efa_rdm_rma_post_read`].
- The first get/put to each of 10^5 PEs therefore pays an extra message round trip and needs target progress.
- RDMA read is not interoperable between 0xEFA0 and 0xEFA1+ devices [src LF/prov/efa/src/rdm/efa_rdm_ep.h `efa_rdm_interop_rdma_read`].

**efa-direct RMA needs target-side address knowledge.** The "Target must pre-insert initiator address in AV for efa-direct fabric". The same applies to the efa fabric when `FI_OPT_EFA_HOMOGENEOUS_PEERS=true` [doc fi_efa(7)]. For OpenSHMEM this forces **every PE to insert every other PE** before any one-sided access: O(N) AV entries plus O(N/p) AHs on every PE at init.

## C4. Ordering and delivery

- **SRD delivers out of order by design.** The NIC sprays packets "over as many network paths as possible, while avoiding overloaded paths" using per-path RTT, and reroutes retransmissions "without costly connection re-establishment" [doc IEEE Micro 2020].
  - On why not reorder in the NIC: it "would either limit scalability or increase average latency in the presence of drops".
  - The design deliberately leaves "message order restoration to the upper layer".
- **Delivery semantics [doc SRD.txt §1, §3.3]:**
  - At-most-once, with send completion "after the WR is acked by the responder".
  - "SRD RNR error – returned for requests rejected by the responder because Receive Queue does not have posted WRs. Requester does not perform any retries". The provider therefore implements RNR backoff per peer [src efa_rdm_peer.h `rnr_backoff_*`, `EFA_RDM_PEER_IN_BACKOFF`].
- **What the `efa` fabric restores in software:**
  - FI_ORDER_SAS plus FI_ORDER_ATOMIC_{RAR,RAW,WAR,WAW} [doc efa_fabric_comparison.md], via `msg_id` in RTM and RTA headers and the per-peer reorder buffer [doc efa_rdm_protocol_v4.md: "msg_id … used to implement ordered atomic operations … required by some applications such as MPICH"].
  - **Writes are not ordered:** "there is no msg_id in the eager RTW header because the EFA provider does not support ordered write operation" [doc efa_rdm_protocol_v4.md §3].
- **efa-direct:** no ordering at all [doc efa_fabric_comparison.md].
- **For OpenSHMEM:** it requires no ordering between independent puts to distinct addresses, but overlapping puts from one context should land in call order. That is not guaranteed by SRD RMA. **[inference]**

## C5. Completion and progress (critical for one-sided semantics)

- **Progress modes [doc fi_efa(7)]:** `efa` RDM is **FI_PROGRESS_MANUAL**; efa-direct is FI_PROGRESS_AUTO.
- **Emulated RMA is two-sided in disguise.**
  - Without device RDMA, a read becomes a SHORT_RTR or LONGCTS_RTR packet [src LF/prov/efa/src/rdm/efa_rdm_rma.c `efa_rdm_rma_post_efa_emulated_read`]. Writes become EAGER_RTW or LONGCTS_RTW, and atomics become *_RTA [doc protocol_v4 packet table].
  - These are all served by the target's libfabric progress engine. With manual progress, the **target PE must be inside `fi_cq_read`** for a remote get, put or atomic aimed at it to complete.
  - A PE busy in a long compute loop stalls its peers unless the OpenSHMEM layer runs a progress thread.
  - Atomics and the handshake always have this property, even on Nitro v4+.
- **Delivery complete (DC) is emulated** too: DC_EAGER_RTW, DC_LONGCTS_RTW, DC_WRITE_RTA [doc protocol_v4 table]. SOS requests `FI_DELIVERY_COMPLETE` on all TX [src SOS/src/transport_ofi.c `tx_attr.op_flags = FI_DELIVERY_COMPLETE`].
- **Queue back-pressure:** efa-direct returns -FI_EAGAIN on full queues, and "the application must call fi_cq_read to make progress and free CQ space before retrying" [doc fi_efa(7)]. Queue sizes are `FI_EFA_TX_SIZE`/`RX_SIZE`/`CQ_SIZE` (device-dependent) [doc fi_efa(7)].

## C6. Threads and contexts

- FI_THREAD_SAFE, FI_THREAD_COMPLETION and FI_THREAD_DOMAIN are supported [doc fi_efa(7)].
- "Instance types that support multiple network cards can be configured with one EFA per network card. All other supported instance types support only one EFA per instance" [doc AWS EFA guide].
- Each endpoint owns an SRD QP plus CQ. QPs are "expensive resources" [doc IEEE Micro 2020]. **[recalled: the device QP count per EFA is limited to a few hundred; unverified]**
- Scalable endpoints / shared TX contexts: not documented for efa in the sources read. SOS falls back to no STX when `max_ep_stx_ctx == 0` [src SOS/src/transport_ofi.c].

## C7. GPU memory

- FI_HMEM on both fabrics. GPUDirect RDMA (GDR) and GPUDirect Async (GDA) only on listed instances: p5/p5e/p5en, p6, g7, and others [doc AWS EFA guide table].
- Without p2p, the efa fabric emulates HMEM with bounce buffers and a software-generated key. efa-direct "requires peer-to-peer support" [doc efa_fabric_comparison.md].
- Neuron/Trainium HBM needs device RDMA read: "rdma read is required to post read for AWS trainium memory" [src efa_rdm_rma.c].

## C8. What OpenSHMEM-style consumers had to emulate on EFA (priority)

1. **Out-of-order to ordered messaging:** the reorder buffer and msg_id per peer (C3, C4).
2. **Tag matching, directed receive, multi-recv:** all in software in the `efa` fabric, all absent in efa-direct [doc efa_fabric_comparison.md].
3. **RMA on hardware without device RDMA** (Nitro v3; RDMA write on c7gn/hpc7g/p4d): emulated with RTW/RTR packets plus target copy, which makes it target-progress dependent (C5) [src efa_rdm_rma.c; doc AWS table].
4. **All atomics**, emulated through RTA packets, MTU-sized only, target-progress dependent. There is no hardware ARMW or AMO [doc protocol_v4].
5. **Segmenting RMA above ~1 GB** into multiple device operations [doc efa_fabric_comparison.md].
6. **Delivery complete** (DC) protocols [doc protocol_v4].
7. **Capability negotiation handshake** per peer before RMA, plus handling of mixed device generations [src efa_rdm_ep.h, efa_rdm_rma.c].
8. **RNR backoff** because SRD does not retry receiver-not-ready [doc SRD.txt; src efa_rdm_peer.h].
9. **HMEM without p2p:** bounce buffers and software keys [doc efa_fabric_comparison.md].
10. **Evidence gap: SOS and OSHMPI on EFA.**
    - SOS's OFI transport requires FI_RMA + FI_ATOMIC, FI_DELIVERY_COMPLETE and inject support [src SOS/src/transport_ofi.c ~L1500–1550]. It carries CXI-specific workarounds but **no EFA-specific code** [src: grep of SOS/src found no `efa`].
    - SOS could only run on the `efa` fabric, with atomics, DC and possibly RMA all emulated, never on efa-direct. **[inference; no SOS-on-EFA report found]**
    - No OSHMPI-on-EFA report was found. **[unverified]**
    - AWS lists Open MPI ≥4.1, Intel MPI, NCCL and NIXL as supported, not OpenSHMEM [doc AWS EFA guide].

## C9. Pathologies and limits

- **TCP comparison:** TCP's 50 ms minimum RTO produces tail latency. SRD's point is to keep tails low via multipath and fast retransmission [doc IEEE Micro 2020 Fig. 3 discussion].
- **Spraying makes incast worse:** "micro-bursts from the same sender … may arrive on different paths simultaneously". SRD therefore uses a BBR-like rate and in-flight limit per connection [doc IEEE Micro 2020].
- **Out-of-order segments** of a large *message* force a copy unless RDMA is used [doc IEEE Micro 2020, "Out of Order Packet Handling Challenges"].
- **Default reorder window of 16 entries per peer:** out-of-order arrivals beyond it go to an overflow list [src efa_rdm_peer.h `overflow_pke_list`]. Under heavy spraying with many in-flight small messages this is extra software cost. **[inference]**
- **fi_cancel:** limited in efa and absent in efa-direct. **MR abort** is incompatible with emulated RMA and atomics [doc fi_efa(7)].
- **Heterogeneous instance fleets:** mixed RDMA read/write capabilities within one job silently fall back to emulation for some peer pairs (C1, C3).
- **Topology:** single AZ, non-routable (C3). Job size is bounded by placement-group capacity. **[recalled]**

## C10. Build and maintenance

- Open source (libfabric `prov/efa`, rdma-core `efa` driver, `amzn-drivers`). Very actively developed; the commit used here is dated the survey day.
- The provider now has **two fabrics** (`efa`, `efa-direct`), a versioned wire protocol (v4) with feature and request bits, and a handshake for backward compatibility [doc protocol_v4 §2]. That is a significant compatibility surface.
- Only on AWS; no emulator for off-cloud CI. **[recalled]**

---

# Cross-substrate comparison

| Property | DMAPP (Gemini/Aries) | uGNI | uTofu (Tofu-D) | EFA `efa` fabric | EFA `efa-direct` |
|---|---|---|---|---|---|
| Hardware RMA put/get | yes (FMA ≤4K, BTE above) [doc] | yes [doc] | yes [doc-excerpt] | Nitro v4+ read; write on most v4+ [doc] | same, ≤1 GB [doc] |
| Hardware atomics | 64-bit ADD/AND/OR/XOR, fetch forms, CSWAP, FAX [doc] | AMOs via FMA [doc] | ARMW 4/8-byte [doc-excerpt/recalled] | none; emulated ≤ MTU [doc] | none [doc] |
| Keys for symmetric heap | none: locally generated descriptor [doc] | per-region MH exchange [doc] | STADD per VCQ; STAG can make it computable [doc-excerpt/recalled] | PROV_KEY per MR, exchange required [doc] | same, plus FI_MR_LOCAL [doc] |
| Per-peer RMA state | ~0 | EP (cheap) + MH | VCQ ID (8 B) (+STADD) | AV entry + peer struct + handshake; AH per remote node | AV entry; **target must know initiator** |
| Per-peer messaging state | n/a | SMSG mailbox 1–4.7 KB per peer [doc] | none (piggyback) [recalled] | reorder buffer etc. [src] | none |
| Ordering | FMA ordered unless adaptive [src OMPI modes] | modes: PERFORMANCE / NO_ADAPT / IN_ORDER [src] | per-path [recalled] | SAS + atomic ordering in software; writes unordered [doc] | none [doc] |
| Target progress needed for RMA/atomics | no | no | no | **yes for emulated RMA, all atomics, handshake** | no for RMA (no atomics at all) |
| Collective offload | DMAPP colls (allreduce/bcast/barrier) [doc-excerpt] | no | barrier gate [recalled] | no | no |
| Context cost | FMA descriptor per process; limited per node [doc] | CDM/NIC handles, up to 128 virtual devices in OMPI [src] | ≤ 9 CQ/TNI × 8 VCQ/CQ, 6 TNIs [doc-excerpt] | QP+CQ per EP | QP+CQ per EP |
| Still shipping | no | no | Fugaku/FX only | yes | yes |

---

# Implications for a core API

**Cheap on these substrates (put in the core):**
1. **RMA put/get against a job-wide symmetric segment named without per-peer keys.**
   - DMAPP proved this at production scale (data segment + sheap pre-registered at init, locally generated descriptor).
   - Tofu's caller-chosen STAG can approximate it **[recalled]**.
   - The core should let `(pe, segment_id, offset)` be the address and let the substrate compute any key. On substrates with provider keys (EFA) the core pays O(N) key storage, and the API should make that cost explicit rather than baked in.
2. **Nonblocking RMA with both handle-based and handle-less completion,** plus an "all outstanding done" barrier: DMAPP's `_nb`/syncid and `_nbi`/gsync pair.
   - This maps directly onto `shmem_put_nbi` + `shmem_ctx_sync`.
   - Cray still exposes the handle form as `shmemx_*_nb` extensions.
   - Every surveyed hardware completion model (GNI CQ, Tofu TCQ, EFA CQ) supports it.
3. **Batched and strided RMA** (DMAPP iput/ixput, PE-strided scatter/gather). DMAPP found them worth API space for CAF and small fan-out. The core can expose a vectored put/get; substrates without it loop.
4. **Hardware atomics for 64-bit (and on Tofu 32-bit) CSWAP, ADD, fetch-ADD, XOR/AND/OR** where available (DMAPP, uTofu).
   - The core primitive should be a *64-bit word* read-modify-write with a returned old value.
   - Other widths and types (OpenSHMEM 1.5/1.6 atomics over many integer types) are layered in software.

**Expensive, or known to need emulation (keep out of the core, or make the cost explicit):**
1. **Per-peer messaging buffers.**
   - uGNI SMSG costs 1–4.7 KB per peer. At 10^5 PEs that is ~110 MB/process and GBs per node [arith].
   - The core's small-message or active-message channel must not require per-peer pre-registered buffers. It should use shared, per-node or lazy resources (MSGQ-style, Open MPI Eager GET, or MTU-sized datagrams as on SRD).
2. **Ordered delivery.**
   - Aries obtained it by disabling adaptive routing.
   - SRD refuses to provide it at all and pushes a per-peer reorder buffer into software.
   - The core should promise **no ordering** for RMA except what OpenSHMEM needs: per-context ordering of overlapping writes. That should be a flag the upper layer can request per context, with a known cost (NO_ADAPT on Aries, msg_id plus reorder on EFA).
3. **Atomics on transports that can retry or emulate.**
   - Non-idempotent remote atomics cannot be blindly replayed. Cray MPICH avoided AMOs for this reason.
   - On EFA every atomic is a two-sided request needing target progress.
   - The core atomic primitive must specify exactly-once semantics. The software fallback needs a target progress engine, i.e. an optional progress thread or a documented "target must poll" rule.
4. **Target-side knowledge of initiators.** efa-direct requires the target to pre-insert initiator addresses, and the `efa` fabric needs a per-peer handshake before RMA. A core init call should build the full address vector collectively, with O(N/ppn) address handles, and allow a "homogeneous peers" assertion that skips per-peer negotiation.
5. **Contexts are a finite per-node hardware resource:** Aries FMA/CDM, Tofu 9 CQ × 8 VCQ per TNI, EFA QPs.
   - The core must expose "how many independent contexts are available" (Cray OpenSHMEMX computes this per PEs-per-node).
   - It must let `shmem_ctx_create` fall back to shared resources.
   - It must not multiply registrations per context: Tofu STADDs are per VCQ.
6. **Registration budgets are per node,** e.g. gni scalable MR has 4K registrations per node. Symmetric-heap registration should use one large-page registration per segment per process, done at init.
7. **Collectives, teams and psync state are software everywhere.**
   - Offloads exist only as narrow barrier and small-allreduce engines (DMAPP colls, Tofu barrier gate).
   - At most the core needs an optional "small reduction offload" hook. Team psync arrays (Cray default 512 teams per process) and strip-mined alltoall belong above the core.
8. **Vendor API lock-in.** Cray SHMEM, UPC and CAF on DMAPP all had to be re-hosted onto OFI for Slingshot. The core should be defined by semantics, not by one vendor's call surface.
