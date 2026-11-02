# The small collectives' cost model

*Written 2026-11-02, for the large-cluster playbook's §5 and C16
(`docs/prompts/large-cluster-playbook.md`), at the author's word that a
small-collective cost model is written before any cluster session
(`docs/decisions/2026-11-01.md`, answer 15). The model is
`tools/modelcheck/small_collective_timing.py`, registered as
`model.small_collective_timing`; the record is
`docs/decisions/2026-11-02-small-collective-model.md`. Nothing in the library
changes.*

## What it is

The small collectives - a barrier, a sync, and a reduction or broadcast of at
most `OSOE_SMALL_COLLECTIVE_BYTES` (1 KiB) - go between a job's node leads up
and down a k-ary tree in level order at a fixed fan-out, `OSOE_COLLECTIVE_FANOUT`
(8), with no cost model and no choice; the large reductions and broadcasts take
a shape `shape_timing.py`'s closed form chooses from the link. This model
gives the small class what the large one has: a prediction of its time from a
few measured numbers, and the alternatives' beside it, so that the playbook's
§5 rows for the barrier and the small reductions and broadcasts are decided
by arithmetic first and a measurement second, and the cluster's ladder has a
line to stand its barrier times beside.

**Inputs**, microseconds:

- *h*, the hop: one message's way from a lead's agent to another's with the
  handling at both ends - half a small put's round trip, as `link-measure.c`
  reads it;
- *o*, the message cost: what a sender spends on each message before the next
  can leave, so that a parent's k sends to its children leave k serial costs
  apart - `link-measure.c`'s message cost means the same thing;
- *w*, the lead's wake: what a data collective's hop costs beyond a barrier's -
  its message lands in the lead PE's internal region and the agent wakes the
  lead thread waiting on the counter, where a barrier's ARRIVE and RELEASE are
  handled by the agents alone (`progress.c`); 0 for a barrier;
- *I*, the intranode phase: the node's members' arrival at their counter, the
  post to the lead's agent and the release's chained wake - *base* + *per_pe* ×
  PEs a node, fitted to H12's two-node barriers at 16 to 256 PEs a node.

**Shapes.** Each has a closed form and an event simulation of a loop of calls:

- the **k-ary tree** as built, k from 2 to 32: a hop and one message cost a
  level up - each child sends once and a parent forwards when its last child's
  arrival is in - and down a hop and the parent's serial sends, child j's
  release leaving j + 1 message costs after the parent took its own. A small
  broadcast's credits go up as a reduction's arrivals do
  (`internode-generic.c`, `in_pieces` at one piece), so it costs what the
  reduction costs. The slowest leaf is the deepest with the largest digit sum
  of its offset in base k, or the last leaf of the level above when the last
  level is partial - `shape_timing.py`'s `tree_terms` arithmetic, checked here
  against a walk of every leaf (TERMS);
- the **binomial tree** over the leads' indices, a lead's children the indices
  that add one bit below its lowest, sent largest subtree first: ⌈log₂ n⌉
  hops each way, and its root's ⌈log₂ n⌉ sends serial too, so that a lead at
  index i is reached popcount(i) hops and R − lowest_bit(i) message costs
  after the root - R(h + o) each way on a full tree;
- the **dissemination barrier**: ⌈log₂ n⌉ rounds, each lead sending to the lead
  2^r after it round the circle and waiting for the one 2^r before, no root
  and no down pass: R(h + o);
- **recursive doubling** for a small allreduce: log₂ rounds of one exchange
  among the largest power of two of leads, every one holding the result; the
  leads past that power of two fold their vectors into a partner's first and
  take the result back after - at most two rounds more, and fewer in effect,
  since a lead with no extra sends its first round at once and the extra's
  rounds overlap the core's (the simulation found a first closed form charging
  two whole rounds out);
- a **broadcast without its up pass**: the root's data down the k-ary tree as
  soon as it has it, no credits first - one call's time alone, since without
  the up pass nothing tells the root a child's slot is free, and a loop of
  such calls has no period of its own.

**Two measures.** `loop`: the period of a loop of calls with nothing between
them, what `osoe-bench`'s barrier and collectives lines report - in steady
state every lead enters as it is released, so the root's next release comes
max over the leaves of (the way down + the way back up) + *I* after its last;
`call`: one call every lead enters at once, until the last is released - the
slowest way up, then the slowest way down. They differ where the leaf with the
longest way down is not the deepest: a level-order tree whose last level is
partial, where the last leaf of the level above, released after k serial
sends at every level, has a shorter way back up than the deepest leaf. On
every grid the model checks, every shape's closed form agrees with its
simulation to the microsecond in both measures (SIMULATION).

