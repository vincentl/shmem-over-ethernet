# A minimal core API for OpenSHMEM 1.4–1.6 at very large scale

Study question Q2, final deliverable. Date: 2026-09-16.

This is revision 2. It revises `q2-architect.md` after the adversary round in `q2-adversary.md`.

## How to read this document

### Evidence reports

Evidence lives in the per-topic reports under `docs/reviews/evidence/q2/`. Citations use the short tag below plus a section number.

| Tag | File | Contents |
|---|---|---|
| SEM | `q2-semantics.md` | per-routine cost classification; Annex G version deltas |
| USE | `q2-usage.md` | 18 public code bases: which routines are called, and how hot |
| OFI | `q2-ofi.md` | libfabric cxi, verbs;rxm, efa, opx; the SOS OFI transport |
| UCX | `q2-ucx.md` | UCX, OSSS-UCX, Open MPI oshmem |
| P4 | `q2-portals.md` | Portals 4, SOS Portals transport, reference implementation (RI), BXI |
| GEX / GPI / LCI | `q2-gasnet-gpi-lci.md` | GASNet-EX, GPI-2, LCI |
| MPI | `q2-mpi.md` | OSHMPI, MPICH ch4, Open MPI osc |
| NV | `q2-nvshmem.md` | NVSHMEM |
| VEN | `q2-vendor.md` | DMAPP/uGNI (A), uTofu (B), EFA/SRD (C) |
| FUT | `q2-future.md` | Ultra Ethernet (A), CXL/OpenFAM (B) |
| ADV | `q2-adversary.md` | the adversary report |
| ARCH | `q2-architect.md` | revision 1 of this proposal |
| SPEC Lnnnn | `docs/reference/OpenSHMEM-1.6.pdf` | line number in `pdftotext -layout` output of the 1.6 spec (not committed; regenerate to follow a line cite) |
| osoe §x | `docs/design/shmem-ethernet-design.md` | osoe design notes |
| DS | `docs/reviews/design-soundness.md` | osoe design-soundness review |

### Evidence labels

The reports label their claims *documented*, *observed in source*, or *recalled*, and those labels carry over. When this document adds anything from memory it says *recalled*. Arithmetic of mine is marked *arith*. A design-bearing fact backed by only one report is marked **single-source**.

---

## 1. Summary: the proposed core API on one page

### 1.1 Design

**Addressing and memory.**
- A remote location is `(world rank, segment, offset)`.
- Segments are registered once per process and shared by all contexts.
- Attributes such as "atomic-capable" and "notify" apply to **ranges** inside a segment. That lets the bulk heap keep its fast registration while signal targets get counters.

**Data primitives.** There are three:
- `put` / `get`, plus word-sized variants;
- `rmw`: a read-modify-write of one 4- or 8-byte word, delivered **exactly once**;
- a **fence**, which orders later ops behind earlier ones per `(ctx, rank)` across all rails. Its gate is *placement at the target*. It can be requested per operation or with a per-context call.

**Completion.**
- Asynchronous ops complete through per-context counters.
- Blocking gets and fetching `rmw` complete **per operation** inside the core.
- Remote *visibility* is established only when `oc_ctx_wait` returns. It uses a per-context dirty set of peers, unless the substrate says transmit-complete already implies visibility.

**Waking waiters.**
- A notify word counts every `rmw` applied to a notify range, by *any* path: NIC, loopback, CPU atomic, emulation, or self. It can also count puts.
- It wakes local waiters.
- Waiters always use a finite deadline and re-check.

**Everything else is layered above:** teams, collectives, locks, strided RMA, multi-element waits, sessions, allocation.

**Capability bits declare the costs:**
- which ops need target CPU progress;
- whether NIC atomics are CPU-coherent;
- whether fence is free;
- the context and counter budgets;
- the per-node memory per peer.

### 1.2 C signature sketch

```c
typedef uint32_t oc_rank_t;   /* world rank; never 16-bit (NV §3.7) */
typedef uint8_t  oc_seg_t;    /* same value on every PE */
typedef struct oc_dom oc_dom_t;  typedef struct oc_ctx oc_ctx_t;

typedef struct {   /* supplied by launcher/runtime */
  int  (*lookup)(void*, oc_rank_t r0, size_t n, void *blobs); /* O(1)/local or node-shared; batchable */
  int  (*node_of)(void*, oc_rank_t, uint32_t *node);
  void (*abort)(void*, int code);
  void  *arg;
} oc_runtime_t;

typedef struct {
  uint64_t caps;           /* OC_CAP_* */
  uint32_t target_cpu;     /* OC_TCPU_* : which ops need target-side CPU progress */
  uint32_t rmw_ops;        /* bit per oc_rmw_op_t native */
  size_t   max_inline, max_rma, max_ctx_hint, max_cntr, max_segs_per_node;
  size_t   bytes_per_peer_per_node;  /* address+key state, node-shared */
} oc_caps_t;
#define OC_CAP_SYMMETRIC_KEYS   (1u<<0)
#define OC_CAP_FENCE_NATIVE     (1u<<1)   /* NIC/provider defers fenced op */
#define OC_CAP_ORDERED          (1u<<2)   /* per-(ctx,rank) order: fence no-op */
#define OC_CAP_RMW_CPU_COHERENT (1u<<3)
#define OC_CAP_NOTIFY_CHEAP     (1u<<4)   /* notify on a range costs puts nothing */
#define OC_CAP_TXCOMP_VISIBLE   (1u<<5)   /* substrate completion implies visibility */
#define OC_CAP_CONNECTIONLESS   (1u<<6)
#define OC_CAP_DEVICE_INITIATED (1u<<7)   /* requires eager addressing of device-visible ranks */
enum { OC_TCPU_PUT=1, OC_TCPU_GET=2, OC_TCPU_RMW=4, OC_TCPU_RELIABILITY=8, OC_TCPU_FENCE=16 };

/* domain */
enum { OC_THREAD_SINGLE, OC_THREAD_FUNNELED, OC_THREAD_SERIALIZED, OC_THREAD_MULTIPLE };
#define OC_DOM_PROGRESS_THREAD (1u<<0)
#define OC_DOM_FENCE           (1u<<1)   /* enable fence capability (CXI: loses PCIe RO) */
int  oc_dom_open(const oc_runtime_t*, oc_rank_t me, oc_rank_t npes, int thread_level,
                 uint64_t flags, oc_dom_t**);
int  oc_dom_query(oc_dom_t*, oc_caps_t*);
int  oc_dom_interrupt(oc_dom_t*);        /* global_exit: fail all blocked waits */
int  oc_dom_close(oc_dom_t*);            /* restartable: open after close is legal */

/* memory: one reservation per segment, attributes per range */
enum { OC_MEM_HOST, OC_MEM_CUDA, OC_MEM_ROCM, OC_MEM_ZE };
#define OC_RANGE_RMW          (1u<<0)
#define OC_RANGE_NOTIFY       (1u<<1)    /* counts rmw by any path */
#define OC_RANGE_NOTIFY_PUTS  (1u<<2)    /* also counts puts/stores (1.4, short waits) */
int   oc_seg_reserve(oc_dom_t*, oc_seg_t, void *base, size_t len, int memkind, uint32_t default_attrs);
int   oc_seg_set_range(oc_dom_t*, oc_seg_t, uint64_t off, size_t len, uint32_t attrs);  /* collective */
void *oc_seg_peer_ptr(oc_dom_t*, oc_rank_t, oc_seg_t);
int   oc_seg_sync(oc_dom_t*, oc_seg_t, uint64_t off, size_t len);  /* no-op if RMW_CPU_COHERENT */

/* contexts */
#define OC_CTX_SERIALIZED (1u<<0)
#define OC_CTX_PRIVATE    (1u<<1)
#define OC_CTX_NOSTORE    (1u<<2)
int  oc_ctx_create(oc_dom_t*, uint32_t flags, oc_ctx_t**);   /* may fail: OC_EAGAIN */
int  oc_ctx_destroy(oc_ctx_t*);

/* per-op flags */
#define OC_F_ASYNC   (1u<<0)
#define OC_F_FENCE   (1u<<1)   /* placed only after all earlier ops on (ctx,rank), all rails */
#define OC_F_MORE    (1u<<2)
#define OC_F_TRY     (1u<<3)
#define OC_F_VISIBLE (1u<<4)   /* make this op visible now (flush-on-op) */

int oc_put     (oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, const void*, size_t, uint32_t fl);
int oc_get     (oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, void*, size_t, uint32_t fl);
int oc_put_word(oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, uint64_t v, unsigned w, uint32_t fl);
int oc_get_word(oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, unsigned w, uint64_t *v, uint32_t fl);

typedef enum { OC_RMW_WRITE, OC_RMW_READ, OC_RMW_CSWAP, OC_RMW_ADD,      /* required */
               OC_RMW_MSWAP, OC_RMW_AND, OC_RMW_OR, OC_RMW_XOR } oc_rmw_op_t;  /* capability */
int oc_rmw(oc_ctx_t*, oc_rank_t, oc_seg_t, uint64_t off, oc_rmw_op_t, unsigned w,
           uint64_t operand, uint64_t cmp_or_mask, uint64_t *old, uint32_t fl);

/* ordering and completion */
enum { OC_OPC_PUT=1, OC_OPC_GET=2, OC_OPC_RMW=4, OC_OPC_ALL=7 };
#define OC_FENCE_STORES (1u<<0)  /* also order CPU stores via peer_ptr (non-NOSTORE ctx) */
int oc_ctx_fence  (oc_ctx_t*, uint32_t flags);
int oc_ctx_count  (oc_ctx_t*, unsigned cls, uint64_t *issued, uint64_t *done, uint64_t *failed);
int oc_ctx_wait   (oc_ctx_t*, unsigned cls, int64_t deadline_ns);   /* completes + makes visible */
int oc_ctx_wait_pe(oc_ctx_t*, const oc_rank_t*, size_t, int64_t deadline_ns);
int oc_progress   (oc_ctx_t*);

/* target-side notification (process-local, shared by all threads) */
int oc_notify_read(oc_dom_t*, oc_seg_t, uint64_t off, uint32_t *seq);   /* 32-bit wake word */
int oc_notify_wait(oc_dom_t*, oc_seg_t, uint64_t off, uint32_t seq, int64_t deadline_ns);
```

### 1.3 Size of the core

