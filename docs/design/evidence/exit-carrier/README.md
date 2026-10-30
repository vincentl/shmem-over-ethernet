# P1: the global exit's single job-wide carrier, modelled (2026-10-29)

The model behind `docs/decisions/2026-10-29-exit-carrier.md`: the proposal it
checks, and what each run found. A proposal only - the build is the author's
call - so the suite's model, `tools/modelcheck/abort_exit_model.py`, is
unchanged, and this directory holds a copy of it with the proposal's variants.

| File | What it is |
|---|---|
| `abort_exit_model.py` | The suite's model of how a process ends on an abort, with P1's proposal as the variants `single` (the rules) and `single_*` (each rule broken), the cases `both_lead_a`, `both_lead_b`, `three_lead_b` and `three`, and the property ONE_CARRIER - its docstring's last section says which rule each variant breaks |
| `engine.py` | The suite's model checker, copied unchanged so that the model runs from here |
| `single-cases.txt` | The proposal's twelve cases as first run - the residual's, the two with a lead calling, and every case the built rules pass, each under `srun -K1` but one: eleven hold, and `three`, with the pidfd option, stopped at the model's half hour after 42 million states without a violation |
| `three-lead-b-cases.txt` | `three_lead_b`, three callers on the two-process nodes - A's PA, and B's lead and PB - complete |
| `three-cases.txt` | `three` again without the pidfd option, the lead's ring's completion for a sibling's death, which none of the proposal's rules uses: the carrier's and the lead's waits check their PEs' ends themselves, as `osoe_liveness_outlast` polls their pidfds |
| `single-breakages.txt` | The six breakages, each failing as its variant's line says |
| `long_three.py` | `three` with the proposal's options and four hours, run alone |
| `old-cases.txt` | The suite's 29 cases run from this copy: every verdict and state count the same as the suite's copy gives, so that the proposal's lines change nothing they do not name |

Run from this directory: `python3 abort_exit_model.py single` for the
proposal's eighteen, `python3 abort_exit_model.py` for all 47, and
`python3 long_three.py` for `three`.

**The residual** (round four's conformance F1; the README's limit "PEs of more
than one node calling `shmem_global_exit` at once can lose a caller's output").
Callers on two nodes each win their own node's word, each carries, and each
node's lead excludes the other's end, since a node whose goodbye named another
caller carries its own (`no_exclusion` shows why: each would wait for the
other). So the first carrier to leave is a bad exit while the other node still
runs, and `srun -K1` kills what is left of it: the suite's
`built [both; kills,pidfd]` fails NO_KILL.

**The proposal**: one carrier for the job, the lowest-ranked of the nodes'
carriers.

1. **Tell the higher.** A carrier's node sends its abortive goodbye to a peer
   whose own has arrived when that goodbye named a caller ranked above this
   node's carrier. The code sends nothing there now (`flush_aborting`'s
   `peer_aborted`: "nobody to tell"). Every other send is the code's.
2. **Outranked.** A carrier's node whose lead reads a goodbye naming a caller
   ranked below its carrier sets one more word in the node's segment, then
   waits for no other node. Its lead's end waits for every other PE of the node,
   the carrier among them. The carrier rereads the word at each poll of its
   wait (`osoe_liveness_outlast` polls every 100 ms), waits for nobody, and
   leaves with 0 by `exit`, its handlers run, as a caller whose declaration
   lost does.
3. **The lowest waits for all.** A carrier's node not outranked waits for every
   other node's end, none excluded, so the job's carrier leaves last.

**The results.** The suite's 29 cases are unchanged (`old-cases.txt`). Under the
proposal the residual's `both` holds - 1,766,635 states, the trapped-state check
included - where the built rules fail NO_KILL in 37; so do `both_lead_a`
(2,520,775) and `both_lead_b` (2,409,391), every case the built rules pass
(35,511 to 8,350,440), and `three_lead_b`, three callers, two of them on the
outranked node and its lead one (24,570,080, the trapped-state check skipped
past 12 million states, as the engine does). `three` found nothing in 42.2
million states with the pidfd option and 32.6 million without, each at the
model's half hour, so it is not exhaustive; `long_three.py` gives it four
hours alone. The six breakages fail as their lines say, in 35 to 42,012 states.