## The fit

The tree at k = 8 against the records, with *h* taken from each cluster's own
measurement and *o*, *I* and *w* fitted (`small_collective_timing.py fit`;
the Pi's *o* held, since its three barrier rows are all on four nodes - a star,
where 3*o* and the node's base are one number). A row's spread is the range of
the sessions that measured it, widened to a tenth either side of their median
where the records show less; the small data collectives are fitted as their
gap over the barrier of their own session, since a session's lines move
together (the cloud's 8 × 8 barrier was 99, 117 and 132 µs in three), and the
model has a reduction and a broadcast cost the same gap, 2*w* on a star.

**The Pi cluster** (`docs/pi-cluster-results.md`): *h* 86 µs -
`shape_timing.py`'s fit, inside `link-measure.c`'s 86.8 to 94.0 and half the
169 to 175 µs 8-byte put round trip; *o* 1, between P4's measured 0 and the
shape fit's 4, inside the 0 to 21 the start-ups read; the node 14 + 0.6 µs a
PE; *w* 5.5.

| | nodes | PEs a node | measured, µs | model | within |
|---|---|---|---|---|---|
| barrier | 4 | 1 | 181 | 191 | yes (163 to 199) |
| barrier | 4 | 2 | 202 | 191 | yes (182 to 222) |
| barrier | 4 | 4 | 176 to 208 | 192 | yes (164 to 208) |
| 8-byte reduce over the barrier | 4 | 1 | 3 | 11 | yes (3 to 22, the gaps pooled) |
| 8-byte reduce over the barrier | 4 | 4 | 11 to 22 | 11 | yes |
| 8-byte broadcast over the barrier | 4 | 4 | 11 to 19 | 11 | yes |

**Google Cloud**, eight `c4-standard-16` nodes (`docs/fast-link-results.md`):
*h* 43 µs - `link-measure.c`'s 43 to 53 (H14) and half the 88.6 µs 8-byte put
round trip of the two-PE job; the fastest round trip, 41.3 µs at 64 PEs, is the
floor an awake agent gives, and an idle one's wake is the rest, P38's finding;
*o* 2, fitted; the node 0 + 2.2 µs a PE, from H12's four rows and the two-PE
job; *w* 12.

| | nodes | PEs a node | measured, µs | model | within |
|---|---|---|---|---|---|
| barrier | 2 | 1 | 87 | 92 | yes (79 to 96) |
| barrier | 2 | 16 | 125 | 125 | yes (112 to 138) |
| barrier | 2 | 32 | 157 | 160 | yes (141 to 173) |
| barrier | 2 | 64 | 221 | 231 | yes (199 to 243) |
| barrier | 2 | 256 | 672 | 653 | yes (605 to 739) |
| barrier | 8 | 8 | 99 to 132 | 120 | yes (99 to 132) |
| barrier | 8 | 16 | 139 to 146 | 137 | yes (128 to 157) |
| barrier | 8 | 32 | 184 | 172 | yes (166 to 202) |
| 8-byte reduce over the barrier | 8 | 8 | 24 to 44 | 24 | yes (13 to 44, the gaps pooled) |
| 8-byte broadcast over the barrier | 8 | 8 | 13 | 24 | yes |

Every row within its spread; the barriers within 5.4% of their medians on the
Pi and 6.3% on the cloud, where the only shape the records hold is a star of
two hops - so the fit pins *h*, 7*o* + *I* and *I*'s slope in the PEs a node,
and not *o* apart from *I*'s base. Four planted errors each fail the check
written for them: a parent's sends counted as one message cost a level
(TERMS), the binomial's sends smallest subtree first (PINS), the cloud's *o*
the trimmed mean's 23 µs (FIT: a barrier of 2 × 16 at 167 µs where 125 was
measured) and no intranode cost a PE (FIT).

**What the fit says that the measurement did not.** The barriers on the cloud
imply a message cost of about 2 µs a serial send, inside the 0 to 36 µs P38's
least-less-least read on three nodes and far under the 23 to 45 µs its trimmed
mean read, which `collective-shape.c` takes Träff's by (the third cluster
session, `docs/decisions/2026-10-28.md`). A barrier's RELEASE to seven children
leaving 23 µs apart would make the 8 × 8 barrier 250 µs; it measured 99 to 132.
Either the data path's fan - a put and its completion to each child - costs
what the control wire's send does not, or the idle CPUs' wakes P38 named are
most of the trimmed mean's number, as the 2026-10-28 record suspected. The
record puts this to the author.

## The crossovers

