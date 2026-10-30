# 2026-10-29: P1 modelled - the global exit's single job-wide carrier

P1 (`docs/prompts/v0.2-open-items.md`; round four's conformance F1): when PEs of more than one node
call `shmem_global_exit` at once - as every PE does when each detects the same failure - each
node's first caller carries that node's end, and the first carrier to leave is a bad exit while
the other node still runs. Under `srun -K1`, which the README recommends, Slurm then kills the
other node's PEs still ending, callers among them, whose handlers and buffered output are lost,
and `srun` may return 143. The README states it as a limit ("Call it from one PE"). The review's
cure was one carrier for the whole job, and the author's word of 2026-10-25 (`2026-10-24.md`,
item 3): "the model comes first and the call is the author's" - how a carrier that is not the
lowest caller's node learns of a lower one, and what a node waits for before it leaves with a
status that is not 0. The author, 2026-10-29 evening: "Do P1 and P4 in parallel. Go!" P4 went to
a session of its own (`2026-10-29-two-node-choice.md`).

**The proposal holds, and each of its three rules is needed: under `srun -K1` with callers on
both nodes - where the rules as built fail NO_KILL in 37 states - one carrier, the lowest-ranked of
the nodes', leaves last and alone with a status, in every interleaving of 1.8 to 2.5 million
states, the trapped-state check included, and with three callers, two of them on the outranked
node, in 24.6 million; every case the built rules pass passes under it; and each rule broken
fails, three STUCK and three NO_KILL. The call is the author's: question below.**

## The proposal

One carrier for the job: the lowest-ranked of the nodes' carriers, each node's being the caller
whose declaration won its node's word, as now.

1. **Tell the higher.** A carrier's node sends its abortive goodbye to a peer whose own has
   arrived when that goodbye named a caller ranked above this node's carrier. The code sends
   nothing there now (`progress.c`'s `flush_aborting` and `flush_control_aborting`: a peer whose
   abort arrived has "nobody to tell"), so the higher node would never hear of the lower caller.
   Every other send stays as it is.
2. **Outranked.** A carrier's node whose lead reads a goodbye naming a caller ranked below its
   carrier sets one more word in the node's segment, then waits for no other node. Its lead's end,
   which ends its stream, waits for every other PE of the node, the carrier among them. The
   carrier rereads the word at each poll of its wait (`osoe_liveness_outlast` polls its pidfds
   every 100 ms, so no new wake is needed), waits for nobody, and leaves with 0 by `exit`, its
   handlers run, as a caller whose declaration lost does now.
3. **The lowest waits for all.** A carrier's node not outranked waits for every other node's end,
   none excluded - where now it excludes a node whose goodbye named another caller - so the job's
   carrier leaves last of the job, and its status is the job's.

The two questions of 2026-10-25 answered: the higher node learns of the lower by the lower's
goodbye, which rule 1 makes sure is sent, and its own segment's new word; a node leaves with a
status that is not 0 only when it is the carrier of a node not outranked, after every other node's
end.

## The model

`docs/design/evidence/exit-carrier/`: the suite's `abort_exit_model.py` copied, with the proposal
as variants and a property, ONE_CARRIER - at most one process leaves with a status other than 0 -
beside its own (TERMINATES, STATUS, JOB_STATUS, NO_KILL, ONE_END). Its 29 cases run from the copy
give every verdict and state count the suite's copy gives (`old-cases.txt`), so the new lines
change nothing they do not name. The suite's model is unchanged until the author decides.

The proposal's cases (`single-cases.txt`, `three-lead-b-cases.txt`, `three-cases.txt`), each
under `srun -K1` but one:

