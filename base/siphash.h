/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * SipHash-2-4 (Aumasson and Bernstein, 2012): a keyed hash that is a pseudo-
 * random function, so a digest proves knowledge of the key without revealing
 * it. The rendezvous and the node-pair mesh use it to authenticate a hello when
 * OSOE_JOB_SECRET is set (§6.2, §10); written here from the published
 * algorithm, since §1 allows no library for it.
 */
#ifndef OSOE_BASE_SIPHASH_H
#define OSOE_BASE_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

uint64_t osoe_siphash(const unsigned char key[16], const void *data, size_t len);

#endif /* OSOE_BASE_SIPHASH_H */