Every shape's time at 8 PEs a node on five links - the Pi's and the cloud's as
fitted, the Pi at the shape fit's *o* of 4, and the brief's two guessed fast
links, *h* 10 with *o* 2 and *h* 40 with *o* 25 - at 8, 64, 512, 4,096 and
32,768 nodes; the barrier's and the reduction's a loop's period, the
broadcast's one call, since without its up pass a loop has none; `best k` the
fan-out from 2 to 32 of least cost; `*` beats the tree at 8
(`small_collective_timing.py crossover`, verbatim):

```
the Pi: h 86, o 1, w 5.5 us; the node 19 us at 8 PEs a node. Microseconds: a loop's period, the broadcasts one call; * beats the tree at 8
   nodes | barrier: tree 8   tree 16      best k  binomial   dissem. | reduce: tree 8      best k  binomial  doubling | bcast: tree 8  binomial  no up pass
       8 |             199      199       199 k7       541       280  |            210      210 k7       574       296  |           210      574         117*
      64 |             379      383       379 k8      1063       541  |            401      401 k8      1129       574  |           401     1129         216*
     512 |             560      569       409*k23     1585       802  |            593      431*k23     1684       851  |           593     1684         315*
    4096 |             741      584*      584*k16     2107      1063  |            785      617*k16     2239      1129  |           785     2239         415*
   32768 |             922      765*      632*k32     2629      1324  |            977      665*k32     2794      1406  |           977     2794         514*

the Pi at shape_timing.py's o of 4: h 86, o 4, w 5.5 us; the node 19 us at 8 PEs a node.
   nodes | barrier: tree 8   tree 16      best k  binomial   dissem. | reduce: tree 8      best k  binomial  doubling | bcast: tree 8  binomial  no up pass
       8 |             223      223       223 k7       559       289  |            234      234 k7       592       305  |           234      592         138*
      64 |             427      443       427 k8      1099       559  |            449      449 k8      1165       592  |           449     1165         258*
     512 |             635      671       547*k23     1639       829  |            668      569*k23     1738       878  |           668     1738         381*
    4096 |             843      731*      731*k16     2179      1099  |            887      764*k16     2311      1165  |           887     2311         505*
   32768 |            1051      939*      923*k32     2719      1369  |           1106      956*k32     2884      1451  |          1106     2884         628*

the cloud: h 43, o 2, w 12 us; the node 18 us at 8 PEs a node.
   nodes | barrier: tree 8   tree 16      best k  binomial   dissem. | reduce: tree 8      best k  binomial  doubling | bcast: tree 8  binomial  no up pass
       8 |             120      120       120 k7       288       153  |            144      144 k7       360       189  |           144      360          87*
      64 |             222      230       222 k8       558       288  |            270      270 k8       702       360  |           270      702         156*
     512 |             326      344       282*k23      828       423  |            398      330*k23     1044       531  |           398     1044         227*
    4096 |             430      374*      374*k16     1098       558  |            526      446*k16     1386       702  |           526     1386         298*
   32768 |             534      478*      470*k32     1368       693  |            654      542*k32     1728       873  |           654     1728         369*

fast, h 10 o 2: h 10, o 2, w 5 us; the node 18 us at 8 PEs a node.
   nodes | barrier: tree 8   tree 16      best k  binomial   dissem. | reduce: tree 8      best k  binomial  doubling | bcast: tree 8  binomial  no up pass
       8 |              54       54        54 k7        90        54  |             64       64 k7       120        69  |            64      120          47*
      64 |              90       98        90 k8       162        90  |            110      110 k8       222       120  |           110      222          76*
     512 |             128      146       128 k8       234       126* |            158      158 k8       324       171  |           158      324         107*
    4096 |             166      176       166 k8       306       162* |            206      206 k8       426       222  |           206      426         138*
   32768 |             204      214       204 k8       378       198* |            254      250*k14      528       273  |           254      528         169*

loaded, h 40 o 25: h 40, o 25, w 12 us; the node 18 us at 8 PEs a node.
   nodes | barrier: tree 8   tree 16      best k  binomial   dissem. | reduce: tree 8      best k  binomial  doubling | bcast: tree 8  binomial  no up pass
       8 |             298      298       278*k6       408       213* |            322      322 k7       480       249* |           322      480         245*
      64 |             578      678       578 k7       798       408* |            626      626 k8       942       480* |           626      942         472*
     512 |             883     1108       863*k5      1188       603* |            955      955 k8      1404       711* |           955     1404         724*
    4096 |            1188     1483      1168*k5      1578       798* |           1284     1284 k8      1866       942* |          1284     1866         976*
   32768 |            1493     1788      1473*k6      1968       993* |           1613     1613 k8      2328      1173* |          1613     2328        1228*

The message cost over the hop, o / h, at which each alternative beats the tree at 8 (w = 0, no intranode), to o = 2h: a dash where it never does
   nodes      tree 16     binomial      dissem.     doubling
       8            -            -    from 0.21    from 0.21
      64            -            -    from 0.21    from 0.21
     512            -    from 1.72    from 0.19    from 0.19
    4096 0.00 to 0.13    from 1.61    from 0.19    from 0.19
   32768 0.00 to 0.13    from 1.54    from 0.18    from 0.18
```

