/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The link a job's large collectives choose their shapes from (v0.2 step 2;
 * collective-shape.h): measured at the end of shmem_init by the root's lead
 * against its first child's, through the data path with both progress threads
 * running - the hop a collective pays - and handed to every PE before
 * shmem_init returns, so that every node chooses the same shape for the same
 * call (the author, docs/decisions/2026-10-06.md item 4).
 *
 * Nothing is measured, and no message sent for it, when the job is one node,
 * when OSOE_COLLECTIVE_SHAPE is `fixed`, or when the OSOE_LINK_* settings give
 * the link by hand: each is the same on every PE, so every PE takes the same
 * path through osoe_link_establish.
 */
#ifndef OSOE_LINK_MEASURE_H
#define OSOE_LINK_MEASURE_H

/*
 * Establish the link, into osoe.job.link and osoe.job.link_source: every PE
 * calls it at the end of session start, once its node's lead speaks. In a
 * job whose link is measured it is collective over the job - the finalize
 * team's in-region AND hands every PE the root's numbers - and the root's lead
 * measures first, which the others wait for there. The node's lead writes the
 * measured link into the control segment for osoe-stat.
 */
void osoe_link_establish(void);

#endif /* OSOE_LINK_MEASURE_H */
