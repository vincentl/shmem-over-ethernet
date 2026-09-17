# Q1 — Collectives and teams review (design §3.8 bound, §5, §9 items 9/10/16/17)

Reviewer role: collectives and teams. Design: `docs/design/shmem-ethernet-design.md`.
Spec: OpenSHMEM 1.6, from `scratchpad/spec16.txt` (the pdftotext output); spec line numbers are given as `L1234`.
Model: `scratchpad/exp/collectives/model.py` (exhaustive BFS model), `coverage.py`, and the output files `out-*.txt` (all output is also at the end of this report).

## Summary

| # | Finding | Severity | Confidence |
|---|---|---|---|
| F1 | Epoch counters, `wait_eq`, the `!is_lead` guard, wake counts and the barrier "whichever increment hits target" rule are **sound**. Exhaustive checks cover L=1,2,3; 3–4 epochs of mixed B/R; 1, 2 and 3 nodes; small moduli; with and without spurious wakeups. The checker was validated with 5 mutations, 4 of which it catches | — (sound) | high |
| F2 | **Downward forward of a reduction result by reference to the lead's `dest` races with the lead leaving.** When a root or interior node forwards the result to child nodes as a §3.8 descriptor that is drained after the local `released++`, the lead can return and write `dest` for the next epoch (or the application can write it) while chunks are still being read. Counterexample found at N=2, L=(1,1), prog RR. "Lead leaves last" (inv. 16) covers local copy-outs only | design change needed | high |
| F3 | The §5.5 accumulate-into-the-lead's-`dest` pattern does not carry over to `alltoall[s]` or `sum_{in,ex}scan`, which have different per-PE results, or to `collect` with variable `nelems`. Two-level scan is also wrong when node membership is not contiguous in team rank. So §7's "mechanical given §5.5" and §5.5's "no per-team data area" are overstated | design change needed (before those collectives) | high |
| F4 | §3.8 control-queue bound `degree × max_teams` never says what `max_teams` counts. Per connection the true bound is one entry per distinct team whose tree uses that edge. `split_2d` makes up to `L × (teams per PE)` distinct teams per node | clarification needed (bound wrong by factor L if `max_teams` is per PE) | high |
| F5 | The "team range allocator" is not defined. A single `split_2d` makes ~`N/xrange + xrange` distinct teams, so a range sized by `max_teams` can be used up by one call. Also, how are ids agreed and shared counter blocks allocated during a parent-collective split? | design change needed | high |
| F6 | `shmem_team_destroy` scope is **not** really ambiguous. INVALID-handle no-op plus the spec's own examples fix the scope to team members. Leaking is also not the only safe choice: a free-mask agreement over the parent at split time reclaims ids | minor (the conclusion partly stands) | medium-high |
| F7 | Barrier vs sync (§5.6): quotes and conclusion are correct | — (sound) | high |
| F8 | Multithreaded PEs: concurrent collectives on different teams are allowed, and the per-team private `k`/`d` do not break | — (sound) | high |
| F9 | Push-not-pull quote is verbatim and correctly cited (9.10.5, 9.10.7–9.10.10; 9.10.6 inherits by reference; Annex G 1.6 change #14) | — (sound) | high |
| F10 | Smaller wording issues: inv. 16 says "only writer before release", but the progress thread writes root/interior `dest`; single-node teams need a stated local release path; `M ≥ L` is the real wraparound condition; the order "write final dest, then bump `released`" must be stated for multi-chunk results | minor / clarification | high |

---

## F1. Exhaustive model of §5.4–§5.5: what was modeled, and the result

### What is modeled (faithful to the §5.5 snippet)

Per node, one team:

- **Shared words.** `arrived`, `released` and `copied` are atomics modulo `M`. The default is `M = 2^32`; M = 16, 8, 4, 3 and 2 were also tried.
- **Ghost counters.** Unbounded ghost copies of each word detect alias and wrong-epoch matches.
- **Futex wait queues.** One wait queue per futex word.
- **Lead dest tag.** The lead's `dest` carries a tag `(epoch, stage ∈ {own, local-combined, subtree-combined, final})`.
- **Per PE (member 0 is the lead).** Program counter, private `k`, `d`, and the loaded value.
- **Sources.** Each PE's `source` carries an epoch tag, set when the PE enters the call and before its increment.

The PE's steps, each an atomic transition:

1. `source := epoch k`.
2. `r = fetch_add(arrived,1)+1` (C11 `atomic_fetch_add` returns the old value; the snippet's `+1` is modeled).
   - Barrier: if `r == (k+1)L`, enqueue the internode arrival for epoch `k`.
   - Reduction: if `r == target && !is_lead`, `futex_wake(&arrived,1)`.
3. Lead in R: `wait_eq(arrived,(k+1)L)`. Then own memcpy, then `combine(source_of(m))` for each m as separate steps, each checking that m's source tag equals `k` and m has already incremented. Then enqueue the arrival.
4. Everyone: `wait_eq(released,k+1)`.
5. Non-lead in R: copy-out in 2 chunks, each checking that the lead's `dest == (k, final)`. Then `copied++`, and `futex_wake(&copied,1)` when the new value equals `(d+1)(L−1)`.
6. Lead in R: `wait_eq(copied,(d+1)(L−1))`, then return (with a check that every non-lead has finished its copy-out).

`wait_eq` is modeled as LOAD, then WAIT. WAIT is `futex_wait(word, v)` with kernel semantics: atomically, if `word != v` it returns EAGAIN and loops back to LOAD, otherwise it enqueues on the word's queue and sleeps. `futex_wake(word, n)` nondeterministically wakes any `min(n,|q|)` sleepers; every choice is explored. A variant allows spurious wakeups of any sleeper at any time.

The internode tree is abstract, with root = node 0:

- **Upward sends.** A node's arrival goes to its parent in 2 chunks. For R, each chunk reads the lead's `dest` by reference and checks its tag.
- **When a node may send up.** Only once its local arrival is enqueued and all children's arrivals for that epoch are merged.
- **Merging.** The parent merges a child's partial only after its own local arrival is enqueued.
- **Release.** The root releases epoch e once its own arrival is enqueued and all children are merged.
- **Per-node release.** Write the final result into the lead's `dest` (R only), check `released == e`, do `released++`, then `futex_wake(released, INT_MAX)`.
- **Forwarding to children, two orders.** `fwd=before`: the node drains the forward to its children (by reference) before its own bump. `fwd=after`: the node drains after its local wake, which is what a §3.8 descriptor queue naturally does.

Checks:

- stale source read;
- stale or clobbered `dest` read (copy-out, upward and downward chunks);
- an arrival enqueued with the wrong epoch, or twice;
- `released` bumped past the expected epoch;
- a waiter matching its target while the true count differs (wraparound alias);
- the lead leaving before copy-outs finish;
- deadlock (no enabled transition and not done), with any sleeper whose predicate holds reported as a lost wakeup;
- liveness: every reachable state can still reach the all-done state. This catches livelock and starvation, including under spurious wakeups.

### Results (full output at the end)

- **1 node (loopback release), M=2^32, L=1,2,3**, programs BBB, RRR, BRB, RBR, RRB, BRR: all pass. The largest is 11,452 states (L=3 RRR).
- **Spurious wakeups**, L=2 and 3, programs RRR, BRB, RBR: all pass (up to 13,440 states).
- **Wraparound**, program RBRR (4 epochs), L=2 with M ∈ {16,8,4,2} and L=3 with M ∈ {16,8,4,3}: all pass. **L=3 with M=2 fails**: the lead sees `arrived == target` after one increment (ghost count 1, expected 3). So the precise condition is `M ≥ L` (and `M ≥ 2` for `released`). This is trivially met by 2^32, which confirms the §5.4 "wraparound is harmless" claim with the right margin.
- **2 nodes, release needs both arrivals**, L ∈ {(2,1),(2,2),(3,1),(1,3)} × programs {BRB, RBR, RRR}, fwd=before: all pass (up to 500,732 states). Also L=(2,2) RBR with M=4 and spurious wakeups: pass (295,295 states).
- **3-node chain 0←1←2**, L=(1,2,1) RRB, fwd=before: pass (959,592 states). The L=(2,1,2) BRR result is given in the appendix.
- **fwd=after**: fails for R and passes for B. This is F2.

**The checker is not vacuous.** Mutations it catches:

- the lead not waiting on `copied` → "lead leaving epoch 0 before pe1 finished copy-out";
- `futex_wake(released,1)` → a deadlock with two sleepers asleep while `released==1` (lost wakeup);
- `futex_wait` without the kernel compare → the lead asleep with `arrived==2` (lost wakeup);
- a non-lead also enqueuing in a reduction → double or premature arrival detected.

**Removing the `!is_lead` guard is harmless** (the model passes). The guard only avoids a syscall that wakes nobody.

A coverage pass counts every reachable edge (`out-coverage.txt`). It confirms these interleavings were actually reached, not assumed away:

- a barrier arrival triggered by the lead's own increment (9 edges: `n0.pe0[k=0] ENQ_B`);
- `futex_wait` EAGAIN on each word (154 on `arrived`, 1,597 on `released`);
- the lead sleeping on `arrived` and `copied`;
- `futex_wake(&arrived,1)` with nobody asleep (648 edges; harmless because of the compare);
- counters wrapping to 0 with M=4.

### Answers to the specific questions

- **Can the barrier arrival be triggered by the lead-to-be's increment?** Yes, and it is correct: any member may enqueue, and the model reaches this.
- **With L>1 in a barrier, is the lead's arrived-wait ever needed?** No. Nobody waits on `arrived` in B epochs, and all mixed B/R programs pass. `arrived` is still incremented in B epochs, which keeps later R targets `(k+1)L` consistent.
- **Can `futex_wake(&arrived,1)` be consumed by nobody when the lead has not yet slept?** Yes, and it is harmless. The lead's later `futex_wait(arrived, v)` returns EAGAIN because `v` is stale, or its LOAD already sees the target.
- **With the `!is_lead` guard, is the lead fine if its own increment is last?** Yes. Its LOAD sees the target directly.
- **Can `released` advance past a waiter's target?** Never observed. The bump check (`released == e` before `++`) and the alias check hold in every passing run. The argument in §5.4 ("k+2 needs this member's own arrival") holds.
- **Double internode arrival, or arrival with the wrong epoch?** Never in the design; caught under mutation.
- **Why the argument holds (prose, matching the model).** `arrived` can reach `(k+1)L` only when every member has made its epoch-k increment. No member can make an epoch-(k+1) increment before `released == k+1`, and that requires the node's epoch-k arrival. Each member increments exactly once per epoch.

Limits of the model (stated honestly):

- One team per node.
- The internode tree is abstract: no loss, and the root is fixed.
- memcpy is 2 chunks, not byte-level.
- Weak-memory-ordering (aarch64 store-buffer) effects are not modeled. Every counter operation here is an atomic or a syscall, so `memory_order` only has to be acquire on loads and release on the fetch_add, as in §3.5.
- The private `k`/`d` are exact integers. In C, targets must be computed in `uint32_t` arithmetic, not signed `int`, where `(k+1)*L` overflow is UB. Minor.

## F2. Downward forward of `dest` by reference vs. "lead leaves last" — design change needed

**Location.** §5.5 (lead leaves last; inv. 16), §3.8 (descriptors carry references, drained later; the "Known future break" names tree reduce), §5.3 (release wakes waiters).

**Scenario** (model, N=2, L=(1,1), prog RR, fwd=after; the shortest trace is in the output):

1. Root node 0 merges node 1's partial, writes the final result into its lead's `dest`, and bumps `released`.
2. Lead 0 wakes. `copied` target is `0`, so it returns at once.
3. Lead 0 re-enters for epoch 1 and memcpys its new source into `dest`.
4. Node 0's progress thread only now drains the release (the result) to node 1, reading `dest` by reference. Node 1 receives the epoch-1 partial as its epoch-0 result.

The same happens at an interior node of a deeper tree (N=3 chain) and with L=(2,1) after local copy-outs.

The application alone can cause it too. After the root lead returns, `dest` is the application's; writing to it is ordinary use. Yet other nodes are still receiving it. Spec §9.10 (L6075) makes concurrent access UB only for "symmetric memory by an OpenSHMEM collective routine and any other means of access". A lead writing its own returned `dest` is not a conforming program doing anything wrong.

**Evidence.**

- Model run `N=2 L=(1,1) prog=RR fwd=after` → violation "downward forward to n1 epoch 0 chunk 0 read dest tagged (1, 1)". Also N=3 chain L=(1,1,1) and N=2 L=(2,1).
- The same configurations pass with fwd=before (drain completes before the local bump). Barriers pass either way.

**Fix options.**

- **(a) Count outbound forwards as copy-outs** — the one consistent with the three-counter design. The lead waits `copied == (d+1)·(L−1+C_T)`, where `C_T` is the number of child nodes in team T's tree at this node. The progress thread does `copied++` and a wake when the drain of each child's forward completes. Keeps "leaves last" literally true.
- **(b) Drain before the bump.** Delays every local member by the downward send time.
- **(c) Copy to staging.** Reintroduces §3.8's memory commitment.

Upward sends are safe as-is: `dest` is not written again until release, and release requires the root to have received every chunk. The model confirms this.

**Severity:** design change needed. **Confidence:** high.

## F3. The lead's `dest` as accumulator does not generalize to all data collectives — design change needed

**Location.** §5.5 ("Push, never pull — for every data-carrying collective"; "no per-team data area… three counters"), §7 Deferred ("The collectives are mechanical *given* §5.5's rules").

**What breaks, collective by collective** (spec text; every quoted statement below was checked against spec16.txt):

- **Reductions.** §9.10.9 (L7381–7383): results "are placed in the dest array on all PEs participating in the reduction"; every PE gets the same result, so the lead's `dest` works. All defined operators are commutative, so node-order combining is fine. *documented*
- **Broadcast.** §9.10.7: "The dest object is updated on all PEs" (1.5 change #4: including root). Same result for all; works.
- **Collect / fcollect.** §9.10.8: result is the concatenation in team-rank order, the same for all PEs, so the lead's `dest` can hold it. But "the collect routines allow nelems to vary from PE to PE". The lead cannot know member j's `nelems` or offset without a per-member published slot (or a size pre-round), so "three counters on three cache lines" is not the whole footprint.
- **Alltoall[s].** §9.10.5: "PE i sends the lth block of its source object to the kth block of the dest object of PE j". Every PE's `dest` is different, so the lead's `dest` cannot hold other members' results. Either each member pulls its column itself after the relevant peers have arrived (legal, since the sources are ready once those PEs called) or scratch is needed. Also, per-pair data volume is inherently O(P²).
- **Scan.** §9.10.10: `dest_i,j = Σ_{k≤i} source_k,j` differs per PE. It is also not decomposable as "reduce within node, then between nodes" when node membership interleaves in team rank (the §8.1 map `0,0,0,1,2,2` or any strided team). A prefix over ranks {0,2} on node A and {1,3} on node B cannot be formed from node sums.

**Severity:** design change needed before those collectives. Barrier and reductions (phase 1 and near-term) are unaffected. **Confidence:** high.

## F4. §3.8 barrier-forward bound and `max_teams` — clarification needed

**Location.** §3.8 item 2 ("bounded by tree degree × concurrent teams"; size `P×G + degree×max_teams + slack`), §5.7.

**Sound part.** For one team, a connection X→Y is at most an up-edge or a down-edge of that team's tree.

- **Up-edge.** If X holds an unforwarded epoch-N arrival, the root has not released N, so there is no N+1 arrival. At most 1 entry.
- **Down-edge.** Release N+1 requires arrivals from Y's subtree for N+1, which requires Y's PEs to have been released for N — i.e. Y received release N. At most 1 entry.

So the bound is **one entry per team per connection**, and the epoch argument is correct for blocking collectives (spec §9.10 L6085: "All OpenSHMEM teams-based collective operations are blocking routines"). Multiple threads do not break it: the spec forbids concurrent collectives on one team (§9.4, "A team may not be used concurrently by multiple threads in the same PE for collective operations"), so each team still has one epoch in flight. The `degree ×` factor overcounts per connection (the queue is per connection), which is harmless.

**Problem.** The per-connection bound is the number of **distinct teams whose trees use that connection**, and nothing ties that number to a per-PE team limit. Example:

- Two nodes A and B with L PEs each; world order a0..a(L−1), b0..b(L−1).
- `shmem_team_split_2d(WORLD, xrange=L, …)` yields L y-teams `{a_i, b_i}` (§9.4.7: `(x,y) = (pe mod xrange, ⌊pe/xrange⌋)`).
- Each PE holds only 2 teams, but L distinct teams cross A→B.
- If all L PEs of A are in a `team_sync` on their own y-team, A→B holds L arrivals.

With THREAD_MULTIPLE, each PE can have one thread per team, so the count grows further.

- **If `max_teams` is a per-PE handle limit:** the §3.8 size is too small by up to a factor of L (256 in the §1 target), and the "assert hard on overflow" fires on a conforming program.
- **If it is the number of distinct team counter blocks per node** (which the node-level counter pool needs anyway): the bound is right.

The design must say which, and size the per-node counter pool by the same unit.

**Severity:** clarification needed (becomes a blocking bug if resolved the per-PE way). **Confidence:** high.

## F5. Team identity: the range allocator is undefined and can be used up by one split — design change needed

**Location.** §5.7 ("team range allocator", "Team ranges leak until finalize"), §5.2.

**Spec constraints.**

- Split is collective over the **parent** (§9.4 L2311: "Team creation is a collective operation on the parent team object … All PEs in a parent team must participate").
- Non-members get `SHMEM_TEAM_INVALID` (§9.4.6 L2609).
- Failure must be agreed: "nonzero value on all PEs in the parent team" (§9.4.6); §9.10 L6163: team creation "must return identical return codes across multiple PEs".
- New teams are "immediately usable … without any intervening synchronization" (§9.4 L2329).

**Consequences.**

1. **What the split must establish before it returns, and on every member node:** an id every member of each child agrees on, the child's internode tree, and a zeroed shared counter block. So the split's release must carry the agreement: it is a data-carrying collective over the parent, not a barrier.
2. **Id consumption.** One `split_2d` on a parent of N PEs creates `⌈N/xrange⌉ + min(xrange,N)` distinct teams (§9.4.7 Notes: 10 PEs, xrange=3 → 12 teams). If child ids come from the parent's id range, a single call uses O(N) ids. Nested splits (the spec's own Example 12 does 3D via two `split_2d` calls) multiply this. A range sized by `max_teams` as a per-PE count is used up by conforming programs. Failure is legal if agreed, but it would happen at modest scale.
3. **The unit that actually matters is per node.** Wire demux and counter blocks need an id unique among teams present on a node pair. A per-node slot table with ids agreed during the split fits better. Two ways to agree them:
   - (a) The parent-collective split carries, from every member node, the node-local slot it chose for each child. Messages then address `(node, slot)`. Destroy frees the slots on member nodes only; no global id space exists, so nothing leaks.
   - (b) A free-bitmask agreement: AND-reduce over the parent. This is how MPICH allocates communicator context ids (*recalled*, `MPIR_Get_contextid`/`_sparse_group` in MPICH `src/mpi/comm/contextid.c`; not re-verified, no web fetch attempted). With THREAD_MULTIPLE, concurrent splits on different parents that share nodes need a node-local claim and retry.
4. **Allocating the shared counter block.** Who allocates and zeroes it on each node? The design says "Fixed pool carved at init", but not who claims a pool entry for a new team (the child's local lead during the split is the natural choice, before release).

**Severity:** design change needed (the §5.7 allocator is referenced by three structures and defined by none). **Confidence:** high.

## F6. `shmem_team_destroy` scope — the ambiguity claim is overstated (minor)

**Location.** §5.7 ("the spec never names the scope it is collective over"; "leaking is the only safe choice"), §10 closed-question table.

**Evidence.**

- §9.4.8 (L2911): "a collective operation that destroys the team referenced by the team handle argument team" and "If team compares equal to SHMEM_TEAM_INVALID, then no operation is performed". *documented*
- Non-members of a child hold `SHMEM_TEAM_INVALID` (§9.4.6), so any non-member call is a no-op by definition. The only PEs that can participate are the team's members.
- The spec's own Example 12 (L2818): every PE calls `shmem_team_destroy(yzteam)`, and each PE holds a different yzteam. That only works if each call is collective over that team's own members.
- The contexts example (L3164–3167): all PEs call `shmem_team_destroy(team_2)` and `(team_3)`, and non-members pass INVALID.
- Annex H (errata) has no entry on destroy.

So scope = members of the destroyed team. This is exactly the design's own "natural reading". The design's consequence — non-member parent PEs cannot observe the destroy — is right. But "leaking is the only safe choice" does not follow: F5 (a)/(b) reclaim ids without parent involvement. Leaking is also not free for applications that split and destroy per iteration (they eventually fail team creation).

**Severity:** minor. **Confidence:** medium-high.

## F7. Barrier vs. sync (§5.6) — sound

- §9.10.1 Notes (L6199): "The shmem_barrier_all routine is equivalent to calling shmem_ctx_quiet on the default context followed by calling shmem_team_sync on the world team." Verbatim match.
- §9.10.3 (team sync): "In contrast with the shmem_barrier routine, shmem_sync only ensures completion and visibility of previously issued memory stores and does not ensure completion of remote memory updates issued via OpenSHMEM routines."
- §9.10.4: sync_all ≡ team_sync on world.
- The deprecated §9.10.2 `shmem_barrier` (active set) also includes the default-context completion. If ever implemented, it too needs the quiet, which the design does not mention (deprecated, so minor).

A note on "visibility of previously issued memory stores" for sync: on aarch64 a store must be visible to other cores after the sync. The `fetch_add` (and, for waiters, the futex syscall) acts as a serialization point after the caller's earlier stores, so the intranode path provides it. *reasoned, not modeled*

## F8. Multithreaded PEs — sound

- §9.4 Thread Safety (L2297): "A team may not be used concurrently by multiple threads in the same PE for collective operations. However, multiple collective operations on different teams may be performed in parallel."
- §9.10 L6089: "team-based collective routines may not be called simultaneously by multiple threads on a given team."
- §9.2 item 5: ordering across threads is the programmer's responsibility.

`k` and `d` live in the private per-team descriptor, and counters are per team per node, so concurrent collectives on different teams share no state. A team used sequentially by different threads hands `k` over under the application's own serialization, which gives the needed cross-core memory ordering. The model's single-team result therefore composes across teams, and the §3.8 bound (F4) is per team.

**Different call orders across teams.** Suppose A does T1 then T2 while B does T2 then T1, single-threaded. That deadlocks in any blocking implementation; §9.10 only requires per-team order, so this is an application hazard, not a design defect. With one thread per team it works here, because trees and counters are independent.

## F9. Push-not-pull quote — verified

The sentence "The application does not need to synchronize to ensure that the source array is ready across all PEs prior to calling this routine." appears verbatim at:

| Section | Collective | Line |
|---|---|---|
| §9.10.5 | alltoall | L6588 |
| §9.10.7 | broadcast | L6878 |
| §9.10.8 | collect | L7036 |
| §9.10.9 | reductions | L7395 |
| §9.10.10 | scan | L7578 |

§9.10.6 (alltoalls) does not repeat it but defers to §9.10.5 for "The pre- and post-conditions for symmetric objects". Annex G 1.6 change #14 lists exactly "Sections 9.10.5 and 9.10.7 to 9.10.10". The design's citation is accurate.

The rule is also stated precisely. The precondition is "The source array at the local PE is ready to be read by any PE in the team" *before the local PE calls*, so reading a peer's source is allowed exactly once that peer has called. "Pull-based is non-conforming" is true only of pulls that do not wait for the peer's arrival; a pull gated on arrival is fine (and is one way out of F3 for alltoall).

§9.10.9 change #17 quote ("The same source and dest arrays must be passed by all PEs that participate in the collective.") is verbatim at L7384.

## F10. Minor and clarification items

1. **Inv. 16 / §5.2 wording.** "the only PE that writes that dest before release" is false for root and interior nodes: the progress thread merges children's partials into the lead's `dest` (§5.3) before release. It is harmless because the lead has stopped writing, but say "the lead, then (after its arrival) the progress thread". Also, a child's partial that arrives before the local lead has enqueued must be buffered, which is the §3.8 "known future break"; the model gates merging on the local enqueue.
2. **Single-node teams.** Teams wholly inside one node (common: node-local splits, single-node jobs) have no internode arrival to send. The release path — loopback AM, or direct `released++` by the enqueuer — should be stated. The model uses loopback and it is correct either way.
3. **Ordering the release.** For a result that arrives in several chunks, the final `dest` write must complete before `released++`. The model writes then bumps. Bumping first (or on the first chunk) is a stale copy-out.
4. **Wraparound margin.** The real condition is `M ≥ L` for `arrived` (model: M=2, L=3 fails; M=3, L=3 passes), `M ≥ L−1` for `copied`, and `M ≥ 2` for `released`. Say this rather than "harmless".
5. **Epoch assert on receipt** (§5.5) is sound and cheap. The model's `enq == k` check is its analogue and never fired in the design runs.
6. **Local lead = lowest team rank among local members** (§5.2): correct and deterministic; every member computes it from the membership list. Broadcast's root need not be the lead; the lead copies the root's source after the root arrives (F9 rule). Fine.

---

## Appendix — model output

Commands: `python3 model.py single`, `python3 model.py multi`, `python3 model.py mut`, `python3 coverage.py`, plus the fwd=after runs (in `out-fwd-after.txt`).
