#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A data wire made on demand while traffic waits for it (H23; design §6.2, "The
mesh is still eager"; the author's decisions of 2026-10-25,
docs/decisions/2026-10-24.md, "The author's answers", item 4).

With OSOE_MESH_DATA_WIRES=on-demand a job starts with its control wires alone,
eager to every node as they always were, and makes a node pair's data wire the
first time a PE of either node has something for the other. Nothing on the
writer's side changes for it: a connection whose data wire is not up keeps both
its buffers sealed, as every connection starts (§3.5), so a writer finds nothing
reservable, marks the connection for its owner, wakes it and sleeps on the
connection's turnover word - the wait it makes whenever no buffer has room. What
is new is the owner's side:

  * the owner of a connection with no data wire opens no buffer: **nothing is
    reservable, so nothing is sent, before the wire is up** at that end;
  * it asks for the wire once. Only the lead's agent dials and answers (§3.12),
    so with progress helpers the helper that owns the connection asks its lead,
    and with none the lead's agent is the owner;
  * **the schedule's caller always dials** (mesh-schedule.h): a lead the
    schedule gives the call dials at once; the other asks it over the pair's
    control wire, so no two calls cross and nothing has to resolve a crossing;
  * the dial is start-up's three-step hello - the caller's hello, the answer,
    the confirmation - after which the caller attaches its end; the callee
    attaches its own when the confirmation arrives, from its listener, which in
    this mode stays open for the job's life;
  * an end attached makes the connection the owner's to visit, which opens a
    buffer, and the turnover that opens it releases the waiting writers. With a
    helper the lead hands the socket over and the helper attaches it to its own
    ring, whose only submitter it is (ring.h).

An abort can arrive at any moment - a PE's global exit or fatal on either node,
or a node killed - and it ends the job as §3.15 and abort_model.py have it:
every wire's last segment is an abortive goodbye carrying the job's status, the
control wires always among them, and an end of stream without one is a peer
that died. So a data wire part-way through being made must not leave a FIN
without a goodbye on an end the other node has attached: the confirmation is the
hello's last step, and the caller attaches its end the moment it has sent it, so
**a callee that is aborting still serves a caller it has answered** until that
caller's confirmation or its end, attaches it, and sends its abortive goodbye
there too; a call or a caller not yet answered is closed, which nobody has
attached. A caller abandons its call when it aborts, and an abort waits for no
wire to be made.