- 22 `oc_*` functions and 3 runtime callbacks. Ten of the functions are on the data path: `put`, `get`, `put_word`, `get_word`, `rmw`, `ctx_fence`, `ctx_count`, `ctx_wait`, `notify_read` and `notify_wait`.
- **Where it is native.** On CXI and on the UEC HPC profile, the data-path primitives for every HOT pattern in USE §2 are native: put/get, word ops, add/cswap `rmw`, and put-with-signal. The caveats are the per-cell ones in §4.
- **Not native everywhere.** Blocking `wait_until` is native only where the notify range is cheap. Portals fence is native only with total data ordering. This claim was narrowed after ADV B4 and A1.

---

## 2. Primitives: semantics and rationale

### 2.1 Addressing, segments, ranges

**Address form.** A remote location is `(rank, seg, off)`, where `off` is the byte offset from the start of the segment on the target.
- The OpenSHMEM layer computes `off = addr − local_base(seg)`.
- A deterministic symmetric allocator gives each object the same offset on every PE.
- The spec's footnote anticipates "the same offset (from an arbitrary memory address)" (SPEC L605).
- **No per-PE base table** is needed, which is what SPEC L2002–2006 requires (SEM §2.7).

**Segments.**
- `0` = data/BSS of the main executable.
- `1` = symmetric heap: one reservation of `SHMEM_SYMMETRIC_SIZE`.
- `2+` = device heaps.
- Statics in shared objects are **not symmetric** per the spec ("must not be defined in a dynamic shared object", SPEC L603–604), so no per-image segments are needed. This rejects ADV A6(2).
- Within one executable, a global's offset from `__data_start` is the same in every process (*recalled*: ELF layout is fixed at link time). SOS and OSHMPI both use data-segment base plus offset (OFI §2; MPI §2).

**Ranges.** `oc_seg_set_range` gives a sub-range its own attributes: `RMW`, `NOTIFY`, or `NOTIFY_PUTS`. The substrate may implement a range as:
- a separate MR over the sub-range (libfabric);
- a separate LE with its own CT (Portals);
- a software watch table (CPU and AM substrates).

The allocator carves hinted allocations from sub-ranges of the one heap reservation:
- `SHMEM_MALLOC_ATOMICS_REMOTE` and `SHMEM_MALLOC_SIGNAL_REMOTE` (SPEC L2205–2225) grow from one end;
- unhinted allocations grow from the other.

This follows ADV A7. It replaces revision 1's fixed per-hint segments, which could not honour `realloc`'s "preserves hints" rule (SPEC ~L2098) or an unknown hint mix.

**Default attributes** are chosen from capabilities, not fixed:
- Heap and data are `RMW` wherever atomic registration costs puts nothing. HOT AMO users never pass hints (USE §1–2; ADV A8). Whether mlx5 atomic registration is free for RDMA is *recalled*; see §7.2 C4.
- `NOTIFY` over the whole heap and data segment is the default when `OC_CAP_NOTIFY_CHEAP`:
  - Portals CT on the LE (P4 §5);
  - osoe's software watch table (osoe §4.9);
  - UET, if the MR is not `IDEMPOTENT_SAFE` (FUT §A.5).
- Otherwise `NOTIFY` stays on signal-hinted ranges only, and `wait_until` on other addresses polls:
  - CXI: a counter bound to an MR disables optimized RMA for every op on it (OFI §2, §9; **single-source**);
  - UET: counters are forbidden on `IDEMPOTENT_SAFE` MRs (FUT §A.5).

**Keys.**
- With `OC_CAP_SYMMETRIC_KEYS` the key is derived from `seg`: zero bytes per peer. Sources: CXI client keys (OFI §2), Portals LEs (P4 §2), UET user keys (FUT §A.2), DMAPP (VEN §A2).
- Otherwise the key and remote base are held in **node-shared** tables (§2.8): 16 B per segment per peer (OFI §2, VEN §C2).

**Reservation limits.**
- Reserving and registering an untouched range needs ODP/ATS, or else pinning the whole reservation at init (OFI §9; UCX §2 GVA).
- Mapping on-node peers costs `PPN × reservation` of virtual address space per process, which caps the reservation size (ADV A7; osoe DS B2 raises the same VA issue).

**Errors.** An out-of-range address returns `OC_EFAULT` when the initiator can check it. Otherwise it counts as `failed` (P4 §2 truncation, `PTL_NI_SEGV`).

### 2.2 `oc_put`, `oc_put_word`

**Ordering.** None, except through a fence. Within a put, bytes may land in any order (SPEC L3564–3567). The core may fragment, spray, or stripe across rails, subject to the fence rule in §2.5.

**Completion.**
- Without `OC_F_ASYNC`, the call returns at *local completion*.
  - Puts up to `max_inline` are copied or inlined.
  - Larger puts are bounce-copied, or sent zero-copy with the core waiting for its own local completion.
  - Zero-copy on CXI costs a target-NIC ack round trip (ADV D5). The core chooses by size, and the spec permits either (SEM §2.3).
- With `OC_F_ASYNC`, the source buffer stays in use until an `oc_ctx_wait` covers the op (SPEC §4.2).
- A fragmented put counts as **one** op.

**Delivery.** Reliable. After `oc_ctx_wait(PUT)` returns success, no duplicate of a covered put is ever applied. At-least-once transports such as UET RUDI therefore do not carry puts by default (§7.2 C1).

**Atomicity.** None against `rmw` on the same bytes. The spec makes that mix undefined (SPEC §3.2; SEM §2.5).

**`oc_put_word`** exists for `shmem_p` and 1-element typed puts. They are the hottest pattern in the corpus: Kokkos Remote Spaces, GUPs, bale `lgp_*`, SSCA1 (USE §1, §2.2–2.5). Native carriers:
- Portals volatile MD (P4 §8a);
- libfabric `FI_INJECT` (OFI §1.1);
- GASNet `PutBlockingVal` (GEX §A.1);
- UET small headers (FUT §A.2).

**Why no per-op ack.** A blocking put is locally complete on return (SEM §2.1). Production transports complete puts by counters: SOS/OFI (OFI §5), SOS/Portals (P4 §5), DMAPP `gsync` (VEN §A1), GASNet NBI regions (GEX §A.1).

### 2.3 `oc_get`, `oc_get_word`

**Blocking forms complete per operation inside the core.** This covers `oc_get` and `oc_get_word` without `OC_F_ASYNC`. Examples of per-op mechanisms:
- a per-op completion slot or flag;
- IBGDA's ibuf slot plus a consistency op (NV §8b.2);
- DMAPP's syncid (VEN §A1).

**Why not a class-wide counter wait.** Revision 1 used one. ADV A4 showed that gets complete out of order, so `done ≥ issued_at_entry` does not prove *this* get arrived. Waiting for `done ≥ issued_now` can starve under THREAD_MULTIPLE on the shared default context; SOS spin-polls in that case (OFI §5).

**`OC_F_ASYNC` gets** fill `dst` by the time `oc_ctx_wait(GET)` returns. The core keeps O(outstanding async gets) internal records for bounce copy-out or dynamic registration (ADV A11; SOS long frags, P4 §8b.1). `oc_ctx_wait` performs the copy-out.

**Ordering.** Gets are never fenced (SPEC L8933–8935).

### 2.4 `oc_rmw`

**Operations.** `w` ∈ {4, 8}. Every AMO type in Tables 6–8 is `int`, `long`, `long long`, their unsigned forms, `int32/64`, `uint32/64`, `size_t`, `ptrdiff_t`, `float` or `double` (SPEC L4241–4290). On LP64 all are 4 or 8 bytes (*recalled*: platform ABI). There are no `short`, `char` or `long double` AMOs.

| op | effect | fetch | status |
|---|---|---|---|
| `WRITE` | `t = v` | → swap | required |
| `READ` | – | yes | required; native READ or get+sync preferred; fetching `ADD 0` is a last resort because it counts as a write on CXI/UET (ADV A9) |
| `CSWAP` | `if t == c: t = v` | opt | required |
| `ADD` | `t += v mod 2^w` | opt | required |
| `MSWAP` | `t = (t & ~m) \| (v & m)` | opt | capability |
| `AND` / `OR` / `XOR` | `t = t op v` | opt | capability |

**Completion.**
- A non-fetching `rmw` may return before it executes (SPEC L4186).
- A blocking fetching `rmw` completes per op (§2.3).
- An `OC_F_ASYNC` fetch fills `*old` by the time `oc_ctx_wait(RMW)` returns.

**Delivery is exactly once.** An `rmw` is never sent over at-least-once transports and never blindly replayed.
- Evidence: UET RUDI rules (FUT §A.4); Cray MPICH avoided AMOs for this reason (VEN §A4); SEM §5.1 R5.
- A failed `rmw` has an unknown outcome (§2.9).

**Atomicity.**
- **Exclusive per segment, per atomicity domain.** One execution path (NIC or CPU) is chosen per `(segment, domain)`. `SHMEM_TEAM_WORLD` and `SHMEM_TEAM_SHARED` are separate domains (SPEC L684–694; SEM §2.5c).
- **Non-coherent NIC.** A CPU-executed `rmw` or a CPU load of the same word must first call `oc_seg_sync`. This covers sequential mixing across domains, which is legal (ADV D4). Coherence is declared by `PTL_COHERENT_ATOMICS` (P4 §1), `UET_AMO_CPU_COHERENT` (FUT §A.1), and UCX `ATOMIC_CPU` (UCX §1).
- **Not required** against put/get or across widths (SEM §2.5a–b).
- **Required against target `wait`/`test` reads** (SPEC L684–685).
- **On-node peers.** With a non-coherent NIC, a WORLD-domain `rmw` to an on-node peer goes through NIC loopback. IBGDA does this for its consistency op (NV §4).

**Multi-rail.** All `rmw` to the same `(rank, seg, off)` use one NIC and queue.
- Portals atomicity is per NI (P4 §1).
- NVSHMEM hashes the target address to pick a QP (NV §4).
- GASNet ibv pins atomics to one QP (GEX §A.4).

**Op-set rationale (cross-checked).** Native coverage per substrate:

| Substrate | Native ops | Sources |
|---|---|---|
| Portals | SUM, bitwise, SWAP, CSWAP, MSWAP | P4 §1; SEM §4.4 |
| CXI | WRITE, SUM, bitwise, CSWAP family, MSWAP | OFI §1.2; SEM §4.4 |
| UET HPC | WRITE, READ, SUM, bitwise, CSWAP, MSWAP | FUT §A.1 |
| mlx5 via DevX/WQE | masked CS and FA, 4/8 B | NV §1; UCX §1; SEM §4.4 |
| UCX mlx5 | ADD, AND, OR, XOR, SWAP, CSWAP (32/64) | UCX §1; GEX §A.9 |
| DMAPP | 64-bit ADD/AND/OR/XOR/CSWAP plus fetch forms | VEN §A1 |
| uTofu | 4/8-B ARMW; op list *recalled* | VEN §B1 |

