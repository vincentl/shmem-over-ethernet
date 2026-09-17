# Q2 architect: a minimal core API for OpenSHMEM 1.4–1.6 at 10^5+ PEs

Architect report for BRIEF-Q2. Date: 2026-09-16.

## 0. How to read this report

### Source tags

This report adds no new primary research. Every design fact is taken from the evidence reports and cited with a short tag:

| Tag | Report (all under `scratchpad/out/`) |
|---|---|
| SEM §x | `q2-semantics.md` (per-routine classification, Annex G deltas) |
| USE §x | `q2-usage.md` (which routines real codes call, and how hot) |
| OFI §x | `q2-ofi.md` (libfabric cxi, verbs;rxm, efa, opx; SOS OFI transport) |
| UCX §x | `q2-ucx.md` (UCX, OSSS-UCX, Open MPI oshmem) |
| P4 §x | `q2-portals.md` (Portals 4, SOS Portals transport, reference implementation, BXI) |
| GEX §A.x / GPI §B.x / LCI §C.x | `q2-gasnet-gpi-lci.md` |
| MPI §x | `q2-mpi.md` (OSHMPI, MPICH ch4, Open MPI osc) |
| NV §x | `q2-nvshmem.md` |
| VEN §A/B/C.x | `q2-vendor.md` (A = DMAPP/uGNI, B = uTofu, C = EFA/SRD) |
| FUT §A/B.x | `q2-future.md` (A = Ultra Ethernet, B = CXL/OpenFAM) |
| SPEC Lnnnn | `scratchpad/spec16.txt`, the OpenSHMEM 1.6 text. I re-read these lines myself. |

### Cross-checking

- The brief requires each design-bearing fact to be backed by at least two reports, or by one report plus the spec.
- Where I could find only one source, the fact is marked **single-source**.
- Where reports disagree, the disagreement is stated inline and collected in §8.
- Anything I add from memory is marked *recalled*.
- Arithmetic of my own is marked *arith*.

### Names

- The API prefix `oc_` ("OpenSHMEM core") is a placeholder.
- "PE" and "rank" mean the same thing here. Core ranks are always **world** ranks.

---

## 1. Summary: the proposed core on one page

### 1.1 Design in six sentences

1. **Memory.** The core moves bytes and 4/8-byte words to a `(world rank, segment id, offset)` address. The segment id is chosen by the caller and is identical on every PE. Each segment is registered once per process, and all contexts share that registration.
2. **Operations.** There are exactly three kinds of data operation:
   - `put`/`get`, with a scalar immediate-data variant;
   - `rmw`, a remote read-modify-write on one word with exactly-once delivery;
   - a fence, either as a per-operation flag or as a lazy per-context call.
3. **Completion.** Completion is reported only through per-context counters (issued, done, failed) with a threshold wait. The wait guarantees remote visibility. No request handles exist on the fast path.
4. **Target side.** Target-side support is limited to a counter of incoming `rmw` operations. It is bound only to segments the OpenSHMEM layer marks for signals or waits, so the bulk heap keeps its fast, counter-free registration.
5. **Everything else sits above the core:** teams, collectives, locks, strided and block RMA, multi-element waits, sessions, and memory allocation.
6. **Declared costs.** The core declares through capability bits which primitives need target CPU progress, whether NIC atomics are CPU-coherent, whether fence is free, and how many contexts are affordable.

### 1.2 C signature sketch

```c
/* ================= domain, runtime, capabilities ================= */
typedef uint32_t oc_rank_t;        /* world rank; never narrower (cf. 16-bit PE fields, NV §3.7) */
typedef uint8_t  oc_seg_t;         /* caller-chosen, same value on every PE */
typedef struct oc_dom oc_dom_t;    /* per process: NIC(s), address cache, segments */
typedef struct oc_ctx oc_ctx_t;    /* per context: tx queue + counters + ordering domain */

typedef struct {                   /* supplied by the launcher/runtime (PMIx, uid, MPI, ...) */
  int  (*lookup)(void *arg, oc_rank_t r, void *blob, size_t *len); /* lazy, per touched peer */
  int  (*node_of)(void *arg, oc_rank_t r, uint32_t *node);          /* locality, O(1) or cached */
  void (*abort)(void *arg, int code);                               /* shmem_global_exit */
  void  *arg;
} oc_runtime_t;

typedef struct {
  uint64_t caps;      /* OC_CAP_* bits below */
  uint32_t target_cpu;/* mask of OC_OPC_* that need target-side CPU progress on this substrate */
  uint32_t rmw_ops;   /* bit per oc_rmw_op_t natively supported (else core emulates, see §7) */
  size_t   max_inline, max_rma, max_ctx_hint;  /* limits; max_ctx_hint is per process */
  uint32_t max_segs;
} oc_caps_t;
#define OC_CAP_SYMMETRIC_KEYS   (1u<<0)  /* no per-peer key/base state */
#define OC_CAP_FENCE_NATIVE     (1u<<1)  /* OC_F_FENCE executed by NIC/provider, no host RTT */
#define OC_CAP_ORDERED          (1u<<2)  /* per-(ctx,peer) delivery order: fence is a no-op */
#define OC_CAP_RMW_CPU_COHERENT (1u<<3)  /* NIC rmw coherent with CPU load/store */
#define OC_CAP_NOTIFY_NATIVE    (1u<<4)  /* oc_notify_* without target polling */
#define OC_CAP_CONNECTIONLESS   (1u<<5)  /* per-peer state = address entry only */
#define OC_CAP_DEVICE_INITIATED (1u<<6)  /* data ops callable from GPU kernels */
#define OC_DOM_PROGRESS_THREAD  (1u<<0)  /* dom_open flag: core runs its own progress thread */

int  oc_dom_open (const oc_runtime_t *rt, oc_rank_t me, oc_rank_t npes,
                  uint64_t flags, oc_dom_t **dom);
int  oc_dom_query(oc_dom_t *dom, oc_caps_t *caps);
int  oc_dom_close(oc_dom_t *dom);

/* ================= memory ================= */
enum { OC_MEM_HOST, OC_MEM_CUDA, OC_MEM_ROCM, OC_MEM_ZE };
#define OC_SEG_RMW     (1u<<0)  /* may be rmw target; registered atomic-capable */
#define OC_SEG_NOTIFY  (1u<<1)  /* bind incoming-rmw counter (never set on the bulk heap) */
int   oc_seg_register(oc_dom_t*, oc_seg_t id, void *base, size_t len, int memkind, uint32_t attrs);
void *oc_seg_peer_ptr(oc_dom_t*, oc_rank_t r, oc_seg_t id);  /* non-NULL iff load/store-mappable */
int   oc_seg_sync    (oc_dom_t*, oc_seg_t id);   /* PtlAtomicSync-like; no-op if RMW_CPU_COHERENT */

/* ================= contexts ================= */
#define OC_CTX_NOLOCK  (1u<<0)  /* caller guarantees serialized use (PRIVATE/SERIALIZED) */
int  oc_ctx_create (oc_dom_t*, uint32_t flags, oc_ctx_t **ctx); /* may fail with OC_EAGAIN */
int  oc_ctx_destroy(oc_ctx_t*);                                /* implicit oc_ctx_wait(ALL) */

/* ================= per-operation flags ================= */
#define OC_F_ASYNC  (1u<<0)  /* return after initiation (nbi); default: blocking semantics */
#define OC_F_FENCE  (1u<<1)  /* apply only after all earlier ops on (ctx, rank) have completed */
#define OC_F_MORE   (1u<<2)  /* more ops follow: coalesce doorbell/packets */
#define OC_F_TRY    (1u<<3)  /* return OC_EAGAIN on backpressure instead of progressing */

/* ================= RMA ================= */
int oc_put     (oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, const void *src, size_t len, uint32_t fl);
int oc_get     (oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, void *dst, size_t len, uint32_t fl);
int oc_put_word(oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, uint64_t val, unsigned w, uint32_t fl);
int oc_get_word(oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, unsigned w, uint64_t *val); /* blocking */

/* ================= remote read-modify-write (w = 4 or 8) ================= */
typedef enum { OC_RMW_WRITE, OC_RMW_READ, OC_RMW_CSWAP, OC_RMW_ADD,    /* required  */
               OC_RMW_MSWAP, OC_RMW_AND,  OC_RMW_OR,    OC_RMW_XOR     /* capability */
} oc_rmw_op_t;
int oc_rmw(oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, oc_rmw_op_t op, unsigned w,
           uint64_t operand, uint64_t cmp_or_mask, uint64_t *old /* NULL: non-fetching */,
           uint32_t fl);

/* ================= ordering and completion ================= */
enum { OC_OPC_PUT = 1, OC_OPC_GET = 2, OC_OPC_RMW = 4, OC_OPC_ALL = 7 };
int oc_ctx_fence  (oc_ctx_t*);  /* O(1), no traffic: next op per peer gets OC_F_FENCE */
int oc_ctx_count  (oc_ctx_t*, unsigned cls, uint64_t *issued, uint64_t *done, uint64_t *failed);
int oc_ctx_wait   (oc_ctx_t*, unsigned cls, int64_t timeout_ns);   /* done==issued AND visible */
int oc_ctx_wait_pe(oc_ctx_t*, const oc_rank_t *pes, size_t n, int64_t timeout_ns); /* may = wait */
int oc_progress   (oc_ctx_t*);  /* drive software paths; cheap no-op on full offload */

/* ================= target-side notification ================= */
int oc_notify_read(oc_dom_t*, oc_seg_t, uint64_t *count);   /* incoming rmw applied to seg */
int oc_notify_wait(oc_dom_t*, oc_seg_t, uint64_t above, int64_t timeout_ns);
```

### 1.3 Size of the core

- The core has 22 entry points, and **only 8 of them are data path**: `put`, `get`, `put_word`, `get_word`, `rmw`, `ctx_fence`, `ctx_count`/`ctx_wait`, and `notify_*`.
- Everything in OpenSHMEM 1.4, 1.5 and 1.6 maps onto these (§4a).
- On Portals 4, Slingshot/CXI, and UEC HPC-profile hardware, the fast path of every HOT usage pattern maps to a native operation (§5).

---

## 2. Primitives: precise semantics and rationale

### 2.1 Addressing and segments (`oc_seg_register`)

**Semantics**

- **Address form.** A remote location is `(rank, seg, off)`.
  - `off` is a byte offset from the start of segment `seg` *on the target*.
  - The OpenSHMEM layer computes `off = addr − local_base(seg)`. Symmetric allocation is deterministic, so the offset of a symmetric object is the same on every PE, and no per-PE base table is needed.
  - For the data/BSS segment, the offset of a global within one executable image is the same across processes even under ASLR (*recalled*: position-independent layouts preserve relative offsets within an image).
- **Registration scope.** Registration is per process, not per context. All contexts of a domain use the same registration.
- **Segment count.** A process registers a small, fixed set of segments at init:
  - `0` = data/BSS
  - `1` = bulk heap
  - `2` = RMW heap (`SHMEM_MALLOC_ATOMICS_REMOTE`)
  - `3` = signal heap (`SHMEM_MALLOC_SIGNAL_REMOTE`)
  - `4+` = device heaps (`memkind`)
