/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The pacer's randomness: every value an exercise makes derives from the run's
 * seed and a few small numbers through the functions here, so a failure line
 * reproduces from the header's seed and its own round (proposal §1.6).
 *
 *   random_next      splitmix64 (Steele, Lea and Flood, 2014, as Vigna's
 *                    reference writes it): a 64-bit state stepped by a
 *                    constant and finalized - every output distinct over a
 *                    period of 2^64, which is what makes a stream of keys
 *                    whose sum and xor can be checked
 *   random_finalize  MurmurHash3's fmix64: every bit of the result depends on
 *                    every bit of the input
 *   random_mix       four words to one, a finalize between each, so that
 *                    (seed, exercise, PE, round) and (seed, exercise, round,
 *                    PE) start different streams
 *   random_name      FNV-1a 64 over a name, to put an exercise's name among
 *                    the words mixed
 */
#ifndef PACER_RANDOM_H
#define PACER_RANDOM_H

#include <stdint.h>

static inline uint64_t random_next(uint64_t *state)
{
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static inline uint64_t random_finalize(uint64_t h)
{
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    return h;
}

static inline uint64_t random_mix(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    uint64_t h = random_finalize(a + UINT64_C(0x9e3779b97f4a7c15));
    h = random_finalize(h ^ (b + UINT64_C(0x632be59bd9b4e019)));
    h = random_finalize(h ^ (c + UINT64_C(0x85157af5a5a3e2d1)));
    return random_finalize(h ^ (d + UINT64_C(0x3c6ef372fe94f82b)));
}

static inline uint64_t random_name(const char *name)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

#endif /* PACER_RANDOM_H */