Model: two nodes, 0 and 1, of which the schedule has node 0 call (two nodes have
one pair, the offset N / 2 from the lower node). Threads: a writer on either or
both nodes - a PE putting once toward the other node, then waiting in a quiet
for the peer to have applied it; each node's lead, which owns its data wire too
unless the case gives the node a progress helper; that helper; and a fault,
which fires once at any moment. A wire is FIFO each way, segments then the
abortive goodbye then the end of stream; the pair's control wire carries the
request for a data wire, then the goodbye, then its end. The call is the
listener's and the caller's phases as mesh_model.py has them (dialled,
answered, confirmed and attached by the caller, attached by the callee), and
dropped where a socket of it closes before both ends hold it; a call dropped
before its confirmation is dialled again, a retry and not a second call. A
node's processes exiting is one step: its kernel ends every stream it holds.
Kernel buffering, the wake protocol and the hello's own retries are abstracted:
a step that is enabled is taken eventually (lead_wake_model.py checks the wake;
mesh_model.py the hello's calls and accepts).

Properties
  EARLY      nothing is sent before the wire is up: no segment is sealed toward
             a peer whose data wire is not attached at the sending end.
  ONCE       no wire is made twice: no second call begun for a pair while one
             is in flight or made, and no end attached twice - the C's
             "connected to twice".
  RULE       no lead calls a node the schedule has call it: the C's listener
             refuses such a caller by name.
  STATUS     in a global exit every node ends with the caller's status, never
             with the failure a FIN without a goodbye leaves (abort_model.py's
             property, here over the wire being made).
  no hang    every writer's put is applied, or the job's end reaches every node
             and its processes exit: no terminal state otherwise (STUCK), and
             from every state such an end is still reachable (TRAPPED) - so no
             request is lost.

Variants
  design               as above.
  open_early           the owner opens a buffer before the wire is up, as the
                       eager mesh's first pass does. Fails EARLY.
  both_dial            the node the schedule does not give the call dials too,
                       where it should ask. Fails RULE.
  call_per_need        the caller begins a call for every need - its own
                       writer's and the peer's request - rather than once. Fails
                       ONCE.
  no_ask               the node that is not the caller waits for the caller to
                       need the wire on its own. Fails STUCK.
  no_visit             attaching the wire leaves the connection unvisited: the
                       writer's mark was taken while it had no wire, and no
                       buffer ever opens. Fails STUCK.
  abandon_answered     an aborting callee closes the caller it has answered,
                       whose end may be attached already. Fails STATUS.
  no_goodbye_new_wire  a wire attached while its node aborts gets no abortive
                       goodbye, only its end of stream. Fails STATUS.
  keep_calling         an aborting caller keeps its call, and its abort waits
                       for it, toward a node that has exited. Fails TRAPPED.
  abort_waits_wire     an abort waits for the data wire its node asked for.
                       Fails TRAPPED.

Usage: data_wire_model.py [substring ...]
"""
import sys

from engine import assemble, explore, report, select

CALLER, CALLEE = 0, 1
NODES = (CALLER, CALLEE)
FAILURE, STATUS = 1, 2          # what a node learns of the job's end: a failure, or a caller's status
NONE, DIALLED, ANSWERED, CONFIRMED, ATTACHED, DROPPED = range(6)
TAKEN = -1                      # a goodbye or an end of stream, read


def other(x):
    return 1 - x


def k(name, x=None):
    return ('$' + name,) if x is None else ('$' + name, x)


class Step:
    """One alternative of a thread's step: memory written, read back as it is
    written, and a failed property's message."""

    def __init__(self, M):
        self.M, self.upd, self.bad, self.loc = M, {}, None, {}

    def __getitem__(self, key):
        return self.upd[key] if key in self.upd else self.M[key]

    def __setitem__(self, key, value):
        self.upd[key] = value

    def fail(self, message):
        if self.bad is None:
            self.bad = message

    def result(self, label):
        loc = dict(self.loc)
        if self.bad is not None:
            loc['bad'] = self.bad
        return (self.upd, loc, label)


def attach(s, x, variant, helper):
    """Node x's lead has its end of the data wire: the callee's from its
    listener, the caller's at its confirmation. Handed to the helper that owns
    it, or attached here, where an aborting engine sends its goodbye on it and
    a running one visits it."""
    y = other(x)
    if helper:
        if s[k('handed', x)] or s[k('end', x)]:
            s.fail(f'ONCE: node {x} attached a second data wire toward node {y}')
        s[k('handed', x)] = 1
        return
    if s[k('end', x)]:
        s.fail(f'ONCE: node {x} attached a second data wire toward node {y}')
    s[k('end', x)] = 1
    if s[k('stopping', x)]:
        if variant != 'no_goodbye_new_wire':
            s[k('data_goodbye', x)] = s[k('abort', x)]
    elif variant != 'no_visit':
        s[k('visit', x)] = 1


def visit(s, x, variant):
    """The owner's visit of node x's connection toward the other node: with
    the wire up, open a buffer and send what writers committed; without it,
    say so (True) - the owner asks for it - and open nothing."""
    y = other(x)
    if not s[k('end', x)]:
        if variant == 'open_early':
            s[k('open', x)] = 1
            if s[k('committed', x)]:
                s.fail(f'EARLY: node {x} sent a segment toward node {y} before its data '
                       f'wire was up')
        return True
    s[k('open', x)] = 1
    n = s[k('committed', x)]
    if n:
        s[k('committed', x)] = 0
        s[k('segments', x)] = s[k('segments', x)] + n
    return False


def need_wire(s, x, variant):
    """Node x's lead acts on a need for the data wire - its writer's, its
    helper's, or the peer's request: once, dialling if the schedule has it
    call, and asking the peer over their control wire if not."""
    y = other(x)
    if s[k('acted', x)] and variant != 'call_per_need':
        return
    s[k('acted', x)] = 1
    if x == CALLER:
        if s[k('call')] not in (NONE, DROPPED):
            s.fail(f'ONCE: node {x} began a second call for its data wire toward node {y}')
        s[k('call')] = DIALLED
        s[k('dials')] = s[k('dials')] + 1
    elif variant == 'both_dial':
        s[k('wrong_call')] = 1
    elif variant != 'no_ask':
        s[k('ask', x)] = 1


def receive_data(s, x, label):
    """The owner reads the data wire from the other node: segments, then its
    abortive goodbye, then its end of stream - which without a goodbye is a peer
    that died."""
    y = other(x)
    if s[k('segments', y)]:
        s[k('segments', y)] = s[k('segments', y)] - 1
        s[k('applied', x)] = s[k('applied', x)] + 1
        return f'{label} applies a segment from node {y}'
    if s[k('data_goodbye', y)] > 0:
        if not s[k('abort', x)]:
            s[k('abort', x)] = s[k('data_goodbye', y)]
        s[k('data_goodbye', y)] = TAKEN
        return f'{label} reads node {y}\'s abortive goodbye on the data wire'
    if s[k('data_end', y)] > 0:
        if not s[k('abort', x)]:
            s[k('abort', x)] = FAILURE
        s[k('data_end', y)] = TAKEN
        return f'{label} reads the end of node {y}\'s data wire'
    return None


def close_sockets(s, x):
    """Node x's processes have exited, or were killed: its kernel ends every
    stream it holds, after what is queued."""
    s[k('exited', x)] = 1
    s[k('control_end', x)] = 1
    if s[k('end', x)] or s[k('handed', x)]:
        s[k('data_end', x)] = 1
    call = s[k('call')]
    if x == CALLER and call in (DIALLED, ANSWERED):
        s[k('call')] = DROPPED
    if x == CALLEE and call in (DIALLED, ANSWERED, CONFIRMED):
        s[k('call')] = DROPPED
        if call == CONFIRMED:
            s[k('data_end', x)] = 1         # the caller's end is attached


def lead(variant, x, helper, fault):
    """Node x's lead: its agent, which owns the control wire, the listener and
    the calls, and the data wire too unless `helper`."""
    y = other(x)
    caller = x == CALLER
    name = f'node {x}'

    def step(M, L):
        if M[k('exited', x)]:
            return []
        out = []

        def alternative(build):
            s = Step(M)
            label = build(s)
            if label:
                out.append(s.result(label))

        stopping = M[k('stopping', x)]
        # A writer's mark, and a visit an attach made: the owner's.
        if not helper:
            def take_mark(s):
                if not s[k('mark', x)] or stopping:
                    return None
                s[k('mark', x)] = 0
                if visit(s, x, variant):
                    need_wire(s, x, variant)
                return f'{name} visits its connection, a writer\'s mark'
            alternative(take_mark)

            def take_visit(s):
                if not s[k('visit', x)] or stopping:
                    return None
                s[k('visit', x)] = 0
                visit(s, x, variant)
                return f'{name} visits its connection, its wire attached'
            alternative(take_visit)

            def read_data(s):
                if not s[k('end', x)]:
                    return None
                return receive_data(s, x, name)
            alternative(read_data)
        else:
            def take_want(s):
                if not s[k('wanted', x)] or stopping:
                    return None
                s[k('wanted', x)] = 0
                need_wire(s, x, variant)
                return f'{name} takes its helper\'s request for the data wire'
            alternative(take_want)

        # The control wire from the other node, in order.
        def read_control(s):
            if s[k('ask', y)]:
                s[k('ask', y)] = 0
                if not caller:
                    s.fail(f'RULE: node {x} was asked for a data wire by node {y}, which the '
                           f'schedule has call it')
                if not stopping:
                    need_wire(s, x, variant)
                return f'{name} reads node {y}\'s request for the data wire'
            if s[k('control_goodbye', y)] > 0:
                if not s[k('abort', x)]:
                    s[k('abort', x)] = s[k('control_goodbye', y)]
                s[k('control_goodbye', y)] = TAKEN
                return f'{name} reads node {y}\'s abortive goodbye on the control wire'
            if s[k('control_end', y)] > 0:
                if not s[k('abort', x)]:
                    s[k('abort', x)] = FAILURE
                s[k('control_end', y)] = TAKEN
                return f'{name} reads the end of node {y}\'s control wire'
            return None
        alternative(read_control)

        call = M[k('call')]
        if caller:
            def wrong_caller(s):
                if s[k('wrong_call')] != 1:
                    return None
                s[k('wrong_call')] = TAKEN
                s.fail(f'RULE: node {x} was called by node {y}, which the mesh\'s schedule has '
                       f'node {x} call')
                return f'{name} answers a call from node {y}'
            alternative(wrong_caller)

            def confirm(s):
                if call != ANSWERED or stopping:
                    return None
                s[k('call')] = CONFIRMED
                attach(s, x, variant, helper)
                return f'{name} confirms and attaches its end'
            alternative(confirm)

            def redial(s):
                if (call != DROPPED or stopping and variant != 'keep_calling' or
                        not s[k('acted', x)] or s[k('end', x)] or s[k('handed', x)]):
                    return None
                s[k('call')] = DIALLED
                return f'{name} dials again'
            alternative(redial)
        else:
            def answer(s):
                if call != DIALLED or stopping:
                    return None
                s[k('call')] = ANSWERED
                return f'{name} answers node {y}\'s hello'
            alternative(answer)

            def attach_caller(s):
                if call != CONFIRMED:
                    return None
                if stopping and variant == 'abandon_answered':
                    return None
                s[k('call')] = ATTACHED
                attach(s, x, variant, helper)
                return f'{name} reads node {y}\'s confirmation and attaches its end'
            alternative(attach_caller)

        # The abort's start: no more of the job's own traffic; a goodbye on the
        # control wire and on every end this lead owns; its own call abandoned.
        def abort_start(s):
            if not s[k('abort', x)] or stopping:
                return None
            s[k('stopping', x)] = 1
            s[k('control_goodbye', x)] = s[k('abort', x)]
            if not helper:
                s[k('mark', x)] = s[k('visit', x)] = s[k('committed', x)] = 0
                if s[k('end', x)]:
                    s[k('data_goodbye', x)] = s[k('abort', x)]
            if caller and call in (DIALLED, ANSWERED) and variant != 'keep_calling':
                s[k('call')] = DROPPED
            if not caller and call in (ANSWERED, CONFIRMED) and variant == 'abandon_answered':
                s[k('call')] = DROPPED
                if call == CONFIRMED:
                    s[k('data_end', x)] = 1     # the caller's end is attached
            return f'{name} begins its abort (it learnt {s[k("abort", x)]})'
        alternative(abort_start)

        def abort_done(s):
            if not stopping or s[k('aborted', x)]:
                return None
            if not caller and call in (ANSWERED, CONFIRMED):
                return None                     # a caller part-way, served to its end
            if caller and variant == 'keep_calling' and call in (DIALLED, ANSWERED, DROPPED) \
                    and not s[k('end', x)]:
                return None
            if helper and (not s[k('helper_stopping', x)] or s[k('handed', x)]):
                return None
            if variant == 'abort_waits_wire' and s[k('acted', x)] and not s[k('end', x)]:
                return None
            s[k('aborted', x)] = 1
            return f'{name} has sent its goodbyes'
        alternative(abort_done)

        def leave(s):
            if not s[k('aborted', x)]:
                return None
            close_sockets(s, x)
            s.loc['finished'] = 1
            return f'{name}\'s processes exit'
        alternative(leave)
        return out

    def status(M, L):
        if fault and fault[1] == 'exit' and M[k('abort', x)] != STATUS:
            return (f'STATUS: node {x} ended with a failure, an end of stream without its '
                    f'goodbye, where a PE of node {fault[0]} gave its status')
        return None

    return assemble([('STEP', ('nd', step, '@BAD?')),
                     ('BAD?', ('br', lambda L: L['bad'], '@FAIL', '@LEFT?')),
                     ('FAIL', ('check', lambda M, L: L['bad'], '@END')),
                     ('LEFT?', ('br', lambda L: L['finished'], '@STATUS', '@STEP')),
                     ('STATUS', ('check', status, '@END')),
                     ('END', ('done',))])


def progress_helper(variant, x):
    """Node x's progress helper, the owner of its data wire: it asks its lead
    for the wire, and attaches the socket its lead hands it to its own ring."""
    y = other(x)
    name = f'node {x}\'s helper'

    def step(M, L):
        if M[k('exited', x)]:
            return [({}, {'finished': 1}, f'{name} ends with its process')]
        out = []

        def alternative(build):
            s = Step(M)
            label = build(s)
            if label:
                out.append(s.result(label))

        stopping = M[k('helper_stopping', x)]

        def take_mark(s):
            if not s[k('mark', x)] or stopping:
                return None
            s[k('mark', x)] = 0
            if visit(s, x, variant) and not s[k('asked_lead', x)]:
                s[k('asked_lead', x)] = 1
                s[k('wanted', x)] = 1
            return f'{name} visits its connection, a writer\'s mark'
        alternative(take_mark)

        def take_socket(s):
            if not s[k('handed', x)]:
                return None
            s[k('handed', x)] = 0
            if s[k('end', x)]:
                s.fail(f'ONCE: node {x} attached a second data wire toward node {y}')
            s[k('end', x)] = 1
            if stopping:
                if variant != 'no_goodbye_new_wire':
                    s[k('data_goodbye', x)] = s[k('abort', x)]
            elif variant != 'no_visit':
                visit(s, x, variant)
            return f'{name} attaches the socket its lead handed it'
        alternative(take_socket)

        def read_data(s):
            if not s[k('end', x)]:
                return None
            return receive_data(s, x, name)
        alternative(read_data)

        def abort_start(s):
            if not s[k('abort', x)] or stopping:
                return None
            s[k('helper_stopping', x)] = 1
            s[k('mark', x)] = s[k('committed', x)] = 0
            if s[k('end', x)]:
                s[k('data_goodbye', x)] = s[k('abort', x)]
            return f'{name} begins its abort'
        alternative(abort_start)
        return out

    return assemble([('STEP', ('nd', step, '@BAD?')),
                     ('BAD?', ('br', lambda L: L['bad'], '@FAIL', '@LEFT?')),
                     ('FAIL', ('check', lambda M, L: L['bad'], '@END')),
                     ('LEFT?', ('br', lambda L: L['finished'], '@END', '@STEP')),
                     ('END', ('done',))])


def writer(x):
    """A PE of node x: one put toward the other node, then a quiet that waits
    for it to be applied. A writer that finds nothing reservable marks the
    connection and sleeps on its turnover; an abort ends it at its next sweep."""
    y = other(x)

    def reserve(M, L):
        if M[k('abort', x)]:
            return [({}, {'stage': 'end'}, f'node {x}\'s writer ends with the job')]
        if M[k('open', x)]:
            return [({k('committed', x): M[k('committed', x)] + 1, k('mark', x): 1},
                     {'stage': 'quiet'}, f'node {x}\'s writer commits its put and marks')]
        return [({k('mark', x): 1}, {'stage': 'wait'},
                 f'node {x}\'s writer finds nothing reservable, marks and sleeps')]

    def wait(M, L):
        if M[k('abort', x)]:
            return [({}, {'stage': 'end'}, f'node {x}\'s writer ends with the job')]
        if M[k('open', x)]:
            return [({}, {'stage': 'reserve'}, f'node {x}\'s writer wakes at the turnover')]
        return []

    def quiet(M, L):
        if M[k('abort', x)]:
            return [({}, {}, f'node {x}\'s writer ends with the job')]
        if M[k('applied', y)]:
            return [({k('writer_done', x): 1}, {}, f'node {x}\'s quiet returns')]
        return []

    return assemble([('RESERVE', ('nd', reserve, '@AFTER')),
                     ('AFTER', ('br', lambda L: L['stage'] == 'quiet', '@QUIET', '@WAIT?')),
                     ('WAIT?', ('br', lambda L: L['stage'] == 'wait', '@WAIT', '@END')),
                     ('WAIT', ('nd', wait, '@WOKEN')),
                     ('WOKEN', ('br', lambda L: L['stage'] == 'reserve', '@RESERVE', '@END')),
                     ('QUIET', ('nd', quiet, '@END')),
                     ('END', ('done',))])


def fault_thread(fault):
    """The job's end, once, at any moment: a PE of the node calls
    shmem_global_exit (`exit`, a status) or a fatal (`fatal`, a failure), or
    the node is killed (`crash`: its processes end with no goodbye)."""
    x, kind = fault

    def fire(M, L):
        if kind == 'crash':
            if M[k('exited', x)]:
                return [({}, {}, f'node {x} has exited already')]
            s = Step(M)
            s[k('abort', x)] = s[k('abort', x)] or FAILURE
            s[k('stopping', x)] = s[k('helper_stopping', x)] = s[k('aborted', x)] = 1
            close_sockets(s, x)
            return [s.result(f'node {x} is killed')]
        learnt = STATUS if kind == 'exit' else FAILURE
        if M[k('abort', x)]:
            return [({}, {}, f'a PE of node {x} finds the job ending already')]
        return [({k('abort', x): learnt}, {},
                 f'a PE of node {x} calls {"shmem_global_exit" if kind == "exit" else "a fatal"}')]

    return assemble([('FIRE', ('nd', fire, '@END')), ('END', ('done',))])


def case(variant, writers, helper, fault):
    name = (f'data_wire/{variant}/writers{"".join(map(str, writers)) or "-"}'
            f'/helper{"-" if helper is None else helper}'
            f'/{"no-fault" if fault is None else f"{fault[1]}@{fault[0]}"}')
    threads, roles = [], []
    for x in NODES:
        threads.append(lead(variant, x, helper == x, fault))
        roles.append('lead')
    if helper is not None:
        threads.append(progress_helper(variant, helper))
        roles.append('helper')
    for x in writers:
        threads.append(writer(x))
        roles.append('writer')
    if fault is not None:
        threads.append(fault_thread(fault))
        roles.append('fault')
    mem = {k('call'): NONE, k('dials'): 0, k('wrong_call'): 0}
    for x in NODES:
        for key in ('mark', 'visit', 'open', 'committed', 'end', 'handed', 'wanted', 'asked_lead',
                    'acted', 'ask', 'control_goodbye', 'control_end', 'segments', 'data_goodbye',
                    'data_end', 'applied', 'abort', 'stopping', 'helper_stopping', 'aborted',
                    'exited', 'writer_done'):
            mem[k(key, x)] = 0
    return name, threads, roles, mem


def run(variant, writers, helper, fault, out=sys.stdout):
    name, threads, roles, mem = case(variant, writers, helper, fault)
    if fault is None:
        done = lambda M: all(M[k('writer_done', x)] for x in writers)
    else:
        done = lambda M: M[k('exited', 0)] and M[k('exited', 1)]
    return explore(name, threads, mem, out=out, done=done,
                   idle_ok=lambda i, M, L: roles[i] in ('lead', 'helper'),
                   liveness=True, max_seconds=1200, liveness_max_states=2_000_000)


FAULTS = (None, (0, 'exit'), (1, 'exit'), (0, 'fatal'), (1, 'crash'), (0, 'crash'))
WRITERS = ((1,), (0,), (0, 1))
HELPERS = (None, 0, 1)

# (variant, writers, helper, fault, expect: True, or the property it must fail)
CASES = [('design', w, h, f, True) for h in HELPERS for w in WRITERS for f in FAULTS] + [
    ('open_early', (1,), None, None, 'EARLY'),
    ('open_early', (0,), 0, None, 'EARLY'),
    ('both_dial', (1,), None, None, 'RULE'),
    ('call_per_need', (0, 1), None, None, 'ONCE'),
    ('no_ask', (1,), None, None, 'STUCK'),
    ('no_visit', (1,), None, None, 'STUCK'),
    ('abandon_answered', (1,), None, (1, 'exit'), 'STATUS'),
    ('no_goodbye_new_wire', (1,), None, (1, 'exit'), 'STATUS'),
    ('no_goodbye_new_wire', (1,), 1, (1, 'exit'), 'STATUS'),
    ('keep_calling', (0,), None, (1, 'exit'), 'TRAPPED'),
    ('abort_waits_wire', (1,), None, (0, 'exit'), 'TRAPPED'),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for variant, writers, helper, fault, expect in CASES:
        name, _, _, _ = case(variant, writers, helper, fault)
        if not keep(name):
            continue
        res = run(variant, writers, helper, fault)
        ok &= report(res, expect=expect, tso=False)
    sys.exit(0 if ok else 1)