**What the tables say.**

- **The tree at 8 stands on both measured links below 512 nodes**, and no
  alternative comes near it: the dissemination costs a third more at 64
  nodes, the binomial twice. At 8 and 64 nodes the tree is a star and a
  two-level tree, and nothing has fewer hops.
- **From 585 nodes a wider tree saves a level.** At 4,096 nodes the tree at 16
  is three levels where 8 is four: 374 against 430 µs on the cloud, 13%; the
  best fan-out the model finds, 16 at 4,096 and 32 at 32,768, is the widest
  that saves a level, since at these message costs a parent's sends cost less
  than a hop. At 512 nodes the two are both three levels and 16 pays eight
  sends more a level for nothing. `OSOE_COLLECTIVE_FANOUT=16` at the ladder's
  top rungs is the measurement that confirms it, as C16 has it.
- **The dissemination and recursive doubling win only from a message cost
  about a fifth of the hop**, at every node count: by the closed forms, R(h +
  o) against about (2/3)R h + 3R o for the tree at 8 on a full tree, so
  o/h > 1/6. The fitted ratios are 0.012 on the Pi and 0.047 on the cloud;
  the trimmed mean's message cost would make the cloud's 0.5 to 1, and then
  the dissemination would be a third faster than the tree at every rung (the
  `loaded` link's rows). So the first barrier rows of the ladder are the
  test of the message cost itself: a barrier near the model's line at 64
  nodes - 222 µs at 8 PEs a node on the cloud's link - says *o* is small and
  the tree stands; one far above it says *o* is large, or the receiver's cost
  the model leaves out matters, and the dissemination is worth building.
- **The binomial tree never pays here.** Its root's R sends are as serial as
  the k-ary tree's k, and it has three times the levels; it wins only past a
  message cost of 1.5 hops, where the dissemination won long before.
- **A broadcast without its up pass is about 0.55 to 0.6 of one call's time**
  on every link - the down pass alone. But a loop of such calls has no period
  of its own: the up pass is what tells the root a child's slot is free, and
  without it the slot's ownership is a new protocol (double-buffered slots
  whose credits trail a call behind). The gain is for programs whose
  broadcasts are spaced, not for `osoe-bench`'s loop.

## What it does not model

- **The fabric's topology.** Every hop costs *h* whatever the two leads; a
  tree whose levels cross a spine pay more than one whose subtrees are racks
  (the playbook's topology row), and a dissemination's partners 2^r apart
  cross it every round.
- **Contention at the root beyond its serial sends.** The closed form charges
  a parent its k sends and nothing for its k arrivals, which land together in
  a barrier and queue at its agent if handling costs. The simulation can
  charge a receiver per message (`small_collective_timing.py disagree`): on
  the cloud's link a cost equal to *o* adds 4 to 5% to the tree at 8 and the
  dissemination at 64 and 4,096 nodes, three times *o* adds 38% to the tree
  and 13% to the dissemination - the tree's root handling eight at once where
  the dissemination handles one a round. Where the cluster's barrier sits above
  the model's line, this is the first term to suspect.
- **The idle agents' wakes.** *h* is half a round trip between agents already
  awake in a loop of calls; a program that calls a barrier after a silence
  pays each level's wake on top (P38's 23 to 45 µs is probably that), and the
  fit's two-PE barrier at 87 µs against the fastest round trip's 41 shows how
  much the wake can be.
- **The intranode phase past the cores**: *I* is linear in the PEs a node,
  fitted to 16 to 256 PEs a node on 16 vCPUs; H12's 256 row, at 16 PEs a vCPU,
  sits 3% under the line and the check's cost at that count (975 against 672
  µs) is not in it.
- **The link's bandwidth, the progress helpers, the control wire's one buffer
  and its acknowledgements**: a small collective's bytes are nothing against
  the hop, the control plane is the lead's agent's at every helper count, and
  a control wire carries a few bytes a barrier - none of these moves a small
  collective's time in the records, and none is here.
- **The small class's large end.** A 1 KiB piece on a gigabit link is 8 µs of
  wire a hop and 56 at a parent's seven sends; the model treats every small
  message as bytes-free. `shape_timing.py`'s closed form with the small class's
  bytes is the model for that end, and its `reduction_cost` already prices it.