- **Reservation.** Each heap segment is a **reserved VA range**, registered once at init. `shmem_malloc` never registers memory.
- **Keys.**
  - With `OC_CAP_SYMMETRIC_KEYS`, the key is derived from `seg` (a caller-chosen, well-known key), so the core stores **zero bytes per peer** for keys.
  - Otherwise the core lazily fetches `(key, remote base)` for each touched peer through `rt->lookup` and caches it at domain level: 16 B × segments per touched peer.
- **Errors.** An `off + len` outside the segment is an initiator-side `OC_EFAULT` where the core can check it. Otherwise it is a target-side failure counted in `failed`, as with Portals truncation or `PTL_NI_SEGV` (P4 §2).

**Rationale**

- **Spec.** It forbids O(N) translation tables in the communication path (SPEC L2002–2006; SEM §2.7).
- **Offset addressing without keys** is already native or near-native on:
  - Portals 4: LE + offset, no keys (P4 §2);
  - DMAPP: locally generated segment descriptors (VEN §A2);
  - libfabric with client keys and no `FI_MR_VIRT_ADDR`: SOS "scalable" mode keeps no per-peer state (OFI §2);
  - UET: well-known user keys and a 64-bit `buffer_offset` (FUT §A.2);
  - GPI-2 `(segment, offset)` (GPI §B.1) and LCI `rmr + disp` (LCI §C.2).
- **Offset addressing also avoids the VA-alignment trap.**
  - GASNet-EX aligned segments are incompatible with PSHM (GEX §A.2).
  - OSHMPI has to do random-address `mmap` coordination and falls back to `base_offsets[N]` (MPI §2, §8.8).
- **Why not per-context registration.**
  - UCX unpacks rkeys per worker, i.e. per context, costing O(N × segments × contexts) (UCX §3, §9).
  - Tofu STADDs are per VCQ (VEN §B2).
  - Open MPI repeats all O(N) setup per `shmem_ctx_create` (UCX §3).
- **Why a fixed small segment count.**
  - CXI has 100 optimized MR keys, 0–99 (OFI §2).
  - UET optimized headers have a 12-bit MR index (FUT §A.2).
  - uGNI allows 4K registrations per node (VEN §A2).
- **Why hints become segments.** The spec already defines `SHMEM_MALLOC_ATOMICS_REMOTE` and `SHMEM_MALLOC_SIGNAL_REMOTE` (SPEC L969–972, L2205–2225). The core turns them into registration attributes.
  - A counter bound to a CXI MR disables optimized RMA for *every* operation on that MR (OFI §2, §9; **single-source** for CXI).
  - UET forbids counters on `IDEMPOTENT_SAFE` MRs (FUT §A.5). This is an analogous rule in a second substrate.
  - UCX reports remote-access errors when memory lacks atomic registration (SEM §4.4, `ucx NEWS` L649).
  - Keeping these attributes off the bulk heap protects the put/get fast path.
- **Thin evidence.** `malloc_with_hints` has no public users (USE §1). The default placement of AMO and wait targets is therefore the bulk heap, which uses the fallbacks in §7. See open question Q10.

### 2.2 `oc_put`, `oc_put_word`

**Ordering**

- There is **no ordering** between operations unless a fence intervenes. This holds even for the fragments of one put.
- The core may spray packets, fragment, stripe across rails, or use unordered transports.
- The only ordering obligation is the fence rule (§2.5).

This matches SPEC L3564–3567: "delivery of data words ... may occur in any order ... two successive put routines may deliver data out of order unless ... shmem_fence" (SEM §2.1, §5.1 R2).

**Completion**

- *Default (blocking) mode* returns at **local completion**: the source buffer may be modified afterwards.
  - Small puts (≤ `max_inline`) are copied or inlined.
  - Larger puts are either zero-copy, with the core waiting for its own local completion, or bounce-copied.
- *`OC_F_ASYNC`* returns after initiation. The source buffer is in use until `done` covers the operation (SPEC §4.2, SEM §2.3).
- Either way, remote completion is observed **only** through `oc_ctx_count` / `oc_ctx_wait(OC_OPC_PUT)`.
- **One put counts as one operation** however the core fragments it. This keeps fence gating and counters meaningful.

**Delivery**

- Delivery is reliable and at-most-once-*applied-after-completion*: once `done` covers a put, the core guarantees that no late duplicate of it will be applied.
- Transports that cannot guarantee this, such as UET RUDI, must not carry puts unless the core puts a guard around them (Q2, §8).

**Atomicity.** None is promised against concurrent `rmw` on the same bytes. The spec makes mixed atomic and non-atomic access undefined (SPEC §3.2 item 3; SEM §2.5a).

**Registration.** The target segment must be registered. The local source buffer may lie anywhere; the core uses inline or bounce copy, or dynamic registration, as it chooses.

**Errors**

- `OC_EAGAIN` is returned only with `OC_F_TRY`; otherwise the core progresses internally.
- Asynchronous failures bump `failed`.
- The OpenSHMEM layer treats `failed > 0` as fatal, as SOS does (P4 §8b.8).

**`oc_put_word`** carries a 4/8-byte value inside the request. It serves `shmem_p` and 1-element typed puts, which are the hottest pattern in Kokkos Remote Spaces, GUPs, bale `lgp_*` and SSCA1 (USE §1, §2.2–2.5). Native carriers include:

- Portals volatile MD (P4 §8a);
- libfabric `FI_INJECT` (OFI §1.1);
- GASNet `PutBlockingVal` (GEX §A.1);
- UET small or optimized RMA headers (FUT §A.2).

**Rationale for no per-op ack**

- A blocking put is locally complete on return (SEM §2.1).
- Every production OpenSHMEM transport completes puts with counters:
  - SOS/OFI: `fi_cntr` (OFI §5);
  - SOS/Portals: `PtlCTWait` (P4 §5);
  - DMAPP `gsync` (VEN §A1);
  - GASNet NBI access regions (GEX §A.1).

### 2.3 `oc_get`, `oc_get_word`

**Semantics**

- *Default mode* returns when the data is in `dst`.
  - Implementation: initiate, then wait on this operation. With counters only, that means wait on `OC_OPC_GET`, as SOS does (OFI §5, P4 §5).
- *`OC_F_ASYNC`* returns after initiation. `dst` is filled by the time `oc_ctx_wait(OC_OPC_GET)` returns (SPEC §9.6.2.2; SEM §4.2).
- **Ordering.** Gets are never fenced (SPEC L8933–8935), so the core may take any path for them.
- **Errors.** Same as put.

**`oc_get_word`** is the blocking scalar read. It exists because a single-word `g` is HOT (USE §2.2–2.3) and because on some substrates a small read needs an internal slot:
- IBGDA uses an ibuf slot plus a CST (NV §8b.2).
- The core can return the value directly and skip the class-wide wait.

### 2.4 `oc_rmw`: remote read-modify-write

**Operations.** `w` ∈ {4, 8}; operands are truncated to `w`.

| op | Effect at target `t` | Fetch `old` | Required? |
|---|---|---|---|
| `WRITE` | `t = operand` | optional (→ swap) | required |
| `READ` | – | yes | required (may be implemented as fetching `ADD 0`) |
| `CSWAP` | `if (t == cmp) t = operand` | optional | required |
| `ADD` | `t = t + operand mod 2^w` | optional | required |
| `MSWAP` | `t = (t & ~mask) \| (operand & mask)` | optional | capability |
| `AND` / `OR` / `XOR` | `t = t op operand` | optional | capability |

**Why widths 4 and 8 are enough.**
- Every standard, extended and bitwise AMO type in Tables 6–8 is `int`, `long`, `long long`, their unsigned forms, `int32/64`, `uint32/64`, `size_t`, `ptrdiff_t`, `float` or `double` (SPEC L4241–4290).
- On LP64 these are all 4 or 8 bytes (*recalled*: platform ABI).
- Floats need only `READ`/`WRITE`, which are byte-exact (SEM §4.4).
- No `short`/`char` AMO exists in 1.4–1.6.

**Completion**
- A non-fetching `rmw` may return before it executes at the target (SPEC §9.7 L4186).
- A fetching blocking `rmw` returns with `*old`.
- A fetching `OC_F_ASYNC` `rmw` fills `*old` by the time `oc_ctx_wait(OC_OPC_RMW)` returns. The `old` buffer is in use until then.
- `rmw` has its own counter class, so nbi AMOs and gets can be drained separately (SOS reuses its get counter, OFI §8).

**Delivery: exactly once.**
- An `rmw` must never be carried by an at-least-once transport. No UET RUDI, no blind replay.
- Evidence: UET §3.4.1.13 via FUT §A.4; Cray MPICH avoided AMOs because they cannot be replayed (VEN §A4); SEM §5.1 R5.
- Where exactly-once is unavailable in hardware, the core must emulate it with a target-side software path. That path needs target progress (§3.4).

**Atomicity scope**
- **Required.** Exclusivity among all `rmw` operations on the same `(rank, seg, off, w)` issued through any context of any domain in the job.
  - This is stronger than GASNet-EX's per-atomic-domain scope, which GEX §A.1 flags as a mismatch with OpenSHMEM. OpenSHMEM programs mix `cswap`, `add` and `set` on one word: Graph500, Kokkos, bale (USE §2), and OSHMPI's forced AM fallback (MPI §8.2).
  - The core therefore picks **one** execution path per segment: all-NIC or all-CPU. It must never mix, which is the UCX `amo_sw` hazard (UCX §1, §9.5).
- **Not required.**
  - Against `put`/`get` on the same bytes (SPEC §3.2; SEM §2.5a).
  - Across different widths overlapping (SEM §2.5b).
- **Required against target-CPU reads by `wait`/`test`** (SPEC L684–685; SEM §2.5d). The mechanism depends on the substrate:
  - `OC_CAP_RMW_CPU_COHERENT`: Portals `PTL_COHERENT_ATOMICS` (P4 §1), UET `UET_AMO_CPU_COHERENT` (FUT §A.1), UCX `ATOMIC_CPU` (UCX §1).
  - Otherwise the target calls `oc_seg_sync(seg)` before its load (`PtlAtomicSync`, P4 §8b.9).
  - UBCL documents the same issue (P4 §8b.9).
- **On-node peers.** If NIC rmw is not CPU-coherent, `rmw` to a load/store-reachable peer on an RMW segment must still go through the NIC (loopback), not CPU `lock` instructions.
  - IBGDA already borrows a DCI to self for its consistency operation (NV §4).
  - `SHMEM_TEAM_SHARED`-derived contexts form a separate atomicity domain and may use CPU atomics (SPEC §3.2 footnote; SEM §2.5c).

**Multi-rail.** All `rmw` to the same `(rank, seg, off)` must go through a single NIC and queue.
- Portals atomicity holds "only ... on the same network interface" (P4 §1).
- NVSHMEM uses address-stable QP hashing (NV §4).
- GASNet ibv requires a single QP on the first HCA for offloaded atomics (GEX §A.4).

**Ordering.** An `rmw` obeys the fence rule like a put (Table 14 fences blocking and nbi AMOs; SPEC L8931–8935). Fetched values of async `rmw` are not fenced.

