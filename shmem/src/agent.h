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

/* Ask the agent to stop and join it. */
void osoe_agent_stop(struct osoe_pe_slot *slot);

#endif /* OSOE_AGENT_H */