- `WRITE`, `CSWAP` and `ADD` with fetch are the intersection.
- The optional ops serve bitwise AMOs (hclib only, USE §1) and SOS's lock (masked swap, OFI §8).

**Reductions.** "RMW SUM into the root" works for integer SUM and for AND/OR/XOR only. Float, complex, PROD, MIN and MAX reductions stay in software (ADV A9; SOS Portals needs 32-byte atomic sizes, P4 §8b.2).

### 2.5 Fence

**Semantics.**
- `OC_F_FENCE` on an op: the op is *placed at the target* only after every op previously issued on the same `(ctx, rank)` has been placed. Ops issued later are deferred behind it.
  - The gate is "placed", not "completed at the initiator". This matches the spec's "delivered" (SPEC L8931–8933) and ADV D2. On UCX, local completion orders nothing (UCX §1, §8.5).
- **Scope covers all rails and all software queues** (ADV A5). Before posting a fenced op, the core must either:
  - keep every op to that rank since the last fence on the fenced op's rail (per-`(ctx, rank)` rail affinity inside a fence window), or
  - drain the other rails.

  Ops held in software (backpressure, bounce buffers) are posted first. NVSHMEM disables consistency-op skipping on multi-NIC for the same reason (NV §4, §8).
- `oc_ctx_fence(ctx, 0)` issues no traffic. It opens a fence window: the next op to each rank carries `OC_F_FENCE`. The window state is either
  - (a) one context-wide word ("fence ops until `done` reaches `issued` sampled at the fence"), or
  - (b) a dense per-rank epoch array for contexts with few peers.

  The core picks one. A sparse hash is rejected because of cost at high touch (ADV C4: ≥16 B/entry × contexts × PPN).
- `oc_ctx_fence(ctx, OC_FENCE_STORES)` is used on contexts without `OC_CTX_NOSTORE`. It also drains in-flight ops to load/store-reachable ranks and issues a CPU memory barrier.
  - Table 14 fences **Memory Store** (SPEC L8893–8896). `NOSTORE` waives it (SPEC L2967–2972). Store ordering is per calling thread (SPEC L8948).
  - OSSS-UCX uses `_mm_mfence` and Open MPI `opal_atomic_wmb` (UCX §8.9).
- **Failure.** A fenced op whose gated predecessor failed is itself failed and not applied (ADV D3). A signal can never announce data that did not arrive.
- **Scope limits.** No ordering across contexts (SPEC L10408), across ranks, within an interval, or for gets and fetched values.

**Implementation tiers** (declared in capabilities):
1. **`OC_CAP_ORDERED`**: fence is a no-op.
   - Portals with total data ordering and small messages (P4 §4).
   - UET ROD, which forfeits spraying (FUT §A.4).
   - osoe's single TCP stream per node pair (osoe §3.1).
2. **`OC_CAP_FENCE_NATIVE`**: the NIC or provider defers the fenced op.
   - CXI `C_CMD_CQ_FENCE`, enabled per domain with `OC_DOM_FENCE`. It disables PCIe relaxed ordering for **all** RMA on that endpoint (OFI §1.2; SEM §5.2; ADV B8).
   - UET: mandatory in all profiles (FUT §A.4; **single-source**).
   - mlx5 fence-mode WQE bits: semantics undocumented (NV §4).
3. **Software gating**: drain earlier ops to the rank, or to the context in the worst case, before posting.
   - This is SOS's `fence = put_quiet` (OFI §4; P4 §8b.3).
   - It costs one round trip for the slowest in-flight op, and only when more traffic follows.

**Put-with-signal** is `oc_put` followed by `oc_rmw(..., OC_F_FENCE)`.
- This is how NVSHMEM IBGDA does it: RDMA-write WQEs, then an atomic WQE with `FM_FENCE` (NV §4; SEM §4.5).
- SOS does the same with `--enable-ofi-fence` (OFI §4).
- Without a fence flag, implementations do put + quiet + AMO: Open MPI and OSSS-UCX (UCX §8.1), SOS Portals (P4 §8b.4).
- Whether a compound call is better is open (§7.2 C2).

**Why lazy.** Graph500 and GUPs call quiet or fence after every op (USE §2.1–2.2). A fence must be free when nothing follows and cheap when something does. ROD-style default ordering would forfeit spraying (FUT §A.4; VEN §C4; P4 §4).

### 2.6 Completion, visibility, progress

**Counters.**
- Each context keeps (`issued`, `done`, `failed`) counts per class.
- `done` means **substrate completion**, which does not by itself imply visibility (ADV D1).
- `issued` is incremented before posting (P4 §5). Under `OC_THREAD_SINGLE`/`FUNNELED` it is a plain increment.
- `GET` and `RMW` may share one hardware counter. CXI allows 16 counters per domain (`cntr_cnt = 16`, OFI §2), so `max_cntr` is reported (ADV B2).

**`oc_ctx_wait(cls)`.**
1. Waits for `done + failed ≥ issued` for the async ops in `cls`.
2. Unless `OC_CAP_TXCOMP_VISIBLE` is set, flushes each peer in the context's **dirty set**: peers that have ops which completed but may not yet be visible. Substrate mechanisms:
   - CXI `FI_DELIVERY_COMPLETE`-style flushing read (OFI §4);
   - UCX `ep_flush` (UCX §4);
   - IBGDA DUMP consistency op (NV §4);
   - Open MPI already keeps such a per-peer set (UCX §8.5).
3. Returns. Visibility is guaranteed only after this return.
   - When nothing is issued and the dirty set is empty, this costs a few loads.
   - A dirty set is proportional to communication degree, not job size (osoe §4.6 makes the same argument).
   - After an all-to-all it is N; that cost is declared (ADV C3).
   - `OC_F_VISIBLE` on an op requests flush-on-op for codes that quiet after every op.

**`oc_ctx_wait_pe`** may be implemented as `oc_ctx_wait(ALL)`. The spec allows it (SPEC L9114–9115), and `pe_quiet` has no users (USE §3).

**Deadlines.** All waits take absolute deadlines. `oc_dom_interrupt` fails all blocked waits for `global_exit` (SPEC L1505–1507; ADV A12).

**No per-op handles for async ops.** Counters suffice: SOS OFI and Portals, DMAPP `gsync`, GASNet `GEX_EC` counters, OpenFAM `fam_progress` (OFI §5; P4 §5; VEN §A1; GEX §A.1; FUT §B.5). A handle form is open (§7.2 C5).

**Progress.**
- `caps.target_cpu` sets `OC_TCPU_*` bits for ops that need target CPU work:
  - onloaded RMA: opx, efa without device RDMA, UCX AM path, Portals RI, MPI AM (OFI §5; VEN §C5; UCX §1; P4 §5; MPI §5);
  - emulated `rmw`: rxm, efa, opx, UCX `amo_sw`, GASNet AM reference, OSHMPI (OFI §1.2; UCX §8.4; GEX §A.8; MPI §8.2);
  - software reliability: UCX UD/DC timers, opx (UCX §9; OFI §9);
  - software fence.
- If any bit is set, the OpenSHMEM layer opens the domain with `OC_DOM_PROGRESS_THREAD`. The spec expects asynchronous one-sided progress (SPEC L836–843).
- The thread may be per process, or a per-node daemon: the Portals RI "PPE" (P4 §5), osoe §3.12. A per-node host must solve reply landing (§8; DS B1).

### 2.7 Target notification

**Semantics.**
- Each `NOTIFY` range has a **32-bit wake word** (a futex-able word; the osoe probe found 64-bit futexes rejected, osoe §4.9).
- The word is bumped after **every `rmw` applied to the range by any path**: NIC counter, NIC loopback, CPU atomic, AM or emulated handler, and a self `rmw` from another local thread (SPEC L7710–7711).
- With `NOTIFY_PUTS` it also counts puts and stores that the core or layer performs through `oc_seg_peer_ptr`.
- **Waiter protocol:**
  1. read `seq`;
  2. load the ivar coherently (`oc_seg_sync` if non-coherent);
  3. test the predicate;
  4. call `oc_notify_wait(seq, deadline)` with a **finite** deadline, then loop.

  The deadline turns any residual lost wake into bounded latency; count timeout wakes (osoe §4.9 has the same rule).
- On substrates where only the NIC sees remote updates, the core bridges the NIC counter into the wake word. Local writes bump the word directly.

**Why revision 1 was wrong (ADV A2, fatal, accepted).**
- It counted only NIC `rmw`.
- Local-thread AMOs and on-node CPU atomics would never wake a blocked waiter: a deadlock.
- SOS forces polling when CMA/XPMEM paths exist, i.e. it met this problem and avoided blocking (OFI §5).

