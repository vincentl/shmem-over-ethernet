/*
 * Agent threads (§3.12). Every PE starts one in shmem_init. The local lead's
 * agent will become the node's progress engine; every other agent does only
 * work that must run inside its own process. In phase 1 step 1 agents have no
 * work: they start, sleep on their slot's wake word, and stop.
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
