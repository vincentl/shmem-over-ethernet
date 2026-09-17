# Refutation: collectives findings (q1-collectives.md F2–F5, plus a check of F1)

Design: `docs/design/shmem-ethernet-design.md` (line numbers below refer to it). Spec: `scratchpad/spec16.txt` (`L` = spec line).
Model: `scratchpad/exp/collectives/model.py`.

## Summary

| ID | Verdict | Severity (reviewer → mine) |
|---|---|---|
| F1 | Stands (sound), with modelling limits listed below | — |
| F2 | SURVIVES-DOWNGRADED | design change needed → clarification needed before tree reduce (not phase 1) |
| F3 | SURVIVES-DOWNGRADED | design change needed → clarification needed (§7 and §5.5 overclaim for deferred collectives) |
| F4 | SURVIVES | clarification needed (a real bug under the per-PE reading, and §6.4 leans toward that reading) |
| F5 | SURVIVES-DOWNGRADED | design change needed → open design gap, blocking only for team-creation routines (not phase 1). One cited number is wrong |

---

## F1 — Are the epoch counters sound? A check for modelling shortcuts

**What I re-ran (observed).**
- `fwd=before`, N=2, L=(1,1), RR: PASS (2,088 states).
- `fwd=after`, N=2, L=(2,2), BBB: PASS (76,814 states).
- **New branching topology:** a star with root 0 and children 1 and 2, L=(1,2,1), RRB, `fwd=before`: PASS (1,241,049 states). The reviewer only tried chains. This closes that gap.

**Shortcuts I looked for in model.py:**

- **`fetch_add` and compare as one step (INC).** Faithful. The compare uses the returned value, which is private, so splitting the step would not add any shared-state interleaving.
- **`wait_eq`.** LOAD and WAIT are separate steps, and WAIT does the kernel compare-and-enqueue atomically (ARR_WAIT/REL_WAIT/COP_WAIT). This is faithful, and the `nocmp` mutation shows the checker notices when that compare is removed.
- **Wake counts.** `wake()` wakes exactly `min(n,|q|)` of the queued sleepers and explores every choice. The `wake1` mutation is caught. This is faithful.
- **Merge gating (a real abstraction, and it matters).** A child's partial is merged only when `enq > e and applied == e`, i.e. after the local lead has enqueued. In other words, the model assumes an early child partial is **buffered**. It never models the progress thread merging into the lead's `dest` while the lead is still combining. That case would be a lost update, and the model cannot show it. The design does not specify this buffering either:
  - §3.8 "Known future break", L446–450, is about size and latency only;
  - reviewer F10.1 mentions it.

  So F1's "sound" holds only if the implementation buffers or defers the merge until the local arrival is enqueued. This should be stated as a rule next to inv. 16.