| Case | Shape | States | Result |
|---|---|---|---|
| `single [both]` | the residual: A's PA calls with 5, B's PB with 6, at once | 1,766,635 | holds, live - `built` fails NO_KILL in 37 |
| `single [both_lead_a]` | A's lead the lower caller | 2,520,775 | holds, live |
| `single [both_lead_b]` | B's lead the outranked carrier | 2,409,391 | holds, live |
| `single [three_lead_b]` | A's PA, and B's lead and PB, at once | 24,570,080 | holds; the trapped-state check skipped past 12 million states, as the engine does |
| `single [three]` | A's PA, and B's PB and QB, at once | 42.2 million, and 32.6 million without the pidfd option | no violation, but unfinished at the model's half hour each; `long_three.py` gives it four hours |
| `single [nonlead]`, `[finish]`, `[exiting]` | one caller, and beside a finalize and an exit | 119,673 each | holds, live |
| `single [nonlead]`, no pidfd | one caller, no help | 119,673 | holds, live |
| `single [lead]` | a lead the caller | 55,707 | holds, live |
| `single [on_b]` | a caller on the three-process node | 705,009 | holds, live |
| `single [same_node_both]` | two callers on one node | 8,350,440 | holds, live |
| `single [early]` | a caller straight after init | 35,511 | holds, live |

Each case has fewer states than the built rules' (133,929 for `nonlead`): the built model sends a
superset of the code's goodbyes, the proposal exactly its rule's.

The breakages (`single-breakages.txt`), each on `both`:

| Variant | The rule broken | States | Result |
|---|---|---|---|
| `single_old_send` | 1: the code's send, nothing to a peer whose goodbye has arrived | 42,012 | STUCK: each node waits for the other's end |
| `single_demoted_unwaited` | 2: the outranked lead waits for every PE but the carrier, as now | 252 | NO_KILL: its end comes before the demoted carrier's |
| `single_status_kept` | 2: the demoted carrier keeps its status | 35 | NO_KILL |
| `single_carrier_unmoved` | 2: the carrier does not reread the word | 36 | STUCK: it waits for its lead, which waits for it |
| `single_outranked_waits` | 2: the outranked node still waits for the lower one's end | 37 | STUCK |
| `single_lowest_excludes` | 3: the lowest excludes a higher caller's node, as now | 88 | NO_KILL |

## What a build would change

- `shmem/src/progress.c`: rule 1's send in `flush_aborting` and `flush_control_aborting`;
  `peer_carries_another` becomes a lower caller's test, and finding one marks the node outranked;
  `osoe_progress_peers_ended` then holds.
- `shmem/src/progress-host.c`: the carrier's node's wait for the others' ends (`:1707`) ends on
  the word.
- `shmem/src/node.h`: the word, a layout change.
- `shmem/src/abort.c`: the carrier's wait rereads the word at each poll and, outranked, leaves it,
  `carrier` and the grace's status then false and 0; an outranked lead waits for every other PE.
- `tools/modelcheck/carrier_model.py`, the goodbyes' two wires a pair: rule 1 on each wire,
  judged as now by the control wire alone with helpers and by either without; and this model
  into the suite.
- Tests: callers on two nodes under `osoe-launch` and under Slurm with `-K1`, the job's status
  the lower carrier's, every caller's handler run, no 143; the tests that state the residual made
  to state the cure.
- The README's limit and "How a job ends"'s second limit, and design §3.15's carrier rules.

## For the author

**Build the single carrier?**

- **(a) Build it as modelled, before round ten** (T13), so that the read takes it in: the changes
  above, a layout change, the tests, and the README's limit gone. About a day; the abort path's
  tests re-run whole, under `osoe-launch` and the VM's Slurm.
- **(b) Leave it**, P1 closed with the README's limit standing: "Call it from one PE."

*Recommended*: (a). The shape it fixes is the ordinary one - every PE that detects the same
failure calls `shmem_global_exit` - under the `-K1` the README itself recommends, and today it
costs 143 and the callers' output. The cure is one word, one send rule and two wait rules, each
shown needed above. What changes for a program: the job's status is the lowest-ranked carrier's,
not the highest any carrier gave or 143 - still one of the callers', as §9.1.6 asks - and the end
takes one more goodbye's round trip and the outranked node's own end, its carrier's handlers
included, within the bounds as they are (`abort.h`: the lowest node's 20 s wait outlasts an
outranked carrier's 10 s grace).
