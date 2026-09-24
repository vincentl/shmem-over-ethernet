# Decisions taken overnight, 2026-09-23 to 24

Vincent authorized an overnight run at about 22:30 EDT on 2026-09-23, ahead of
the next day's first experiments on the Raspberry Pi cluster: merges to main,
Slurm's daemons left running in the shared VM, and this record of every
decision point passed. Each entry says what was decided, why, and how to undo
it.

The plan, in the author's order of priority:

1. finish question 10 of the previous log - a failed send keeps receiving, so
   the goodbye ahead of a reset is read - and fix the README's stale line;
2. the Slurm path (§6.1), with Slurm installed in the VM, real `srun`
   environments captured as fixtures, and an adversarial review;
3. the Pi experiment kit: a benchmark against the floor, a runbook, a memory
   check, and a dress rehearsal at tier 3 as 4 nodes of 4 PEs;
4. the exit follow-up (the previous log's 22a): `exit` on the caller's PE,
   `fflush` and `_exit` on every other;
5. stretch: typed RMA - `shmem_TYPE_put/get`, `_p/_g`, `iput/iget`, and the
   `_nbi` forms as the blocking ones;
6. stretch: the watchdog's retirement - the ending agent names the routine its
   PE was blocked in, and `OSOE_WATCHDOG` and `OSOE_LIVENESS` go.

**The cluster**, for the runbook: all Raspberry Pi 5; a head node with 8 GB and
a 512 GB NVMe; four compute nodes, `pi-001` to `pi-004`, 16 GB each, booting
from the head node over the network with no local storage, so every node sees
one filesystem; a PSw16-2GV3 PoE switch at 1 Gbit/s; Slurm as the launcher.
Assumed until confirmed: Raspberry Pi OS Trixie, so kernel 6.12 and Slurm
24.11, as in the VM.

## Where things stand

*(At about 01:45 on the 24th.)*

Everything the plan listed is on main, each piece verified on the tree it
merged, in this order:

1. question 10, a failed send keeps receiving (9d9c8ee);
2. the exit follow-up and the watchdog's retirement (3898de9);
3. the Slurm path - srun's names, fixtures captured from Slurm 24.11.5,
   `osoe-launch -s`, the review's fixes - with a host whose own name resolves
   to loopback listening where its peers can reach it (f66742b), and the
   README's notes on both merges (2a4cd2f);
4. the memory check's one finding, an `OSOE_HEAP_BASE` off a page boundary
   (8fadbc9), and a fatal the agent raises itself naming where its PE waited
   (c89bd6e);
5. typed RMA (bca06ef).

**The Pi kit** is `docs/pi-cluster.md`, the runbook, in order; `osoe-bench`;
and `tools/launch/capture-slurm-fixtures.sh`. The dress rehearsal passed every
step (entry 18): at tier 3 as four nodes of four PEs under srun's names on the
Pi-like, lossy and small-MTU links, and the runbook's own srun steps through
the VM's Slurm, the whole test suite inside one step among them.

**For the Pi day**, the runbook's step 3 first: its captures replace the
four-node fixtures written from Slurm's documentation (entry 6), and one that
fails is a finding about the bootstrap or §6.1. Then the smoke test, the floor
and the benchmark, which measures Slurm's default binding against none
(entry 9).

**Open for the author:**
- entry 20: should a fatal on a lead leave the ending to its agent, as a
  global exit does, so the other nodes hear the abort rather than a death?
- entry 16: the strided forms cost a round trip an element across nodes until
  a strided primitive exists; ibput and ibget stay deferred.

Nothing in the plan was left undone.

## Question 10: a failed send keeps receiving

**1. The model found a hang in the first form of the fix, and the close now
follows the same rule.** The first form stopped a node's sends on the peer's
reset and judged the peer at its stream's end. The whole model then went
STUCK in its crash cases: a peer that crashes during the close, after its own
goodbye and before reading this node's, leaves this node waiting to send a
goodbye nobody will read - where the engine as it was had ended the node on
the failed send, which for a crash is the right status. Now the close treats a
connection whose sends met the reset as owed nothing, and done once the peer's
goodbye and end of stream have arrived. *To reverse:* nothing to reverse - the
first form was never merged.

**2. The model takes a failed send only while the node is not aborting.** With
the step allowed at any moment, the fatal case grew from 5.7 to 17 million
states, past the liveness check's bound of 12 million, so its liveness went
unchecked. An aborting node sends an exited peer nothing more either way
(`flush_aborting` counts it done), so the step changes nothing there but the
count: restricted, the case is 11.8 million states and checked. It is near the
bound, which the next change to that model should watch.

**3. The tier-3 two-buffer tests stay, though they do not pin the fix.** With
the fix undone, the tier-2 test fails ("the connection to node 0 failed:
Broken pipe") and the three tier-3 ones pass: the shaped links' timing does
not starve the ring the way loopback does. They are kept as coverage of the
fix on real-shaped links, and the commit says which test pins it.

## The Slurm path

**4. The bootstrap reads srun's names when OSOE_PROCID is unset, and never a
mixture.** §6.1's names are read as srun sets them or as osoe-launch mirrors
them; `OSOE_PROCID` decides, and every name is then read with that one
prefix. A test that `osoe-launch` starts inside a Slurm allocation has both
sets, and must be the launcher's job, not the allocation's. *To reverse:*
nothing to reverse - there was no Slurm path.

**5. Slurm 24.11.5 installed in the VM, as the author decided; the daemons
stay running.** One slurmctld and one slurmd, task affinity, `MpiDefault=none`
(`build/scratch/step3/slurm/install-slurm.sh`). Its captures confirmed §6.1:
every task of a step has `SLURM_GTIDS` and the step names, so no fallback was
needed. A batch script's environment has `SLURM_PROCID` and `SLURM_GTIDS` but
no `SLURM_STEP_ID`, which is what the refusal keys on - checked against a
real capture, not only the written fixture it replaced.

**6. The four-node fixtures are written, provisional; the Pis' own captures
replace them.** The VM has one slurmd, and a container cluster for multi-node
captures would have cost the night; the author's cluster runs Slurm, and the
runbook's step 3 captures the real ones first thing.

**7. Test helpers read the launcher's names by the bootstrap's rule.** The
first `srun` run found the tests' own checks looking for `/osoe.0.0.0.0`:
`invariants.h`, `arrivals.h` and three tests read `OSOE_JOB_ID` and its kin
directly. `shmem/tests/launcher.h` gives them the rule; the tests below the
library, which only osoe-launch runs, keep theirs.

**8. A host whose own name resolves to loopback listens where its peers can
reach it.** Found reading the rendezvous for the Pis: a lead binds its
rendezvous listener to, and advertises for the mesh, its own host-list entry
as its host resolves it - and Raspberry Pi OS, as Debian, maps a host's own
name to 127.0.1.1. The root would have listened on loopback, and the job hung
at init. Now such a node - a name, not an IP literal, in a job of several
nodes - uses the address its host would send from toward another node. Tier 2
and 3 name IP literals and keep today's behaviour exactly. Only a tier-1 test
of the rule pins it; the real test is the Pis. *To reverse:* revert the
commit; the alternative is to require `/etc/hosts` fixed on the cluster.

**9. Slurm's task affinity pins each task to one core**, as the captures
show (`SLURM_CPU_BIND=...mask_cpu:0x1,0x2,0x4,0x8`): each PE's main thread
and agent share it. Not changed - it is the runbook's experiment, with
`--cpu-bind=none` beside the default.

## The exit follow-up

**10. Only the caller's PE ends with exit; the hand-off to a thread inside
exit is gone** (the author's 22a and 23a). A PE whose program called
`shmem_global_exit` is recorded before the declaration; its agent ends it with
`exit(status)`, every other PE's with `fflush(NULL)` then `_exit(status)`.
`_exit` takes no lock, so the third review's hand-off - a marked thread
inside exit ending the process with a second exit - had no case left, and
with it went the seq_cst pairing it needed. A caller whose declaration lost
to another's may go unseen by its agent and end as the others do: benign,
said in abort.c. The exit model's `exit_everywhere` is STUCK, the built case
passes.

## The watchdog retired

**11. The ending agent names the routine from a pointer, not the blocked
record.** The record is written with plain stores between two locked bumps,
for osoe-stat in another process; a thread of the same process reading it
would race those stores, which tsan would flag. A release store of the
routine's name (a string literal) beside it costs nothing a sleeping wait
notices.

**12. What went with the watchdog, said in the notes.** Every waiting PE
checked every sibling, so a lead alive but wedged still had its siblings'
deaths found; now only the lead's agent watches them. And the exit model loses
the watchdog's three variants; the routing of a fatal that loses to a global
exit stays in fatal.c, no longer modelled.

**13. unswept-wait watches the sweep count.** Without the watchdog, a
quiet's sweep no longer finds a dead sibling - the agent does - so the test
that exists to show an internode quiet sweeps now reads osoe-stat's sweep
count rising while PE 0 waits in `shmem_quiet`.

## The Slurm path's review

**14. Slurm's own steps and heterogeneous jobs are refused, not guessed at.**
The reviewer found both unread by §6.1. A step id of 0xfffffff0 or above is
one of Slurm's own - the batch script, the extern step, an interactive step -
never one `srun` started for the program. A heterogeneous job's tasks carry
names per component that §6.1 does not describe. Each is refused by name, with
a written fixture. *To reverse:* read them, once a capture shows what they
carry.

**15. The review's own fix broke the strict-C11 build, and the reviewed code
stays.** Renaming osoe-launch's `names` to `prefix` shadowed a local of that
name - a warning, which the strict-C11 check fails on - in all four presets;
it is `launcher_prefix` now. Chasing it, I rewrote the own-address rule's
literal test in plain C, on the guess that `inet_pton` in a header was what
strict C11 refused. A one-file compile showed it was not, and the reviewed
`inet_pton` form is back. Four more test cases pin what counts as a literal.

## Typed RMA

**16. Typed RMA is merged as the subagent wrote it, with four changes.** Its
draft was complete - Table 5's 24 types and the five sizes, each with put,
get, p, g, iput, iget and the nonblocking forms, the typed put-with-signal,
the C11 generics, 566 routines - and three things in it were wrong, one
stale:
- *Diagnostics named the wrong routine.* Every typed routine called
  `pshmem_ctx_putmem`, so a bad PE in `shmem_double_put` was reported as
  `shmem_ctx_putmem`'s. The byte bodies now take the routine's name and the
  element width (`rma.h`), and a count of elements whose bytes a `size_t`
  cannot hold is fatal instead of wrapping.
- *The test raced itself.* In its generic and sized blocks a PE wrote its
  neighbour's copy again while the neighbour might still be checking the
  last value. A barrier now stands before each such write.
- *`shmem_put_signal_nbi`'s C11 generic was left out*, which the spec
  gives; added and tested, with the sized put-with-signal.
- *The nonblocking rationale was stale*: it said osoe had no backend but the
  intranode one. The notes now say why the blocking code is a conforming
  nonblocking form on both backends - a put returns at local completion and
  completes at quiet, which is `put_nbi`'s contract, and a get may complete
  before it returns - and what a real nonblocking get would add: requests in
  flight together, through the flag §4.10 reserves.

## The Pi kit

**17. The memory check found one thing, and it is fixed: an `OSOE_HEAP_BASE`
off a page boundary.** A subagent audited every mapping for a page-size
assumption, and I checked what it reported. Every mapping takes the page from
`sysconf`, and only the heap needs a page multiple - at a base that must be
page-aligned, which `OSOE_HEAP_SHIFT` beside it was checked for and
`OSOE_HEAP_BASE` was not. An unaligned base failed to map and was reported as a
base not free. A Pi 5's pages are 16 KiB, so a base chosen on a 4 KiB machine
can be unaligned there; it is refused by name now, with a test. The defaults
fit easily: a node's four PEs hold about 1 GiB of `/dev/shm`, touched on
demand, against a 16 GB node's 8 GB. The runbook says both. *To reverse:*
nothing to reverse.

**18. The dress rehearsal runs the runbook's own commands.** At tier 3, four
namespaced nodes of four PEs on each of the three link shapes, under srun's
names (`osoe-launch -s`), with the runbook's settings: the internode test
block and cyclic, the benchmark, a global exit and a dead PE; the floor and the
full benchmark on the Pi-like link. Then steps 1, 2 and 4-7 through the VM's
own Slurm, one node: the node checks, `probe`, the whole test suite inside one
step of four cores, and `srun`'s own jobs, where a global exit must reach
`srun` as the caller's status (`build/scratch/step3/rehearsal/rehearse.sh`).
It proves the kit runs as written; on four vCPUs it measures nothing.

**19. The Slurm branch's last form was verified merged, not alone.** After the
strict-C11 fix, a run of its own would have repeated one that had passed
everything else. Main, the Slurm branch and the README's notes were merged in
a scratch worktree instead and verified there, rehearsal included; main moved
only when that passed, to that tree exactly, with each branch's commits
rebuilt on their own trees under their final messages. The exit and watchdog
branch merged the same way, on the tree its own run had verified. *To
reverse:* nothing to reverse.

## A test that failed once

**20. The agent's own fatal now says where its PE was waiting; whether a
fatal should wait for the agent's goodbye is a question for the author.**
Typed RMA's first run failed one test it does not touch, once, in the debug
preset: `shmem.death-in-wait.barrier.0-0-1-1`, from the watchdog's
retirement. Rank 3 died; node 1's lead - its main thread sweeping into the
abort its own agent had just declared - ended the process at once, as a fatal
does (§3.15), before the agent's abortive goodbye went out. Node 0 then read
node 1's end of stream as a death and ended on its agent's own fatal, which
said why and not where PE 0 had been waiting, so the test, which requires
every survivor to name its routine, failed. Every PE still ended with the
failure: that behaviour is the design's, and the model's (`waiter`: "a fatal
ends it at once"). The fix is to the message: a fatal the agent raises itself
now says where this PE's thread was, as the ending agent does
(`shmem.fatal-order` pins it), so the survivor names its routine either way.
*The question:* on a lead whose agent runs, a fatal could leave the ending to
the agent, as a global exit does (`osoe_abort_end`), so that the other nodes
hear the abort and not a death - which §3.15 already says on-demand
connections will need. It changes the model's waiter and wants a model run;
not done overnight.
