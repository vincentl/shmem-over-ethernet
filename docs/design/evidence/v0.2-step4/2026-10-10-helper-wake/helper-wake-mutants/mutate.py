# SPDX-License-Identifier: LGPL-3.0-or-later
"""The scale investigator's mutant: a progress helper that spins up to 200 us,
watching its ring's completions and its wake word, before it sleeps in its idle
wait - the "spin a little for latency" change. usage: mutate.py SOURCE_ROOT"""
import sys
p = sys.argv[1] + '/shmem/src/progress-host.c'
s = open(p).read()
old = "        osoe_ring_wait_wake(o->ring, wake_word(helper), seen, OSOE_SWEEP_NS);\n    }\n}"
new = """        {   /* MUTANT: spin up to 200 us before the idle wait. */
            uint64_t spin_until = osoe_now_ns() + 200000;
            bool woke = false;
            while (osoe_now_ns() < spin_until)
                if (io_uring_cq_ready(&o->ring->uring) ||
                    atomic_load_explicit(wake_word(helper), memory_order_acquire) != seen) {
                    woke = true;
                    break;
                }
            if (!woke)
                osoe_ring_wait_wake(o->ring, wake_word(helper), seen, OSOE_SWEEP_NS);
        }
    }
}"""
assert s.count(old) == 1, 'pattern'
s = s.replace(old, new)
open(p, 'w').write(s)
print('mutated', p)
