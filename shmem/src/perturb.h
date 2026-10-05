/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * OSOE_TEST_PERTURB=1: at the points between a protocol's steps, a thread
 * sometimes yields or sleeps up to 200 us, so that the interleavings the models
 * find happen in a run (phase 1 step 8 slice 3, where the AM transport's points
 * began; the team redesign's step E added the generic internode phase's). Off,
 * each point is one load of a flag. Neither on nor off changes what is correct.
 */
#ifndef OSOE_PERTURB_H
#define OSOE_PERTURB_H

/* Read OSOE_TEST_PERTURB, 0 or 1, or end the job naming it: once a session,
 * from the transport's init, before any point is reached. */
void osoe_perturb_init(void);

/* A point between two steps of a protocol. */
void osoe_perturb(void);

#endif /* OSOE_PERTURB_H */