- **Root / internode release.** The root is fixed, there is no loss, and the release is applied as a sequence of atomic progress-thread steps (write dest, bump, wake). That matches §5.3 L1077–1080. Nothing is hidden for the counter argument itself.
- **Cross-process futex.** The model has one abstract wait queue per word. The real code needs a *shared* (non-`FUTEX2_PRIVATE`) futex on the shared mapping. The design covers this at L985 (documented). Not a model issue.
- **One team per node.** Teams use separate futex words, so composition is fine (F8's argument).
- **Not modelled:** the quiet inside `barrier_all`, and team creation.

**Conclusion.** F1 stands. The one assumption to write into the design is "merge of child partials is gated on the local arrival being enqueued".

---

## F2 — The downward forward of a reduction result by reference races with the lead leaving

**Reproduced (observed).** `Cfg([1,1],"RR",fwd='after')` → FAIL, with "downward forward to n1 epoch 0 chunk 0 read dest tagged (1,1)". The trace is identical to `out-fwd-after.txt`. The same configuration with `fwd=before` passes, and so does barrier-only with `fwd=after`.

**Does the design imply `fwd=after`? No — it does not specify the order at all.**

- §5.3 L1077–1080 says only: "Release arrives at the progress thread, which advances the team's `released` counter and wakes all local waiters". It says nothing about forwarding to children, or in what order relative to the bump.
- §3.8 puts "collective forwards" in the control queue (L389). The by-reference-at-drain-time semantics is stated only for *get replies* (L408–411: "Get reply | At drain time | A **reference**").
- §3.8 L446–450, "Known future break", says a tree reduce forward "fits neither queue … Do not let the structure harden around it". So the design explicitly leaves the reduce forward mechanism unresolved.
- Note: the text says `shmem_reduce`, but 1.6 has only `*_reduce` typed routines — a cosmetic point.

**Does that text already handle the race? No.**

- It covers size and latency, not the lifetime of the `dest` being referenced.
- Inv. 16 (L1636–1638) and the §5.5 snippet comment (L1172, "leaves last, so its dest stays valid for every copy-out") scope "leaves last" to local copy-outs. An internode forward that reads `dest` is not a copy-out.
- So an implementer who reuses the §3.8 descriptor-by-reference pattern for the reduce forward — the pattern the doc's own table favours — gets this bug.
- The reviewer's point about the application is correct and makes it worse. After the root lead returns, `dest` belongs to the application. §9.10 (L6075) makes concurrent access UB only *during* the collective.

**Why downgraded.**
- Reductions are deferred (§7, L1408–1411).
- Barrier, the only phase-1 collective, is unaffected (model PASS).
- The design contains no wrong statement — it is silent where it has already flagged the area as unfinished.
- The fix is local: it does not change the counter scheme, it only extends one target.

**Tightest statement.** Any node that forwards a data-collective result to child nodes by reading its lead's `dest` must finish that read before the lead can leave. Today "leaves last" (inv. 16) guards only local copy-outs, and §3.8's known-future-break note does not mention lifetime.

**Minimal fix.** Add one sentence to inv. 16 and to L446: "downward forwards are readers of `dest`". Then either:
- count them in `copied` — target `(d+1)(L−1+C)`, where C is the number of child nodes in this team's tree; or
- complete them (or copy them by value into the wire buffer) before `released++`.

---

## F3 — The lead's-`dest` accumulator does not generalize to alltoall[s], scan, or variable-nelems collect

**Spec checks (documented).**
- L6580: alltoall — "PE i sends the lth block of its source object to the kth block of the dest object of PE j". Each PE's `dest` differs.
- L7025 and L7047: collect — "allow nelems to vary from PE to PE".
- §9.10.10 L7544ff: scan gives a different prefix per rank.

All of the reviewer's spec claims are correct.

**What the design already handles.**
- §5.5 is titled **"Reduction destination"** (L1147), and its accumulate rule is argued for reductions.
- §7 L1413–1415 says the deferred collectives inherit §5.5's **rules**, and lists exactly which rules: "the local lead is the single writer before release and leaves last, and a peer's `source` is read only after that peer has called". It does not say they must accumulate into the lead's `dest`.
- §5.5 L1219–1220 already has a fallback: "Fallback for cases where `dest` cannot serve: chunk through fixed scratch".
- §4.5 L747 says "*Verify against each collective during the spec read*".

So the design never claims that alltoall accumulates into the lead's `dest`. Part of F3 attacks a generalization the doc does not make.

**What still survives.**
1. **Footprint claim.** L1216–1218 ("no per-team data area … **three counters on three cache lines**") is stated without scope.
   - Variable-nelems collect needs per-member sizes, so either a size pre-round or L published words.
   - alltoall needs scratch, or per-member pulls gated on arrival.
   - The claim is true only for reductions and broadcast.
2. **Scan claim.** The §7 table at L1421 ("inherits the §5.1 two-level skeleton; nothing architectural once barrier exists") is wrong when a node's team members are not contiguous in team rank.
   - Example: the §8.1 map `0,0,0,1,2,2` or any strided team.
   - Node sums cannot form prefixes, so the node must ship per-run (in the worst case per-PE) contributions.
   - That breaks §5.1's "internode cost is a function of node count" for scan.
   - It still fits the AM skeleton, so it is a cost and design-note issue, not an architecture break.

**Why downgraded.**
- All of these collectives are deferred under §7's own rule (L1382: "Mechanical expansion … waits").
- The design already has a scratch fallback.
- No phase-1 decision depends on the claim.
- What is wrong is the word "mechanical" and the unscoped footprint sentence.

**Tightest statement.** §5.5's "three counters, no per-team data area" and §7's "mechanical / nothing architectural" apply to reductions and broadcast only.
- collect with variable `nelems` needs a size agreement step.
- alltoall[s] needs per-PE results: gated pulls or scratch.
- scan over non-contiguous node membership cannot reduce within the node first, so its internode cost is not O(nodes).

**Minimal fix.** Scope L1216–1218 to reductions and broadcast. In the §7 rows for collect, alltoall and scan, name these three items as open design points.

---

## F4 — `degree × max_teams` bound for the §3.8 control queue

**Checked (documented).**
- **Where the bound appears.**
  - §3.8 L436: "bounded by tree degree × concurrent teams".
  - §3.8 L442: "Size as `P×G + degree×max_teams + slack`".
  - §5.7 L1250–1252: the same `max_teams` bounds the control queue and the range allocator.
  - No text defines what `max_teams` counts.
- **§6.4 and §5.7 point to the per-PE reading.**
  - §6.4 L1358: the team pool is "carved once at init".
  - §5.7 L1256–1258: "the pool cannot be sized until the bootstrap has supplied local PE count".
  - Together these suggest pool = f(local PEs, max_teams), i.e. `max_teams` counted per PE.
- **Scenario check.** Spec L2711: `(x,y) = (pe mod xrange, ⌊pe/xrange⌋)`; yaxis_team = same x (L2701).
  - Take nodes A = world 0..L−1 and B = L..2L−1, with `xrange = L`.
  - This gives L yteams `{a_i, b_i}`, each spanning A–B, and each PE holds only 2 new teams.
  - L single-threaded PEs on A, each in `team_sync` on its own yteam at the same moment, put L entries on A→B.
  - That is legal: §9.4 L2297 allows parallel collectives on different teams, and here they are even in different processes.

**No refutation found.**
- With a small per-PE `max_teams` (e.g. 4) and degree 1, the queue holds `degree×max_teams` entries.
- It must hold L+1 (with L up to 256), so the "Assert hard on overflow" fires on a conforming program.
- One correction to the reviewer: THREAD_MULTIPLE does not add anything beyond distinct teams, because one epoch is in flight per team. The bound is simply the number of distinct teams using the edge.

**Tightest statement.** For each team, a connection holds at most one entry. So the control-queue bound is the number of distinct teams whose tree uses that connection, which is at most the number of team counter blocks on the sending node. §3.8 sizes it as `degree × max_teams`, which undercounts by up to a factor of L if `max_teams` is a per-PE limit.

**Minimal fix.** Define `max_teams` as distinct live teams per node, which is also the node counter-pool size. Drop the `degree ×` factor, and replace it with the per-node team count in the §3.8 size formula.

---

## F5 — The team range allocator is undefined and can be exhausted

**Checked (documented).**
- "Team range allocator" occurs only at L1251–1252 and L1266 ("Team ranges leak until `finalize`").
- No section defines:
  - what a range is;
  - how members agree on an id;
  - how wire demux keys a team;
  - who claims and zeroes a counter block during a split.
- §10's "still open" lists (L1676–1706) do not mention it.
- Spec constraints quoted by the reviewer, verified:
  - L2311 / L2329: "immediately usable";
  - L2612–2613: failure is "nonzero value on all PEs in parent";
  - L2744–2746: split_2d failure invalidates the handles on all parent PEs.

Together these confirm that a split must carry agreement data over the parent, i.e. it is a data-carrying collective.

**Error in the reviewer's evidence.**
- F5 states `⌈N/xrange⌉ + min(xrange,N)` teams and cites the spec's "10 PEs, xrange=3 → 12 teams".
- The spec prose (L2753) does say "12 teams", but its own grid (L2754–2761) shows 4 xteams + 3 yteams = **7**, which matches the reviewer's formula.
- The "12" is a spec typo, not support for the claim. The count is O(N/xrange + xrange) either way, so the conclusion holds.

**Why downgraded.**
- Phase 1 implements only `SHMEM_TEAM_WORLD` (§7 "Plumb but do not implement", L1402–1406), so no team creation is in scope.
- The "exhausted by one split_2d" claim depends on an assumed design: ids drawn from a parent range sized per PE. The doc leaves "range" undefined, so that is one plausible reading, not a stated defect.
- Failure is also legal if every PE agrees on it.
- What does survive fully: the allocator is referenced as one of three structures bounded by `max_teams` but never defined, and the split protocol (id agreement, per-node counter-block claim, and concurrent splits under THREAD_MULTIPLE) is absent.

**Tightest statement.** The design names a team range allocator but defines none of these:
- the id space;
- how ids are agreed during a parent-collective split;
- how per-node counter blocks are claimed.

It also has no answer for split_2d consuming O(N/xrange + xrange) teams per call.

**Minimal fix.** Add a §5.7 subsection before any split routine lands:
- make ids node-local slots, chosen by each member node and exchanged in the split's release (reviewer option (a));
- have a split claim and zero its counter blocks before release;
- specify destroy as freeing those slots on member nodes. This also removes the need to leak ranges (ties to F6).