**Registration.** The target segment needs `OC_SEG_RMW`. Otherwise the core may still execute the `rmw` (the OpenSHMEM spec lets AMOs target any symmetric object), but it may have to use a slower path, e.g. UCX atomic-registration errors (SEM §4.4).

**Errors**
- `OC_EUNSUPP` is never returned for required ops: the core must emulate them.
- Optional ops not in `rmw_ops` are emulated in the core, never refused, with the cost stated in §7.

**Rationale for this op set** (cross-checked across substrates)

| Substrate | Native coverage | Sources |
|---|---|---|
| Portals 4 | SUM, BOR/BAND/BXOR, SWAP, CSWAP, MSWAP, fetch forms | P4 §1; SEM §4.4 |
| libfabric cxi | WRITE, SUM, bitwise, CSWAP family, MSWAP, 1 element | OFI §1.2; SEM §4.4 |
| UET HPC profile | WRITE, READ, SUM, bitwise, CSWAP, MSWAP, single element for fetching | FUT §A.1 |
| mlx5 (IBGDA, UCX DEVX) | masked compare-swap and masked fetch-add, 4/8 B: covers WRITE/READ/CSWAP/ADD/MSWAP | NV §1; SEM §4.4; UCX §1 |
| UCX rc/dc_mlx5 | ADD, AND, OR, XOR, SWAP, CSWAP, 32/64 | UCX §1; GEX §A.9 |
| DMAPP | 64-bit ADD/AND/OR/XOR, fetch forms, CSWAP, AFAX | VEN §A1 |
| uTofu | ARMW 4/8 B | VEN §B1 (doc-excerpt); SEM §4.4 (search summary); op list *recalled* in VEN |

- WRITE, CSWAP and ADD with optional fetch are the intersection of these substrates.
- The optional ops cover `and`/`or`/`xor` (hclib only, USE §1) and the SOS lock's masked swap (SEM §4.11; OFI §8).

**Disagreement on mlx5 atomics.**
- OFI §1.2 saw IBTA atomic verbs implemented only in rdma-core's `rxe`/`ionic`, not mlx5.
- GEX §A.9 says NVIDIA dropped the experimental verbs API for *masked* atomics, leaving FADD/FCAS on I64.
- UCX §1 says `rc_verbs` offloads only ADD, while `rc_mlx5`/`dc_mlx5` use DEVX atomic caps.
- NV §1 shows masked CS/FA WQE opcodes posted directly.
- My reconciliation: full masked atomics on ConnectX exist **only through DevX / direct WQE construction** (IBGDA, UCX mlx5), not through portable verbs. The matrix (§5) reflects this.

### 2.5 Fence: `OC_F_FENCE` and `oc_ctx_fence`

**Semantics**

- **`OC_F_FENCE` on an operation** means: the operation is applied at the target only after every operation previously issued on the same `(ctx, rank)` has *completed at the initiator*. Operations issued later on that `(ctx, rank)` are deferred behind it. This is libfabric `FI_FENCE` semantics (SEM §5.2, FUT §A.4).
- **`oc_ctx_fence(ctx)`** is O(1) and generates no traffic.
  - It bumps a per-context fence epoch.
  - The first operation issued to each rank after the bump is sent as if it carried `OC_F_FENCE`.
  - Ranks that receive no later operation cost nothing.
- **Scope.** Ordering holds per `(ctx, rank)` only.
  - No order between contexts (SPEC L10408, 1.6 erratum H.1.3).
  - No order between ranks.
  - No order within an interval.
  - No order for gets or fetched values.

**Implementation tiers** (reported by capabilities)

1. **`OC_CAP_ORDERED`.** The transport already delivers `(ctx, rank)` in order, so fence is a no-op.
   - Portals with `PTL_TOTAL_DATA_ORDERING` and small operations (P4 §4).
   - UET ROD, which forfeits spraying (FUT §A.4).
   - GPI-2 RC per queue per peer (GPI §B.4).
   - This tier costs path diversity, so it must never be the default on sprayed fabrics.
2. **`OC_CAP_FENCE_NATIVE`.** The NIC or provider defers the fenced operation.
   - CXI in hardware via `C_CMD_CQ_FENCE`; requesting it disables PCIe RO for RMA (OFI §1.2, SEM §5.2, FUT §A.4).
   - UET, where it is mandatory in every profile (FUT §A.4; **single-source** for the mandate).
   - mlx5 fence-mode bits used by NVSHMEM, whose exact semantics are undocumented (SEM §5.2, NV §4).
3. **Software completion gating.** Before posting the fenced operation, the core waits until earlier operations to that rank are done.
   - With only per-context counters, this degenerates to a full context drain: exactly SOS's `fence = put_quiet` (OFI §4, P4 §8b.3).
   - The cost is one round trip for the slowest in-flight operation, and only when a fence is followed by more traffic.
   - EFA's strict-ordering MR mode is a provider-side version of this (SEM §5.2).

**Put-with-signal (R3, signal after its own data).**
- It needs no compound primitive: `oc_put(..., fl)` followed by `oc_rmw(..., OC_F_FENCE)`.
- This is exactly what the hot NVSHMEM path does: RDMA write WQEs, then an atomic WQE with `FM_FENCE` (NV §4; SEM §4.5).
- It is also what SOS does with `--enable-ofi-fence` (OFI §4).
- Implementations without a fence flag pay put + quiet + AMO:
  - Open MPI (UCX §8.1), OSSS-UCX (UCX §8.1), SOS Portals (P4 §8b.4).

**Rationale**
- Usage shows `quiet`/`fence` are called after *every* operation in Graph500 and GUPs (USE §1, §2.1–2.2). A fence must therefore be free when nothing follows it and cheap when something does.
- The lazy epoch achieves "free when nothing follows".
- The per-op flag lets the NIC do the "cheap when something does" part.
- Promising ROD-style ordering by default would forfeit spraying (FUT §A.4; VEN §C4; P4 §4 on BXI).

**Cost of the epoch state.**
- It is one per-(ctx, rank) epoch word, kept only for ranks this context has touched.
- On connection-based substrates (QP or DC state) it fits in existing per-peer state.
- On connectionless CXI it is a sparse table: ≈ 4 B × touched peers per context (*arith*).
- See Q3.

### 2.6 Completion: `oc_ctx_count`, `oc_ctx_wait`, `oc_ctx_wait_pe`, `oc_progress`

**Counters.** Each context keeps three counter classes (PUT, GET, RMW), each a triple (`issued`, `done`, `failed`) of 64-bit counts.
- `issued` is incremented *before* posting (SOS "NOTE-MT", P4 §5).
- `done` counts operations that are **remotely complete and visible**.

**`oc_ctx_wait(cls)`**
- Returns when `done + failed ≥ issued` for the classes in `cls`, as sampled at entry. Operations issued concurrently by other threads may extend the wait; SOS loops the same way (P4 §5).
- Returns `OC_EIO` if `failed` advanced.
- **When nothing is pending it must cost two loads and a compare.** Graph500 calls `quiet` after every edge (USE §2.1), whereas OSHMPI/MPICH scans all N ranks and mallocs 2N requests (MPI §9).

**Visibility guarantee.** If a substrate's completion event can precede target placement, `oc_ctx_wait` performs the substrate's flush:
- CXI transmit-complete vs `FI_DELIVERY_COMPLETE` flushing read (OFI §4);
- UCX `ucp_worker_flush_nbx` (UCX §4);
- IBGDA DUMP-WQE CST (NV §4);
- EFA emulated DC protocols (VEN §C5).

The cost goes to the wait, not to every operation.
- SOS today sets `FI_DELIVERY_COMPLETE` on every put and pays the flush per op (OFI §4, §9).
- The spec needs visibility only at quiet (SPEC L9010–9013).

**`oc_ctx_wait_pe`** serves `shmem_ctx_pe_quiet`. A substrate without per-peer counts may implement it as `oc_ctx_wait(ALL)`, which the spec explicitly allows (SPEC L9114–9115). `pe_quiet` has no users (USE §3), so this is fine.

**Events vs counters.** The core has no CQ entries on the fast path and no per-op handles.
- Evidence for counters: SOS/OFI (OFI §0.1, §5), SOS/Portals (P4 §5), DMAPP `gsync` (VEN §A1), GASNet `GEX_EC` counters (GEX §A.1), OpenFAM `fam_progress` (FUT §B.5).
- Cray exposes handle-returning `shmemx_*_nb` (VEN §A8.4) and DMAPP has `syncid` (VEN §A1). A handle form is left as optional extension Q12.

**Progress.**
- `oc_progress(ctx)` drives software paths: bounce-buffer recycling, emulated `rmw`, and EAGAIN retries (OFI §8).
- A substrate that needs no target work reports `target_cpu = 0`.
- Otherwise the OpenSHMEM layer passes `OC_DOM_PROGRESS_THREAD` (§3.4).

### 2.7 Target-side notification: `oc_notify_read`, `oc_notify_wait`

**Semantics**
- The counter counts `rmw` operations applied to segment `seg` of the local process. A substrate may over-count by also counting puts, e.g. Portals `CT_COMM` counts all incoming ops (P4 §5).
- `oc_notify_wait(seg, above)` blocks until the count exceeds `above` or the timeout expires.
- There is no address filtering: waiters re-check their predicate. SOS does the same, and it "wakes on any remote write" (OFI §9).
- Available only on segments registered with `OC_SEG_NOTIFY`.

**Why rmw-only is enough.** Since 1.5, `wait`/`test` ivars "must be updated using OpenSHMEM AMOs" (SPEC L10471–10472; SEM §1). Signals are also `rmw`. The counter therefore never needs to see puts, and the bulk heap stays counter-free.

**Waiter protocol (in the layer above).**
1. Read the count.
2. Coherent-load the variable (`oc_seg_sync` if needed).
3. If the predicate fails, `oc_notify_wait(count)`.

This is SOS's `SHMEM_WAIT_UNTIL_BLOCK` (SEM §4.6; P4 §5).

**Native carriers**
- Portals target CT + `PtlCTWait` (P4 §5).
- libfabric `FI_RMA_EVENT` + `fi_cntr_wait` on CXI (OFI §1.2, §5).
- UET HPC remote write counters (FUT §A.5).
- uGNI `GNI_POST_CQWRITE` (VEN §A1).

**Fallback: poll with backoff plus `oc_progress`.** This is what OSSS-UCX, OSHMPI, GPI-2 and NVSHMEM do today (UCX §8.2; MPI §8.4; GPI §B.1; NV §8b.5).

**Usage.** Single-variable `wait_until` on a counter is HOT in pplx-kernels, Triton-distributed and PyTorch, and WARM in PRK (USE §1, §2.7–2.12). The vector/any/some forms have no users (USE §3).

---

## 3. Resource model

### 3.1 What a domain costs (per process)

- One set of NIC resources per rail.
- One address cache, filled lazily.
- One registration per segment (≤ ~8 segments).
- One target-side counter per `OC_SEG_NOTIFY` segment.
  - SOS's `shmemx_ct` objects are capped at 11–22 on Portals (P4 §6).
  - Hardware counters are scarce on offload NICs (P4 §1, HWCNT).
  - So the core uses **one** counter per notify segment, not one per variable.

### 3.2 What a context costs

