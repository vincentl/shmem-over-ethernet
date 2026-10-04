# Plan: the fixes between the round-three review and the first release

Written 2026-10-01 in answer to `phase1-release-fixes-prompt.md`, for the
author, who has not read the review. For each finding it says what happens,
how that was checked, what could be done and what this plan recommends.
Nothing has been changed or committed: the six review files and the brief are
still untracked, and this file is too.

**How the findings were checked.** Each item below says which of these it
rests on:

- *run today*: reproduced by this session in the review's VMs, on the review's
  build of `b0e0b53` (scripts and their logs in
  `build/scratch/review-r3/refutation/lead/`);
- *refuted and held*: today's refutation pass - six fresh subagents, each
  given one group of findings and asked to refute them
  (`build/scratch/review-r3/refutation/*.md`);
- *read*: the cited source and README lines, read at `b0e0b53` by this session.

**The result.** All sixteen hold. Fifteen points correct, sharpen or add to
what the review and the brief say: three change a fix the brief proposes
(3, 4 and 5 below), one is a limit neither mentions (2), and one is a
sentence of the specification both quote without its condition (15). §1 lists
them; §2 is what needs the author; §3 and §4 are the findings; §5 is the
order of work. A fresh subagent then checked this file's own citations and
numbers against the tree; its ten corrections are in.

## 1. What the validation changed

| # | Finding (the review's label) | Checked | Result |
|---|---|---|---|
| 1 | The C11 forms `shmem_signal_add(ctx, ...)` and `shmem_signal_set(ctx, ...)` do not compile (C1) | run today | holds |
| 2 | `shmem_global_exit` under `srun -K1` kills the caller inside `exit` (C2, R5) | run today, under Slurm | holds; the caller has about 100 ms, not 50 |
| 3 | A start-up failure leaves the other PEs waiting in silence (R8); no test holds the silent-peer check's gate (K5) | run today (two cases; the refuter four); the gate by the refuter's mutation | holds; the brief's sketch of the fix does not match the code |
| 4 | A node that cannot reach a peer prints nothing (N1) | read | holds |
| 5 | The rendezvous acts on a hello before the secret is proved (N2) | read | holds; the brief's time bound would itself end healthy jobs |
| 6 | `shmem.floor`'s stream pin (R2) | read, the lead's logs, two idle runs by the refuter | holds as a thin margin; seen failing only under `ctest -j4`, which is not how the README's command runs it |
| 7 | Three things the scaling table and Limits omit (P1, P2, P3) | P1 run today; P2 and P3 read | hold |
| 8 | `OSOE_MAX_THREADS` described wrongly, twice (C3) | read | holds |
| 9 | Eight behaviours the Limits do not list (C4) | read for four; the investigators' runs for the rest | all eight are real and unlisted; three are not departures |
| 10 | The README's requirements, first example and Slurm advice (R5a, R4) | read | hold; one settings row lacks a range, not three |
| 11 | Private paths, a private page, notices, SPDX lines (R1) | `git grep` today | holds; the counts are slightly off |
| 12 | Two README passages on RDMA (R3) | read | hold |
| 13 | The README's model-checking sentence (K6) | read (28 models, the engine) | holds, and item 16 is its example |
| 14 | x86-64 and mixed architectures (X1, X2) | read (the README names no architecture) | hold; the Rosetta runs were not repeated, and no mixed job was ever run |
| 15 | Two Pi numbers (P9, P10) | the investigator's tables against the records | supported; not re-run (the Pi was rebuilt on 2026-09-28) |
| 16 | The torn "blocked in" record (K1) | read; litmus re-run by the refuter | holds, now reproduced twice |

The fifteen points, each carried into its item below:

1. **Global exit, the window.** The caller gets about 100 ms under `-K1`
   (five runs, 100.6-106.7 ms), where the brief says about 50.
2. **Global exit, a second bound nobody mentions** (*run today*): under a
   launcher that kills nothing (`osoe-launch -k`), a caller whose `atexit`
   handler takes 12 s is cut off by the library's own grace
   (`abort.c:53-74`) - the job ended at 10.0 s, the handler unfinished; a 3 s
   handler finishes. The README promises the handlers run and says nothing of
   10 s.
3. **Global exit, the fix.** The brief has remote nodes wait for "a second
   goodbye". None is needed: the end of the stream that follows the abortive
   goodbye already says the caller's node is gone (§3.1).
4. **Start-up, the fix.** The brief would publish a failure in "the job
   object, which every PE opens first". The job object is written by the lead
   *after* the socket rendezvous (`job.c:156-193`) and does not exist on a
   one-node job. What every PE opens first is the control segment, and the
   lead creates it only after the checks that can refuse (`node.c:611-631`).
5. **The secret, the fix.** The brief would bound the confirmation read by
   the hello's 0.1 s. A caller that has been answered believes it is
   connected; if the answerer has given up meanwhile, that healthy caller ends
   the job. That is a wait giving up because time has passed, which design
   §3.15 rules out (decided 2026-09-20); see §3.2c here.
6. **The secret, the attack.** The refuter left open whether the session
   number in a hello is guessable. It is a counter from 0 (`init.c:274`).
7. **The floor pin.** The failure (0.0999 against 0.10) was under `ctest -j4`.
   The README's command runs the suite one test at a time, so "fails the
   README's first command" was not observed. One idle run at 0.103 shows the
   margin is thin all the same.
8. **The default build's hour, measured.** The review says the README's
   command takes "the better part of an hour". From the lead's logs: the
   suite is about 6 minutes of test time (summed from a `-j4` run), and the
   24 models are 55 more, three of them 45 (`model.abort` 18 min,
   `abort_exit` 17, `tree_barrier` 11).
9. **The repository.** Four fixtures carry the VM's address and five its host
   name. Three of the 61 files are review prompts, outside the evidence
   trees. The new readiness review quotes the path itself and becomes the
   62nd file once committed.
10. **Settings rows.** One lacks its range (`OSOE_WIRE_BYTES`,
    `OSOE_WIRE_BUFFERS`), one its floor (`OSOE_AM_BUFFER`), one a note
    (`OSOE_MAX_THREADS`); the review says three lack ranges.
11. **The unlisted eight.** Silent mismatched collectives, exit without
    finalize and the 100 ms wake are behaviour on undefined input, on a
    non-conforming program and of a conforming routine: worth listing, not
    departures.
