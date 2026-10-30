#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""How a process ends when its node aborts (design §3.15; phase 1 step 4).

abort_model.py decides how an abort crosses nodes: as an abortive goodbye, the
last segment a dying lead sends on each connection, read before the end of
stream it precedes. This model is the other half: which thread of which
process ends it, and when, so that the goodbye is sent before the lead's
sockets close, and so that the job ends with the status a caller of
shmem_global_exit gave (spec §9.1.6). One process leaves with it, the carrier
- the PE its node's word names, whose declaration won - and every other with
0 (the author's decision of 2026-10-02, after the round-three review's C2). A
launcher which ends the step at the first bad exit (srun -K1, KillOnBadExit)
kills every task still running then, and a killed task counts 143, which
srun's status, the highest of all its tasks', would then be; so the carrier
also leaves last (the author's decision of the same evening, after Part 2's
adversarial read found the race on one node too):

  * The carrier, before its end, waits for every other PE of its node to end
    (pidfds).
  * A lead, before its end, waits for every other PE of its node but the
    carrier: its end ends its stream, which tells the carrier's node that this
    node is done.
  * The lead of the carrier's node, after its goodbye, waits for the other
    node's end of stream - but not for a node whose goodbye named another
    caller, which carries that one (two callers on two nodes at once).

Every PE is a process with two threads, its main thread and its agent
(§3.12). The lead's agent owns the node's sockets: only it can tell the other
nodes, and if its process ends first, its kernel ends every stream without a
word - which a peer takes for a death, ending with a failure. So:

  * Declaring (osoe_node_declare_abort) is one CAS of the node's abort word
    from zero - the first declarer wins, and its disposition and status stay -
    then a bump of every local agent's wake word, in local-id order, the
    lead's first.
  * An agent reads the word at every turn of its loop, after loading its wake
    word. The lead's agent, finding it set, wakes every local agent again, then
    sends the abortive goodbye, then ends its process. Any other agent ends
    its process at once.
  * The thread that ends a process on an abort is its agent. Any other thread
    that finds the abort - a caller of shmem_global_exit, a library wait that
    sweeps into it - waits for the agent to do so (abort.h: exactly one thread
    ends a process, and while the agent runs it is the agent).
  * A library wait's sweep reads the word. It used to check its siblings' pids
    too, with the development watchdog on, and call a dead one a fatal - which
    a PE a global exit had ended also was, so the watchdog read the word again
    first, and a fatal whose declaration lost to a global exit ended as that
    exit did. The watchdog was retired (2026-09-23), and its three variants
    with it; the routing of a fatal that loses remains, in fatal.c.
  * A pidfd per local PE (Part C): a PE that ends is found by the lead's
    agent, which declares a fatal naming it. That declaration loses to an
    abort already declared, and does nothing more.

The code sends no abortive goodbye to a peer whose own has arrived; the model
sends one anyway, a superset of the code's sends. The model's end of stream
follows what was queued, as TCP's FIN does; the code makes that so for a reset
too, by counting a goodbye only once the peer's kernel has acknowledged it.

Model: nodes A and B, each a lead process (L) and one other (P), and B a
third (Q), so five processes; A and B share one connection, each direction a
FIFO that holds at most one ABORT, then the kernel's end of stream. Main
threads that compute outside the library take no steps and are not threads
here. A process ending is one step: its threads stop, and a lead's kernel ends
its stream after what it has queued.

Cases
  nonlead   A's P calls shmem_global_exit(5); A's lead's main thread is in a
            library wait, sweeping.
  lead      A's lead's main thread calls shmem_global_exit(5).
  both      A's P calls it with 5 and B's P with 6, at once (no waiter: nonlead
            has it, and with it this case is ten times the size).

  on_b      B's P calls it with 5, B having a third process, Q.
  finish    A's P calls it with 5 while A's lead's main thread calls
            shmem_finalize, at any moment.
  early     A's P calls it with 5 as soon as its shmem_init returns, while
            A's lead's main thread may still be connecting its mesh, with no
            agent running.
  exiting   A's P calls it with 5 while A's lead's main thread is inside exit
            - it returned from main - and in the library's handler: start_pes's
            finalize, a barrier that sweeps. It holds exit's lock, which since
            glibc 2.41 (BZ 31997) is never given back, so an exit(s) from any
            other thread would wait on it for good. Only a PE whose program
            called shmem_global_exit ends with exit(s), its handlers and then
            stdio (spec §9.1.6); every other ends with fflush and _exit(0),
            which takes no lock and runs no handler beside a main thread still
            running (the author's decisions after the third review, and after
            the round-three review for the 0).
  same_node_both
            B's P calls it with 5 and B's Q with 6, at once: one wins B's word
            and carries; the other leaves with 0, after its handlers.
  caller_exiting
            A's P calls it with 5 while another thread of A's P is inside exit
            holding its lock - main returned into the library's handler. The
            caller's PE ends with exit, which waits on that lock: STUCK here,
            where the code's grace ends the process after
            OSOE_ABORT_GRACE_SECONDS with the status, stdout flushed. Kept as
            the known limit of dropping the third review's hand-off.

