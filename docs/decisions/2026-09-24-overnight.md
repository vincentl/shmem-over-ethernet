# Decisions taken overnight, 2026-09-23 to 24

Vincent authorized an overnight run at about 22:30 EDT on 2026-09-23, ahead of
the next day's first experiments on the Raspberry Pi cluster: merges to main,
Slurm's daemons left running in the shared VM, and this record of every
decision point passed. Each entry says what was decided, why, and how to undo
it.

The plan, in his order of priority:

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
Assumed until he confirms: Raspberry Pi OS Trixie, so kernel 6.12 and Slurm
24.11, as in the VM.

## Where things stand

*(Kept current through the night.)*

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
