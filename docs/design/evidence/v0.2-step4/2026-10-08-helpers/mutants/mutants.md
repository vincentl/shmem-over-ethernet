# Part 3's mutants

Each mutant is the tree with one change - the defect it stands for - built in
debug and run against the tests of what it breaks (`mutants.sh DIR`, the tests
named in each mutant's `.regex`, its environment in its `.env`;
`make-mutants.py` makes the first rounds' trees, `checks-5.sh` and `checks-6.sh`
the last rounds' and run them). A mutant every test passes is a gap, said in
the record and closed by a test where one could be written.

| Mutant | The change | Round | Caught by |
|---|---|---|---|
| `deaf-unwatched` | the agent never ends the job for a helper that has turned twice since its stop without answering (the check of its turns off) | 1 | `shmem.helpers.deaf.0-1`: the job hangs to the launcher's timeout instead of ending by name |
| `helpers-counted` | the deadlock detector counts the helpers' tids as the PE's threads | 1 | `shmem.deadlock.{waits,reinit,mismatch}.*` at two helpers: no deadlock is ever declared |
| `unsent-agent-only` | only the agent takes the unsent-limit request; the helpers' wires keep the old limit | 1 | `shmem.unsent.{set,by-hand,floor}.*` at two helpers |
| `wake-other-owner` | a writer's wake goes to an owner other than its wire's | 1 | `shmem.helper-wake.0-1-2-3` |
| `abort-without-helpers` | an ending node's agent does not wait for its helpers' goodbyes | 1 | none of 65 abort tests at two helpers; then (round 3, as `no-wait`) `shmem.helpers.abort-wait.0-0-1-1` |
| `no-abort-excuse` | a reader does not excuse an end of stream while its node's abort is declared | 1 | none of 65; none of 66 in round 3 (`no-excuse`); then (rounds 5 and 6) `shmem.helpers.reset-stream.0-0-1-1`, 10 of 10 runs, and `shmem.progress.reset-while-ending`, 5 of 5 |
| `no-excuse-no-wait` | both of the above | 2 | none of 65: each mechanism covers the other's absence in every abort test there was |
| `unagreed` | the pacer's mix stops by each PE's own clock (the code before `bc0751d`) | 4, 5 | `tools.pacer.mix-end.0-0-0-0`: with PE 1 two seconds late, once (round 4); with PE 1 an hour late, 3 of 3 runs (round 5) |
| `unfixed-flush` | `flush` and `flush_control` offer a segment on a wire that has failed (the code before `232278a`) | 5, 6 | `shmem.progress.reset-while-ending`, 5 of 5 runs ("the connection to node 1 failed"); `shmem.helpers.reset-stream.0-0-1-1` 1 of 10 |

Round 1 ran on the C as first committed (`64f48c4`); rounds 2 and 3 on the
first read's answers; round 4 on the pacer's fix; rounds 5 and 6 on the second
read's answers (`232278a`). Every round's fixed tree passed the same tests.