Options
  pidfd     a sibling's death is a completion that wakes the lead's agent at
            once: the pidfds on the lead's ring, which the design has and the
            build does not yet (the second slice's Part C). As built, the
            agents poll their pidfds at their sweeps, which the `sweeps` option
            models - so Part B's two cases below are also the build's: with
            sweeps it passes, and the ring version is what would let it pass
            without them. On in every case but those two.
  sweeps    an agent's wait also ends at every sweep (OSOE_SWEEP_NS), as every
            wait of the library's does. Off unless named, so that a case that
            passes shows the design ends promptly without one. Part B alone
            passes with its sweeps and is STUCK without them - the declarer's
            own agent can end its process before the declarer reaches the
            lead's wake word, and then only a sweep, or Part C's pidfd, wakes
            the lead. That finding is what makes the pidfd more than liveness
            for a crash. Since the carrier leaves last, its process cannot end
            before its node's lead, so the declarer's bumps always finish
            first, and the case passes without sweeps or pidfds; the variant
            no_carrier_wait still shows the finding.
  kills     srun -K1: after the first nonzero exit, a launcher thread kills
            every process still running, one step each, interleaved with
            everything else. On in every built case it can pass; with it,
            two callers on two nodes at once (`both`) fail NO_KILL - the
            residual the README states.

Variants (each breaks one rule above)
  caller_exits      the caller ends its process itself. On the lead, the
                    process ends before its agent can send: STATUS.
  pidfd_fatal       the lead's agent takes a sibling's death for a fatal
                    rather than declaring one, and ends through the fatal path
                    without sending: STATUS. Shown on B with its Q, which ends
                    at once while B's lead may still be at its pass: the
                    carrier now outlives its lead, so A's P cannot show it.
  wake_first        a declarer bumps the wake words before the CAS: an agent
                    can take the bump, find the word still clear, and sleep on
                    the new value. STUCK.
  finalize_returns  shmem_finalize stands aside by returning, on any thread:
                    a main thread that reaches it once its agent is ending
                    the process returns into main's exit(0), a second exit
                    beside the agent's (phase 1 step 4's review). ONE_END:
                    STATUS caught it while every process carried the status,
                    and no longer can on a process that leaves with 0.
  no_init_wait      shmem_init returns before the lead's agent runs: a caller
                    straight after init finds its lead still connecting, and
                    the lead's main thread ends it with nobody sent a goodbye
                    (phase 1 step 4's review). STATUS.
  exit_everywhere   every PE a global exit ends calls exit(s), the caller's or
                    not, as the first slice built it: the lead's agent waits
                    on the lock its main thread holds inside exit. STUCK.
  no_rewake         the lead's agent does not wake the others again: with the
                    declarer's own agent ending its process right after the
                    CAS, the lead is woken by its pidfd and the others by
                    nothing. B's Q is the other that nothing wakes, so the
                    case that shows it has B's P call. STUCK until the carrier
                    left last; now the declarer's process outlives its bumps,
                    and the rewake is a second line - kept, and passing.
  status_everywhere every process a global exit ends leaves with the caller's
                    status, as the code did until the round-three review: the
                    first of them to leave is a bad exit before the caller's,
                    and srun -K1 kills the caller inside exit. STATUS.
  caller_zero       the carrier leaves with 0 as well, and the job's status is
                    lost. JOB_STATUS.
  called_carries    the code before the carrier: every process whose program
                    called leaves with the status, and nobody waits. NO_KILL -
                    with one caller, the caller leaving before the other node
                    has been told; with two on a node, the first of them.
  no_carrier_wait   the carrier does not wait for its node. NO_KILL.
  no_lead_wait      a lead does not wait for its node before its end of
                    stream: the carrier's node takes it for the node's end.
                    NO_KILL.
  no_remote_wait    the carrier's node's lead does not wait for the other
                    node. NO_KILL.
  wait_every_node   every lead waits for the other node's end, not only the
                    carrier's: each waits for the other. STUCK (the code's
                    bound would end both, and then race).
  no_exclusion      the carrier's node's lead waits for a node whose goodbye
                    named another caller, which waits for it in turn. STUCK.

Properties
  TERMINATES  every process ends: no state is STUCK with a process alive and
              every thread asleep, and from every state some state in which all
              have ended is reachable (the trapped-state check).
  STATUS      a process whose program called shmem_global_exit ends with 0 or
              a status some caller gave; every other process ends with 0. In
              these cases none is a fatal or a crash, so a failure never is.
  JOB_STATUS  srun's status, the highest of every process's, is a status some
              caller gave.
  NO_KILL     with `kills`: no process is still running at the first nonzero
              exit, for srun -K1 to kill.
  ONE_END     a process is ended by the thread that claimed it (abort.c's
              `ending`), never by a second thread beside it.

The single job-wide carrier (P1; this copy, docs/design/evidence/exit-carrier/,
a proposal the author has not taken): with callers on two nodes, each node's
word names its own caller, each carries, and each excludes the other node's
end (no_exclusion) - so the first to leave with its status is a bad exit
while the other node runs, and `both` fails NO_KILL under `kills`, the
residual the README states (round four's conformance F1). The variants
`single*` are the cure the review named, one carrier for the whole job, the
lowest-ranked of the nodes' carriers:

  * A carrier's node tells a peer even where the peer's own goodbye has
    arrived, when it named a caller ranked above this node's carrier (the code
    tells nobody there now: flush_aborting's peer_aborted, "nobody to tell").
    Every other send is as the code's: none to a peer whose goodbye arrived.
  * A carrier's node whose lead reads a goodbye naming a caller ranked below
    its carrier is outranked: one more word in the node's segment, set before
    anything else. Its lead then waits for no other node, and before its end
    for every other PE of its node, the carrier among them; the carrier, which
    rereads the word at each of its waits' polls (osoe_liveness_outlast polls
    every 100 ms), waits for nobody and leaves with 0 - by exit, its handlers
    run, as a caller whose declaration lost does.
  * A carrier's node not outranked waits for every other node's end - none
    excluded - so the lowest carrier leaves last of the job.

  single                 the proposal.
  single_old_send        the code's send: nothing to a peer whose goodbye has
                         arrived, so a higher carrier's node never hears of the
                         lower and each waits for the other's end. STUCK.
  single_demoted_unwaited  the outranked node's lead waits, as now, for every
                         PE but the carrier: its end tells the lowest carrier's
                         node that it is done while its demoted carrier still
                         runs. NO_KILL.
  single_status_kept     the demoted carrier leaves with its own status: a bad
                         exit while the other node runs. NO_KILL.
  single_carrier_unmoved the carrier does not reread the word: it waits for its
                         lead, which waits for it. STUCK.
  single_outranked_waits the outranked node's lead waits for the lower node's
                         end, which waits for its own. STUCK.
  single_lowest_excludes the lowest carrier's node excludes a node whose goodbye
                         named a higher caller, as the code does: it leaves
                         while that node runs. NO_KILL.

  ONE_CARRIER  with `single*`: at most one process leaves with a status other
               than 0 - one carrier for the job.

