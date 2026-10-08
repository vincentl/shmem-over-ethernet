/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Agent threads (§3.12). Every PE starts one in shmem_init, and lends it to
 * the backend through transport.h's osoe_transport_agent_pass, _wait and
 * _exit. On AM the local lead's agent is the node's
 * progress engine; every other agent does only work that must run inside its
 * own process, which today is none: they start, sleep on their slot's wake
 * word, and stop.
 */
#ifndef OSOE_AGENT_H
#define OSOE_AGENT_H

#include "node.h"

/* Start this PE's agent with every asynchronous signal blocked, so
 * application signal handlers keep running on application threads. */
void osoe_agent_start(struct osoe_pe_slot *slot);

/*
 * For a thread of the node's progress engine that the agent starts beside it -
 * a progress helper (progress-host.c): the scheduler slice the agent asks, and
 * the CPUs OSOE_AGENT_CPUS gives it, set on the calling thread, as the agent
 * sets its own. Read by osoe_agent_start, which every helper follows.
 */
void osoe_agent_place_this_thread(void);

/* Ask the agent to stop and join it. */
void osoe_agent_stop(struct osoe_pe_slot *slot);

/* Whether this PE's agent is running: started, and not yet joined. */
bool osoe_agent_running(void);

/* Whether this PE's agent is running and the calling thread is not it: then
 * the agent, not the caller, ends the process on an abort (abort.h), since it
 * may have the network to tell first. */
bool osoe_agent_speaks(void);

/* Whether the calling thread is this PE's agent, running: a fatal it raises
 * itself then says where this PE's own thread was (fatal.c). */
bool osoe_agent_is_this_thread(void);

#endif /* OSOE_AGENT_H */