A context is: a transmit queue (or share of one), a completion source, 3 × 3 counters (72 B), a lock unless `OC_CTX_NOLOCK`, a fence epoch, and a sparse per-touched-peer epoch table. **It owns no per-peer connection and no per-peer key.**

Hardware cost per substrate:

| Substrate | Context maps to | Hardware budget (sources) |
|---|---|---|
| CXI | one libfabric endpoint + CQ (no STX, no SEP) | `ep_cnt = 128`, `cq_cnt = 32` per domain (OFI §6) |
| verbs;rxm, efa, opx | endpoint (+QP, CQ); efa: SRD QP + CQ | "QPs are expensive resources" (VEN §C6; count *recalled*); opx has SEP (OFI §6) |
| Portals 4 (SOS) | 3 MDs + 2 CTs on a shared NI and EQ, no hardware isolation | ≈340 at requested limits (P4 §6) |
| UCX | UCP worker: RX rings ≈32 MB per iface, 32 DCIs + DCT | UCX §6 |
| IBGDA DC | DCI(s), ≈394 KiB each at defaults | NV §3.2 |
| IBGDA RC (QP handle) | N more QPs per context | NV §6: **must not be the model** |
| uTofu | VCQ | 54 CQs × 8 VCQs = 432 per node, ≈9 per PE at 48 PPN (VEN §B6, doc-excerpt + *arith*) |
| GPI-2 | queue | ≤ 16 queues; each queue = QP per connected peer (GPI §B.6) |
| DMAPP/Aries | FMA descriptor / CDM | per-node, divided among PEs; OpenSHMEMX `SHMEM_MAX_CTX` (VEN §A6) |
| MPI RMA | none: OSHMPI refuses `ctx_create` | MPI §6 |
| UET | libfabric endpoint, consuming a 12-bit Resource Index | vendor-specific (FUT §A.6) |

**Rules**
1. `oc_ctx_create` may fail. The spec says failure "is not treated as an error" (SPEC §9.5.1; SEM §4.8), so the OpenSHMEM layer falls back to a shared context.
2. `max_ctx_hint` is a per-process share of a per-node budget (VEN §A6, §B6; OFI §6).
3. Private contexts skip the lock: SOS (SEM §4.8) and UCX `THREAD_MODE_SINGLE` (UCX §6).

### 3.3 Per-peer state at 10^5–10^6 PEs

**Target budget.** At most ~64 B per *touched* peer per process, shared by all contexts, and 0 B for untouched peers when addressing is algorithmic.

| Item | Bytes per touched peer | Evidence |
|---|---|---|
| Address entry | 8 (Portals logical rank, uTofu VCQ ID) to ~36–40 (UET AV) to ~90 (CXI AV, estimate) | P4 §3; VEN §B3; FUT §A.3; OFI §3 |
| Keys + bases | 0 with symmetric keys; 16 × segments with provider keys (SOS: 32 B/PE for 2 segments) | OFI §2; VEN §C2 |
| Fence epoch | ~4 per context (sparse) | *arith* |
| Transport reliability | 0 at the API level: shared RUD PDCs per FEP pair (UET), AH per remote *node* (EFA), DC | FUT §A.3; VEN §C3; UCX §3 |

**Worked totals** (*arith*)

| Configuration | Per-peer size | At 10^6 touched peers, per process |
|---|---|---|
| Worst acceptable: 40 B AV + 2 × 16 B keys + 1 ctx epoch | 76 B | 76 MB |
| Symmetric keys + 8 B logical addresses | 12 B | 12 MB |

**Designs the core must rule out** (all from the reports):

| Design | Cost | Source |
|---|---|---|
| IBGDA RC, 2 QPs per peer | ≈394 KiB per QP → **75 GiB GPU memory per PE at 10^5** | NV §3.2 |
| NVSHMEM IBRC | ≳12–15 GiB per PE at 10^5 | NV §3.1 (*recalled* per-QP size) |
| GPI-2 static topology | 10 RC QPs per peer | GPI §B.3 |
| LCI | 1 RC QP + 1 thread domain per peer per endpoint | LCI §C.3 |
| uGNI SMSG mailboxes | ≈109 MB per process at 10^5 | VEN §A3 |
| Open MPI/UCX | 0.5–1 KB per peer **per context** ≈ 50–100 MB per context | UCX §3 |
| MPICH OFI | 24 B × N per window, plus a collective per window | MPI §2 |
| NVSHMEM IBGDA rkey tables | 16 B × chunks × N × NICs: 3 MiB–763 MiB at 10^5, rebuilt on growth | NV §2 |

### 3.4 Lazy addressing

**Mechanism**
- The first operation to rank r calls `rt->lookup(r)` and caches the result in the domain.
- The operation then proceeds, or returns `OC_EAGAIN` under `OC_F_TRY`. This is `rxm` on-demand connection behaviour (OFI §3), GASNet-EX `QueryBoundSegmentNB` + `IMMEDIATE` (GEX §A.2), and Open MPI osc/rdma first-use peer setup (MPI §2).
- If the runtime can compute addresses (Portals logical NI rank map, UET FA table + PIDonFEP, DMAPP), `lookup` is a function and nothing is cached (P4 §3; FUT §A.3; VEN §A2).

**What this avoids**
- O(N) KVS gets per PE (SOS: OFI §3, P4 §3).
- `PMIx_Get` per PE (OSSS-UCX: UCX §3).
- Bootstrap alltoall of QP handles, O(N²) bytes (NV §3.1).

**Where laziness is impossible.** The cost is declared, not hidden:
- **efa-direct** (and `efa` with homogeneous peers): the target must pre-insert the initiator into its AV, forcing O(N) AV entries on every PE at init (OFI §3; VEN §C3).
- **efa**: a per-peer handshake before the first RMA, which also needs target progress (VEN §C3).

### 3.5 Progress: which primitives need target CPU

| Primitive | Needs target CPU when… | Examples |
|---|---|---|
| `put`/`get` | the substrate onloads RMA | opx (OFI §5), efa without device RDMA (VEN §C5), UCX AM-emulated RMA (UCX §1), Portals reference implementation (P4 §5), MPI AM fallback (MPI §5) |
| `rmw` | there are no NIC atomics | rxm, efa, opx, UCX `amo_sw`, GASNet AM reference, OSHMPI AM, NVSHMEM IBRC send-based atomics (OFI §1.2; UCX §8.4; GEX §A.8; MPI §8.2; NV §8b.4) |
| fence (native) | the provider's fence is software | sockets provider (FUT §A.4) |
| `notify` | there are no target counters | polling on the target, but that is the waiter's own CPU, so not a peer stall |
| reliability timers | the transport runs software reliability | UD/DC flow-control timeouts in UCX (UCX §9.4), opx PING/ACK (OFI §9) |

**How the core exposes this**
- `caps.target_cpu` is a bitmask over `OC_OPC_*`, plus a reliability bit folded into it.
- The spec says implementations are "expected to leverage hardware offload" and "discourages" releasing ones without asynchronous one-sided progress (SPEC L836–843; SEM §2.4).
- So if `target_cpu != 0`, the OpenSHMEM layer opens the domain with `OC_DOM_PROGRESS_THREAD`.
- The core then owns one progress thread per process. The OpenSHMEM reference implementation used one per host (GEX §A.8.1); a per-node shared progress daemon is the Portals reference implementation's PPE alternative (P4 §5).
- The cost is then explicit: one core per PE or per node, plus added latency for emulated operations (§7).

---

## 4. Mapping tables

### 4(a) Core primitive → OpenSHMEM routines

Version labels follow Annex G (SEM §1). "≤1.3" means present before 1.4.

| Core primitive | OpenSHMEM routines (version) |
|---|---|
| `oc_put` (blocking) | `shmem_[ctx_]{TYPE_}put`, `put{8,16,32,64,128}`, `putmem` (≤1.3; ctx 1.4; team-relative PE 1.5); loop body of `iput` (≤1.3) and `ibput` (1.6); data part of `put_signal` (1.5); collectives' data movement |
| `oc_put(OC_F_ASYNC)` | `put_nbi`, `putmem_nbi` (1.3); `put_signal_nbi` data (1.5); `ibput` (1.6) loop |
| `oc_put_word` | `shmem_[ctx_]TYPE_p` (≤1.3), 1-element typed put, `put{32,64}` of 1 element; pSync scalars in collectives |
| `oc_get` / `OC_F_ASYNC` | `TYPE_get`, `getmem`, `get{8..128}`; `get_nbi` (1.3); `iget` / `ibget` loops (≤1.3 / 1.6) |
| `oc_get_word` | `TYPE_g` (≤1.3) |
| `oc_rmw(WRITE)` | `atomic_set` (1.4 name; `set` ≤1.3); `signal_set` (1.6); `put_signal` with `SHMEM_SIGNAL_SET` (1.5) |
| `oc_rmw(WRITE, fetch)` | `atomic_swap` (1.4; `swap` ≤1.3); `swap_nbi` (1.5) |
| `oc_rmw(READ)` | `atomic_fetch` (1.4), `fetch_nbi` (1.5) |
| `oc_rmw(CSWAP, fetch)` | `atomic_compare_swap` (1.4; `cswap` ≤1.3), `compare_swap_nbi` (1.5); lock release; Kokkos `*=` loops |
| `oc_rmw(ADD [,fetch])` | `atomic_{inc,add,fetch_inc,fetch_add}` (1.4; `inc`/`add`/`finc`/`fadd` deprecated 1.4) and their `_nbi` (1.5); `signal_add` (1.6); `put_signal` with `SHMEM_SIGNAL_ADD` (1.5); reductions (SUM) |
| `oc_rmw(AND/OR/XOR [,fetch])` | `atomic_{and,or,xor,fetch_and,fetch_or,fetch_xor}[_nbi]` (1.4; nbi 1.5) |
| `oc_rmw(MSWAP)` | `set_lock`/`clear_lock` internals (≤1.2, SOS queue lock) |
| `OC_F_FENCE` on `rmw` | the signal half of `put_signal[_nbi]` (1.5) |
| `oc_ctx_fence` | `shmem_[ctx_]fence` (pre-1.4; per-ctx clarified 1.6 erratum) |
| `oc_ctx_wait(ALL)` | `shmem_[ctx_]quiet`; `barrier_all` first half; `ctx_destroy` (1.4); memory-management implicit barrier; `finalize` |
| `oc_ctx_wait_pe` | `shmem_[ctx_]pe_quiet` (1.6) |
| `oc_ctx_count` | cheap "nothing pending" test inside quiet/fence; `shmemx_pcntr`-style performance counters (OFI §8) |
| `oc_notify_*` | `wait_until` (typed 1.4 C11 generic; `wait` deprecated 1.4); `signal_wait_until`; `wait_until_{all,any,some}[_vector]` (1.5); `set_lock` waits; collective rendezvous (pSync wait) |
| `oc_seg_register` | `shmem_init[_thread]` (heap reservation + data segment); `malloc_with_hints` hints (1.5) choose the segment |
| `oc_seg_peer_ptr` | `shmem_ptr` (≤1.2), `team_ptr` (1.6); `SHMEM_TEAM_SHARED` construction (1.5) |
| `oc_seg_sync` | coherent load inside `test`/`wait_until`, `signal_fetch` (1.5), lock acquisition |
| `oc_ctx_create/destroy` + `NOLOCK` | `shmem_ctx_create` (1.4), `team_create_ctx` (1.5), `SHMEM_CTX_PRIVATE`/`SERIALIZED` |
| `OC_F_MORE` | `shmem_ctx_session_start/stop` with `SHMEM_SESSION_BATCH` (1.6) |
| `rt->abort` | `shmem_global_exit` (1.2) |
| none (purely above core) | teams (1.5), all collectives, active-set collectives, `info_*`, `query_*`, `addr/pe_accessible`, `malloc/free/realloc/align/calloc` allocator logic, `sync`/`team_sync` (1.4/1.5), scans (1.6) |