12. **XRC against DC.** DC is a later review's recommendation, at medium
    confidence and from recalled device limits, not a recorded decision; and
    the review's claim that the README's `RemoveIPC=no` advice switches off
    the cleanup is overstated - `RemoveIPC` was never a cleanup for Slurm
    jobs.
13. **The silent-peer test** needs a stop of at least 15 s with 64 MiB in
    flight; at the 10 s the review suggests, the mutation survives 3 of 3.
14. **The per-number cost** is about 200 KiB (the README says 160, which
    predates the scans' area), and a job whose nodes lose their team exits 0
    (*run today*).
15. **Global exit, the specification.** The review and the refuter quote
    §9.1.6's "the exit status returned to the environment shall be one of the
    values passed" as if it bound every global exit. The sentence begins "If
    more than one PE calls `shmem_global_exit`". For one caller the text that
    applies is that the routine passes its status to the environment and
    "flushes I/O ... in accordance with C/C++ language requirements for
    normal program termination". A caller killed inside `exit` does neither,
    so the finding stands on those two.

## 2. What needs the author

The first three change what is built. The rest have a default this plan takes
unless told otherwise; the author read them on 2026-10-02 and struck none
(`docs/decisions/2026-10-02.md`, item 7).

| | Decision | Options | Recommended |
|---|---|---|---|
| D1 | `shmem_global_exit` under a launcher that ends a step on a bad exit (§3.1) | only the caller carries the status; hold the other PEs until the caller has ended; or state the limit | **decided by the author, 2026-10-02: only the caller carries the status** |
| D2 | How far the start-up fixes go before the tag (§3.2) | notices and corrected sentences; plus a failure on one node ends that node; plus a refusing node tells the others | the middle one - **confirmed by the author, 2026-10-02** |
| D3 | Team numbers (§3.3) | the README sentences; a per-node range; numbers held by each member PE and given back at destroy; and on top of that, counters per team with the buffers per PE | **decided by the author, 2026-10-02: numbers per PE, then counters per team with the buffers per PE, both before the tag** - the design is `docs/design/team-resources-proposal.md`; the README sentence meanwhile |
| D4 | The floor test's pins (§4.1) | checked only when asked; widened; explained | only when asked |
| D5 | `OSOE_AGENT_SLICE_US` default (§4.10) | keep 100; or 0 | keep 100, both sides documented |
| D6 | `OSOE_MAX_THREADS` (§4.3) | default 16 or larger; compared PE by PE or the first PE's wins | keep 16; compare |
| D7 | What the README's first command runs (§4.5) | everything, an hour; or the models under a label | a label |
| D8 | The version at the tag (§4.5) | | 0.1.0, if the author has no other |
| D9 | The evidence trees (§4.6) | scrub the paths or leave them; SPDX lines on 154 files or a narrower sentence | scrub; add the lines |

## 3. The three findings that matter

### 3.1 `shmem_global_exit` under `srun -K1` (C2, R5)

**What happens.** A PE calls `shmem_global_exit(7)`. The library ends that PE
with `exit(7)`, so its `atexit` handlers run and its streams are flushed, and
ends every other PE at once with `_exit(7)` (`abort.c:109-113`). Under
`srun -K1` Slurm treats the first task to exit nonzero as a failure and
terminates the step. The other PEs are gone within tens of milliseconds, so
about 100 ms after the call Slurm's SIGTERM reaches the caller, still inside
`exit`. A handler that needs longer never finishes, output still in a buffer
is lost, and `srun` returns 143, not 7.

**Why it matters.** The README recommends `-K1` (line 173) and promises that
the caller's "`atexit` handlers run and its streams are flushed, as the
specification asks" (615-617). §9.1.6 has the routine pass its status to the
execution environment and flush I/O as normal termination does; neither
happens for a caller killed inside `exit`. (Its one "shall", about the
status, is for several callers: point 15 of §1.) It is the README's advice
against the README's promise.

**Checked** (*run today*, the VM's Slurm 24.11, 4 PEs on one node):

| Launch | Caller's handler | `srun` returns | Handler finished | stdout flushed |
|---|---|---|---|---|
| `srun -K1` | 0 ms, 50 ms | 7 | yes | yes |
| `srun -K1` | 200 ms | 143 | no | no |
| `srun` | 200 ms | 7 | yes | yes |
| `osoe-launch -k` (kills nothing) | 3 s | - | yes | yes |
| `osoe-launch -k` (kills nothing) | 12 s | - | **no**: the job ended at 10.0 s | yes |

Every `srun` row was run at least twice; every rank's own status was 7 in the
last two rows. The last row is point 2 of §1: the library ends the caller
itself after `OSOE_ABORT_GRACE_SECONDS`, 10 s. No test sees any of this:
`shmem/tests/global-exit.sh` always passes `-k` (lines 24-27), and
`osoe-launch` reports a failed rank of any status as 1 (`launch.c:517`).

**Options.**

- **A. Hold the other PEs until the caller's process has ended.** Each other
  process waits for one thing before its `_exit(status)`:
  - on the caller's node, for the caller's process to end, by its pidfd. The
    lead already holds one for every local PE; any other PE holds one for the
    lead only (`liveness.c:42-53`) and opens one for the caller from the pid
    in its slot;
  - on another node, the lead waits for the stream from the caller's node to
    end - the abortive goodbye already tells a peer that the end of stream
    behind it is not a death (`progress.c:517-518`) - and its node-mates wait
    for their lead's pidfd, which they hold.

  Every wait is bounded by the grace the caller's own `exit` already has, so
  nothing lasts longer than today's worst case. No new message and no new
  shared word. The costs: the other PEs keep computing until the caller's
  `exit` completes, at most 10 s; and it changes when a process ends in a
  protocol that is model-checked, so `abort_exit_model.py` comes first. That
  model already takes 17 minutes in the review's VM, and its `caller_exiting`
  case (the caller's `exit` waiting on glibc 2.41's exit lock) is where the
  hold lasts the whole grace. A day to a day and a half with the model, the
  tests and an adversarial read.
- **B. State the limit.** Beside `-K1` and in "How a job ends": under a
  launcher that ends the step at the first nonzero exit, a nonzero global exit
  leaves the caller about 100 ms; flush before calling. Minutes. Nothing in
  the README is then false, and the recommended launch line still returns 143
  to any program with a slow handler.

- **C. Only the caller carries the status** *(added 2026-10-02, after the
  author asked whether this can be made reliable at all)*. Every other PE
  leaves as it does today - at once, flushing, `_exit` - but with status 0.
  `KillOnBadExit` reacts to "a non-zero exit code" (slurm.conf), so the step
  is not terminated until the one bad exit there is, the caller's own, after
  its handlers and its flush; and `srun` returns "the highest exit code of
  all tasks", which is the caller's. Nothing waits for anything, the abort
  protocol and its model's interleavings are unchanged, and there is no race
  with the launcher to lose. Two lines in `abort.c` (the `_exit` and the grace
  thread's status), the model's STATUS property restated, `global-exit.sh`'s
  expectation, and the README's "ends every PE with `status`".

  *Measured* on a tree with those two lines changed
  (`build/scratch/global-exit-zero/`, the VM's Slurm):

  | Launch | Caller's handler | Launcher returns | Handler finished | stdout flushed |
  |---|---|---|---|---|
  | `srun -K1`, 4 PEs | 0 ms, 200 ms, 1 s, 5 s | 7, each twice | yes | yes |
  | `srun -K1`, status 0 | 200 ms | 0 | yes | yes |
  | `srun -K1`, the caller PE 1, 2 or 3 | 300 ms | 7 | yes | yes |
  | `osoe-launch` without `-k`, map `0,0,1,1` | 200 ms, 3 s | nobody killed; only "rank 0 exited with status 7" | yes | yes |

  The same shape with no library at all - three tasks leaving with 0, one
  running a 3 s handler and exiting 7 - gave `srun -K1` 7 three times of
  three, and 143 three of three when the three left with 7. On that tree the
  twelve `shmem.global-exit.*` tests fail on one line each, "1 of N ranks
  left with status 7", with every line written, no rank killed and no goodbye
  lost; the fatal, dead-PE and exit-without-finalize tests and both abort
  models pass.

  What it gives up: the other PEs' own exit codes say 0, so the status is in
  the job's code and the caller's, not in each task's. A caller that dies
  before its `exit` completes still leaves a bad status (a signal's), and a
  second caller on another node still exits with a caller's value. One limit
  to state: a site that sets `WaitTime`, or `srun -W n`, terminates the rest
  n seconds after the *first* task leaves, whatever its code; the default is
  never.

**Why not A after all.** A leaves a race it cannot close: once the caller has
exited nonzero, Slurm starts terminating the step, and every other PE must
notice the caller's end and leave by itself inside the same 100 ms. On a slow
node one is killed first, and `srun` returns 143 again, since it reports the
highest code or signal. C has no such window. Sandia's implementation does
neither: its global exit is `PMIx_Abort(status)`, which runs no handler at
all.

**Either way:** the README says the caller's `exit` has 10 s; and
`global-exit.sh` runs one case under a launcher that kills, checking the
launcher's own status, for which `osoe-launch` should report the ranks'
status as `srun` does (the highest exit code, or 128 plus the signal).

**Recommendation: C.** It is the smallest change and the only one of the
three with nothing left to race; half a day with the model's property, the
tests and the sentences. B's sentence about 10 s is still owed.

### 3.2 Start-up that hangs in silence (R8, N1, N2)

Three separate defects with one symptom: something went wrong while the job
was starting, and the PEs that survive wait forever and print nothing.

#### a. A failure before the node's shared segment exists (R8)

**What happens.** Start-up runs in this order: every PE reads its
environment; the node's first PE, the lead, creates the control segment; the
node's other PEs wait for it by name; then the leads meet over TCP. The way a
PE tells its node-mates "I failed" is a word in that segment
(`node.c:164-185`). A failure before the segment exists has nowhere to say
so:

| What fails | Who ends, with a good message | Who waits, silently |
|---|---|---|
| The lead refuses the job for `/dev/shm` space (`node.c:611-628`, before the segment is created at `:631`) | the lead | the node's other PEs |
| A setting only the lead validates is bad on **every** PE: `OSOE_MAX_THREADS=0` (`metrics.c:455`, called only from `node.c:512`), `OSOE_GET_LANDING_BYTES=1000` | the lead | the node's other PEs |
| One PE's own environment is bad | that PE | its node-mates |
| A PE exits before `shmem_init` | nobody: no library code runs in it | every other PE |
| One PE of node 1 has a different `SHMEM_SYMMETRIC_SIZE` | node 1's PEs, naming the variable | node 0, waiting for node 1's lead to call |
| `OSOE_JOB_SECRET` on one node only | the node that notices, naming both | the other node |

The README says a job whose PEs disagree "ends naming the variable"
(497-500, 351-352) and design §6.2 says it "ends the job naming both". The
PE that notices ends; the job does not. The suite cannot see it: the refusal
tests launch without `-k`, so `osoe-launch` kills the survivors, and
`srun -K1` hides it the same way - but the README's first `srun` example has
no `-K1`.

**Checked** (*run today*, twice, `osoe-launch -k -t 8`): `OSOE_MAX_THREADS=0`
on both PEs of one node - PE 0 exits naming the variable, PE 1 waits until
the launcher's timeout kills it; one PE of node 1 with a 1 GiB heap - both
PEs of node 1 end, that PE naming the variable and its node-mate the variable
or "another local PE failed", and node 0 waits to the timeout. The refuter
ran four cases, the investigator the whole table.

**Options**, each adding to the one before:

1. **Every PE validates every setting.** Move the two checks only the lead
   makes to where every PE reads its environment. A bad value given to all
   PEs is then refused by each of them. It also lets `OSOE_MAX_THREADS` be
   compared PE by PE, as `OSOE_MAX_CONTEXTS` is (§4.3).
2. **The segment exists before anything can refuse.** The lead creates the
   control segment before the `/dev/shm` check, so that refusal goes through
   the `failed` word the later ones already use; and a PE that refuses its
   own environment first gets the segment - the lead creating it, any other
   PE opening it, bounded by the abort grace - to set the word. Its
   node-mates then end with "another local PE failed", as they do today for a
   failure a moment later.
3. **A notice from every start-up wait** that has lasted 30 s, repeated each
   minute, saying what it waits for - shared with (b). It names the rank that
   never called `shmem_init` and the node that never called; it ends nothing,
   which is §3.15's rule.
4. **A refusing node tells its neighbours in the rendezvous tree**, so the
   other nodes end too: a new purpose for the hello, sent on the way out of a
   fatal, and a case for `rendezvous_model.py`.
5. **Tests**: the refusal tests run with `-k` and require every PE of the
   node to end (one node), or the notice (several).
6. **The sentences**: the README and §6.2 say which failures the library ends
   by itself and which need the launcher, and the first example gets `-K1`.

The least that makes the README true is 3 and 6, half a day.
**Recommendation: 1, 2, 3, 5 and 6 before the tag** (a day); 4 after it. The
`/dev/shm` refusal is the likeliest first-day failure of all - sixteen PEs at
the default heap size need about 4.2 GiB there, more than an 8 GiB machine
gives `/dev/shm` - and with 2 it ends the node instead of hanging it. What is
left for the launcher is then two cases, both named by the notice: a PE that
never reached the library, and other nodes.

#### b. A node that cannot reach a peer (N1)

**What happens.** A lead dials its rendezvous parent and then every higher
node's mesh port, and retries a failed connection every 0.2 to 10 ms forever
(`listener.c:435-481`, `:550-598`). The connect error is thrown away
(`dial`, `:409-433`). With a firewall on the mesh's kernel-chosen ports, a
wrong address, or a name that resolves differently on another node, the job
hangs with nothing on stderr. `OSOE_NO_PROGRESS_TIMEOUT` ends it, with a
message that gives a node number and a port but no host, address or error.

**Checked**: *read*. The investigator measured the three causes in network
namespaces; the code is unambiguous and they were not repeated.

**Fix.** The notice of (a)3, naming the peer's host-list entry, the address
dialled, the port or ports, and the last error ("Connection refused", "timed
out", "no answer to the hello"). It never gives up. There is no real
alternative.

**One choice the brief makes that this plan does not.** The brief also binds
the rendezvous listener to every address, as the mesh's listener is. That
removes one hang (a node whose name resolves to a different address
elsewhere) and opens the rendezvous port on every interface of a multi-homed
node, which sits badly beside (c). **Recommendation**: not before the tag;
the README gains the sentence that a node's name must resolve to the same
address on every node - today it says only that every node must resolve the
names (169-170) - and the notice names the address dialled. The review itself
rated the bind after release (N4).

#### c. The rendezvous acts on a hello before the secret is proved (N2)

**What happens.** A lead's listener handles a hello in this order
(`listener.c:361-400`): a wrong job, step, session or node is answered "not
this node"; a wrong task or node count is **fatal** (`:371-375`); a node
claim out of range is **fatal** (`:378-380`); only then is the answer sent,
the confirmation read **with no time limit** (`:389`), and the caller's proof
of the secret checked (`:395`). So with `OSOE_JOB_SECRET` set, a host that
can reach the port and knows the job id, the step id and a node number -
`squeue` shows them, and the session number is 0 - can:

- end that node's start-up with one hello carrying a wrong task count;
- stall it with one valid hello followed by silence: the accept loop serves
  one caller at a time, so the real peer waits behind it.

It cannot join the job: the proof still guards that. It is a denial of start,
open only while `shmem_init` runs. The README says the secret "keeps other
hosts out of the job's rendezvous".

**Checked**: *read*; it is the order the investigator's fuzzer found.

**Fix, three parts.**

1. **Prove first.** With a secret, check the counts and the node claim after
   the confirmation's proof. A genuine mislaunch still ends naming both
   sides, and the calling node now ends too - it sees its connection close -
   where today it retries forever. Without a secret nothing can be proved,
   and the order stays.
2. **An unconfirmed caller must not hold the loop.** Two ways:
   - *a time bound on the confirmation*, as the brief suggests. Five lines.
     But a caller that has been answered writes its confirmation and carries
     on (`listener.c:573-576`); it never learns whether the answerer waited.
     If the answerer gave up - the caller's lead was off its core for longer
     than the bound, which at 0.1 s can happen on a node running a PE on
     every core - the caller meets a closed connection and ends the job with
     "the rendezvous lost node N". The hello's own bound is safe only because
     an unanswered caller retries;
   - *serve unconfirmed callers side by side*: keep them in a small set
     polled with the listening socket. None blocks the loop, nothing is
     timed, and a full set answers "not yet", which callers already retry.
     About 70 lines in `listener.c`; the investigator's fuzzer
     (`build/scratch/review-r3/trust-kernel/`) becomes the test.
3. **A "Trust" paragraph in the README**: a private cluster network is
   assumed; the secret protects joining the job, not its availability and not
   the traffic after the handshake; without it, any host that can reach the
   ports can join.

**Recommendation**: all three, with the set for part 2. Half a day.

### 3.3 Team numbers: spent job-wide, never given back (P1, C4.1)

**What happens.** A program makes a team of each node's PEs with one line:
`shmem_team_split_strided(SHMEM_TEAM_SHARED, 0, 1, n, ...)`. That is a
separate split on every node, and every split claims its number from one
job-wide counter on world PE 0 (`team.c:813-840`), never reused. With the
default `OSOE_MAX_TEAMS=64`, 61 can be made. On a job of 62 nodes, 61 nodes
get their team and one gets `SHMEM_TEAM_INVALID`; which one is a race. The
loser is told on stderr and by the return value, but a program that ignores
the return runs its per-node collectives as no-ops there
(`collectives.c:111-113`) and the job exits 0. Raising `OSOE_MAX_TEAMS` costs
every PE about 200 KiB of address space a number, which the `/dev/shm` check
counts: 500 nodes of 256 PEs need about 25 GiB a node for it, and past 1,021
nodes no setting works. The README's team row and Limits give `split_2d`'s
2√N as the only cost of this kind.

**Checked** (*run today*, one PE a node on logical nodes): 61 nodes, 61 valid
teams; 62 nodes, 61 valid and 1 invalid; 64 nodes, 3 invalid; launcher status
0 each time.

**How limiting it is beyond that example** *(added 2026-10-02, when the
author asked)*. The per-node split is one way of three to meet the same two
properties: a number is spent job-wide, and it is never given back.

- *A program that makes teams as it runs.* Sandia's `shmem_team_reuse_teams`
  creates a team and destroys the one before, once per PE: two alive at a
  time. It passes at 16 PEs and fails at 70 (*run 2026-10-02*,
  `build/scratch/teams-limit/`: the 62nd split refused, launcher status 1).
  `shmem_team_max` asks for 64 teams and, when a split fails, destroys one
  and tries again - which Sandia's library rewards and osoe does not: 61
  made, every retry refused.
- *Silently.* `reduce_active_set`, `broadcast_active_set`,
  `collect_active_set` and `repeated_barriers` make one team per PE and take
  `SHMEM_TEAM_INVALID` to mean "this PE is not a member". At 70 PEs the last
  nine splits fail on every PE, the tests skip those iterations'
  collectives, and exit 0 (the first and last *run*).
- *Disjoint teams cost as if they overlapped.* G groups that share no PE
  take G numbers; a whole-job `split_2d` takes 2√N though each PE is in two
  teams; halving a job recursively takes a number for every team at every
  level, P - 1 in all, though each PE is in log P of them.

The applications in the reading corpus are untouched: the one that splits at
all (`deepep`, on NVSHMEM) makes one team at start-up. What the limit meets
is a program that creates teams in a loop, a library that makes a team per
call, anything hierarchical at scale - and the SOS suite itself on more than
61 PEs, which the sweeps so far, at 2, 4 and 8 PEs, could not show.

**Options.**

- **A. The sentences.** In the team row and the Limits: "every split claims
  job-wide numbers; a split made on each node - of `SHMEM_TEAM_SHARED`, say -
  spends one per node"; and 200 KiB for 160. Minutes.
- **B. A per-node range for teams that cannot leave their node.** A team on
  one node never puts its number on the wire - only a team with an internode
  phase calls `internode.h` (`barrier.c:60`, `collectives.c:198`), and the
  engine learns of a team from its first arrival, a peer's message naming it
  or its first internode data collective (`internode.h:191-195`), none of
  which such a team has - so its number need be unique only on its node, as
  design §10 already says. The simplest sound form: a split
  whose *parent* lies on one node (`SHMEM_TEAM_SHARED` and its descendants,
  the only teams that are not strided) claims from a counter in its own
  node's control segment, by CPU atomics, out of a range kept for it; splits
  of teams that span nodes stay exactly as they are. No wire change and no
  model change. Inside it sits one real choice: the range is either carved
  out of `OSOE_MAX_TEAMS`, which lowers the 61 job-wide teams and the largest
  whole-job `split_2d` grid (about 930 PEs today), or added on top, at
  200 KiB a number in every PE. A day, with `osoe-stat`'s labels and the two
  investigators' programs on 64 or more logical nodes.
- **C. The general form**: any split whose children land on one node - the
  world split node by node, `split_2d`'s rows - and §10's colouring. It needs
  the member set from the rank map at the claim and a mixed claim for
  `split_2d`. The record of 2026-09-26 puts it with a RoCE backend.
- **D. Numbers held by each member PE, and given back** *(added 2026-10-02)*.
  What a number must guarantee is narrower than "unique in the job": no PE
  may be in two live teams with one number, because a number indexes that
  PE's own state - its internal region's blocks, its published epoch, its
  descriptors. So the number becomes each PE's resource. Every PE keeps a
  mask of the numbers free *on it*; a split ANDs the masks of the new team's
  members in one small reduction over the parent - PEs outside the new team
  contribute all ones - and takes the lowest bit free on all of them; destroy
  frees the bit on each member as it leaves. Sandia's library picks a team's
  `psync` index exactly so (`src/shmem_team.c`: a `SHM_INTERNAL_BAND`
  reduction of `psync_pool_avail` over the new team's PEs, the bit set again
  in destroy; `SHMEM_TEAMS_MAX` is "Maximum number of teams per PE", at most
  64), and MPI libraries pick a communicator's context id the same way
  (*recalled*).

  *What it removes.* The limit becomes the live teams **one PE** belongs to.
  A job may hold any number of teams; a destroyed team costs nothing;
  disjoint teams share numbers. The per-node split takes one number, the same
  on every node; a whole-job `split_2d` two; a recursive halving its depth.
  No job-wide counter remains, and a PE that runs one thread is in one split
  at a time, so nothing races; under `SHMEM_THREAD_MULTIPLE` two threads of a
  PE splitting at once need the retry rule MPI libraries use.

  *What it keeps.* Offsets stay the same on every member of a team, so the
  collectives' protocols, their models and the internal region's layout do
  not change.

  *What it costs.* (1) The reduction, in place of the claim and the broadcast
  of the number: `data_collective_at` over the parent's spare words, as
  `osoe_team_broadcast_word` is today. (2) A node's block for a team can no
  longer be found by the number alone - two teams that share no PE may share
  a number on one node - so it is found through the team's local lead: a
  small table in the lead's PE slot, the blocks from a pool in the control
  segment. (3) The engine keeps its per-team state by block, and a barrier
  message names the destination's lead beside the number; it has a spare
  word (`am.h:145-150`). (4) Reuse: a block and a lead's counters are reset
  when a number is taken again, on an argument the models should carry -
  after a team's last sync, nothing addressed to it is in flight, since
  every write of every collective is waited for by its target before that
  target returns. (5) Handles that tell a destroyed team from the next one
  under its number.

  *Memory.* The internal region is sized by the multi-node teams one PE may
  be in at once - 16 would be 3.2 MiB a PE, where the 64 job-wide numbers are
  12.7 - and a team that stays on one node needs no block there at all, only
  its node's kilobyte.

  About two weeks with a model of reuse, the tests and a review; Sandia's
  two tests above, at 70 PEs, are its acceptance tests.

- **E. Counters per team, buffers per PE** *(added 2026-10-02, when the
  author asked why it was not the recommendation and accepted a rewrite for a
  better design)*. D decides whose a number is; E decides what a number
  costs, and sits on top of D rather than beside it. Today a number owns, on
  every PE, the slots its collectives' pieces land in - 160 KiB for
  reductions and broadcasts and 40 KiB for scans - and that is nearly all of
  its 200 KiB. Under E:
  - *a team keeps only counters*, with one small slot beside each - the
    position's arrival counter or the release counter - for a payload that
    fits it. A collective that small is one piece, and moves exactly as it
    does today, at today's latency;
  - *a PE keeps the large slots, one set per thread* (a "lane": the same
    rows of D slots a team number owns today). A lead binds its lane to the
    collective it is in and tells its tree neighbours where it is: its
    children by one message as it enters (a grant), its parent beside its
    first arrival, which it sends already. The pieces then move by
    `in_pieces`' rules unchanged - the window, the cumulative counters, the
    header check - with the slot's address taken from the neighbour's word
    instead of computed from the team number. The lane is free again at
    return: every write into a lead's slots is one that lead waits for
    before it returns, which is true of today's protocols and becomes a
    stated rule;
  - the gather has no slots already and does not change; the scans' loop is
    the reductions' and changes the same way.

  *What it costs.* A child's first large piece up waits for its parent's
  grant. A timing simulation of the loop
  (`build/scratch/teams-design/grant_timing.py`) puts that at one hop at
  most, once per collective, whatever the depth, the node count or the
  number of pieces: +1.00 hop when a leaf has its partial result at once,
  +0.70 when it takes 0.3 of a hop, nothing once it takes a hop - from 4
  nodes to 4,096 and from 1 piece to 64. Broadcasts pay nothing (a child's
  arrival already precedes its parent's data), and neither do gathers,
  barriers or anything that fits the small slot. The worst case is a
  reduction of one large piece on five nodes or fewer: three hops where two
  were. The size of the small slot is the dial: with 256-byte lines a
  number costs about 6 KiB a PE, with 1 KiB lines about 15.

  *What else is in it.* Under `SHMEM_THREAD_MULTIPLE` a lane per thread, or a
  wait for a lane could close a cycle across PEs: 160 KiB of address space a
  thread, touched only by a thread that leads a large collective. Every lead
  of one collective must use the same slot class, which the payload's size
  decides alike everywhere. `pipeline_model.py` and `scan_pipeline_model.py`
  gain the grant and the second class; the Pi's collective numbers are
  measured again before it merges.

  *Memory.* A PE reserves a lane per thread and about 6 KiB per number:
  about 3 MiB at 16 threads and 64 numbers, where today's 64 job-wide
  numbers are 12.7, and a program on one thread touches one lane. The count
  of teams stops mattering to memory, so the numbers a PE may hold can be
  256 or more.

  No scaling problem was found in it. Two more weeks after D, with the
  models and the measurements; nothing here is built or measured on
  hardware.

**Recommendation: A now; then D, then E.** D replaces B and C: B removes the
per-node case alone and leaves a job-wide number, never returned, for
everything else. D is the change of meaning - whose a number is - and makes
Sandia's tests pass at any size; E is the change of cost, and is what lets a
PE hold hundreds of teams in three megabytes. They touch different code -
allocation and lookup, then the pieces' addresses - so they are built and
verified one after the other, about four weeks together. If the first release
is to pass `shmem_team_reuse_teams` at any size, D goes before the tag.

*Decided by the author, 2026-10-02: D and E, both before the tag.* The design
is `docs/design/team-resources-proposal.md`, whose §8 lists what it still
asks the author; its slices get briefs of their own. A's sentence goes into
the README with the other release fixes (`task-release-fixes.md`, Part 4)
and stands until the team work replaces it.

## 4. The rest of the sixteen

"Fix" with no options means there is one sensible fix.

### 4.1 `shmem.floor` pins a ratio that depends on the machine (R2)

The test fails a release build if the library's stream over loopback runs
below 0.10 of a bare TCP echo's (`test-floor.c:83`, `:323`). That ratio is
one progress thread against the machine's loopback `memcpy`. In the review's
fresh VM it measured 0.0999 beside the rest of a `ctest -j4` run, and alone
0.103, 0.250, 0.282, 0.311, 0.321, 0.211 and 0.201. A stranger on faster
hardware sees a red test named "floor" on a library that works.

Options: check the three pins only when asked - an environment variable the
author's merge runs set - and print the ratios otherwise; lower the pin to
about 0.05; or explain it in the README. **Recommendation**: only when asked,
plus one README sentence. A pin is the author's regression guard, not a
stranger's install check; the project's other performance pin,
`env.echo-uring`'s, failed one idle run in ten before it was reworked on
2026-09-23.

### 4.2 Two scaling facts the README leaves out (P2, P3)

- **One thread a node carries all of its internode traffic.** The README
  says so; it does not say what follows: a node's message rate and bandwidth
  between nodes are one thread's whatever its PE count, "streams run at line
  rate" was measured at 1 Gbit/s, and the sharded threads design §3.12
  sketches for 256 PEs ("perhaps four") are not built. Over loopback in the
  review's VM the library streamed about 3 GiB/s idle where the bare echo
  streamed 14.5, and 0.5-1.6 against 5.2-8.4 beside a `-j4` suite run. Fix:
  those sentences under Limits, and that the per-message cost of one thread
  serving 256 PEs has not been measured on any machine.
- **The connection-buffer budget cannot shrink at the defaults.** The README
  says the buffers "shrink, saying so, to fit" `OSOE_AM_BUFFER_BUDGET`. The
  loop shrinks only above the floor of twice the chunk, and the default *is*
  the floor (`connection-area.c:118-123`): 1,025 nodes fit exactly and 1,026
  are refused. Where a raised `OSOE_AM_BUFFER` does shrink, every PE prints
  the notice (`:204-211`), 128,000 lines at 500 × 256. Fix: the row says "at
  the defaults a ceiling of 1,025 nodes; raise the budget or lower the
  chunk", and the notice prints once a node.

### 4.3 `OSOE_MAX_THREADS` is described wrongly, twice (C3)

- **What it counts.** The README (339-341, 518) and `SHMEM_INFO`
  (`init.c:95-97`) say threads "in the library at once", and the fatal itself
  says "more threads of this PE are in the library than ... allows"
  (`metrics.c:537`). A thread takes a slot at its first call and gives it
  back only when it exits (`metrics.c:487-538`): the limit is on live threads
  that have *ever* called. An OpenMP team of 16 workers making strictly one call
  at a time ends the job at the sixteenth worker's first call (the
  investigator's measurement; the mechanism *read*).
- **Where it is read.** Only the node's first PE reads the variable
  (`metrics.c:455`, called from `node.c:512`); the others copy its value
  (`node.c:888`). A user who sets it for one rank with `-e` gets silence,
  where the sibling `OSOE_MAX_CONTEXTS` is compared and refused by name.

Fix: the wording in those four places ("live threads that have called the
library; a slot is freed when the thread exits"). **Recommendation** for D6:
keep the default of 16 - it was chosen on purpose on 2026-09-26, the failure
is loud and names the variable, and nothing measured argues for another
number - and compare it PE by PE, which §3.2a's option 1 gives for nothing.
The alternative is a larger default: each slot is about 768 bytes of the node
segment a PE.

### 4.4 Eight behaviours the Limits section does not list (C4)

The section claims "every limit a program can meet". All documentation; the
labels are the refuter's.

| Behaviour | What it is |
|---|---|
| A split on each node spends a job-wide number per node | a limit (§3.3) |
| `shmem_team_create_ctx` ignores the team's `num_contexts` (`context.c:163-216`) | a permissive departure |
| `SHMEM_SYMMETRIC_SIZE=0` ends the job (`bootstrap.c:168-170`) | a choice; §8 allows zero |
| `shmem_init_thread` never returns nonzero; a failure ends the program | a choice |
| The cache routines, deprecated in 1.3, are gone from the specification after 1.4; osoe keeps them as no-ops, which the README puts down to "their deprecation" (422-423) | wording |
| Collectives mismatched among one node's PEs give wrong data and exit 0; between nodes they are named | undefined input; "Loud, never hung" should say where loud stops |
| A process that exits without `shmem_finalize` ends the job with status 1 | a non-conforming program; older programs do this |
| A wait woken only by a plain store through `shmem_ptr` wakes at the 100 ms sweep - 115 ms a round trip against 28 µs by `shmem_atomic_set` | conforming; a thousandfold cliff for a common idiom |

### 4.5 The README's first steps, followed as a stranger (R5a, R4, N3)

- **Python 3 is required and unlisted.** `tools/pacer/CMakeLists.txt:37`
  makes it `REQUIRED` for the stress tester's counted build. Options: list
  it; or make that build optional, as the models already are.
  **Recommendation**: optional - the README's opening says the build needs a
  C compiler, CMake, pkg-config and liburing, and that stays true.
- **There is no version.** `project(osoe VERSION 0.0.0)`, the vendor string
  is "osoe", `SHMEM_VERSION` promises the library's version and prints none,
  no tool answers `--version`. Fix at the tag, from one CMake value (D8).
- **The first `srun` example** (line 70) lacks the `-K1 --cpu-bind=none` the
  README requires ninety lines later.
- **`cmake --workflow --preset release` takes about an hour** and says
  nothing: the presets set no job count, and the models are 55 of the 61
  minutes of tests. Options: say so; or put the models under a label the
  default preset leaves out. **Recommendation** (D7): the label, with the
  time and the command for the models in the README. The models check the
  design, which does not vary with the reader's machine.
- **Failed starts under `srun` leave objects in `/dev/shm`** that nothing
  removes: the `.conn` and `.job` names, which `osoe-stat --gc` does not know
  (`stat.c:984`). `osoe-launch` hides it by cleaning up itself. Fix now: an
  epilog line in the README, `rm -f /dev/shm/osoe.${SLURM_JOB_ID}.*`.
- **Three settings rows** are incomplete (point 10 of §1).
- **Of `env.probe`, "If it fails, nothing else will work on that machine"**
  (98-100) is wrong for a one-node job, which needs no io_uring and runs
  (design §1; the investigator's measurement).

### 4.6 The repository (R1)

The author's home-directory path is in 61 tracked files, 218 lines, all under
`docs/`: review and design evidence, three finished briefs, three review
prompts. Four also carry an older path under the desktop. (This file spells
neither, so as not to be the next.) `docs/README.md:3` opens by
sending the reader to a status page only the author can open. Four fixtures
hold the development VM's address and five its host name. Four specification
examples kept as evidence lack the notice the specification's permission asks
for. `dev/lima/create.sh` and `dev/lima/osoe.yaml` have no SPDX line, and
neither do 154 evidence programs, though the README says "every source file"
has one.

Fix: mechanical, and the kind of change a green suite cannot check, so it
follows the rename rules in `CLAUDE.md` - longest pattern first, and an audit
command shown to find something before its empty result is believed. Two
things to settle (D9): whether historical evidence is edited at all
(**recommendation**: yes, to a placeholder, with a line in `docs/README.md`
saying so); and the readiness review, whose finding quotes the path - commit
it as it is and it is the 62nd file. **Recommendation**: commit the review
with the path written `/Users/<author>`, the one edit made to it.

### 4.7 Two README passages on RDMA (R3)

- "XRC queue pairs, one per context and target node" (444-445): the second
  review's seam findings put XRC at about 2.1 million queue pairs a NIC at
  4,096 × 256 and recommended DC. Fix: name both and say which the later
  review preferred, as a recommendation from recalled limits.
- "in-network reduction (SHARP and its kind) is out of reach" (449-451)
  contradicts `internode.h:137-145`, which offers exactly that offload. Fix:
  one sentence; the internode phase is below the seam.

### 4.8 The model-checking sentence (K6)

"Every lock-free and futex protocol ... is checked exhaustively" (467-474).
Two gaps: the deprecated active-set protocol has no model in the tree (the
record mentions only a random one, 20,000 trials, never committed); and every
model runs under sequential consistency or x86-TSO, while the targets are
aarch64. The second is not theoretical: the "blocked in" record of §4.11
has a model, `blocked_record_model.py`, which passes, and the record tears on
aarch64. Fix: the sentence says which memory models and names the exception.
A model for `active-set.c` is after release.

### 4.9 x86-64 (X1, X2)

The README names no architecture. Everything has run on aarch64; under
Rosetta the tree builds for x86-64 and passes its one-node tests, but Rosetta
refuses io_uring, so no x86-64 process has ever sent data between nodes. And
a job mixing the two architectures would pass the rendezvous, which compares
no architecture, and then reduce `long double` wrongly - x87's 80-bit format
against binary128 (`internode.h:77-80` says so itself; nobody has run one).
Fix: two sentences, "tested on aarch64; x86-64
builds and passes its one-node tests; the network path is untested there"
and "every node of a job must have the same architecture". The check is
after release.

### 4.10 Two Pi numbers (P9, P10)

- **"A 16-PE barrier takes about 180 µs"** holds under Slurm's default
  binding (176-183 µs) and not under the `--cpu-bind=none` the README tells
  users to run with (196-208 µs), where small collectives cost 4-17% more.
  `docs/pi-cluster-results.md:27-29` says "every line within 5%" against its
  own raw files. Fix: quote the figure with its binding, state the trade in
  the binding advice, correct the record.
- **The agent's 100 µs slice** was recorded as "no difference in small
  operations" (`pi-cluster-results.md:533`). On this tree - the collectives
  three alternating runs each way, the mailbox one 60 s run a setting - the
  slice against none:

  | | Bound (Slurm's default) | Unbound (the README's launch) |
  |---|---|---|
  | mailbox messages a second | 1.7× more with the slice | 2% more |
  | 64 KiB reduce | 24% faster | the same |
  | 4 KiB alltoall | 10% faster | 8% faster |
  | barrier | 8% slower | 2-4% slower |
  | 8-byte reduce and broadcast | 2-13% slower | about 10% slower |

  **Recommendation** (D5): keep 100. Bound is what a stranger gets from
  Slurm without asking, and there the slice is worth 1.7×; its cost is a
  tenth on small collectives. The README row and the record carry both
  sides. The decisions index also says `OSOE_AGENT_CPUS` defaults to `node`
  (line 70); the code and the README say `task`.

### 4.11 The torn "blocked in" record (K1)

When a PE is about to sleep it publishes the routine it waits in, a detail
and a time, between two bumps of a generation word; a reader accepts the
record if the generation is even and unchanged around its reads. The three
numbers are atomic words. The routine name is a plain `char[32]`, copied with
`memcpy` on both sides (`metrics.c:575-576`, `deadlock.c:308`,
`tools/stat/segment.c:475-476`). On aarch64 nothing orders the reader's plain
loads before its second load of the generation, so it can accept a name from
a later wait. The refuter's litmus, copying the protocol field for field,
accepted 28,000 to 218,000 torn records in 60 to 90 million; with the name as
four atomic words, none. The effect is diagnostic only: `osoe-stat` can show
a routine that does not belong to the detail beside it.

Options: the name as four atomic 64-bit words, release stores and acquire
loads, as the fields beside it already are; or one acquire fence in the
reader. **Recommendation**: the four words. A fence needs §3.16 amended and
leaves the plain accesses a data race; the words stay inside §3.16 as it is
written, and by the same reasoning cover the writer on cores without LSE,
which nobody measured; the bytes in the segment do not move. The litmus
becomes a test.

### 4.12 Nothing tests the gate of the silent-peer check (K5)

The check that declares a node vanished reads how long its kernel has
acknowledged nothing, and only while something is unacknowledged
(`wire.c:302`). That condition is what protects a node that is merely
stopped with a full receive buffer. Remove it and 36 of 36 related tests
still pass (the refuter's mutation): the one test of a stopped node puts 8
bytes, which the stopped node's kernel acknowledges. Fix: the investigator's
experiment as a test - 64 MiB toward a node stopped for 15 s with
`OSOE_KEEPALIVE=1,1,3` - which completes with the gate and ends the job at
4 s without it. It adds about 20 s to the suite.

### 4.13 The missing C11 forms (C1)

§9.8.5 and §9.8.7 give `shmem_signal_add(ctx, sig_addr, signal, pe)` and
`shmem_signal_set(ctx, ...)` in their C11 synopsis. `shmem.h:182-185`
declares only the three-argument functions and the `shmem_ctx_` names, with
no macro choosing by argument count, so the four-argument call does not
compile (*run today*). It is the one hole in "everything the specification
requires". Fix: two macros in the style of `shmem_atomic_add`'s
(`shmem.h:1097`) and a compile test.

### While in these files

Rated after release by the review, but one line each in files the batch
already edits: the intranode claim is measured to 16 PEs a node (P4);
`shmem_collect` reduces all P lengths through the tree first (P6); design
§3.15's bullet says the detector never ends a job woken by a signal handler,
and it does; design §4.11 still gives `OSOE_MAX_THREADS` the CPU count as its
default; the start-up row's "⌈log₈ n⌉ levels" leaves out the mesh, n - 1
connections a lead, made one after another.

## 5. Order of work

| | Work | Depends on | Effort |
|---|---|---|---|
| 0 | Commit the six review files and the brief | the author's word; D9 for the one path | minutes |
| 1 | Small code fixes in one branch: the C11 forms; the four atomic words and their litmus; the silent-peer test; the budget notice once a node; the pins on request; Python optional; the version; the thread limit's text | D4, D6, D7, D8 | half a day |
| 2 | The README and the records, every sentence checked against the code before it is written | D5, and D1-D3 for their sentences | half a day |
| 3 | The repository scrub | D9 | two hours |
| 4 | Start-up: the notices, the proof before the counts, unconfirmed callers side by side, the segment before the refusals, the tests with `-k` | D2 | a day and a half |
| 5 | Global exit: the model's property, the two lines, the launcher's status, the test under a launcher that kills (D1 as decided: only the caller carries the status) | D1 | half a day |
| 6 | The four presets, tier 3 and the suite under load on the merged tree; an adversarial read of 4 and 5 | | half a day |
| 7 | **The teams** (§3.3 D, then E), on the proposal's four slices: D's brief is `phase1-step7-slice3-prompt.md`, E's follows D's merge point | D3; the proposal's §8, answered 2026-10-02 | about four weeks |
| 8 | The release point's conditions on that tree: the SOS sweep (now past 61 PEs), the Pi runbook end to end (the tree must be copied to the rebuilt cluster first), an hour of `mix`, the tag, the release note | | a day |

The changes themselves, steps 1 to 5, are about four days on the
recommendations. Steps 6 and 8, a day and a half, follow; step 7 is the
author's decision of 2026-10-02 to rebuild the teams before the tag, and is
the long pole. The refusing node's message (§3.2a, option 4) is the first
work after the tag.

*Written up on 2026-10-02:* steps 0 to 6 are one brief for a fresh session,
`task-release-fixes.md`, with the decisions of §2 taken as this plan
recommends them (D1 and D3 as the author decided); step 7 follows
`docs/design/team-resources-proposal.md` on briefs of its own. The brief
orders the work code first and README after, so that every sentence is
checked against the code as merged: the small fixes, global exit, start-up,
then the README and records, then the scrub - with the author's rule of the
same day that the README is true of the tree at every commit, so each
step's branch carries its own sentences and step 2 is what no code makes
true or false, plus a last read of the whole.

Steps 2 and 3 and the checklist passes are work for subagents; the models,
the start-up protocol and the global-exit change stay in the main session.

The review's VMs (`osoe-x86`, `osoe-load`) and the worktree
`build/scratch/wt/review-r3` are still there; `osoe-load` is the fresh
machine to verify steps 1 and 6 on, and all three go when step 6 is done.
