# SPDX-License-Identifier: LGPL-3.0-or-later
"""The scale investigator's second mutant: the lead's agent spins up to 200 us,
watching its ring's completions and its wake word, before its idle wait.
usage: mutate-agent.py SOURCE_ROOT"""
import sys
p = sys.argv[1] + '/shmem/src/progress-host.c'
s = open(p).read()
old = """    if (osoe_am.lead)
        return osoe_ring_wait_wake(&osoe_am.owner[0].ring, wake, seen, OSOE_SWEEP_NS);"""
new = """    if (osoe_am.lead) {
        /* MUTANT: spin up to 200 us before the idle wait. */
        uint64_t spin_until = osoe_now_ns() + 200000;
        while (osoe_now_ns() < spin_until)
            if (io_uring_cq_ready(&osoe_am.owner[0].ring.uring) ||
                atomic_load_explicit(wake, memory_order_acquire) != seen)
                return true;
        return osoe_ring_wait_wake(&osoe_am.owner[0].ring, wake, seen, OSOE_SWEEP_NS);
    }"""
assert s.count(old) == 1, 'pattern'
s = s.replace(old, new)
open(p, 'w').write(s)
print('mutated', p)