### 4(b) Every routine family → fast / derived / slow

Definitions:
- **fast**: 1:1 onto a core primitive, no extra software on the path.
- **derived**: a few core calls or O(1) local work.
- **slow**: deliberately software, collective or looped.

Usage evidence is from USE §1, §2 and Appendix A. "thin" marks evidence judged weak by USE itself or by me.

| Family | Class | How | Usage evidence (USE) |
|---|---|---|---|
| init / finalize / query_initialized / info / query_thread | slow | runtime + `oc_dom_open` + reservation; finalize = barrier | universal, COLD; `query_thread` only in tests |
| `my_pe` / `n_pes` | derived (local) | cached variables | universal; lexically HOT (bale `index % n_pes`); must be an inline read |
| blocking typed put/get, `putmem`/`getmem`, sized put/get | **fast** | `oc_put`/`oc_get` (+ O(1) team translation) | strongly used; NPB halos, convey, stencil, OSU |
| scalar `p` / `g` | **fast** | `oc_put_word`/`oc_get_word` | **hottest pattern**: Kokkos (212 wrapper sites), GUPs, bale `lgp_*`, SSCA1 |
| `put_nbi` / `get_nbi` | **fast** (data), derived (completion) | `OC_F_ASYNC` + `oc_ctx_wait` | well used: OSU, convey, hclib; DeepEP per-token GPU put |
| `iput` / `iget` | slow | loop of `oc_put(ASYNC)` + wait; one put when stride = 1 | **no app users** (thin) |
| `ibput` / `ibget` (1.6) | derived / slow | `dst == bsize` → one put; else loop | no users (expected for 1.6; thin, do not demote on usage alone, USE §4.5) |
| AMO `add` / `inc` / `fetch_add` / `fetch_inc` | **fast** | `oc_rmw(ADD)` | strongly used: bale, Kokkos, GUPs, ISx, PRK, hclib; DeepEP `amo_nonfetch_add` |
| AMO `compare_swap` | **fast** | `oc_rmw(CSWAP)` | Graph500 (MIN/OR emulation), Kokkos, bale |
| AMO `set` / `swap` / `fetch` (int + float) | **fast** | `oc_rmw(WRITE/READ)` | Kokkos, OSU |
| AMO `and` / `or` / `xor` [fetch] | fast where the `rmw_ops` bit is set, else derived (READ + CSWAP loop) | | hclib (4) + tests only (thin) |
| AMO `_nbi` (1.5) | **fast** + derived completion | `OC_F_ASYNC` | no app users seen (thin) |
| `put_signal[_nbi]` (1.5) | **derived** (2 core ops) | `oc_put` + `oc_rmw(ADD/WRITE, OC_F_FENCE)` | 0 CPU apps; **HOT in NVSHMEM AI codes** (pplx per token, Triton-distributed, PyTorch); hand-rolled in bale and PRK |
| `signal_add` / `signal_set` (1.6) | **fast** | `oc_rmw(ADD/WRITE, w=8)` | NVSHMEM `signal_op` HOT (pplx, Triton); no OpenSHMEM users |
| `signal_fetch` (1.5) | derived (local) | coherent local load | NVSHMEM codes |
| `wait_until` / `test` (single) | derived (native notify) or slow (poll) | `oc_notify_*` + `oc_seg_sync` | PRK per iteration; HOT in pplx/Triton/PyTorch (NVSHMEM dialect) |
| `signal_wait_until` | derived | same | Triton 45 sites, pplx, PyTorch |
| `wait_until_{all,any,some}[_vector]`, `test_*` (1.5) | slow | loops over single checks | **no users** |
| `fence` | derived (O(1)) | `oc_ctx_fence` | PRK after each halo put; bale; Triton 19 |
| `quiet` | derived (O(1) when idle) | `oc_ctx_wait(ALL)` | very widely used; **after every op** in Graph500/GUPs |
| `pe_quiet` (1.6) | derived / slow | `oc_ctx_wait_pe` → may be quiet | **no users**, not even tests |
| contexts create/destroy + options | slow (create), derived (options) | `oc_ctx_create` / `NOLOCK` | threaded GUPs/Graph500, hclib, SOS perf; hot ops unchanged |
| teams `split_strided` / `split_2d` / destroy | slow | collective agreement above core | split_strided light (SOS perf, DeepEP); split_2d no users |
| `team_my_pe` / `n_pes` / `translate_pe` / `get_config` | derived (local) | affine map (strided, 2D); table for SHARED | node team used for device selection (pplx, PyTorch); get_config no users |
| team-relative PE on ctx ops | derived | `world = a + b·i`, O(1) (SEM §4.9) | via contexts; thin |
| `team_create_ctx` / `ctx_get_team` | slow / derived | ctx + team pointer | no users |
| `shmem_ptr` | derived | `oc_seg_peer_ptr` | bale convey intra-node path; GPU `mc_ptr` codes |
| `team_ptr` (1.6) | derived | same + translation | no users |
| `addr_accessible` / `pe_accessible` | slow (local) | registry lookup | Graph500 asserts only (thin) |
| locks | slow (derived) | SOS queue lock on `rmw` CSWAP/WRITE/MSWAP + notify + quiet | hclib UTS work stealing (52 sites), WARM |
| `malloc` / `free` / `align` | slow | deterministic allocator in the reserved segment + barrier | universal, COLD |
| `realloc`, `calloc`, `malloc_with_hints` | slow | same | realloc: PRK only; calloc, with_hints: none (thin) |
| `barrier_all` / `sync_all` / `team_sync` | derived (quiet) + slow (sync) | `oc_ctx_wait` + tree sync over put + notify | most-called collective; phase boundaries; 2 per collective in bale; Triton from kernels |
| broadcast | slow | tree over put + notify | NPB, seeds; COLD |
| reduce (team 1.5 and `*_to_all`) | slow, with a fast small-message path | 1-element reductions dominate; RMW SUM into root (SOS Portals, P4 §8a) or recursive doubling | NPB per iteration, G500 per BFS level; **most CPU codes still use deprecated pSync `*_to_all`** |
| collect / fcollect | slow | two-phase size agreement + gets | ISx stats, OSU; COLD |
| alltoall | slow | strip-mined nb puts (Cray, VEN §A8.2) | PRK transpose; modest |
| alltoalls | slow | | **no users** |
| scans (1.6) | slow | prefix algorithm | **no users** |
| active-set collectives (deprecated 1.5, still required 1.6) | slow | cached internal strided team (OSHMPI, MPI §8.12) + pSync semantics | still what most CPU apps call |
| sessions (1.6) | derived (hint) | `OC_F_MORE` or no-op | **no users, no tests** |
| `global_exit` | slow | `rt->abort` | error paths |
| profiling `pshmem_` | n/a | name-shifted wrappers | — |

---

## 5. Network feasibility matrix

### 5.1 Legend

**Cell codes**
- **N** = native
- **C** = cheaply emulated (host-side only, no extra round trip on the common path, or bounded O(1) extra state)
- **E** = expensively emulated (extra round trips, target CPU, O(N) state, or spin polling)
- **U** = unsupported
- **?** = unverified or reports disagree

**Columns**
- **A** `put`/`get`
- **B** `put_word`/`get_word` inline
- **C** keyless symmetric segment (no per-peer key or base)
- **D** required `rmw` ops, exactly-once
- **E** optional `rmw` ops (bitwise, MSWAP)
- **F** fence (per-op flag, or order)
- **G** counter completion + wait
- **H** target notify counter
- **I** per-peer state ≤ tens of B, lazy or connectionless
- **J** cheap context
- **K** no target CPU for A and D

**Source tags** are abbreviated: `o`=OFI, `u`=UCX, `p`=P4, `g`=GEX/GPI/LCI, `m`=MPI, `n`=NV, `v`=VEN, `f`=FUT, `s`=SEM.

### 5.2 Matrix