Usage: abort_exit_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select

FAIL = 1                        # the status a fatal leaves with
KILLED = 15                     # a process the launcher kills: 128 + SIGTERM, 143, in the code
FATAL, GLOBAL = 1, 2            # the word's dispositions
MAIN, AGENT = 1, 2              # the holders of exit's lock
NODES = ('A', 'B')
PEER = {'A': 'B', 'B': 'A'}
# Each node's processes in local-id order, the lead first. B has a third, Q,
# only in the case that needs one; every other case is smaller without it.
LOCAL = {'A': ('LA', 'PA'), 'B': ('LB', 'PB')}
LOCAL_WITH_Q = {'A': ('LA', 'PA'), 'B': ('LB', 'PB', 'QB')}
NODE_OF = {p: x for x in NODES for p in LOCAL_WITH_Q[x]}
RANK = {'LA': 1, 'PA': 2, 'LB': 3, 'PB': 4, 'QB': 5}
NAME_OF = {r: p for p, r in RANK.items()}


def is_lead(p):
    return p in ('LA', 'LB')


def WORD(x):        return ('$word', x)            # 0, or word_of(disposition, rank, status)
def WAKE(p):        return ('$wake', p)            # the agent's wake word
def ENDING(p):      return ('$ending', p)          # abort.c's claim: 0, MAIN or AGENT
def ALIVE(p):       return ('$alive', p)
def STAT(p):        return ('$status', p)          # the status it exited with
def QUEUED(x, y):   return ('$queued', x, y)       # an ABORT x sent y and y has not read: its word
def KFIN(x, y):     return ('$kfin', x, y)         # x's kernel ended its stream to y
def SENT(x, y):     return ('$sent', x, y)
def HEARD(x, y):    return ('$heard', x, y)        # x read y's ABORT: y's end of stream is no death
def EOF(x, y):      return ('$eof', x, y)
def REAPED(p):      return ('$reaped', p)          # the lead's agent has had p's pidfd completion
def AGENT_UP(p):    return ('$agent_up', p)        # p's agent has started (init.c)
def EXIT_LOCK(p):   return ('$exit_lock', p)       # who holds exit's lock: 0, MAIN or AGENT
def CALLED(p):      return ('$called', p)          # p's program called shmem_global_exit
def ORIGIN(x, y):   return ('$origin', x, y)       # the rank y's ABORT named, once x read it
def KILLING():      return ('$killing',)           # a process has left nonzero: -K1 ends the rest
def OUTRANKED(x):   return ('$outranked', x)       # single*: x's carrier is not the job's


def single(variant):
    """Whether the variant is P1's proposal or one of its breakages."""
    return variant.startswith('single')


def word_of(disposition, rank, status):
    """The abort word: how the job ends, the rank that declared it, the
    status (node.h's OSOE_ABORT_*_SHIFT). A fatal's rank is 0 here."""
    return disposition * 256 + rank * 16 + status


