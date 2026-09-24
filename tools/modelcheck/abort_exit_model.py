#!/usr/bin/env python3
"""How a process ends when its node aborts (design §3.15; phase 1 step 4).

abort_model.py decides how an abort crosses nodes: as an abortive goodbye, the
last segment a dying lead sends on each connection, read before the end of
stream it precedes. This model is the other half: which thread of which
process ends it, and when, so that the goodbye is sent before the lead's
sockets close, and so that every process of the job ends with a status some
caller of shmem_global_exit gave (spec §9.1.6).

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
  * A library wait's sweep reads the word; with the development watchdog on it
    then checks its siblings' pids, and a dead one is a fatal - unless the word
    is set by then, since a PE an abort ended is dead too. A fatal whose own
    declaration loses to a global exit ends as that global exit does.
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
            library wait with the watchdog on, sweeping.
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
            stdio (spec §9.1.6); every other ends with fflush and _exit, which
            takes no lock and runs no handler beside a main thread still
            running (the author's decision after the third review).

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
            for a crash.

Variants (each breaks one rule above)
  caller_exits      the caller ends its process itself. On the lead, the
                    process ends before its agent can send: STATUS.
  neither           the watchdog does not re-read the word before calling a
                    dead sibling a fatal, and a fatal that loses to a global
                    exit still ends with a failure: STATUS.
  watchdog_unchecked, fatal_unrouted
                    one of those two defences each. Either alone suffices
                    here; the tree has both, since the first also spares a
                    misleading message and the second covers any fatal an
                    abort provokes, not only the watchdog's.
  pidfd_fatal       the lead's agent takes a sibling's death for a fatal
                    rather than declaring one, and ends through the fatal path
                    without sending: STATUS.
  wake_first        a declarer bumps the wake words before the CAS: an agent
                    can take the bump, find the word still clear, and sleep on
                    the new value. STUCK.
  finalize_returns  shmem_finalize stands aside by returning, on any thread:
                    a main thread that reaches it once its agent is ending
                    the process returns into main's exit(0), a second exit
                    beside the agent's (phase 1 step 4's review). STATUS.
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
                    case that shows it has B's P call. STUCK.

Properties
  TERMINATES  every process ends: no state is STUCK with a process alive and
              every thread asleep, and from every state some state in which all
              have ended is reachable (the trapped-state check).
  STATUS      every process ends with a status some caller gave; in these
              cases none is a fatal or a crash, so a failure never is.

Usage: abort_exit_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select

FAIL = 1                        # the status a fatal leaves with
FATAL, GLOBAL = 1, 2            # the word's dispositions
MAIN, AGENT = 1, 2              # the holders of exit's lock
NODES = ('A', 'B')
PEER = {'A': 'B', 'B': 'A'}
# Each node's processes in local-id order, the lead first. B has a third, Q,
# only in the case that needs one; every other case is smaller without it.
LOCAL = {'A': ('LA', 'PA'), 'B': ('LB', 'PB')}
LOCAL_WITH_Q = {'A': ('LA', 'PA'), 'B': ('LB', 'PB', 'QB')}
NODE_OF = {p: x for x in NODES for p in LOCAL_WITH_Q[x]}


def is_lead(p):
    return p in ('LA', 'LB')


def WORD(x):        return ('$word', x)            # 0, or disposition * 16 + status
def WAKE(p):        return ('$wake', p)            # the agent's wake word
def ENDING(p):      return ('$ending', p)          # abort.c's process-private claim
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


def disposition(word):
    return word // 16


def exit_status(word):
    return FAIL if disposition(word) == FATAL else word % 16


# ---- steps shared by every thread ------------------------------------------

def end_process(M, p, status, label):
    """p ends: its threads stop; a lead's kernel ends its stream after what it
    queued."""
    upd = {ALIVE(p): 0, STAT(p): status}
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
 PC_EXITING) = range(9)


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
                upd = {QUEUED(y, x): 0, HEARD(x, y): 1}
                loc = start_declaring(value, PC_TOP)
                return [(upd, loc, f'{name}: reads {y}\'s ABORT (status {exit_status(value)})')]
            if M[KFIN(y, x)] and not M[EOF(x, y)]:
                upd = {EOF(x, y): 1}
                if M[HEARD(x, y)]:
                    return [(upd, {'pc': PC_TOP}, f'{name}: reads {y}\'s end of stream, after its ABORT')]
                loc = start_declaring(FATAL * 16 + FAIL, PC_TOP)
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
                            upd[WORD(x)] = FATAL * 16 + FAIL
                            w = upd[WORD(x)]
                        if M[ENDING(p)]:
                            return [(upd, {'pc': PC_PAUSE}, f'{name}: {q} has died; pauses')]
                        upd[ENDING(p)] = 1
                        u, _, lbl = end_process(M, p, exit_status(w), f'a fatal for {q}')
                        upd.update(u)
                        return [(upd, {}, f'{name}: {q} has died - ' + lbl)]
                    loc = start_declaring(FATAL * 16 + FAIL, PC_TOP)
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
            dead_peer = M[EOF(x, y)] and not M[HEARD(x, y)]
            if not M[SENT(x, y)] and not dead_peer:
                return [({QUEUED(x, y): M[WORD(x)], SENT(x, y): 1}, {'pc': PC_END},
                         f'{name}: sends {y} the abortive goodbye')]
            return [({}, {'pc': PC_END}, f'{name}: has told every peer')]
        if pc == PC_END:
            # osoe_abort_end from the agent: claim the process, then end it -
            # two steps, since exit runs the process's handlers and flushes
            # before it is gone, and other threads run meanwhile.
            if M[ENDING(p)]:
                return [({}, {'pc': PC_PAUSE}, f'{name}: another thread ends this process')]
            return [({ENDING(p): 1}, {'pc': PC_EXITING}, f'{name}: claims the process')]
        if pc == PC_EXITING:
            # exit(s) only where this process's program called shmem_global_exit,
            # and exit's lock is then needed; everywhere else fflush and _exit.
            calls_exit = disposition(M[WORD(x)]) == GLOBAL and (
                M[CALLED(p)] or variant == 'exit_everywhere')
            if calls_exit:
                if M[EXIT_LOCK(p)] == MAIN:
                    return []           # exit's lock, held for good by the main thread
                upd, _, lbl = end_process(M, p, exit_status(M[WORD(x)]), 'its agent, by exit')
                upd[EXIT_LOCK(p)] = AGENT
                return [(upd, {}, lbl)]
            return [end_process(M, p, exit_status(M[WORD(x)]), 'its agent, by _exit')]
        if pc == PC_PAUSE:
            return []                   # until the process ends
        raise AssertionError(pc)

    return step, PC_TOP


# ---- main threads -------------------------------------------------------------

(PC_CALL, PC_MAIN_END, PC_SWEEP, PC_LIVENESS, PC_RECHECK, PC_FATAL_TAIL) = range(20, 26)


def main_end(variant, M, p, name):
    """osoe_abort_end from a thread that is not the agent: it waits for the
    agent, unless the variant has it end the process itself."""
    x = NODE_OF[p]
    if variant == 'caller_exits':
        if M[ENDING(p)]:
            return [({}, {'pc': PC_PAUSE}, f'{name}: another thread ends this process')]
        u, _, lbl = end_process(M, p, exit_status(M[WORD(x)]), 'its caller')
        u[ENDING(p)] = 1
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
            return [({CALLED(p): 1}, start_declaring(GLOBAL * 16 + status, PC_MAIN_END),
                     f'{name}: calls shmem_global_exit({status})')]
        if pc == PC_MAIN_END:
            return main_end(variant, M, p, name)
        if pc == PC_PAUSE:
            return []
        raise AssertionError(pc)

    return step, PC_CALL


def waiter(variant, local, p):
    """A library wait that never completes, sweeping, with the watchdog on."""
    x = NODE_OF[p]
    name = f'{p}\'s main thread'
    siblings = tuple(q for q in local[x] if q != p)

    def on_word(M, w):
        if disposition(w) == GLOBAL:
            return main_end(variant, M, p, name)
        u, _, lbl = end_process(M, p, FAIL, 'a fatal: the job is aborting')
        return [(u, {}, lbl)]

    def step(M, L):
        if not M[ALIVE(p)]:
            return [({}, {'finished': 1}, f'{name}: its process has ended')]
        d = declare_step(variant, local, M, L, x, name)
        if d is not None:
            return d
        pc = L['pc']
        if pc == PC_SWEEP:
            w = M[WORD(x)]
            if w:
                return on_word(M, w)
            return [({}, {'pc': PC_LIVENESS}, f'{name}: its sweep finds no abort')]
        if pc == PC_LIVENESS:
            dead = [q for q in siblings if not M[ALIVE(q)]]
            if not dead:
                return [({}, {'pc': PC_SWEEP}, f'{name}: every sibling is alive')]
            unchecked = variant in ('watchdog_unchecked', 'neither')
            if unchecked:
                return [({}, start_declaring(FATAL * 16 + FAIL, PC_FATAL_TAIL),
                         f'{name}: {dead[0]} has died: a fatal')]
            return [({}, {'pc': PC_RECHECK}, f'{name}: {dead[0]} has died; reads the word again')]
        if pc == PC_RECHECK:
            w = M[WORD(x)]
            if w:
                return on_word(M, w)
            return [({}, start_declaring(FATAL * 16 + FAIL, PC_FATAL_TAIL),
                     f'{name}: the word is clear: a fatal')]
        if pc == PC_FATAL_TAIL:
            # fatal.c after its declaration: a global exit declared first ends
            # this PE as that exit does; anything else, _exit with a failure.
            w = M[WORD(x)]
            routed = variant not in ('fatal_unrouted', 'neither')
            if routed and disposition(w) == GLOBAL:
                return main_end(variant, M, p, name)
            u, _, lbl = end_process(M, p, FAIL, 'a fatal')
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
                u, _, lbl = end_process(M, p, exit_status(w), 'its main thread, no agent yet')
                u[ENDING(p)] = 1
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

def thread(step, initial, callers):
    def checked(M, L):
        out = []
        for upd, loc, label in step(M, L):
            bad = 0
            for k, v in upd.items():
                if k[0] == '$status' and v not in callers:
                    bad = 1
            out.append((upd, dict(loc, bad=bad), label))
        return out

    return assemble([('INIT', ('set', 'pc', lambda L: initial, '@STEP')),
                     ('STEP', ('nd', checked, '@BAD?')),
                     ('BAD?', ('br', lambda L: L['bad'], '@FAIL', '@FINISHED?')),
                     ('FAIL', ('check', lambda M, L: 'STATUS: a process exited with a status '
                               'no caller of shmem_global_exit gave', '@END')),
                     ('FINISHED?', ('br', lambda L: L['finished'], '@END', '@STEP')),
                     ('END', ('done',))])


CASES_OF = {
    'nonlead': {'callers': {'PA': 5}, 'waiters': ('LA',), 'local': LOCAL},
    'lead': {'callers': {'LA': 5}, 'waiters': (), 'local': LOCAL},
    'both': {'callers': {'PA': 5, 'PB': 6}, 'waiters': (), 'local': LOCAL},
    'on_b': {'callers': {'PB': 5}, 'waiters': (), 'local': LOCAL_WITH_Q},
    'finish': {'callers': {'PA': 5}, 'waiters': (), 'finishers': ('LA',), 'local': LOCAL},
    'early': {'callers': {}, 'early_callers': {'PA': 5}, 'waiters': (), 'connecting': ('LA',),
              'local': LOCAL},
    'exiting': {'callers': {'PA': 5}, 'waiters': (), 'exiters': ('LA',), 'local': LOCAL},
}


def case(variant, which, options):
    shape = CASES_OF[which]
    local = shape['local']
    processes = [p for x in NODES for p in local[x]]
    callers = set(shape['callers'].values()) | set(shape.get('early_callers', {}).values())
    threads = []
    for p in processes:
        step, initial = agent(variant, local, p, options)
        threads.append(thread(step, initial, callers))
    for p, status in shape['callers'].items():
        step, initial = caller(variant, local, p, status)
        threads.append(thread(step, initial, callers))
    callers |= set(shape.get('early_callers', {}).values())
    for p in shape['waiters']:
        step, initial = waiter(variant, local, p)
        threads.append(thread(step, initial, callers))
    for p in shape.get('finishers', ()):
        step, initial = finisher(variant, local, p)
        threads.append(thread(step, initial, callers))
    for p in shape.get('connecting', ()):
        step, initial = connecting_lead(variant, local, p)
        threads.append(thread(step, initial, callers))
    for p, status in shape.get('early_callers', {}).items():
        step, initial = early_caller(variant, local, p, status)
        threads.append(thread(step, initial, callers))
    for p in shape.get('exiters', ()):
        step, initial = exiter(variant, local, p)
        threads.append(thread(step, initial, callers))
    mem = {}
    for x in NODES:
        mem[WORD(x)] = 0
        y = PEER[x]
        for k in (QUEUED, KFIN, SENT, HEARD, EOF):
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


BUILT = {'pidfd': True, 'sweeps': False}
PART_B = {'pidfd': False, 'sweeps': True}
NO_HELP = {'pidfd': False, 'sweeps': False}

CASES = [
    ('built', 'nonlead', BUILT, True),
    ('built', 'lead', BUILT, True),
    ('built', 'both', BUILT, True),
    ('built', 'on_b', BUILT, True),
    ('built', 'nonlead', PART_B, True),
    ('built', 'nonlead', NO_HELP, False),     # Part B needs its sweeps: the finding
    ('caller_exits', 'lead', BUILT, False),
    ('neither', 'nonlead', BUILT, False),
    ('watchdog_unchecked', 'nonlead', BUILT, True),
    ('fatal_unrouted', 'nonlead', BUILT, True),
    ('pidfd_fatal', 'nonlead', BUILT, False),
    ('wake_first', 'nonlead', BUILT, False),
    ('no_rewake', 'on_b', {'pidfd': True, 'sweeps': False}, False),
    ('built', 'finish', BUILT, True),
    ('finalize_returns', 'finish', BUILT, False),
    ('built', 'early', BUILT, True),
    ('no_init_wait', 'early', BUILT, False),
    ('built', 'exiting', BUILT, True),
    ('exit_everywhere', 'exiting', BUILT, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for variant, which, options, expect in CASES:
        name = case(variant, which, options)[0]
        if not keep(name):
            continue
        ok &= report(run(variant, which, options), expect_ok=expect, tso=False)
    sys.exit(0 if ok else 1)