| Network | A | B | C | D | E | F | G | H | I | J | K |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **libfabric cxi** | N o1.2 | N o1.1 (inject, optimized MR) | N o2 (client keys 0–99) | N o1.2 (1 elem; READ via fetch-SUM(0) o8) | N o1.2 | N o1.2, s5.2 (HW; disables PCIe RO) | N o5 (HW counters) | N but per-MR, kills optimized RMA → use a separate segment o2,o9 | C o3 (~90 B/peer est.; AMO reliability state unquantified) | C o6 (≤128 EP / 32 CQ per domain) | N o5 (manual progress only for non-offloaded counters) |
| **verbs;ofi_rxm** | N o1.2 | C | C o2 (PROV_KEY + VIRT_ADDR: 16 B/seg/peer) | E o1.2 (rxm target CPU) | E | E o4 (FI_ATOMIC removes RMA order; gating) | C o1.2 (software cntr via CQ) | E o1.2 (no RMA_EVENT → poll) | E o3 (RC per peer on demand, KB–tens of KB) | E (per-EP connections) o3,o6 | N for A; E for D o5 |
| **efa** (`efa` / `efa-direct`) | N Nitro v4+ / E else v1,o1.2 | C v1 | C o2,v2 (prov keys, exchange) | E o1.2,v1 (RTA pkts); U on efa-direct | E | C s5.2 (strict-ordering MR = completion-gated); no FI_FENCE o1.2 | N/C o1.2 | **?** o1.2 "no RMA_EVENT" vs v1 "local and remote counters" | E v3 (target must pre-insert initiator; per-peer handshake; AH per node) | C v6 (QP+CQ per EP) | E o5,v5 (manual progress; emulated RMA/atomics/handshake) |
| **opx** | E o1.2 (onloaded, *recalled* in o) | C | N o2 (SCALABLE only) | E o1.2 (*recalled* in o) | E | ? o1.2 (not listed) | C o1.2 | E o1.2 (no RMA_EVENT) | C o3 (replay state per active flow, ≤7 MB default) | C o6 (SEP) | E o5 |
| **UCX (mlx5 RC/DC)** | N u1 | N u1 | C u2 (per-PE rkey unpack; N with mlx5 GVA + symmetric rkey, opt-in) | N u1 (rc/dc_mlx5 DEVX; rc_verbs ADD only; else amo_sw E) | N AND/OR/XOR u1; MSWAP E | C u4 (`worker_fence`); OMPI needed per-peer dummy gets u8.5 | C u5 (requests + flush; no counter object) | E u5,u8.2 (`wait_mem` / poll) | C u3 (DC O(1) + DCI pool; impls create eps eagerly) | E u6 (worker ≈32 MB RX rings per iface) | N offload; E amo_sw, UD/DC timers u5,u9 |
| **Portals4 / BXI** | N p1 (non-matching LE) | N p8a (volatile MD; size limit, RI default 8 B) | N p2 (no keys; one LE via BIND_INACCESSIBLE) | N p1 (CSWAP/SWAP/SUM, fetch; PtlAtomicSync unless coherent) | N p1 | E p8b.3 (fence = quiet); N only with TDO + small p4 | N p5 (CT + CTWait) | N p5 (target CT) | N p3 (8 B logical map; RI eager conn_t) | C p6 (~340 MD/CT bundles, no HW isolation) | N p5 (BXI UBCL passive-lock caveat p5; BXI HW details ?) |
| **GASNet-EX (ibv/ucx/ofi)** | N g-A.1 | N g-A.1 (`PutBlockingVal`) | C g-A.2 (ibv 4 B rkey/peer/HCA + base table; offset addressing UNIMPLEMENTED) | C ucx / C–E ibv (I64 FADD/FCAS only) / E AM ref g-A.9; per-AD scope mismatch g-A.1 | N ucx / E ibv g-A.9 | E g-A.4 (no order except ACQ/REL) | C g-A.1 (NBI regions, EC counters) | E g-A.1 (none) | E default g-A.3 (ibv static all-to-all; ucx all eps) | C g-A.6 (EP; hardware cost *recalled*) | N RDMA; E AM g-A.5 |
| **GPI-2** | N g-B.1 | C | E g-B.2 (`rrmd[seg][rank]` ≥64 B eager) | E g-B.1 (u64 FADD/CAS only; global lock, blocking) | E | C g-B.4 (RC per queue per peer ⇒ ordered) | C g-B.5 (`gaspi_wait` per queue) | E g-B.1 (notify = write + busy-poll) | E g-B.3 (static 10 RC QPs/peer default) | E g-B.6 (≤16 queues, QP per peer each) | N g-B.5 |
| **LCI** | N g-C.1 | C | C g-C.2 (24 B rmr/peer, OOB) | E g-C.1 (no atomics → AM) | E | ? g-C.4 (not examined) | N g-C.5 (counter comp objects) | C g-C.1 (remote completion; needs target progress) | E g-C.3 (RC QP + TD per rank per EP, eager) | E g-C.6 | N RMA; E AM g-C.5 |
| **MPI-3 RMA** | C m1,m9 (333 vs 71 instrs before fixes) | C | C/E m2 (24 B × N per window unless scalable keys) | E m8.2 (mixed ops → AM default) | E | E m4 (flush_all + sync = quiet) | C m5 (flush; O(N) sync loops m9) | E m8.4 (poll) | E m2–3 (per-window O(N) tables; osc/rdma lazy) | U m6 (ctx_create refused) | C/E m5 (full progress when AM) |
| **NVSHMEM IBGDA** | N n1,n3.2 | C n8b.2 (scalar get via ibuf + CST) | E n2 (16 B × chunk × peer × NIC, rebuilt on growth) | N n1,s4.4 (mlx5 masked CS/FA; API lacks int64 add n1) | C (bitwise not a single masked op; *inference*) | N? n4,s5.2 (WQE `FM_FENCE`; semantics undocumented) | C n4–5 (GPU CQ poll; DUMP CST) | E n8b.5 (GPU busy-poll) | E RC (75 GiB @10^5) / C DC (9.2 MiB DCT AV @10^5) n3.2 | E RC / C DC n6 | N n5 |
| **uGNI / DMAPP** (historical) | N v-A1 | N v-A1 (FMA) | N v-A2 (DMAPP symmetric; uGNI MH exchange = C) | N v-A1 (64-bit only; 32-bit emulated v-A8.5) | N v-A1 (AND/OR/XOR; MSWAP via AFAX *recalled*) | C v-A4 (NO_ADAPT / IN_ORDER modes cost adaptive routing) | N v-A1 (gsync) | C v-A1 (`POST_CQWRITE`) | N RMA / E SMSG v-A3 | C v-A6 (per-node FMA/CDM budget) | N v-A5 |
| **uTofu (Tofu-D)** | N v-B1 (excerpt) | C v-B1 (piggyback *recalled*) | C v-B2 (STAG-computable *recalled*; STADD per VCQ) | N v-B1, s4.4 (ARMW 4/8 B; op list *recalled*) | ? v-B1 (*recalled*) | ? v-B4 (*recalled*) | C v-B5 (TCQ poll) | ? v-B5 (MRQ notify *recalled*) | N v-B3 (VCQ ID 8 B) | E v-B6 (432 VCQs/node) | N v-B5 |
| **UEC HPC profile** (spec only, no products) | N f-A.1 (RUD; RUDI opt.) | N f-A.2 (small / optimized hdr) | N f-A.2 (user keys SHOULD) | N f-A.1 (fetching + compare_atomic; RUD only) | N f-A.1 | N f-A.4 (FI_FENCE mandatory) | N f-A.5 | N f-A.5 (REMOTE_WRITE cntr; not on IDEMPOTENT_SAFE MR) | N/C f-A.3 (ephemeral PDCs; AV ~36–40 B unless algorithmic) | ? f-A.6 (vendor; 12-bit RI) | N f-A.5 (if HW-offloaded; software providers need progress) |
| **CXL shared memory** (≤4096 nodes) | N f-B.1 (memcpy) | N | N f-B.2 (no keys) | E f-B.4 (CPU CAS needs HWcc, missing on prototypes; mCAS device region) | E | N/C f-B.1, f-B.4 (sync stores; SWcc needs flush) | N (synchronous) | E f-B.1 (no cross-host futex) | C f-B.3 (fabric cap; N × heap VA unless shared extent) | N f-B.6 | N f-B.5 |

### 5.3 Profile notes

- **UEC AI Base** has no RMA read and no fetching atomics. **AI Full** has no `compare_atomic`. On those profiles, D and A(get) drop to **E** via send/AM (FUT §A.1, §A.8).
- **efa-direct** lacks FI_ATOMIC entirely, so SOS cannot select it (OFI §9; VEN §C8.10).
- **CXI cell D.** SOS notes cxi "does not support fetch atomics with FI_DELIVERY_COMPLETE" (OFI §4; FUT §A.8). This supports moving visibility into `oc_ctx_wait` (§2.6).

---

## 6. Substrates as a basis for OpenSHMEM

- **libfabric + cxi**
  - *Strengths:* the most complete match. Native AMOs, `FI_FENCE` in hardware, hardware counters, client keys, connectionless AV (OFI §0, §1.2). HPE's own OpenSHMEMX is OFI-only (VEN §A8.9).
  - *Weaknesses:* ≤128 endpoints per domain; counter binding kills optimized RMA; `FI_DELIVERY_COMPLETE` costs a flush; manual progress; a single vendor. Open MPI developers report CXI provider performance issues (OFI §9).
- **libfabric generic (verbs;rxm, efa, opx)**
  - *Strengths:* one API, and SOS proves a thin counter-driven transport (OFI §8).
  - *Weaknesses:* the same `FI_ATOMIC` bit can mean hardware or target-CPU emulation; MR modes, progress and ordering diverge per provider; SOS needs per-provider configure flags, so one binary is not portable (OFI §10). The core's capability bits exist precisely to surface these differences.
- **UCX**
  - *Strengths:* the best mlx5 coverage (DC for scale, DEVX atomics, symmetric-rkey hint, GVA); a GPU device API with `ucp_device_put` + `counter_inc` (UCX §1).
  - *Weaknesses:* rkeys bound to workers, so O(N × segments × contexts) unpacks; no lazy ep; a worker per context with large RX rings; software AMOs need target progress; no wakeup use in SHMEM layers; mlx5-centric, no CXI transport (UCX §9–10). Open MPI oshmem leaves wait/test, team_create_ctx and nbi fetch AMOs unimplemented (UCX §9.8).
- **Portals 4 / BXI**
  - *Strengths:* the semantic best fit. Keyless LEs, native AMOs including MSWAP, counting events, target CTs, unordered-by-default. The SOS transport is ~2.2 kLOC and adds essentially no overhead over raw Portals (P4 §9–10).
  - *Weaknesses:* fence = quiet unless TDO; implementation size limits (volatile/atomic 8/512 B in the reference implementation); contexts without hardware isolation; failure detail lost. The reference implementation needs progress threads and ummunotify; BXI software is proprietary and now MPI-first (UBCL); SOS Portals CI is disabled (P4 §10).
- **GASNet-EX**
  - *Strengths:* the core/extended split model itself; local vs remote completion (`lc_opt`); `IMMEDIATE` backpressure; lazy segment queries; per-domain atomics that select NIC or CPU coherently (GEX §A.0–A.2).
  - *Weaknesses:* the core is *AM*-centric, not RMA/RMW-centric, so atomics and progress-less targets are not guaranteed. Offset addressing is unimplemented; ibv connects all-to-all by default; RAtomics offload is narrow; collectives are minimal; the spec is still beta; vendor conduits keep disappearing (GEX §A.9–A.10). Good as a model, poor as a substrate for passive-target AMOs.
- **GPI-2**
  - *Strengths:* `(segment, offset)` RMA, queues with backpressure, `write_notify` as put+signal (GPI §B.1).
  - *Weaknesses:* static 10 RC QPs per peer, per-segment per-rank tables, ≤16 queues, 255 segments, globally locked blocking atomics, busy-poll notifications, GPL-3. No OpenSHMEM port exists (GPI §B.3–B.9).
- **LCI**
  - *Strengths:* rich completion objects (counters, remote completion) and explicit progress, designed for many threads (LCI §C.5–C.6).
  - *Weaknesses:* no atomics; eager RC QP + thread domain per peer per endpoint; v2 beta; no PGAS clients (LCI §C.1–C.9).
- **MPI-3 RMA**
  - *Strengths:* universal availability; a passive-target lock_all epoch works; with MPICH fixes it approaches SOS latency (MPI §5, §9).
  - *Weaknesses:* mixed-op atomics force AM; no endpoints, so no contexts; collective O(N) window creation; O(N) sync loops; no notification; relies on MPICH-specific hints; OSHMPI stops at 1.4 plus teams (MPI §8–10). Suitable only as a portability fallback.
- **NVSHMEM (IBGDA)**
  - *Strengths:* proves GPU-initiated put + NIC atomics + fence bits at zero library overhead; it is the substrate behind the hottest put-with-signal users (NV §1; USE §2.9–2.12).
  - *Weaknesses:* RC per peer by default (75 GiB at 10^5); per-chunk × peer × NIC key tables; no OpenSHMEM contexts or locks; 16-bit PE proxy fields; ConnectX-only DevX (NV §3, §8–10). A design reference, not a portable base.
- **uGNI / DMAPP** (historical)
  - *Strengths:* the closest historical precedent for this core. Keyless symmetric segments, 64-bit NIC AMOs, `_nbi` + `gsync`, no target progress (VEN §A1–A5).
  - *Weaknesses:* SMSG per-peer mailboxes; ordering only by disabling adaptive routing; AMO replay hazard; the whole stack was rewritten for Slingshot (VEN §A9–A10), a lesson against mirroring one vendor API.
