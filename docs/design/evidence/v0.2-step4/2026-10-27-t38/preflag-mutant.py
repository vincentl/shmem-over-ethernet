# SPDX-License-Identifier: LGPL-3.0-or-later
"""Round nine, scale-and-performance: the "spin a little for latency" change
placed where an implementer would put it - before the owner raises the flag
that tells writers to wake it - rather than after, where round eight's and the
helper-wake record's mutants put it. A helper, and the lead's agent, that has
moved nothing polls its ring's completions and its dirty bits for up to 200 us
before raising its sleeping flag; work found goes round, none raises the flag
and sleeps as before. No wake can be lost: the flag's raise and its look at the
dirty bits are untouched. usage: preflag-mutant.py SOURCE_ROOT"""
import sys

p = sys.argv[1] + '/shmem/src/progress-host.c'
s = open(p).read()

helper_old = """        if (moved || raise_flag_and_look(helper) || unsent_asked(helper))
            continue;
        osoe_ring_wait_wake(o->ring, wake_word(helper), seen, OSOE_SWEEP_NS);"""
helper_new = """        if (!moved) {   /* MUTANT: spin up to 200 us before raising the flag. */
            uint64_t spin_until = osoe_now_ns() + 200000;
            const struct osoe_connection_area_owner *mw = &osoe_am.area.owner[helper];
            bool work = false;
            while (!work && osoe_now_ns() < spin_until) {
                if (io_uring_cq_ready(&o->ring->uring) ||
                    atomic_load_explicit(wake_word(helper), memory_order_acquire) != seen)
                    work = true;
                for (uint32_t w = 0; !work && w < mw->dirty_words; w++)
                    if (atomic_load_explicit(&mw->dirty[w], memory_order_acquire) != 0)
                        work = true;
            }
            if (work)
                continue;
        }
        if (moved || raise_flag_and_look(helper) || unsent_asked(helper))
            continue;
        osoe_ring_wait_wake(o->ring, wake_word(helper), seen, OSOE_SWEEP_NS);"""
assert s.count(helper_old) == 1, 'helper pattern'
s = s.replace(helper_old, helper_new)

agent_old = """    if (osoe_progress_poll(o) || moved)
        return true;
    if (raise_flag_and_look(0))
        return true;"""
agent_new = """    if (osoe_progress_poll(o) || moved)
        return true;
    {   /* MUTANT: spin up to 200 us before raising the flag. */
        uint64_t spin_until = osoe_now_ns() + 200000;
        const struct osoe_connection_area_owner *mw = &osoe_am.area.owner[0];
        while (osoe_now_ns() < spin_until) {
            if (io_uring_cq_ready(&o->ring->uring))
                return true;
            for (uint32_t w = 0; w < mw->dirty_words; w++)
                if (atomic_load_explicit(&mw->dirty[w], memory_order_acquire) != 0)
                    return true;
        }
    }
    if (raise_flag_and_look(0))
        return true;"""
assert s.count(agent_old) == 1, 'agent pattern'
s = s.replace(agent_old, agent_new)
open(p, 'w').write(s)
print('mutated', p)