**Puts and short types.**
- Since 1.5, ivars "must be updated using OpenSHMEM AMOs" (SPEC L10471–10472).
- PRK Synch_p2p updates flags with puts (ADV A2(3), *observed in source* by ADV).
- The deprecated but required `short` waits have no matching AMO type (SPEC L7678–7695).
- `NOTIFY_PUTS` covers both. It is the default where cheap (Portals `CT_COMM` counts all incoming ops, P4 §5; osoe's watch table sees every heap write, osoe §4.9).

**Native carriers.**
- Portals target CT (P4 §5).
- UET HPC remote-write counters (FUT §A.5).
- uGNI `POST_CQWRITE` (VEN §A1).
- CXI `FI_RMA_EVENT` counters. No shipping OpenSHMEM blocks on them under manual progress, their wait objects have a documented race (OFI §5, §8), and binding kills optimized RMA on the MR. So CXI is **C/?** (ADV B1).

**Thread level.** SOS blocks on target counters only in THREAD_SINGLE (P4 §5; OFI §5), and the reason is not recorded (§7.2 C6).

**Fallback.** Poll with backoff plus `oc_progress`, as OSSS-UCX, OSHMPI, GPI-2 and NVSHMEM do (UCX §8.2; MPI §8.4; GPI §B.1; NV §8b.5).

### 2.8 Resource model

**Context cost.** A context owns:
- a transmit queue or a share of one;
- a completion source;
- 2–3 counters;
- a lock unless `PRIVATE` or `SERIALIZED`;
- fence-window state and a dirty set.

It owns **no per-peer connection or key**. Per-substrate hardware:

| Substrate | Per-context hardware | Source |
|---|---|---|
| CXI | endpoint; ≤128 EP, 32 CQ, **16 counters** per domain | OFI §2, §6 |
| SOS Portals | 3 MD + 2 CT, ≈340 at requested limits, no HW isolation | P4 §6 |
| UCX | worker, ≈32 MB RX rings per interface | UCX §6 |
| IBGDA DCI | ≈394 KiB | NV §3.2 |
| uTofu | VCQ, 432 per node | VEN §B6, doc-excerpt |
| GPI-2 | queue, ≤16 | GPI §B.6 |
| Aries | per-node budget | VEN §A6 |
| MPI | none; OSHMPI refuses `ctx_create` | MPI §6 |

`shmem_ctx_create` may fail without error (SPEC §9.5.1), so the OpenSHMEM layer falls back to a shared context. `SERIALIZED` (handed between threads) and `PRIVATE` (one thread) are distinct flags; `PRIVATE` permits thread-local doorbells (ADV A10; VEN §A8.4).

**Per-peer state is node-shared** (ADV C1).
- The address cache and key/base tables live in node-shared read-mostly memory. They are filled by batched `lookup` calls or computed algorithmically:
  - Portals `PTL_IGNORED` / runtime-owned map (P4 §3);
  - UET FA table + PIDonFEP (FUT §A.3);
  - DMAPP (VEN §A2).
- `lookup` must be O(1) and local, or batchable. Per-process lazy RPC lookups would total N² at job scale (ADV C2).
- For all-to-all codes (ISx, Graph500, alltoall), laziness saves nothing at steady state, so the budget is quoted **per node**.

**Per-node budget** (*arith*):

| Item | Bytes per peer per node | Source |
|---|---|---|
| Address | 8 (Portals map, uTofu VCQ ID) / ~36–40 (UET AV) / ~90 (CXI AV, estimate) | P4 §3; VEN §B3; FUT §A.3; OFI §3 |
| Keys + bases | 0 (symmetric keys) / 32 (2 segments × 16) | OFI §2 |
| Connection or reliability state | 0 at the API level on RUD/SRD/DC; **unquantified for CXI AMOs** | FUT §A.3; VEN §C3; UCX §3; OFI §3 |

At 10^6 PEs, 40 + 32 B gives **≈ 72 MB per node** node-shared. Process-private copies would multiply this by PPN: 9.2 GiB at 128 PPN (ADV C1).

**Per-context per-peer state** (dense fence epochs or dirty bitmaps) is ≤ 4 B × N (4 MB at 10^6), and only for contexts that choose it (§2.5).

**Designs ruled out** (from the reports):

| Design | Cost | Source |
|---|---|---|
| IBGDA RC per peer | 75 GiB GPU memory per PE at 10^5 | NV §3.2 |
| GPI-2 static topology | 10 RC QPs per peer | GPI §B.3 |
| LCI | RC QP + thread domain per peer per endpoint | LCI §C.3 |
| uGNI SMSG | ≈109 MB per process at 10^5 | VEN §A3 |
| Open MPI/UCX | 50–100 MB per context | UCX §3 |
| MPICH OFI | 24 B × N per window | MPI §2 |

**Laziness impossible.**
- efa-direct: the target must pre-insert every initiator (OFI §3; VEN §C3).
- Device-initiated paths: a GPU kernel cannot call `lookup`, so `OC_CAP_DEVICE_INITIATED` requires eager fill (NV §6–7; ADV C2).

**Registrations.** ≤ `max_segs_per_node / PPN` per process, e.g. CXI's 2k optimized MRs (OFI §2) and UET's 12-bit optimized MR index (FUT §A.2) (ADV C5).

### 2.9 Errors

- `OC_EAGAIN` is returned only with `OC_F_TRY` or from `ctx_create`.
- An asynchronous failure bumps `failed`. A failed op's outcome is **unknown**: an `rmw` may or may not have executed. The core guarantees that nothing covered by an `oc_ctx_wait` returning `OC_EIO` is applied afterwards (ADV D3).
- `OC_EIO` from `oc_ctx_wait` may reflect another thread's op on a shared context. The OpenSHMEM layer aborts on it, and this is documented behaviour.
- An error-record queue is open (§7.2 C7). Portals counters lose which op failed (P4 §8b.8).

---

## 3. Mapping tables

### 3(a) Core primitive → OpenSHMEM routines

Versions follow Annex G (SEM §1).

| Core | Routines (version) |
|---|---|
| `oc_put` | `[ctx_]TYPE_put`, `put{8..128}`, `putmem` (≤1.3; ctx 1.4; team-relative 1.5); `iput` (≤1.3) / `ibput` (1.6) loops; data of `put_signal` (1.5); collective data |
| `oc_put(ASYNC)` | `put_nbi`, `putmem_nbi` (1.3); `put_signal_nbi` data (1.5) |
| `oc_put_word` | `TYPE_p`; 1-element typed puts; pSync scalars |
| `oc_get` / `ASYNC` | `TYPE_get`, `getmem`, `get{8..128}`; `get_nbi` (1.3); `iget`/`ibget` loops |
| `oc_get_word` | `TYPE_g` |
| `rmw WRITE` [fetch] | `atomic_set` (1.4; `set` older), `atomic_swap`, `swap_nbi` (1.5); `signal_set` (1.6); `put_signal` + `SHMEM_SIGNAL_SET` (1.5) |
| `rmw READ` | `atomic_fetch` (1.4), `fetch_nbi` (1.5) |
| `rmw CSWAP` | `atomic_compare_swap` (1.4; `cswap`), `_nbi` (1.5); locks |
| `rmw ADD` [fetch] | `atomic_{inc,add,fetch_inc,fetch_add}` (1.4; `inc/add/finc/fadd` deprecated 1.4), `_nbi` (1.5); `signal_add` (1.6); `SHMEM_SIGNAL_ADD`; integer SUM reductions |
| `rmw AND/OR/XOR` | `atomic_{and,or,xor,fetch_*}` (1.4), `_nbi` (1.5) |
| `rmw MSWAP` | lock internals (SOS queue lock) |
| `OC_F_FENCE` on `rmw` | signal half of `put_signal[_nbi]` (1.5) |
| `oc_ctx_fence(0 / STORES)` | `[ctx_]fence` (per-ctx per 1.6 erratum); `STORES` unless `SHMEM_CTX_NOSTORE` |
| `oc_ctx_wait(ALL)` | `[ctx_]quiet`; `barrier_all` first half; `ctx_destroy` (1.4); allocation implicit barrier; `finalize` |
| `oc_ctx_wait_pe` | `[ctx_]pe_quiet` (1.6) |
| `oc_notify_*` + `oc_seg_sync` | `wait_until` (C11 generic 1.4), deprecated `wait` / `short` waits, `signal_wait_until`, `wait_until_{all,any,some}[_vector]` (1.5), `test*` (sync only), `signal_fetch` (1.5), lock waits, pSync rendezvous |
| `oc_seg_reserve` / `set_range` | `shmem_init[_thread]` (1.4); `malloc_with_hints` hints (1.5) choose ranges |
| `oc_seg_peer_ptr` | `shmem_ptr`, `team_ptr` (1.6), `SHMEM_TEAM_SHARED` (1.5) |
| `oc_ctx_create` + flags | `ctx_create` (1.4), `team_create_ctx` (1.5), `SERIALIZED` / `PRIVATE` / `NOSTORE` |
| `oc_dom_open(thread_level)` | `init_thread` / `query_thread` (1.4) |
| `OC_F_MORE` | `ctx_session_start/stop` with BATCH (1.6) |
| `rt->abort` + `oc_dom_interrupt` | `global_exit` |
| `oc_dom_close` / reopen | multiple init/finalize, `query_initialized` (1.6) |
| above core only | teams (1.5); all collectives incl. active-set (deprecated 1.5, still required 1.6) and scans (1.6); `info_*`; `*_accessible`; allocator logic; `sync` / `team_sync` |

### 3(b) Routine family → fast / derived / slow, with usage evidence

Classes:
- **fast**: 1:1 onto a core primitive.
- **derived**: a few core calls or O(1) local work.
- **slow**: deliberately software, collective, or looped.

Evidence is from USE §1–3 and Appendix A. **thin** = weak evidence, per USE's own caveats or mine.

| Family | Class | Usage evidence |
|---|---|---|
| init / finalize / info / query | slow | universal, COLD; `query_thread` tests only |
| `my_pe` / `n_pes` | derived (cached read) | universal; lexically HOT in bale index maths |
| blocking typed put/get, `putmem`/`getmem` | **fast** | strongly used: NPB halos, convey, stencil, OSU |
| scalar `p` / `g` | **fast** (`*_word`) | **hottest**: Kokkos 212 wrapper sites, GUPs, bale `lgp_*`, SSCA1 |
| `put_nbi` / `get_nbi` | **fast** + derived completion | OSU, convey, hclib; DeepEP per-token GPU put |
| `iput` / `iget` | slow | **no app users** (thin) |
| `ibput` / `ibget` (1.6) | derived (dst = bsize) / slow | none; expected for 1.6 (thin; USE §4.5 warns against demoting on usage alone) |
| AMO add / inc / fetch_add / fetch_inc | **fast** | bale, Kokkos, GUPs, ISx, PRK, hclib 81; DeepEP `amo_nonfetch_add` |
| AMO compare_swap | **fast** | Graph500 (MIN/OR emulation per edge), Kokkos, bale |
| AMO set / swap / fetch | **fast** | Kokkos, OSU |
| AMO and/or/xor | fast if native, else derived (READ+CSWAP loop) | hclib 4 + tests (thin) |
| AMO `_nbi` (1.5) | **fast** + derived | no app users seen (thin) |
| `put_signal[_nbi]` (1.5) | **derived** (put + fenced rmw) | 0 CPU apps; **HOT in NVSHMEM AI codes** (pplx per token, Triton-distributed, PyTorch); hand-rolled in bale, PRK |
| `signal_add` / `signal_set` (1.6) | **fast** | NVSHMEM `signal_op` HOT; no OpenSHMEM users |
| `signal_fetch` | derived | NVSHMEM codes |
| `wait_until` / `test` (single) | derived (notify) / slow (poll) | PRK per iteration; HOT in pplx / Triton / PyTorch (NVSHMEM dialect) |
| `signal_wait_until` | derived | Triton 45, pplx, PyTorch |
| `wait_until_{all,any,some}[_vector]`, `test_*` (1.5) | slow | **no users** |
| `fence` | derived (O(1)) | PRK after each halo put; bale; Triton 19 |
| `quiet` | derived | very wide; **after every op** in Graph500, GUPs |
| `pe_quiet` (1.6) | derived / slow | **no users**, no tests |
| contexts + options | slow create, derived options | threaded GUPs / Graph500, hclib, SOS perf (moderate) |
| team split / destroy | slow | split_strided light (SOS perf, DeepEP); `split_2d` none |
| team queries / translate | derived (affine; SHARED table) | node team for GPU selection (pplx, PyTorch); `get_config` none |
| `team_create_ctx` / `ctx_get_team` | slow / derived | none (thin) |
| `shmem_ptr` | derived | bale convey intra-node; GPU `mc_ptr` |
| `team_ptr` (1.6) | derived | none |
| `addr` / `pe_accessible` | slow (local) | Graph500 asserts only (thin) |
| locks | slow (CSWAP / WRITE / MSWAP + notify + quiet) | hclib UTS 52 sites, WARM |
| malloc / free / align | slow (allocator + barrier) | universal, COLD |
| realloc / calloc / malloc_with_hints | slow | realloc PRK only; calloc, hints none (thin) |
| barrier_all / sync_all / team_sync | derived quiet + slow sync | most-called collective; 2 per collective in bale; Triton in kernels |
| broadcast | slow | NPB, seeds; COLD |
| reduce (`*_to_all`, team 1.5) | slow; integer 1-element via rmw SUM | NPB per iteration, Graph500 per level; **CPU codes still use deprecated pSync `*_to_all`** |
| collect / fcollect | slow | ISx stats, OSU; COLD |
| alltoall | slow | PRK transpose; modest |
| alltoalls | slow | **none** |
| scans (1.6) | slow | **none** |
| active-set collectives | slow (cached strided team) | still what most CPU apps call |
| sessions (1.6) | derived (`OC_F_MORE` or no-op) | **none, no tests** |
| global_exit | slow | error paths |

---

## 4. Network feasibility matrix

### 4.1 Legend

**Cell codes**
- **N** = native
- **C** = cheaply emulated (host-local work, no extra RTT on the common path)
- **E** = expensively emulated (RTTs, target CPU, O(N) state, or spin)
- **U** = unsupported
- **?** = unverified or disputed

**Columns**
- **A** put/get
- **B** word inline
- **C** keyless segments
- **D** required `rmw`, exactly once
- **E** optional `rmw` ops
- **F** fence
- **G** counter completion + wait
- **H** notify (blocking)
- **I** per-peer state small (node-shared possible)
- **J** cheap context
- **K** no target CPU for A and D

**Source tags**: o = OFI, u = UCX, p = P4, g = GEX/GPI/LCI, m = MPI, n = NV, v = VEN, f = FUT, s = SEM.

### 4.2 Matrix

Changes from revision 1 are marked †.

| Network | A | B | C | D | E | F | G | H | I | J | K |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **libfabric cxi** | N o1.2 | N o1.1 | N o2 (client keys 0–99; whole-reservation needs pin/ODP o9)† | N o1.2 (1 elem; READ via fetch-SUM(0) o8) | N o1.2 | N o1.2,s5.2 (domain cap; **loses PCIe RO for all RMA**)† | N o5 (**≤16 counters/domain** o2)† | **C/?** counter exists, no shipping blocking use under manual progress, kills optimized RMA on MR o2,o5,o8† | **?** AV ~90 B est. + **unquantified per-peer AMO connection state** o3† | C ≤ min(128 EP, 32 CQ, 16 cntr / k) o2,o6† | N o5 |
| **verbs;ofi_rxm** | N o1.2 | C | C o2 (16 B/seg/peer) | E o1.2 (rxm target CPU) | E | E o4 (ATOMIC drops RMA order) | C o1.2 | E o1.2 | E o3 (RC per peer on demand) | E o3,o6 | N A / E D o5 |
| **efa** (`efa` / `efa-direct`) | N Nitro v4+ / E else v1,o1.2 | C v1 | C o2,v2 | E o1.2,v1; U efa-direct | E | C s5.2 (strict-ordering MR); no FI_FENCE o1.2 | N/C o1.2 | **?** o1.2 vs v1 disagree | E v3 (target pre-inserts initiators; handshake) | C v6 | E o5,v5 |
| **opx** | E o1.2 (onload *recalled* in o) | C | N o2 (SCALABLE only) | E (*recalled* in o) | E | ? o1.2 | C o1.2 | E o1.2 | C o3 (replay state per active flow) | C o6 (SEP) | E o5 |
| **UCX (mlx5 RC/DC)** | N u1 | N u1 | C u2 (per-PE rkeys; N with GVA + symmetric rkey, mlx5 opt-in) | **N if heap atomic-registered** (rc/dc_mlx5), else E amo_sw u1,s4.4† | N bitwise; E MSWAP u1 | C u4 (`worker_fence`; OMPI per-peer dummy gets u8.5) | C u5 | E u5,u8.2 | C u3 (DC; impls create eps eagerly) | E u6 | N offload / E amo_sw, UD/DC timers u5,u9 |
| **Portals 4 spec / BXI**† | N p1 | N p8a (volatile size limit) | N p2 (needs BIND_INACCESSIBLE + NIC translation) | N p1 (sync unless coherent) | N p1 | **E** p8b.3 (fence = quiet); N only with TDO + small p4† | N p5 | N p5 (target CT, counts all ops) | **C** p3 (8 B × N map via `PtlSetMap` unless `PTL_IGNORED`)† | C p6 | N p5 (BXI UBCL passive-lock caveat; BXI HW ?) |
| **Portals 4 reference impl.**† | C/E p5 (software target thread, *inference* in p) | C p9 (volatile 8 B) | C p2 (MR cache, ummunotify) | E p5,p9 (software, *inference*) | E | E p9 (waw ordered 8 B) | C p5 | C p5 (software CT) | **E** p3 (eager `conn_t` per rank) | C p6 | **E** p5,p9 (progress + event threads) |
| **GASNet-EX** (ibv/ucx/ofi) | N g-A.1 | N g-A.1 | **E** g-A.2 (rkey + base per peer; offset addressing unimplemented)† | C (ucx AD) / C–E (ibv I64 FADD/FCAS) / E (AM) g-A.9; per-AD scope g-A.1 | N ucx / E ibv | E g-A.4 | C g-A.1 | E g-A.1 | E default g-A.3 | C g-A.6 (cost *recalled*) | N RDMA / E AM g-A.5 |
| **GPI-2** | N g-B.1 | C | E g-B.2 | E g-B.1 (u64 FADD/CAS, global lock) | E | C g-B.4 (RC per queue per peer) | C g-B.5 | E g-B.1 | E g-B.3 | E g-B.6 | N g-B.5 |
| **LCI** | N g-C.1 | C | C g-C.2 | E g-C.1 (none) | E | ? g-C.4 | N g-C.5 | C g-C.1 (remote completion; progress) | E g-C.3 | E g-C.6 | N RMA / E AM |
| **MPI-3 RMA** | C m1,m9 | C | C/E m2 | E m8.2 | E | E m4 | C m5,m9 | E m8.4 | E m2–3 | U m6 | C/E m5 |
| **NVSHMEM IBGDA** | N n1,n3.2 | C n8b.2 | E n2 (key × chunk × peer × NIC) | N n1,s4.4 (masked CS/FA; API lacks int64 add n1) | C (*inference*) | N? n4,s5.2 (FM bits undocumented) | C n4–5 | E n8b.5 | E RC / **C DC, latency E under all-to-all (DCI renegotiation)** n3.2† | E RC / C DC n6; CST off on multi-NIC n8† | N n5 |
| **uGNI / DMAPP** (historical) | N v-A1 | N v-A1 | N v-A2 (DMAPP) | N v-A1 (64-bit; 32-bit emulated v-A8.5) | N v-A1 (MSWAP via AFAX *recalled*) | C v-A4 (NO_ADAPT / IN_ORDER) | N v-A1 | C v-A1 (POST_CQWRITE) | N RMA / E SMSG v-A3 | C v-A6 | N v-A5 |
| **uTofu** | N v-B1 (excerpt) | C (*recalled*) | C v-B2 (STAG *recalled*; STADD per VCQ) | **N?** (4/8-B ARMW excerpt; op list *recalled*)† | ? | ? | C v-B5 | ? (*recalled*) | N v-B3 | E v-B6 (432 VCQ/node) | N v-B5 |
| **UEC HPC profile** (spec only) | N f-A.1 | N f-A.2 | N f-A.2 (user keys SHOULD; pin/ODP ?)† | N f-A.1 (RUD, never RUDI) | N f-A.1 | N f-A.4 (mandatory; **single-source**) | N f-A.5 | N f-A.5 (not on IDEMPOTENT_SAFE MR) | N/C f-A.3 (AV ~36–40 B unless algorithmic) | ? f-A.6 | N if HW-offloaded f-A.5 |
| **CXL shared memory** (≤4096 nodes) | N f-B.1 | N | N f-B.2 | E f-B.4 (HWcc absent on prototypes) | E | N/C f-B.1,B.4 | N | E f-B.1 (no cross-host wake) | C f-B.3 | N f-B.6 | N f-B.5 |
| **osoe AM over TCP** (reference; osoe §§2–4)† | E (AM handler, target progress) | C | N (region, offset) osoe §4.2 | C (CPU atomic in handler; coherent by construction) osoe §4.9 | C | N (single ordered stream) osoe §3.1 | C (segment acks + dirty set) osoe §4.6 | C (watch table, all writes) osoe §4.9 | N per node pair osoe §3.1 | N (sequence space) osoe §4.6 | **E always** osoe §3.12 |

### 4.3 Profile notes

- **UEC.** AI Base lacks read and fetching atomics; AI Full lacks compare_atomic. On those profiles column D and gets become E (FUT §A.1).
- **efa-direct** lacks FI_ATOMIC, so SOS cannot use it (OFI §9; VEN §C8).
- **Remaining disagreement.** EFA remote counters: OFI §1.2 vs VEN §C1 vs SEM §4.6.

---

## 5. Substrates: strengths and weaknesses as a basis for OpenSHMEM

- **libfabric + cxi.**
  - *Strengths:* native AMOs, hardware fence, hardware counters, client keys, connectionless AV (OFI §0, §1.2). HPE's OpenSHMEMX is OFI-only (VEN §A8.9).
  - *Weaknesses:* ≤128 EP / 32 CQ / 16 counters per domain; the fence capability costs PCIe RO for all RMA; counter binding kills optimized RMA; unquantified per-peer AMO connection state; manual progress; one vendor (OFI §2–9).
- **libfabric generic (verbs;rxm, efa, opx).**
  - *Strengths:* one API; SOS shows a thin counter-driven transport is enough (OFI §8).
  - *Weaknesses:* `FI_ATOMIC` may mean hardware or target-CPU emulation; MR modes, progress and ordering diverge per provider; SOS needs per-provider configure flags (OFI §10). Capability bits exist to surface exactly this.
- **UCX.**
  - *Strengths:* best mlx5 coverage (DC, DevX atomics, symmetric rkeys, GVA); a GPU device API with put + counter_inc (UCX §1–2).
  - *Weaknesses:* rkeys bound to workers, so unpacks multiply with contexts; no lazy endpoints; large per-worker RX rings; `amo_sw` needs target progress; no CXI transport; Open MPI oshmem leaves 1.5/1.6 features unimplemented (UCX §9–10).
- **Portals 4 / BXI.**
  - *Strengths:* best semantic fit — keyless LEs, native AMOs incl. MSWAP, counting events, target CTs, unordered by default. SOS transport is ~2.2 kLOC with near-zero overhead (P4 §8–10).
  - *Weaknesses:*
    - fence = quiet without TDO;
    - volatile and atomic size limits;
    - contexts without hardware isolation;
    - the reference implementation needs progress threads, eager per-rank connections and ummunotify;
    - BXI software is proprietary and MPI-first;
    - SOS Portals CI is disabled (P4 §3–10).
- **GASNet-EX.**
  - *Strengths:* the core/extended split itself; `lc_opt`; `IMMEDIATE`; lazy segment queries; atomic domains that choose NIC or CPU coherently (GEX §A.0–A.2).
  - *Weaknesses:* an AM-centric core; offset addressing unimplemented; all-to-all ibv connections by default; narrow atomic offload; minimal collectives; beta spec; conduit churn (GEX §A.9–A.10). A good model, a poor passive-target base.
- **GPI-2.**
  - *Strengths:* `(segment, offset)` RMA, queues with backpressure, `write_notify` (GPI §B.1).
  - *Weaknesses:* 10 RC QPs per peer by default, per-rank segment tables, ≤16 queues, globally locked atomics, busy-poll notify, GPL-3, no OpenSHMEM port (GPI §B.3–B.9).
- **LCI.**
  - *Strengths:* rich completion objects, remote completion, explicit progress, many-thread design (LCI §C.5–C.6).
  - *Weaknesses:* no atomics; eager RC QP + thread domain per peer; beta; no PGAS clients (LCI §C).
- **MPI-3 RMA.**
  - *Strengths:* everywhere; lock_all passive epoch; near-SOS latency after MPICH fixes (MPI §5, §9).
  - *Weaknesses:* mixed-op atomics become AM; no endpoints, so no contexts; collective O(N) windows; O(N) sync loops; no notify; MPICH-specific hints (MPI §8–10). Portability fallback only.
- **NVSHMEM (IBGDA).**
  - *Strengths:* proves GPU-initiated put + NIC atomics + fence bits at zero library overhead; carries the hottest put-with-signal users (NV §1; USE §2.9–2.12).
  - *Weaknesses:* RC per peer by default (75 GiB at 10^5); key tables × chunk × peer × NIC; no contexts or locks; 16-bit PE proxy fields; ConnectX-only (NV §3–10). A design reference, not a base.
- **uGNI / DMAPP.**
  - *Strengths:* closest precedent — keyless symmetric segments, 64-bit NIC AMOs, `_nbi` + gsync, passive target (VEN §A1–A5).
  - *Weaknesses:* SMSG per-peer mailboxes; order only without adaptive routing; AMO replay hazard; the whole stack was rewritten for Slingshot, a lesson against mirroring one vendor API (VEN §A9–A10).
- **uTofu.**
  - *Strengths:* hardware RMA + 4/8-B ARMW, 8 B per peer, passive target (VEN §B1–B5).
  - *Weaknesses:* hard VCQ caps; registration per VCQ; proprietary; primary guide unread, so much is *recalled* (VEN §B0).
- **UEC HPC profile.**
  - *Strengths:* the whole core on paper — user keys, fetching and compare atomics, mandatory FI_FENCE, remote counters, ephemeral PDCs, spraying on RUD (FUT §A).
  - *Weaknesses:* no products or numbers; AI profiles lack read and CAS; RUDI conflicts with counters and strict fence; AV may be O(N) (FUT §A.9).
- **CXL / OpenFAM.**
  - *Strengths:* put/get/ptr become load/store; fence trivial (FUT §B.1).
  - *Weaknesses:* ≤4096 nodes; cross-host CAS needs HWcc; no cross-host wake; failure isolation; OpenFAM `fam_map` unimplemented in the memory-server model (FUT §B.3–B.8). An intra-pod tier under `oc_seg_peer_ptr`, not a 10^5-PE core.

---

## 6. Slow paths: features relegated, and expected cost

| Relegated feature | Implementation | Expected cost (evidence) |
|---|---|---|
| `rmw` without NIC atomics | core-internal AM + CPU atomic + progress thread; bumps notify word | 2 messages + target CPU. GASNet Aries offload cut fetch-add latency 70% vs AM (GEX §A.9), so emulation ≈ 3.3× (*arith*); one progress core per PE or node |
| bitwise / MSWAP where not native | READ + CSWAP retry | ≥2 RTT, livelock-prone (UCX §8.4; SEM §4.4) |
| fence without native flag or order | drain rank (or ctx) before posting | 1 RTT of the slowest in-flight op, only if traffic follows (OFI §4; P4 §8b.3) |
| fence on multi-rail | rail affinity per fence window, or drain other rails | lost striping inside the window, or extra drains (ADV A5; NV §8) |
| store-aware fence (non-NOSTORE ctx) | drain on-node in-flight ops + memory barrier | zero with no on-node traffic in flight |
| put-with-signal without native fence | put + drain + rmw | +1 ACK RTT per call (UCX §8.1; SEM §4.5) |
| visibility at quiet | dirty-set flush per peer | O(dirty peers) flush RTTs; N after an all-to-all; free with `TXCOMP_VISIBLE` (ADV C3; UCX §8.5) |
| `wait_until` outside cheap notify ranges | poll with backoff + `oc_progress` | a core per waiter (MPI §9; UCX §9.7; GPI §B.9) |
| multi-element / vector wait and test | loops | no users (USE §3) |
| `pe_quiet` | `quiet` | over-wait; spec allows (SPEC L9114) |
| `iput` / `iget` / `ibput` / `ibget` | async put/get loops | nblocks headers (SEM §4.3) |
| teams | collective split; affine translation; SHARED table | O(log N) per split (≈17 levels at 10^5, SEM §4.12) |
| active-set collectives | cached strided teams | a team creation on first use (MPI §8.12) |
| barrier / sync | quiet + tree over put + notify | O(log N); frequent (USE §1) |
| reduce | integer 1-element via rmw SUM or recursive doubling; float / complex / PROD / MIN / MAX in software | software everywhere today (OFI §8; P4 §8a) |
| alltoall[s], collect, fcollect, scans | strip-mined nb puts; two-phase sizes | O(N²) data (VEN §A8.2) |
| locks | SOS queue lock | ≥3 AMOs + wait per contended acquire (OFI §8; MPI §9) |
| malloc / free / realloc / align / calloc / hints | allocator in one reservation; hinted sub-ranges | barrier per call; no registration; no growth past reservation (MPI §8.10) |
| context creation | may fail → shared ctx | per-node budgets (§2.8) |
| sessions | `OC_F_MORE` or no-op | none (SEM §4.14) |
| global_exit, init/finalize, accessibility queries | runtime / local | cold |
| RMW to on-node peer, non-coherent NIC | NIC loopback | 1 RTT (NV §4) |
| coherent read, non-coherent NIC | `oc_seg_sync` | implementation-defined (P4 §8b.9) |
| GPU memory without device-initiated support | CPU proxy thread | ~1.7 vs ~180 MOPS (NV §7) |
| efa first contact | handshake | 1 RTT + target progress (VEN §C3) |

---

## 7. Contested points and open questions

### 7.1 Adversary round

`q2-adversary.md` raised 30 attacks. I checked each against the spec text and the evidence reports.

**Verdict counts**

| Verdict | Count |
|---|---|
| Accepted | 23 |
| Partially accepted | 6 |
| Rejected | 1 (in part: A6(2)) |

**Largest changes**
- Notify now counts every `rmw` path and puts can be counted too (A2).
- The fence gate is placement at the target, across all rails, and fails when a predecessor fails (A5, D2, D3).
- Blocking gets complete per operation (A4).
- Attributes apply to ranges, not whole segments (A1, A7, A8).
- Per-peer state is node-shared (C1, C2).
- Visibility uses a dirty set (C3).
- Matrix corrections.

| # | Attack (rating) | Verdict | Verification and change |
|---|---|---|---|
| A1 | wait targets outside NOTIFY segments always poll (sig.) | **Accepted** | SPEC L653–654 and L5560–5562 checked: statics may be signals. PRK wait targets come from unhinted `shmem_align` (ADV, *observed in source*). → `NOTIFY` is a range attribute, default on the whole heap and data segment where `OC_CAP_NOTIFY_CHEAP`; the §1.3 claim is narrowed. |
| A2 | rmw-only notify loses wakeups (**fatal**) | **Accepted** | SPEC L7710–7711 checked: local-thread AMOs update ivars. SOS forces polling with on-node paths (OFI §5). → The wake word is bumped by every rmw path, including self; `NOTIFY_PUTS` added; finite deadlines are mandatory (§2.7). |
| A2(3) | put-updated ivars / short waits | **Partially** | SPEC L10471–10472 makes AMO-only updates a clarification. `short` waits (SPEC L7678–7695) have no matching AMO, so the attack stands for them. → `NOTIFY_PUTS` is optional, default where cheap. |
| A3 | memory-store ordering, NOSTORE unmodelled (sig.) | **Accepted** | SPEC L8893–8896, L2967–2972, L8948 checked. → `OC_FENCE_STORES`, `OC_CTX_NOSTORE`. |
| A4 | class-wide wait for blocking get starves (sig.) | **Accepted** | Gets are unordered (SPEC L8933–8935); SOS spin-polls in MULTIPLE (OFI §5). → Blocking gets and fetching rmw complete per op. |
| A5 | put-with-signal on multi-rail (sig.) | **Accepted** | SEM §5.2 quotes FI_FENCE scope as "peer endpoint"; NV §8 disables CST on multi-NIC. → Fence scope covers all rails and software queues. |
| A6(1) | segment 0 attribute conflict (sig.) | **Accepted** | → Range attributes. |
| A6(2) | shared-library statics break offsets | **Rejected** | SPEC L603–604: symmetric statics "must not be defined in a dynamic shared object". MPMD `pe_accessible` stays a runtime query above the core. |
| A7 | fixed per-hint partitions; realloc; pinning; VA × PPN (sig.) | **Accepted** | → One reservation, hinted sub-ranges, pin/ODP and VA bound declared (§2.1). |
| A8 | UCX AMOs on unhinted heap take amo_sw (sig.) | **Partially** | UCX NEWS L649 (SEM §4.4) supports the problem. That atomic registration costs RDMA nothing is *recalled* by ADV and me, not verified. → Heap is `RMW` by default where free; UCX cell D conditional; open question C4. |
| A9 | reductions via RMW SUM; READ as ADD 0 (minor) | **Accepted** | Tables 6–8 and 10–11 checked. → Integer-only note; native READ preferred. |
| A10 | NOLOCK conflation; thread level; SOS single-thread blocking (minor) | **Accepted** (last point open) | → `SERIALIZED` / `PRIVATE` flags; `thread_level` in `oc_dom_open`; C6 open. |
| A11 | async gets need per-op state (minor) | **Accepted** | → Bound stated (§2.3). |
| A12 | global_exit unblocking; re-init (minor) | **Accepted** | SPEC L1505–1507. → `oc_dom_interrupt`; `close` → `open` legal. |
| B1 | cxi H = N overclaimed (sig.) | **Accepted** | OFI §5, §8 confirm manual progress forces polling. → C/?. |
| B2 | cxi `cntr_cnt = 16` (sig.) | **Accepted** | OFI §2 `cxip_info.c` confirms the value; enforcement unverified. → `max_cntr`; GET/RMW may share a counter. |
| B3 | cxi AMO connection state per peer (sig.) | **Accepted** | OFI §3 "connection-based reliability model ... for AMOs", size unquantified. → I = ?; open C3. |
| B4 | Portals row mixes BXI and RI; F = E vs §1.3 (sig.) | **Accepted** | P4 §3, §5, §9. → Row split; §1.3 narrowed. |
| B5 | IBGDA DC renegotiation; CST multi-NIC (minor) | **Accepted** | NV §3.2, §8. → Annotated. |
| B6 | uTofu D = N weak (minor) | **Accepted** | VEN §B0. → N?. |
| B7 | GASNet C = C (minor) | **Accepted** | GEX §A.2. → E. |
| B8 | CXI FI_FENCE is endpoint-wide (minor) | **Accepted** | SEM §5.2 `cxip_info.c` L1987–1992. → `OC_DOM_FENCE`. |
| C1 | touched = N; budget must be per node (sig.) | **Accepted** | USE §2 (ISx, Graph500). → Node-shared tables, per-node budget. |
| C2 | lazy lookup storms; device paths (sig.) | **Partially** | The N² arithmetic holds for per-process RPC lookups. It does not hold for algorithmic or node-shared maps (P4 §3; FUT §A.3), which the revision now requires. → Batch `lookup`; eager fill for device-initiated. |
| C3 | visibility needs per-peer dirty flush (sig.) | **Accepted** | UCX §8.5; osoe §4.6 uses the same structure. → Dirty set, `TXCOMP_VISIBLE`, `OC_F_VISIBLE`. |
| C4 | sparse fence epochs cost ≫ 4 B (minor→sig.) | **Accepted** | OFI §3 (UT_hash_handle ≈ 56 B). → Context-wide word or dense array; no sparse hash. |
| C5 | registrations × NIC × PPN (minor) | **Accepted** | → `max_segs_per_node`. |
| D1 | `done` ≠ visible (sig.) | **Accepted** | UCX §1. → Redefined. |
| D2 | fence gate "completed" vs "delivered" (sig.) | **Accepted** | SPEC L8931–8933. → Gate = placed at target. |
| D3 | failure semantics (sig.) | **Accepted** | → Fenced op fails if a predecessor failed; failed = outcome unknown; cross-thread EIO documented. |
| D4 | atomicity-scope contradiction (minor) | **Accepted** | SPEC L684–694. → Per segment per domain; sync before CPU rmw. |
| D5 | count, target_cpu bits, notify variance, get_word flags, zero-copy blocking put (minor) | **Accepted** | → Fixed in §1.2 and §2.2. |

**Remaining contested points from the round**
- **A8.** Is atomic registration on mlx5 free for plain RDMA? Unverified.
- **C2.** Does node-shared addressing actually exist in the runtimes (PMIx) at 10^6? Deployment-dependent, *recalled*.
- **B2.** Is CXI `cntr_cnt` a hard limit? Unverified.
- **A10 / C6.** Why does SOS refuse to block on target counters outside THREAD_SINGLE? No evidence either way.

### 7.2 Open questions

**Correctness**
- **C1 — UET RUDI.** A late duplicate can land after completion and violate fence (FUT §A.4). Guards could be TSS epochs, packet-lifetime waits, or RUD after a fence. Until resolved, puts never use RUDI.
- **C3 — CXI AMO state.** How large is CXI's per-peer AMO connection state at 10^5–10^6 peers? All-to-all AMO codes touch every peer (USE §2).
- **C7 — error reporting.** Error-record queue vs abort-on-EIO at 10^5 PEs (P4 §8b.8).

**Design choices**
- **C2 — compound call.** Compound `oc_put_rmw` vs flag. For the compound form: UCX device API, GPI `write_notify`, LCI remote completion, pplx hot path (UCX §8.1; GPI §B.1; LCI §C.1; USE §2.9). The flag form keeps the core smaller and matches IBGDA.
- **C5 — handles.** Optional per-op handles for async ops: Cray `shmemx_*_nb`, DMAPP syncid (VEN §A1, §A8.4).
- **C8 — device API.** A GPU-callable subset needs a lock-free device API. The design above is host-shaped (NV §6–7).
- **C9 — MIN/MAX rmw extension.** Graph500 emulates MIN/BOR at ≥4 RTT per edge (USE §2.1). Native on Portals, CXI, UET, but not an OpenSHMEM AMO.
- **C10 — alltoallv.** Real demand (bale, ISx, MoE libraries; USE §4.3), absent from the spec.
- **C11 — reservation size.** Default reservation size given pin/ODP and the `PPN × reservation ≤ VA` bound; GPU heaps of 256 GiB class without per-chunk keys (NV §2).

**Verification needed**
- **C4 — mlx5 atomic registration.** Does atomic registration on mlx5 cost RDMA anything (ADV A8)?
- **C6 — SOS thread level.** SOS blocks on target counters only in THREAD_SINGLE (P4 §5). Why?
- **C12 — mlx5 fence-mode bits.** Semantics undocumented (SEM §5.2; NV §4).
- **C13 — CXI visibility.** Does CXI transmit-complete imply visibility? It decides `OC_CAP_TXCOMP_VISIBLE` on CXI (OFI §4).
- **C14 — EFA remote counters.** Reports disagree (OFI §1.2; VEN §C1; SEM §4.6).
- **C15 — mlx5 atomics access path.** The reports differ. OFI §1.2 finds no IBTA atomic verbs in mlx5. GEX §A.9 says the masked verbs API is gone. UCX §1 and NV §1 use DevX/WQEs. **Reconciled reading:** masked atomics are reachable only through DevX/PRM.
- **C16 — uTofu details.** Op list, STAG addressing, ordering, MRQ notify: all *recalled* (VEN §B0).
- **C17 — spec and implementation issues.**
  - SOS and OSSS-UCX appear not to translate team-relative PEs in RMA paths (SEM §4.9, medium confidence).
  - The `signal_add`/`set` "at the calling PE" wording looks editorial (SEM §4.5).
- **C18 — minor inconsistencies between reports** (none drives the design):
  - SOS lock op naming (SEM vs OFI);
  - NV *recalled* FI_FENCE not upstream, contradicted by OFI §1.1 and FUT §A.4;
  - CXI doc vs code on ATOMIC_WAR/RAW (OFI §4);
  - Portals RI `waw` 8 vs spec minimum 64 (P4 §4);
  - NVSHMEM docs vs code on the DC/RC default (NV §3.2);
  - UCX atomic API: deprecated (SEM) vs current `ucp_atomic_op_nbx` (UCX §1) vs removed (VEN, *recalled*).

---

## 8. Relation to osoe

Everything about osoe below is *documented* in `docs/design/shmem-ethernet-design.md` or `docs/reviews/design-soundness.md`.

### 8.1 osoe as a substrate for the core

osoe is an AM layer (`am_request`, `am_reply`, `am_poll`, `am_progress_start/stop`, osoe §2.1). Puts, gets, AMOs, signalled puts and collectives are all AM handlers (osoe §2.2). Viewed through the core, it is **one software substrate**, with the capabilities of the last matrix row:

- **`OC_CAP_ORDERED`.** One TCP stream per node pair, applied in arrival order. "`shmem_fence` requires no per-operation tracking" (osoe §3.1). Fence is free; the cost is head-of-line blocking, which osoe bounds with 64 KB chunks (osoe §3.2).
- **Offset addressing.** `(region id, offset)` on the wire (osoe §4.2), exactly as in §2.1.
- **`rmw`.** A CPU atomic executed by the handler on the shared mapping. This is `OC_CAP_RMW_CPU_COHERENT` by construction (osoe §4.9).
- **Notify = the §4.9 watch table.** A 32-bit sequence word per watched address, bumped by the progress thread *or a local PE* on every heap write. This is already the revised §2.7 semantics (all paths, puts included, finite deadline, count timeout wakes), and finer-grained: per address, not per range. ADV A2 is exactly the bug osoe's design avoids.
- **Completion.**
  - Segment acks plus a **per-PE dirty set of nodes**, and `bytes_issued` / `bytes_landed` for gets (osoe §4.6). This is the §2.6 dirty-set visibility model, with visibility equal to "applied at target".
  - `pe_quiet` is the dirty set restricted to the listed peers, as `oc_ctx_wait_pe`.
- **`target_cpu` = everything.** A per-node progress pool, "not one thread per PE" (osoe §3.12). osoe is the case where the core's `OC_DOM_PROGRESS_THREAD` is a per-node daemon.
- **Per-peer state** scales with nodes (osoe §1, §3.1), matching §2.8's node-shared budget.

### 8.2 Where osoe is not a special case

1. **The lower interface is more general than the core.** Its handler table is fixed at init (osoe §3.13). The core deliberately has no AM. osoe's AM layer could *implement* `oc_*`, but the core could not host osoe's collectives-as-handlers.
2. **It addresses nodes, not ranks.** `am_request(ep, node, …)`. The core names world ranks. DS B2 finds that osoe's single shared heap per node violates "a symmetric heap per PE" (SPEC L1885). The fix direction (L heaps per node, watch key `(pe, region, offset)`) makes the osoe wire carry a PE index, i.e. `(rank, seg, off)` as in §2.1.
3. **Completion of gets into private memory.** DS B1: a per-node progress thread in another process cannot write the origin PE's stack. The fix direction is landing zones copied by the origin. This is §2.3's per-op completion of blocking gets and O(outstanding) records for async gets, forced by process boundaries rather than by threads.
4. **Contexts.** osoe treats a context as a sequence-number space (osoe §4.6), with no hardware cost, which the core permits. But ordering on a shared node-pair stream couples contexts and PEs for head-of-line blocking; the core does not require that coupling.

### 8.3 Suggested changes to osoe (small)

- **Adopt B2's per-PE `(pe, region, offset)` key.** The core analysis independently arrives at it.
- **Name the SHMEM↔transport boundary in core terms** (`put`, `get`, `rmw`, fence, wait, notify) inside osoe's SHMEM layer, even though the only implementation is AM. A future RDMA substrate could then replace handlers without touching §4. This costs a header, not a layer.
- **Add a store-aware fence** for non-NOSTORE contexts (ADV A3). osoe's intranode memcpy path (osoe §4.1) orders stores only by memory barriers, and §4.6 does not mention them.
- **Keep the §4.9 rules as written.** Insert before check, seq-before-ivar loads, safety-sweep deadline. They are the load-bearing parts that the core's notify primitive had to copy.

---

## 9. Sources

Consolidated from the evidence reports. Commits are as pinned there.

### Specifications and documentation

- OpenSHMEM 1.6 (1 Nov 2024), `docs/reference/OpenSHMEM-1.6.pdf` (line cites refer to its `pdftotext -layout` output)
- Portals 4.3, SAND2022-8810: https://www.sandia.gov/app/uploads/sites/144/2023/03/portals43.pdf
- Ultra Ethernet Specification v1.0.2: https://ultraethernet.org/wp-content/uploads/sites/20/2026/01/UE-Specification-1.0.2-1.pdf
- CXL 3.0 white paper: https://computeexpresslink.org/wp-content/uploads/2023/12/CXL_3.0_white-paper_FINAL.pdf
- libfabric man pages: https://ofiwg.github.io/libfabric/main/man/fi_msg.3.html, https://ofiwg.github.io/libfabric/main/man/fi_efa.7.html, https://ofiwg.github.io/libfabric/v1.9.0/man/fi_collective.3.html, https://ofiwg.github.io/libfabric/v1.15.2/man/fi_gni.7.html
- UCX FAQ: https://openucx.readthedocs.io/en/master/faq.html
- NVSHMEM docs:
  - https://docs.nvidia.com/nvshmem/api/latest/api/overview.html
  - https://docs.nvidia.com/nvshmem/api/latest/gen/mem-model.html
  - https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/performance.html
  - https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/unsupported-operations.html
- NVIDIA IBGDA blog: https://developer.nvidia.com/blog/improving-network-performance-of-hpc-systems-using-nvidia-magnum-io-nvshmem-and-gpudirect-async/
- AWS EFA: https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/efa.html; SRD: https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/efa/SRD.txt
- Cray OpenSHMEMX: https://github.com/PE-Cray/cray-openshmemx/blob/master/doc/source/intro_shmem.rst
- Fujitsu uTofu guide (not read in full): https://software.fujitsu.com/jp/manual/manualfiles/m210007/j2ul2482/02enz003/j2ul-2482-02enz0.pdf

### Source code (pinned)

- SOS: https://github.com/Sandia-OpenSHMEM/SOS/blob/4aa91d7fa9041785bd367fc9cefab99d313dd9df/
- libfabric: https://github.com/ofiwg/libfabric/blob/0d5527ae694af929014da0385d409f638e81105e/ (future report: 056b8ae)
- rdma-core: https://github.com/linux-rdma/rdma-core/blob/00357df3a344b3515a9a4d345cf0b8c8efb7f116/
- UCX: https://github.com/openucx/ucx/blob/9c85cdf1425441e4820e9d2ead9534291b5802f0/
- Open MPI: https://github.com/open-mpi/ompi/blob/8caa67e74b7bb3144b4a995156bd6102baa44dcb/; v4.1.6: https://github.com/open-mpi/ompi/blob/v4.1.6/
- OSSS-UCX: https://github.com/openshmem-org/osss-ucx/blob/cbbb91d4a6eb1f6ecd8ccd77b35c174afd2e1040/
- OSHMPI: https://github.com/pmodels/oshmpi/blob/ee5cf110e673c098707257bb025404e17ac0a5fc/
- MPICH v5.0.1: https://github.com/pmodels/mpich/blob/v5.0.1/
- NVSHMEM: https://github.com/NVIDIA/nvshmem/blob/b0d9d3dc08fc3ee0840fdb6f3a2c11932d85a2e2/
- Portals 4 RI: https://github.com/Portals4/portals4/tree/4e787c1330d956f9d924b7aed21094eb40dfd83d
- GASNet-EX: https://github.com/BerkeleyLab/gasnet @19143e8b1b8965727555194110beeb81a57080b5
- GPI-2: https://github.com/cc-hpc-itwm/GPI-2 @3f5dbc68ef8c2eb89f92aa2514c49be6f1865aa3
- LCI: https://github.com/uiuc-hpc/lci @663de28b9287eaae1313ccec826c0ca2ca9e0c2b
- OpenSHMEM RI: https://github.com/openshmem-org/openshmem @5f91637b27690c148cbf81523f2bc0b0fb7829b4
- OpenFAM: https://github.com/OpenFAM/OpenFAM @2dfb164; API: https://github.com/OpenFAM/API/blob/master/fam.h
- RIKEN QWS uTofu: https://github.com/RIKEN-LQCD/qws/blob/master/rdma_utofu_comlib.h
- UET providers: https://github.com/ultraethernet/uet-ref-prov, https://github.com/spcl/suet

### Usage corpus (USE §0)

- OSU OMB 7.5: https://mvapich.cse.ohio-state.edu/download/mvapich/osu-micro-benchmarks-7.5.tar.gz
- OSB: https://github.com/ornl-languages/osb @39cfcadd
- NPB: https://github.com/openshmem-org/openshmem-npbs @0028e8d0
- ISx: https://github.com/ParRes/ISx @d31936a6
- PRK: https://github.com/ParRes/Kernels @25051e46. ADV adds: `SHMEM/Stencil/stencil.c`, `SHMEM/Synch_p2p/p2p.c`.
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
- ORNL Graph500 SHMEM paper: https://www.osti.gov/servlets/purl/1302887

### Papers and articles

- GASNet-EX, LCPC'18: https://gasnet.lbl.gov/pubs/gasnet-ex-lcpc18-tech.pdf
- OpenSHMEM over MPI (2021): https://minsii.github.io/assets/pdf/openshmem2021-mpi.pdf
- OpenSHMEM over MPI-3 (2014): https://www.csm.ornl.gov/workshops/openshmem2013/documents/ImplementingOpenSHMEM%20UsingMPI-3.pdf
- OSHMPI SC19 BoF: https://www.mcs.anl.gov/~minsi/pdf/oshmpi-shmem-bof-201911.pdf
- DMAPP, CUG 2010: https://cug.org/5-publications/proceedings_attendee_lists/CUG10CD/pages/1-program/final_program/CUG10_Proceedings/pages/authors/01-5Monday/03B-tenBruggencate-Paper-2.pdf
- uGNI MPICH2, CUG 2011: https://cug.org/5-publications/proceedings_attendee_lists/CUG11CD/pages/1-program/final_program/Wednesday/13C-Pritchard-Paper.pdf
- Open MPI on Cray XE, HOTI 2012: https://www.cs.unm.edu/~samuel/publications/2012/perf-eval-ompi-cray-xexk.pdf
- SRD, IEEE Micro 2020: https://ieeexplore.ieee.org/document/9167399
- UCX on Tofu-D: https://link.springer.com/article/10.1007/s11227-024-06201-x, https://www.researchgate.net/publication/367563945
- Portals 4 Exascale, OFA 2012: https://downloads.openfabrics.org/Media/Monterey_2012/2012_Workshop_Mon_Portals_4_Exascale.pdf
- NET4EXA: https://arxiv.org/pdf/2601.19413
- SeaStar Portals, CUG 2006: https://www.sandia.gov/app/uploads/sites/210/2022/11/p3-accel-cug06.pdf
- Hardware counters / triggered ops: https://dl.acm.org/doi/abs/10.1145/3343211.3343222, https://link.springer.com/chapter/10.1007/978-3-642-15646-5_26
- OpenSHMEM contexts over OFI: https://link.springer.com/chapter/10.1007/978-3-319-73814-7_2
- Cxlalloc, ASPLOS '26: https://www.cs.utexas.edu/~witchel/pubs/ni26asplos-cxlalloc.pdf
- OpenFAM, CUG 2022: https://cug.org/proceedings/cug2022_proceedings/includes/files/pap102s2-file1.pdf
- "Demystifying NVSHMEM", arXiv 2606.05951; "GPU-Initiated Networking for NCCL", arXiv 2511.15076
- Slingshot, SC20: https://arxiv.org/pdf/2008.08886
- LINKx: https://www.osti.gov/servlets/purl/2438730
- OpenSHMEM-UCX on Cray XK: https://www.osti.gov/servlets/purl/1265689
- ALCF Theta MPI: https://www.alcf.anl.gov/sites/default/files/2020-05/Chunduri_MPI_Theta_CPW_May_5_2020_final.pdf
- BXI (journalism): https://www.nextplatform.com/2024/12/10/eviden-mainstreams-bxi-interconnect-thanks-to-ultra-ethernet-and-ai-boom/
- Open MPI UBCL: https://github.com/open-mpi/ompi/tree/main/ompi/mca/osc/ubcl
- Issues: https://github.com/open-mpi/ompi/issues/12702, https://github.com/open-mpi/ompi/issues/9003, https://github.com/openucx/ucx/issues/2408, https://github.com/openucx/ucx/issues/3731
- Sandia Portals projects: https://www.sandia.gov/portals/projects-using-portals-4-0/
- SOS tuning wiki: https://github.com/Sandia-OpenSHMEM/SOS/wiki/Performance-Tuning
- LWN UET RFC: https://lwn.net/Articles/1013363/