- **uTofu**
  - *Strengths:* hardware RMA plus 4/8-byte ARMW, 8 B per peer, no target progress (VEN §B1–B5).
  - *Weaknesses:* hard VCQ caps (≈9 per PE); registration per VCQ; proprietary; **primary documentation unreadable, much *recalled*** (VEN §B0).
- **UEC (HPC profile)**
  - *Strengths:* on paper the full core. User keys, fetching and compare atomics, mandatory `FI_FENCE`, remote counters, ephemeral PDCs, spraying on RUD (FUT §A).
  - *Weaknesses:* no products or numbers; AI profiles lack read and CAS; RUDI conflicts with counters and strict fence; AV may be O(N) (FUT §A.9).
- **CXL shared memory / OpenFAM**
  - *Strengths:* put/get/ptr become load/store; fence and quiet are trivial (FUT §B.1).
  - *Weaknesses:* ≤4096 nodes; cross-host CAS needs HWcc, which prototypes lack; no cross-host wakeup; failure isolation. OpenFAM `fam_map` is unimplemented in the memory-server model (FUT §B.3–B.8). An intra-pod tier under `oc_seg_peer_ptr`, not a 10^5-PE core.

---

## 7. Slow paths: what is relegated and what it costs

Costs are taken from the reports where measured; "RTT" means one network round trip.

| Relegated feature | Implementation above (or inside) the core | Expected cost (evidence) |
|---|---|---|
| `rmw` without NIC atomics | core-internal AM + CPU atomic at target + progress thread | 2 messages + target CPU per AMO. GASNet Aries offload cut 64-bit fetch-and-add latency by 70% vs AM (GEX §A.9), i.e. emulated ≈3.3× (*arith*). A progress core per PE or node (GEX §A.8.1; UCX §9.5; VEN §C5) |
| AND/OR/XOR where not native (mlx5-verbs, GPI-2, MPI) | READ + CSWAP retry loop | ≥2 RTT, livelock-prone under contention (UCX §8.4; SEM §4.4) |
| MSWAP where not native (UCX) | READ + CSWAP loop | same; only locks use it |
| Fence without native flag or order | context drain before the next op | one RTT of the slowest in-flight op, only when traffic follows (OFI §4; P4 §8b.3) |
| Put-with-signal without native fence | put + drain + rmw | +1 ACK RTT per call (SEM §4.5b; UCX §8.1) |
| `wait_until` / `signal_wait_until` without notify | spin/backoff + `oc_progress` | burns a core per waiting PE (MPI §9; UCX §9.7; GPI §B.9) |
| `wait_until_all/any/some[_vector]`, `test_*` | loops of single checks | O(n) loads per poll; no users (USE §3) |
| `pe_quiet` | `quiet` | may over-wait; spec allows (SPEC L9114) |
| `iput` / `iget` / `ibput` / `ibget` (dst stride > block) | nblocks async puts/gets + wait | nblocks headers; a vectored RMA extension could fix it (UCX SGL, OFI `rma_iov` limits; SEM §4.3) |
| Teams split / destroy | collective over parent; affine translation | O(log N) latency per split (≈17 tree levels at 10^5, SEM §4.12); translation O(1); SHARED needs a node-size table |
| Active-set collectives | cached internal strided teams + pSync logic | a team-creation collective on first use (MPI §8.12) |
| Barrier / sync | quiet + tree sync over puts + notify | O(log N) latency; frequent at phase boundaries (USE §1) |
| Reduce | 1-element: RMW SUM into root or recursive doubling; large: ring | software only (OFI §8; P4 §8a); offload hooks (CXI ≤32 B, DMAPP, Tofu) left as extensions |
| alltoall[s], collect, fcollect, scans | strip-mined nb puts (Cray), two-phase sizes | O(N²) data for alltoall; Cray extra symmetric scratch (VEN §A8.2) |
| Locks | SOS queue lock (CSWAP/WRITE/MSWAP + notify + quiet) | ≥3 AMOs + a wait per contended acquire; a rank-0 hotspot if built like OSHMPI (MPI §9) |
| `malloc` / `free` / `realloc` / `align` / `calloc` / hints | deterministic allocator in the reserved segment + barrier | barrier per call; **no registration**. Growth beyond the reservation is not supported (OSHMPI also cannot grow, MPI §8.10) |
| Context creation | `oc_ctx_create` or fall back to a shared ctx | may fail at the per-node budget (§3.2) |
| Sessions | `OC_F_MORE` doorbell coalescing or no-op | none; batching is legal because blocking put needs only local completion (SEM §4.14) |
| `global_exit`, init/finalize, `info`, `*_accessible` | runtime / local registry | cold |
| RMW to on-node peers with non-coherent NIC | NIC loopback | an RTT instead of a `lock` instruction (NV §4 CST precedent) |
| `wait`/`test` coherent read with non-coherent NIC | `oc_seg_sync` before load | an implementation-defined flush (P4 §8b.9) |
| GPU-memory ops without device-initiated support | CPU proxy thread | ~1.7 MOPS vs ~180 MOPS put rate (NV §7, §9) |
| First contact on efa | per-peer handshake | an extra RTT + target progress (VEN §C3) |
| RUDI-class bulk puts | not used by default | would save target PDC state; blocked by Q2 |

---

## 8. Open questions

### 8.1 Design questions

- **Q1 — compound primitive or flag?** Should `put + fenced rmw` be a single core call (`oc_put_rmw`)?
  - *For:* UCX `ucp_device_put` + `counter_inc` on one channel (UCX §8.1), GPI `write_notify` (GPI §B.1), LCI remote completion (LCI §C.1), and hot usage (USE §2.9) favour one call.
  - *For the flag:* keeps the core smaller, and IBGDA already achieves it with two WQEs (NV §4).
  - *Open:* measure doorbell cost on CXI and UET.
- **Q2 — RUDI and late duplicates.** Can at-least-once RMA (UET RUDI) carry puts without breaking fence? A late duplicate could land after the initiator saw completion and issued a newer overlapping put (FUT §A.4). Candidate guards: TSS epochs, packet-lifetime waits, or RUD after a fence. Until resolved, the core never uses RUDI for OpenSHMEM puts.
- **Q3 — fence epoch state.** Per-(ctx, peer) state on connectionless NICs is ~4 B per touched peer per context. Is that acceptable, or should `oc_ctx_fence` flag *all* ops until the context drains? That is cheaper in memory but serialises every peer during the window.
- **Q10 — hint-less placement.** `malloc_with_hints` has no users (USE §1). Should the implementation *auto-place* AMO and wait targets? It cannot know them in advance; tracking and relocation are unsafe. Otherwise HOT `wait_until` users (PRK, and NVSHMEM-style codes if ported) get the polling fallback.
- **Q11 — MIN/MAX rmw.** Graph500 emulates MIN and BOR with ≥4 round trips per edge (USE §2.1). MIN/MAX exist natively on Portals, CXI and UET (P4 §1; OFI §1.2; FUT §A.1) but are not OpenSHMEM AMOs. Should they be a core extension?
- **Q12 — handle-returning nonblocking ops.** Should there be an optional per-op completion handle? Evidence: Cray `shmemx_*_nb`, DMAPP syncid, GASNet NB events (VEN §A8.4; GEX §A.1). With counters only, blocking gets on shared contexts may over-wait under THREAD_MULTIPLE.
- **Q13 — GPU-callable subset.** The GPU-callable subset (put/get/rmw/fence/count) needs a lock-free submission model and polling only (NV §6–7). The API above is host-shaped; a device header is unspecified.
- **Q14 — error model.** Counters lose *which* op failed (P4 §8b.8). Is "failed > 0 ⇒ abort" acceptable at 10^5 PEs, or does the core need an error record queue?
- **Q15 — reservation size.** Heap reservation size and whole-VA registration depend on ODP/ATS on some NICs. CXI documents network page faults as a significant performance hit (OFI §9); UCX GVA is mlx5-only, opt-in (UCX §2). What should the default reservation be, and how are 256 GiB-class GPU heaps registered without per-chunk keys (NV §2)?
- **Q16 — context budget.** Should the per-node context budget be negotiated collectively at init (Cray computes it from PEs per node, VEN §A6) or left first-come?
- **Q17 — alltoallv.** The demand is alltoallv (bale `shmemx_alltoallv`, ISx, MoE libraries; USE §4.3), which OpenSHMEM lacks. It is out of scope for conformance but relevant to where "slow" collectives land.

### 8.2 Facts to verify

- **Q4 — mlx5 fence-mode bits.** The exact semantics of `NO_FENCE`/`FENCE`/`STRONG_ORDERING` are undocumented (SEM §5.2; NV §4). The matrix cell is `N?`.
- **Q5 — EFA remote counters.** OFI §1.2 says efa has no `FI_RMA_EVENT`. VEN §C1 quotes "both fabrics support local and remote operation counters". SEM §4.6 cites `fi_cntr_wait` on EFA hardware counters. The reports disagree; check `fi_efa.7` and the comparison doc together.
- **Q6 — mlx5 atomics portability.** Reports disagree on which path exposes them (OFI §1.2 vs GEX §A.9 vs UCX §1 vs NV §1; reconciled in §2.4). Confirm that masked atomics are reachable only via DevX/PRM.
- **Q7 — uTofu.** Guide unreadable. ARMW op list, STAG-computable STADD, ordering and MRQ notification are all *recalled* (VEN §B0).
- **Q8 — team-relative PE translation.** SOS and OSSS-UCX appear not to translate team-relative PEs in their RMA paths (SEM §4.9, medium confidence). The core addresses world ranks, so the layer above must translate. Worth a conformance test.
- **Q9 — visibility vs transmit-complete.** Does transmit-complete (target NIC ACK) imply visibility on CXI? The `FI_DELIVERY_COMPLETE` flushing read suggests not always (OFI §4). This decides the cost of `oc_ctx_wait`.
- **Q18 — spec editorial issue.** The `signal_add`/`signal_set` text says "at the calling PE" (SEM §4.5). It looks editorial; clarify with the spec committee.
- **Q19 — minor inconsistencies seen across reports** (none drives the design):
  - Semantics describes SOS locks as "swap on lock->last" while OFI §8 says "cswap on lock->last" (both use CSWAP, WRITE and MSWAP overall).
  - NV §3.4 *recalled* that `FI_FENCE` is not upstream, contradicted by OFI §1.1 (`fabric.h` bit 1<<21) and FUT §A.4.
  - CXI man page vs source on ATOMIC_WAR/RAW ordering (OFI §4).
  - Portals reference implementation `max_waw_ordered_size` default 8 vs spec minimum 64 (P4 §4).
  - NVSHMEM docs vs code on DC vs RC default (NV §3.2).
  - NET4EXA "Portals 3" vs "Portals 4" (P4 §10).
  - UCX atomic API: SEM calls it deprecated (`ucp_compat.h`), UCX §1 documents `ucp_atomic_op_nbx` as current, VEN *recalled* it as removed.

---

## 9. Sources

These are consolidated from the reports I relied on. Commits are as pinned by those reports.

### Specifications and documents