def rank_of(word):
    return (word // 16) % 16


def disposition(word):
    return word // 256


def exit_status(word):
    return FAIL if disposition(word) == FATAL else word % 16


def status_at_end(variant, M, p, word):
    """What osoe_abort_end ends p with: a fatal's failure status; on a global
    exit, the status for the PE its node's word names - the carrier - and 0
    for every other, a caller whose declaration lost among them (abort.c)."""
    if disposition(word) == FATAL:
        return FAIL
    if variant == 'called_carries':
        return word % 16 if M[CALLED(p)] else 0
    if variant == 'status_everywhere':
        return word % 16
    if single(variant) and M[OUTRANKED(NODE_OF[p])] and variant != 'single_status_kept':
        return 0
    if rank_of(word) == RANK[p]:
        return 0 if variant == 'caller_zero' else word % 16
    return 0


def waits_for(variant, local, M, p):
    """Who p's end waits for, on a global exit (abort.c): the carrier for every
    other PE of its node, so that it leaves last; a lead for every other but
    the carrier, so that its end of stream - its end makes it - tells the
    carrier's node that its whole node is done. Nobody else waits."""
    x = NODE_OF[p]
    w = M[WORD(x)]
    if disposition(w) != GLOBAL or variant == 'called_carries':
        return ()
    others = tuple(q for q in local[x] if q != p)
    if single(variant) and M[OUTRANKED(x)]:
        # Outranked: nobody here carries. The lead's end - its end of stream -
        # waits for every other PE, the demoted carrier among them; nobody else
        # waits, the demoted carrier included, once it has reread the word.
        if variant == 'single_carrier_unmoved' and rank_of(w) == RANK[p]:
            return others
        if is_lead(p):
            if variant == 'single_demoted_unwaited':
                return tuple(q for q in others if RANK[q] != rank_of(w))
            return others
        return ()
    if rank_of(w) == RANK[p]:
        return () if variant == 'no_carrier_wait' else others
    if is_lead(p) and variant != 'no_lead_wait':
        return tuple(q for q in local[x] if q != p and RANK[q] != rank_of(w))
    return ()


def carrier_node(M, x):
    """Whether node x's word names one of its own PEs: a global exit called
    here, whose lead waits for the other nodes to end."""
    w = M[WORD(x)]
    return disposition(w) == GLOBAL and NODE_OF[NAME_OF[rank_of(w)]] == x


# ---- steps shared by every thread ------------------------------------------

def end_process(M, p, status, label):
    """p ends: its threads stop; a lead's kernel ends its stream after what it
    queued. A nonzero status is a bad exit, which srun -K1 answers by ending
    every task still running (the launcher thread, with the `kills` option)."""
    upd = {ALIVE(p): 0, STAT(p): status}
    if status != 0:
        upd[KILLING()] = 1
    x = NODE_OF[p]
    if is_lead(p):
        upd[KFIN(x, PEER[x])] = 1
    return (upd, {}, f'{p} exits with status {status} ({label})')


PC_DECL_CAS, PC_DECL_ADD = 100, 101


def declare_step(variant, local, M, L, x, name):
    """One step of osoe_node_declare_abort in progress - the CAS, then a bump of
    each local agent's wake word in local-id order - or None if none is."""
    pc, value, k = L['pc'], L['decl'], L['k']
    agents = local[x]
    if variant == 'wake_first' and pc == PC_DECL_CAS and L['k'] < len(agents):
        q = agents[k]
        return [({WAKE(q): M[WAKE(q)] + 1}, {'k': k + 1}, f'{name}: bumps {q}\'s wake word')]
    if pc == PC_DECL_CAS:
        if M[WORD(x)] != 0:
            return [({}, finish_declaring(L), f'{name}: declares, and loses')]
        loc = finish_declaring(L) if variant == 'wake_first' else {'pc': PC_DECL_ADD, 'k': 0}
        return [({WORD(x): value}, loc,
                 f'{name}: declares {"a global exit" if disposition(value) == GLOBAL else "a fatal"}'
                 f', status {exit_status(value)}')]
    if pc == PC_DECL_ADD:
        if k == len(agents):
            return [({}, finish_declaring(L), f'{name}: has woken every agent')]
        q = agents[k]
        return [({WAKE(q): M[WAKE(q)] + 1}, {'k': k + 1}, f'{name}: bumps {q}\'s wake word')]
    return None


def start_declaring(value, then):
    return {'pc': PC_DECL_CAS, 'decl': value, 'k': 0, 'then': then}


def finish_declaring(L):
    return {'pc': L['then'], 'decl': DEL, 'k': DEL, 'then': DEL}


# ---- the agent --------------------------------------------------------------

(PC_TOP, PC_WORD, PC_WORK, PC_WAIT, PC_REWAKE, PC_SEND, PC_END, PC_PAUSE,
 PC_EXITING, PC_REMOTE) = range(10)


def agent(variant, local, p, options):
    x = NODE_OF[p]
    y = PEER[x]
    lead = is_lead(p)
    name = f'{p}\'s agent'

    def pending(M):
        """What the lead's ring would complete: a message, an end of stream, a pidfd."""
        if not lead:
            return False
        if M[QUEUED(y, x)] or (M[KFIN(y, x)] and not M[EOF(x, y)]):
            return True
        return options['pidfd'] and any(not M[ALIVE(q)] and not M[REAPED(q)]
                                        for q in local[x] if q != p)

    def step(M, L):
        if not M[ALIVE(p)]:
            return [({}, {'finished': 1}, f'{name}: its process has ended')]
        pc = L['pc']
        d = declare_step(variant, local, M, L, x, name)
        if d is not None:
            return d
        if pc == PC_TOP:
            if not M[AGENT_UP(p)]:
                return []               # not started yet: its main thread is still in init
            return [({}, {'pc': PC_WORD, 'seen': M[WAKE(p)]}, f'{name}: loads its wake word')]
        if pc == PC_WORD:
            w = M[WORD(x)]
            if w:
                return [({}, {'pc': PC_REWAKE if lead else PC_END, 'k': 0},
                         f'{name}: finds the abort')]
            return [({}, {'pc': PC_WORK if lead else PC_WAIT}, f'{name}: finds no abort')]
        if pc == PC_WORK:
            # The lead's pass: one completion, then round again.
            if M[QUEUED(y, x)]:
                value = M[QUEUED(y, x)]
                upd = {QUEUED(y, x): 0, HEARD(x, y): 1, ORIGIN(x, y): rank_of(value)}
                loc = start_declaring(value, PC_TOP)
                return [(upd, loc, f'{name}: reads {y}\'s ABORT (status {exit_status(value)})')]
            if M[KFIN(y, x)] and not M[EOF(x, y)]:
                upd = {EOF(x, y): 1}
                if M[HEARD(x, y)]:
                    return [(upd, {'pc': PC_TOP}, f'{name}: reads {y}\'s end of stream, after its ABORT')]
                loc = start_declaring(word_of(FATAL, 0, FAIL), PC_TOP)
                return [(upd, loc, f'{name}: reads {y}\'s end of stream without a word: a death')]
            for q in local[x]:
                if q != p and options['pidfd'] and not M[ALIVE(q)] and not M[REAPED(q)]:
                    upd = {REAPED(q): 1}
                    if variant == 'pidfd_fatal':
                        # osoe_fatal on the agent: its declaration loses, and
                        # the fatal ends the process as the word says - from
                        # the agent, so without the abortive goodbye.
                        w = M[WORD(x)]
                        if w == 0:
                            upd[WORD(x)] = word_of(FATAL, 0, FAIL)
                            w = upd[WORD(x)]
                        if M[ENDING(p)]:
                            return [(upd, {'pc': PC_PAUSE}, f'{name}: {q} has died; pauses')]
                        upd[ENDING(p)] = AGENT
                        u, _, lbl = end_process(M, p, status_at_end(variant, M, p, w),
                                                f'a fatal for {q}')
                        upd.update(u)
                        return [(upd, {}, f'{name}: {q} has died - ' + lbl)]
                    loc = start_declaring(word_of(FATAL, 0, FAIL), PC_TOP)
                    return [(upd, loc, f'{name}: {q}\'s pidfd completes: declares its death')]
            return [({}, {'pc': PC_WAIT}, f'{name}: nothing to do')]
        if pc == PC_WAIT:
            out = []
            if M[WAKE(p)] != L['seen']:
                out.append(({}, {'pc': PC_TOP}, f'{name}: woken'))
            elif pending(M):
                out.append(({}, {'pc': PC_TOP}, f'{name}: woken by its ring'))
            elif options['sweeps']:
                out.append(({}, {'pc': PC_TOP}, f'{name}: its wait\'s sweep ends'))
            return out
        if pc == PC_REWAKE:
            k = L['k']
            if variant == 'no_rewake' or k == len(local[x]):
                return [({}, {'pc': PC_SEND, 'k': DEL}, f'{name}: has woken every agent again')]
            q = local[x][k]
            return [({WAKE(q): M[WAKE(q)] + 1}, {'k': k + 1}, f'{name}: wakes {q}\'s agent again')]
        if pc == PC_SEND:
            # The abortive goodbye, unless the peer's stream ended without one.
            # Then, on the carrier's node, the other nodes' ends (PC_REMOTE).
            dead_peer = M[EOF(x, y)] and not M[HEARD(x, y)]
            w = M[WORD(x)]
            remote = disposition(w) == GLOBAL and variant not in ('called_carries',
                                                                 'no_remote_wait') and (
                carrier_node(M, x) or variant == 'wait_every_node')
            then = PC_REMOTE if remote else PC_END
            if single(variant):
                # Nothing to a peer whose goodbye has arrived, unless it named a
                # caller ranked above this node's carrier, which must hear of it.
                tell = not M[HEARD(x, y)] or (variant != 'single_old_send' and carrier_node(M, x)
                                              and M[ORIGIN(x, y)] > rank_of(w))
            else:
                tell = True             # a superset of the code's sends (above)
            if not M[SENT(x, y)] and not dead_peer and tell:
                return [({QUEUED(x, y): M[WORD(x)], SENT(x, y): 1}, {'pc': then},
                         f'{name}: sends {y} the abortive goodbye')]
            return [({}, {'pc': then}, f'{name}: has told every peer')]
        if pc == PC_REMOTE:
            # The carrier's node's lead waits for the other node's end of
            # stream, reading what comes before it - but not for a node whose
            # goodbye named another caller, which carries its own.
            w = M[WORD(x)]
            excluded = (M[HEARD(x, y)] and M[ORIGIN(x, y)] != rank_of(w)
                        and variant != 'no_exclusion')
            if single(variant):
                # Only a lower caller excuses a node from waiting: it is then
                # outranked, the word set first, and waits for no node at all.
                lower = M[HEARD(x, y)] and M[ORIGIN(x, y)] < rank_of(w)
                if lower and not M[OUTRANKED(x)]:
                    return [({OUTRANKED(x): 1}, {},
                             f'{name}: {y} carries {NAME_OF[M[ORIGIN(x, y)]]}, ranked below '
                             f'{NAME_OF[rank_of(w)]}: this node is outranked')]
                if M[OUTRANKED(x)] and variant != 'single_outranked_waits':
                    return [({}, {'pc': PC_END}, f'{name}: outranked; waits for no other node')]
                higher = M[HEARD(x, y)] and M[ORIGIN(x, y)] > rank_of(w)
                excluded = higher and variant == 'single_lowest_excludes'
            if M[EOF(x, y)]:
                return [({}, {'pc': PC_END}, f'{name}: {y} has ended')]
            if excluded:
                return [({}, {'pc': PC_END},
                         f'{name}: {y} carries {NAME_OF[M[ORIGIN(x, y)]]}; waits no longer')]
            if M[QUEUED(y, x)]:
                value = M[QUEUED(y, x)]
                return [({QUEUED(y, x): 0, HEARD(x, y): 1, ORIGIN(x, y): rank_of(value)}, {},
                         f'{name}: reads {y}\'s ABORT, naming {NAME_OF[rank_of(value)]}')]
            if M[KFIN(y, x)]:
                return [({EOF(x, y): 1}, {}, f'{name}: reads {y}\'s end of stream')]
            return []                   # waits on its ring for what the peer sends
        if pc == PC_END:
            # osoe_abort_end from the agent: claim the process, then end it -
            # two steps, since exit runs the process's handlers and flushes
            # before it is gone, and other threads run meanwhile.
            if M[ENDING(p)]:
                return [({}, {'pc': PC_PAUSE}, f'{name}: another thread ends this process')]
            return [({ENDING(p): AGENT}, {'pc': PC_EXITING}, f'{name}: claims the process')]
        if pc == PC_EXITING:
            # exit(s) only where this process's program called shmem_global_exit,
            # and exit's lock is then needed; everywhere else fflush and _exit -
            # with 0 on a global exit, the failure status on a fatal.
            w = M[WORD(x)]
            # Before its end, a carrier or a lead waits for the PEs whose end
            # must come first (waits_for: pidfds in the code).
            if any(M[ALIVE(q)] for q in waits_for(variant, local, M, p)):
                return []
            calls_exit = disposition(w) == GLOBAL and (
                M[CALLED(p)] or variant == 'exit_everywhere')
            if calls_exit:
                if M[EXIT_LOCK(p)] == MAIN:
                    return []           # exit's lock, held for good by the main thread
                upd, _, lbl = end_process(M, p, status_at_end(variant, M, p, w),
                                          'its agent, by exit')
                upd[EXIT_LOCK(p)] = AGENT
                return [(upd, {}, lbl)]
            return [end_process(M, p, status_at_end(variant, M, p, w), 'its agent, by _exit')]
        if pc == PC_PAUSE:
            return []                   # until the process ends
        raise AssertionError(pc)

    return step, PC_TOP


# ---- main threads -------------------------------------------------------------

(PC_CALL, PC_MAIN_END, PC_SWEEP) = range(20, 23)


def main_end(variant, M, p, name):
    """osoe_abort_end from a thread that is not the agent: it waits for the
    agent, unless the variant has it end the process itself."""
    x = NODE_OF[p]
    if variant == 'caller_exits':
        if M[ENDING(p)]:
            return [({}, {'pc': PC_PAUSE}, f'{name}: another thread ends this process')]
        u, _, lbl = end_process(M, p, status_at_end(variant, M, p, M[WORD(x)]), 'its caller')
        u[ENDING(p)] = MAIN
        return [(u, {}, lbl)]
    return [({}, {'pc': PC_PAUSE}, f'{name}: waits for its agent to end the process')]


def caller(variant, local, p, status):
    x = NODE_OF[p]
    name = f'{p}\'s main thread'

    def step(M, L):
        if not M[ALIVE(p)]:
            return [({}, {'finished': 1}, f'{name}: its process has ended')]
        d = declare_step(variant, local, M, L, x, name)
        if d is not None:
            return d
        pc = L['pc']
        if pc == PC_CALL:
            # abort.c records the call before declaring (osoe_abort_caller).
            return [({CALLED(p): 1}, start_declaring(word_of(GLOBAL, RANK[p], status), PC_MAIN_END),
                     f'{name}: calls shmem_global_exit({status})')]
        if pc == PC_MAIN_END:
            return main_end(variant, M, p, name)
        if pc == PC_PAUSE:
            return []
        raise AssertionError(pc)

    return step, PC_CALL


def waiter(variant, local, p):
    """A library wait that never completes, sweeping: each sweep reads the word
    (sweep.c's stop_if_aborting). A global exit it leaves to its agent; a fatal
    ends it at once, with a failure."""
    x = NODE_OF[p]
    name = f'{p}\'s main thread'

    def step(M, L):
        if not M[ALIVE(p)]:
            return [({}, {'finished': 1}, f'{name}: its process has ended')]
        d = declare_step(variant, local, M, L, x, name)
        if d is not None:
            return d
        pc = L['pc']
        if pc == PC_SWEEP:
            w = M[WORD(x)]
            if not w:
                return [({}, {}, f'{name}: its sweep finds no abort')]
            if disposition(w) == GLOBAL:
                return main_end(variant, M, p, name)
            u, _, lbl = end_process(M, p, FAIL, 'a fatal: the job is aborting')
            return [(u, {}, lbl)]
        if pc == PC_PAUSE:
            return []
        raise AssertionError(pc)

    return step, PC_SWEEP


PC_FINALIZE = 30


def finisher(variant, local, p):
    """A main thread that calls shmem_finalize, at any moment, while the job
    may be ending: its final barrier sweeps like any wait, and it may also find
    this process already ending on an abort. The library's exit paths stand
    aside then (abort.h). Only the thread ending the process may return - it is
    running exit's handlers - and any other waits for that end; returning into
    main's own exit(0) beside the agent's exit(s) is a second exit, which C
    leaves undefined and which the model takes at its worst, the second
    status."""
    x = NODE_OF[p]
    name = f'{p}\'s main thread'

    def step(M, L):
        if not M[ALIVE(p)]:
            return [({}, {'finished': 1}, f'{name}: its process has ended')]
        pc = L['pc']
        if pc == PC_FINALIZE:
            if M[ENDING(p)]:
                if variant == 'finalize_returns':
                    u, _, lbl = end_process(M, p, 0, 'shmem_finalize returned into exit(0)')
                    return [(u, {}, lbl)]
                return [({}, {'pc': PC_PAUSE}, f'{name}: finalize stands aside; waits for the end')]
            w = M[WORD(x)]
            if w and disposition(w) == GLOBAL:
                return main_end(variant, M, p, name)
            return [({}, {}, f'{name}: in the final barrier, which does not complete')]
        if pc == PC_PAUSE:
            return []
        raise AssertionError(pc)

    return step, PC_FINALIZE


PC_EXIT_SWEEP = 50


def exiter(variant, local, p):
    """A main thread that returned from main: inside exit, holding exit's lock
    for good (glibc 2.41), in the library's handler - start_pes's finalize,
    whose final barrier sweeps like any wait. Finding the abort, it waits for
    its agent to end the process, as every thread but the agent does: the
    agent's _exit needs no lock, and its exit(s), only on a caller's PE, would
    wait on this one - which exit_everywhere shows."""
    x = NODE_OF[p]
    name = f'{p}\'s main thread'

    def step(M, L):
        if not M[ALIVE(p)]:
            return [({}, {'finished': 1}, f'{name}: its process has ended')]
        pc = L['pc']
        if pc == PC_EXIT_SWEEP:
            if M[ENDING(p)] or M[WORD(x)]:
                return [({}, {'pc': PC_PAUSE}, f'{name}: inside exit, waits for its agent')]
            return [({}, {}, f'{name}: in the final barrier, which does not complete')]
        if pc == PC_PAUSE:
            return []
        raise AssertionError(pc)

    return step, PC_EXIT_SWEEP


PC_CONNECTING, PC_EARLY_CALL = 40, 41


def connecting_lead(variant, local, p):
    """A lead's main thread still in shmem_init, connecting its node's mesh, its
    agent not yet started. Its connection loop sweeps: a global exit it finds
    there it ends itself, since no agent runs to speak for it (running is 0:
    osoe_agent_speaks is false) and no engine exists yet to send a goodbye.
    Otherwise it finishes, starts its agent and computes."""
    x = NODE_OF[p]
    name = f'{p}\'s main thread'

    def step(M, L):
        if not M[ALIVE(p)]:
            return [({}, {'finished': 1}, f'{name}: its process has ended')]
        pc = L['pc']
        if pc == PC_CONNECTING:
            w = M[WORD(x)]
            if w and disposition(w) == GLOBAL:
                u, _, lbl = end_process(M, p, status_at_end(variant, M, p, w),
                                        'its main thread, no agent yet')
                u[ENDING(p)] = MAIN
                return [(u, {}, lbl)]
            return [({AGENT_UP(p): 1}, {'finished': 1},
                     f'{name}: has connected the mesh and starts its agent')]
        raise AssertionError(pc)

    return step, PC_CONNECTING


def early_caller(variant, local, p, status):
    """A non-lead that calls shmem_global_exit as soon as its own shmem_init
    returns. Init waits for its lead's agent (init.c: lead_agent_running);
    the variant no_init_wait returns before it."""
    x = NODE_OF[p]
    lead = local[x][0]
    name = f'{p}\'s main thread'
    inner_step, _ = caller(variant, local, p, status)

    def step(M, L):
        if not M[ALIVE(p)]:
            return [({}, {'finished': 1}, f'{name}: its process has ended')]
        if L['pc'] == PC_EARLY_CALL:
            if variant != 'no_init_wait' and not M[AGENT_UP(lead)]:
                return []               # still in init: its lead's agent does not run yet
            return [({}, {'pc': PC_CALL}, f'{name}: returns from shmem_init')]
        return inner_step(M, L)

    return step, PC_EARLY_CALL


# ---- assembly -------------------------------------------------------------------

BAD_STATUS_CALLER, BAD_STATUS_OTHER, BAD_SECOND_END, BAD_KILL, BAD_JOB, BAD_TWO = 1, 2, 3, 4, 5, 6
BAD = {
    BAD_STATUS_CALLER: 'STATUS: a process whose program called shmem_global_exit exited with '
                       'a status neither 0 nor one a caller gave',
    BAD_STATUS_OTHER: 'STATUS: a process that did not call shmem_global_exit exited with a '
                      'status other than 0',
    BAD_SECOND_END: 'ONE_END: a thread ended a process another thread had claimed',
    BAD_KILL: 'NO_KILL: a process was still running at the first nonzero exit, and srun -K1 '
              'killed it',
    BAD_JOB: 'JOB_STATUS: srun\'s status, the highest of every task\'s, is no caller\'s',
    BAD_TWO: 'ONE_CARRIER: two processes left with a status other than 0',
}


def thread(step, initial, callers, role):
    """A thread of the role MAIN or AGENT, its steps checked for STATUS and
    ONE_END as they are taken."""
    def checked(M, L):
        out = []
        for upd, loc, label in step(M, L):
            bad = 0
            for k, v in upd.items():
                if k[0] != '$status':
                    continue
                p = k[1]
                if M[CALLED(p)]:
                    if v != 0 and v not in callers:
                        bad = BAD_STATUS_CALLER
                elif v != 0:
                    bad = BAD_STATUS_OTHER
                if M[ENDING(p)] not in (0, role):
                    bad = BAD_SECOND_END
            out.append((upd, dict(loc, bad=bad), label))
        return out

    return assemble([('INIT', ('set', 'pc', lambda L: initial, '@STEP')),
                     ('STEP', ('nd', checked, '@BAD?')),
                     ('BAD?', ('br', lambda L: L['bad'], '@FAIL', '@FINISHED?')),
                     ('FAIL', ('check', lambda M, L: BAD[L['bad']], '@END')),
                     ('FINISHED?', ('br', lambda L: L['finished'], '@END', '@STEP')),
                     ('END', ('done',))])


def launcher(processes, callers, kills, one_carrier=False):
    """srun, with -K1 under the `kills` option: once any task has left with a
    nonzero status, every task still running is sent SIGTERM - one step each,
    in any order and interleaved with everything else, as slurmstepd's signal
    reaches each when it does - and leaves with 143. At the end, srun's own
    status: the highest of every task's, which must be a caller's."""
    def step(M, L):
        alive = [p for p in processes if M[ALIVE(p)]]
        if not alive:
            job = max(M[STAT(p)] for p in processes)
            bad = 0 if job in callers else BAD_JOB
            if one_carrier and sum(1 for p in processes if M[STAT(p)] != 0) > 1:
                bad = bad or BAD_TWO
            return [({}, {'finished': 1, 'bad': bad}, f'srun returns {job}')]
        if not (kills and M[KILLING()]):
            return []
        out = []
        for q in alive:
            upd = {ALIVE(q): 0, STAT(q): KILLED}
            if is_lead(q):
                x = NODE_OF[q]
                upd[KFIN(x, PEER[x])] = 1
            out.append((upd, {'bad': BAD_KILL}, f'srun -K1 kills {q}, still running'))
        return out

    return assemble([('INIT', ('set', 'pc', lambda L: 0, '@STEP')),
                     ('STEP', ('nd', step, '@BAD?')),
                     ('BAD?', ('br', lambda L: L['bad'], '@FAIL', '@FINISHED?')),
                     ('FAIL', ('check', lambda M, L: BAD[L['bad']], '@END')),
                     ('FINISHED?', ('br', lambda L: L['finished'], '@END', '@STEP')),
                     ('END', ('done',))])


CASES_OF = {
    'nonlead': {'callers': {'PA': 5}, 'waiters': ('LA',), 'local': LOCAL},
    'lead': {'callers': {'LA': 5}, 'waiters': (), 'local': LOCAL},
    'both': {'callers': {'PA': 5, 'PB': 6}, 'waiters': (), 'local': LOCAL},
    'both_lead_a': {'callers': {'LA': 5, 'PB': 6}, 'waiters': (), 'local': LOCAL},
    'both_lead_b': {'callers': {'PA': 5, 'LB': 6}, 'waiters': (), 'local': LOCAL},
    # `three` outgrows the model's half hour: long_three.py runs it alone.
    'three': {'callers': {'PA': 5, 'PB': 6, 'QB': 7}, 'waiters': (), 'local': LOCAL_WITH_Q},
    'three_lead_b': {'callers': {'PA': 5, 'LB': 6, 'PB': 7}, 'waiters': (), 'local': LOCAL},
    'on_b': {'callers': {'PB': 5}, 'waiters': (), 'local': LOCAL_WITH_Q},
    'same_node_both': {'callers': {'PB': 5, 'QB': 6}, 'waiters': (), 'local': LOCAL_WITH_Q},
    'finish': {'callers': {'PA': 5}, 'waiters': (), 'finishers': ('LA',), 'local': LOCAL},
    'early': {'callers': {}, 'early_callers': {'PA': 5}, 'waiters': (), 'connecting': ('LA',),
              'local': LOCAL},
    'exiting': {'callers': {'PA': 5}, 'waiters': (), 'exiters': ('LA',), 'local': LOCAL},
    'caller_exiting': {'callers': {'PA': 5}, 'waiters': (), 'exiters': ('PA',), 'local': LOCAL},
}


def case(variant, which, options):
    shape = CASES_OF[which]
    local = shape['local']
    processes = [p for x in NODES for p in local[x]]
    callers = set(shape['callers'].values()) | set(shape.get('early_callers', {}).values())
    threads = []
    for p in processes:
        step, initial = agent(variant, local, p, options)
        threads.append(thread(step, initial, callers, AGENT))
    for p, status in shape['callers'].items():
        step, initial = caller(variant, local, p, status)
        threads.append(thread(step, initial, callers, MAIN))
    for p in shape['waiters']:
        step, initial = waiter(variant, local, p)
        threads.append(thread(step, initial, callers, MAIN))
    for p in shape.get('finishers', ()):
        step, initial = finisher(variant, local, p)
        threads.append(thread(step, initial, callers, MAIN))
    for p in shape.get('connecting', ()):
        step, initial = connecting_lead(variant, local, p)
        threads.append(thread(step, initial, callers, MAIN))
    for p, status in shape.get('early_callers', {}).items():
        step, initial = early_caller(variant, local, p, status)
        threads.append(thread(step, initial, callers, MAIN))
    for p in shape.get('exiters', ()):
        step, initial = exiter(variant, local, p)
        threads.append(thread(step, initial, callers, MAIN))
    threads.append(launcher(processes, callers, options['kills'], single(variant)))
    mem = {KILLING(): 0}
    for x in NODES:
        mem[WORD(x)] = 0
        mem[OUTRANKED(x)] = 0
        y = PEER[x]
        for k in (QUEUED, KFIN, SENT, HEARD, EOF, ORIGIN):
            mem[k(x, y)] = 0
    for p in processes:
        mem[WAKE(p)] = 0
        mem[ENDING(p)] = 0
        mem[ALIVE(p)] = 1
        mem[STAT(p)] = 0
        mem[REAPED(p)] = 0
        mem[AGENT_UP(p)] = 0 if p in shape.get('connecting', ()) else 1
        mem[EXIT_LOCK(p)] = MAIN if p in shape.get('exiters', ()) else 0
        mem[CALLED(p)] = 0
    flags = ','.join(k for k, v in sorted(options.items()) if v)
    return f'abort_exit/{variant} [{which}; {flags}]', threads, mem, processes


def run(variant, which, options, out=sys.stdout):
    name, threads, mem, processes = case(variant, which, options)
    return explore(name, threads, mem, out=out,
                   done=lambda M: all(not M[ALIVE(p)] for p in processes),
                   liveness=True, max_seconds=1800, liveness_max_states=12_000_000)


BUILT = {'pidfd': True, 'sweeps': False, 'kills': False}
KILLS = {'pidfd': True, 'sweeps': False, 'kills': True}
PART_B = {'pidfd': False, 'sweeps': True, 'kills': False}
NO_HELP = {'pidfd': False, 'sweeps': False, 'kills': False}

CASES = [
    ('built', 'nonlead', KILLS, True),
    ('built', 'lead', KILLS, True),
    ('built', 'on_b', KILLS, True),
    ('built', 'same_node_both', KILLS, True),
    ('built', 'both', BUILT, True),
    ('built', 'both', KILLS, 'NO_KILL'),        # the residual: two nodes, two callers
    ('built', 'nonlead', PART_B, True),
    ('built', 'nonlead', NO_HELP, True),        # the carrier's wait: no sweep needed
    ('no_carrier_wait', 'nonlead', NO_HELP, 'STUCK'),   # Part B's finding, without it
    ('caller_exits', 'lead', BUILT, 'STATUS'),
    ('pidfd_fatal', 'on_b', BUILT, 'STATUS'),
    ('wake_first', 'nonlead', BUILT, 'STUCK'),
    ('no_rewake', 'on_b', BUILT, True),         # a second line now (see the variant)
    ('built', 'finish', KILLS, True),
    ('finalize_returns', 'finish', BUILT, 'ONE_END'),
    ('built', 'early', KILLS, True),
    ('no_init_wait', 'early', BUILT, 'STATUS'),
    ('built', 'exiting', KILLS, True),
    ('exit_everywhere', 'exiting', BUILT, 'STUCK'),
    ('built', 'caller_exiting', BUILT, 'STUCK'),   # ends at the grace in the code
    ('status_everywhere', 'nonlead', BUILT, 'STATUS'),
    ('caller_zero', 'nonlead', BUILT, 'JOB_STATUS'),
    ('called_carries', 'nonlead', KILLS, 'NO_KILL'),
    ('called_carries', 'same_node_both', KILLS, 'NO_KILL'),
    ('no_carrier_wait', 'nonlead', KILLS, 'NO_KILL'),
    ('no_lead_wait', 'nonlead', KILLS, 'NO_KILL'),
    ('no_remote_wait', 'nonlead', KILLS, 'NO_KILL'),
    ('wait_every_node', 'nonlead', KILLS, 'STUCK'),
    ('no_exclusion', 'both', BUILT, 'STUCK'),
    # P1's proposal: the residual closed, every built case still passing, and
    # each of its rules shown needed.
    ('single', 'both', KILLS, True),
    ('single', 'both_lead_a', KILLS, True),
    ('single', 'both_lead_b', KILLS, True),
    ('single', 'three_lead_b', KILLS, True),
    ('single', 'nonlead', KILLS, True),
    ('single', 'lead', KILLS, True),
    ('single', 'on_b', KILLS, True),
    ('single', 'same_node_both', KILLS, True),
    ('single', 'nonlead', NO_HELP, True),
    ('single', 'finish', KILLS, True),
    ('single', 'early', KILLS, True),
    ('single', 'exiting', KILLS, True),
    ('single_old_send', 'both', KILLS, 'STUCK'),
    ('single_demoted_unwaited', 'both', KILLS, 'NO_KILL'),
    ('single_status_kept', 'both', KILLS, 'NO_KILL'),
    ('single_carrier_unmoved', 'both', KILLS, 'STUCK'),
    ('single_outranked_waits', 'both', KILLS, 'STUCK'),
    ('single_lowest_excludes', 'both', KILLS, 'NO_KILL'),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for variant, which, options, expect in CASES:
        name = case(variant, which, options)[0]
        if not keep(name):
            continue
        ok &= report(run(variant, which, options), expect=expect, tso=False)
    sys.exit(0 if ok else 1)