- OpenSHMEM 1.6 (local extraction `scratchpad/spec16.txt` of `docs/reference/OpenSHMEM-1.6.pdf`, 1 Nov 2024).
- Portals 4.3, SAND2022-8810: https://www.sandia.gov/app/uploads/sites/144/2023/03/portals43.pdf
- Ultra Ethernet Specification v1.0.2 (28 Jan 2026): https://ultraethernet.org/wp-content/uploads/sites/20/2026/01/UE-Specification-1.0.2-1.pdf
- CXL 3.0 white paper: https://computeexpresslink.org/wp-content/uploads/2023/12/CXL_3.0_white-paper_FINAL.pdf
- libfabric man pages (main): https://ofiwg.github.io/libfabric/main/man/fi_msg.3.html, https://ofiwg.github.io/libfabric/main/man/fi_efa.7.html; v1.9 fi_collective: https://ofiwg.github.io/libfabric/v1.9.0/man/fi_collective.3.html; v1.15.2 fi_gni: https://ofiwg.github.io/libfabric/v1.15.2/man/fi_gni.7.html
- UCX FAQ: https://openucx.readthedocs.io/en/master/faq.html
- NVSHMEM docs: https://docs.nvidia.com/nvshmem/api/latest/api/overview.html, https://docs.nvidia.com/nvshmem/api/latest/gen/mem-model.html, https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/performance.html, https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/unsupported-operations.html
- NVIDIA IBGDA blog: https://developer.nvidia.com/blog/improving-network-performance-of-hpc-systems-using-nvidia-magnum-io-nvshmem-and-gpudirect-async/
- AWS EFA guide: https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/efa.html; SRD driver note: https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/efa/SRD.txt
- Cray OpenSHMEMX docs: https://github.com/PE-Cray/cray-openshmemx/blob/master/doc/source/intro_shmem.rst
- HPE CPE hugepages: https://cpe.ext.hpe.com/docs/24.03/craype/intro_hugepages.html
- Fujitsu uTofu guide (redirects; not read in full): https://software.fujitsu.com/jp/manual/manualfiles/m210007/j2ul2482/02enz003/j2ul-2482-02enz0.pdf

### Source code (pinned commits)

- SOS: https://github.com/Sandia-OpenSHMEM/SOS/blob/4aa91d7fa9041785bd367fc9cefab99d313dd9df/ (`src/transport_ofi.{c,h}`, `src/transport_portals4.{c,h}`, `src/shmem_synchronization.h`, `src/shmem_lock.h`, `src/collectives.c`, `configure.ac`)
- libfabric: https://github.com/ofiwg/libfabric/blob/0d5527ae694af929014da0385d409f638e81105e/ (`prov/cxi/src/cxip_atomic.c`, `cxip_cmdq.c`, `cxip_info.c`, `cxip_dom.c`, `man/fi_cxi.7.md`, `prov/efa/docs/*`, `prov/rxm/src/rxm_cq.c`, `prov/verbs/src/verbs_ep.c`); future report also used commit 056b8ae
- rdma-core: https://github.com/linux-rdma/rdma-core/blob/00357df3a344b3515a9a4d345cf0b8c8efb7f116/
- UCX: https://github.com/openucx/ucx/blob/9c85cdf1425441e4820e9d2ead9534291b5802f0/ (`src/ucp/rma/amo_sw.c`, `src/ucp/api/ucp.h`, `src/uct/ib/mlx5/dc/dc_mlx5.c`, `src/ucp/core/ucp_rkey.c`)
- Open MPI (oshmem, osc): https://github.com/open-mpi/ompi/blob/8caa67e74b7bb3144b4a995156bd6102baa44dcb/; v4.1.6 `btl/ugni`: https://github.com/open-mpi/ompi/blob/v4.1.6/
- OSSS-UCX: https://github.com/openshmem-org/osss-ucx/blob/cbbb91d4a6eb1f6ecd8ccd77b35c174afd2e1040/
- OSHMPI: https://github.com/pmodels/oshmpi/blob/ee5cf110e673c098707257bb025404e17ac0a5fc/
- MPICH v5.0.1: https://github.com/pmodels/mpich/blob/v5.0.1/
- NVSHMEM: https://github.com/NVIDIA/nvshmem/blob/b0d9d3dc08fc3ee0840fdb6f3a2c11932d85a2e2/ (`src/include/non_abi/device/pt-to-pt/ibgda_device.cuh`, `src/modules/transport/ibgda/ibgda.cpp`, `src/include/device_host/nvshmem_proxy_channel.h`)
- Portals 4 reference implementation: https://github.com/Portals4/portals4/tree/4e787c1330d956f9d924b7aed21094eb40dfd83d
- GASNet-EX: https://github.com/BerkeleyLab/gasnet (commit 19143e8b1b8965727555194110beeb81a57080b5)
- GPI-2: https://github.com/cc-hpc-itwm/GPI-2 (3f5dbc68ef8c2eb89f92aa2514c49be6f1865aa3, v1.7.0)
- LCI: https://github.com/uiuc-hpc/lci (663de28b9287eaae1313ccec826c0ca2ca9e0c2b)
- OpenSHMEM reference implementation (GASNet): https://github.com/openshmem-org/openshmem (5f91637b27690c148cbf81523f2bc0b0fb7829b4)
- OpenFAM: https://github.com/OpenFAM/OpenFAM (2dfb164); API header https://github.com/OpenFAM/API/blob/master/fam.h
- RIKEN QWS uTofu comlib: https://github.com/RIKEN-LQCD/qws/blob/master/rdma_utofu_comlib.h
- UET reference providers: https://github.com/ultraethernet/uet-ref-prov, https://github.com/spcl/suet

### Usage corpus (USE §0)

- OSU OMB 7.5: https://mvapich.cse.ohio-state.edu/download/mvapich/osu-micro-benchmarks-7.5.tar.gz
- OSB: https://github.com/ornl-languages/osb @39cfcadd
- NPB: https://github.com/openshmem-org/openshmem-npbs @0028e8d0
- ISx: https://github.com/ParRes/ISx @d31936a6
- PRK: https://github.com/ParRes/Kernels @25051e46
- tests-sos: https://github.com/openshmem-org/tests-sos @ca9cb12b
- Examples: https://github.com/openshmem-org/openshmem-examples @4c742d5e
- bale: https://github.com/jdevinney/bale @1b8f673b
- HClib: https://github.com/habanero-rice/hclib @50970656
- Kokkos Remote Spaces: https://github.com/kokkos/kokkos-remote-spaces @b8b6f069
- DeepEP: https://github.com/deepseek-ai/DeepEP @a56d6156
- pplx-kernels: https://github.com/ppl-ai/pplx-kernels @f70d1eb7
- Triton-distributed: https://github.com/ByteDance-Seed/Triton-distributed @8260bc34
- openshmem_rs: https://github.com/kumaryash6352/openshmem_rs @5e1fa06f
- Lamellar: https://github.com/pnnl/lamellar-runtime @06b7746b
- Graph500 SHMEM paper: https://www.osti.gov/servlets/purl/1302887

### Papers

- Bonachea & Hargrove, GASNet-EX, LCPC'18: https://gasnet.lbl.gov/pubs/gasnet-ex-lcpc18-tech.pdf
- Si, Fu, Hammond, Balaji, OpenSHMEM over MPI, OpenSHMEM 2021: https://minsii.github.io/assets/pdf/openshmem2021-mpi.pdf
- Hammond, Ghosh, Chapman, OpenSHMEM using MPI-3 one-sided, 2014: https://www.csm.ornl.gov/workshops/openshmem2013/documents/ImplementingOpenSHMEM%20UsingMPI-3.pdf
- Si & Balaji, SC19 BoF slides: https://www.mcs.anl.gov/~minsi/pdf/oshmpi-shmem-bof-201911.pdf
- ten Bruggencate & Roweth, DMAPP, CUG 2010: https://cug.org/5-publications/proceedings_attendee_lists/CUG10CD/pages/1-program/final_program/CUG10_Proceedings/pages/authors/01-5Monday/03B-tenBruggencate-Paper-2.pdf
- Pritchard, uGNI MPICH2, CUG 2011: https://cug.org/5-publications/proceedings_attendee_lists/CUG11CD/pages/1-program/final_program/Wednesday/13C-Pritchard-Paper.pdf
- Gutierrez & Hjelm, Open MPI on Cray XE, HOTI 2012: https://www.cs.unm.edu/~samuel/publications/2012/perf-eval-ompi-cray-xexk.pdf
- Shalev et al., SRD, IEEE Micro 2020: https://ieeexplore.ieee.org/document/9167399
- Tagawa et al., UCX for Tofu-D, J. Supercomputing 2024: https://link.springer.com/article/10.1007/s11227-024-06201-x; HPC Asia 2023 workshop: https://www.researchgate.net/publication/367563945
- Brightwell, Portals 4 Exascale, OFA 2012: https://downloads.openfabrics.org/Media/Monterey_2012/2012_Workshop_Mon_Portals_4_Exascale.pdf
- Martinelli et al., NET4EXA, arXiv:2601.19413: https://arxiv.org/pdf/2601.19413
- Brightwell et al., Portals on SeaStar, CUG 2006: https://www.sandia.gov/app/uploads/sites/210/2022/11/p3-accel-cug06.pdf
- Islam et al., hardware counters for triggered collectives: https://dl.acm.org/doi/abs/10.1145/3343211.3343222
- Hemmert, Barrett, Underwood, triggered-op collectives, EuroMPI 2010: https://link.springer.com/chapter/10.1007/978-3-642-15646-5_26
- Grant et al., OpenSHMEM contexts over OFI, OpenSHMEM 2017: https://link.springer.com/chapter/10.1007/978-3-319-73814-7_2
- Ni et al., Cxlalloc, ASPLOS '26: https://www.cs.utexas.edu/~witchel/pubs/ni26asplos-cxlalloc.pdf
- Singhal et al., OpenFAM, CUG 2022: https://cug.org/proceedings/cug2022_proceedings/includes/files/pap102s2-file1.pdf
- "Demystifying NVSHMEM", arXiv 2606.05951
- "GPU-Initiated Networking for NCCL", arXiv 2511.15076
- De Sensi et al., Slingshot, SC20: https://arxiv.org/pdf/2008.08886
- LINKx / CXI performance (Open MPI): https://www.osti.gov/servlets/purl/2438730
- OpenSHMEM-UCX on Cray XK: https://www.osti.gov/servlets/purl/1265689
- ALCF Theta MPI slides (DMAPP collectives): https://www.alcf.anl.gov/sites/default/files/2020-05/Chunduri_MPI_Theta_CPW_May_5_2020_final.pdf
- The Next Platform on BXI (journalism): https://www.nextplatform.com/2024/12/10/eviden-mainstreams-bxi-interconnect-thanks-to-ultra-ethernet-and-ai-boom/
- Open MPI UBCL OSC: https://github.com/open-mpi/ompi/tree/main/ompi/mca/osc/ubcl
- Open MPI issues: https://github.com/open-mpi/ompi/issues/12702, https://github.com/open-mpi/ompi/issues/9003
- UCX issues: https://github.com/openucx/ucx/issues/2408, https://github.com/openucx/ucx/issues/3731
- Sandia Portals projects page: https://www.sandia.gov/portals/projects-using-portals-4-0/
- SOS performance-tuning wiki: https://github.com/Sandia-OpenSHMEM/SOS/wiki/Performance-Tuning
- LWN UET kernel RFC: https://lwn.net/Articles/1013363/
