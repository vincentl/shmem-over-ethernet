/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * What OSOE_COLLECTIVE_SHAPE=fixed promises (T23; the README's Limits): the
 * same bits from every run of this version, for the same team, node map and
 * settings, whatever the progress helpers and whatever else the machine runs -
 * and nothing across versions, which stay free to change the tree.
 *
 *   test-fixed-shape
 *
 * Floating-point reductions over the world whose results depend on the order
 * they combine in, each at three sizes - one the small class takes, one of a
 * few pieces and one of a MiB, pipelined:
 *   - a sum of doubles that rounds, of magnitudes from 1e-9 to 1e9;
 *   - a sum that overflows on the way: 1e308, 1e308 and -1e308 at three PEs an
 *     element, rotating, the rest 0 - infinite, or 1e308, by the order;
 *   - the maximum and the minimum of +0.0 and -0.0, rotating, which
 *     `x > y ? x : y` returns by the order;
 *   - the maximum of numbers with a NaN at one PE an element, rotating, NaN or
 *     not by the order, its NaNs made one before hashing, so that the hash
 *     holds which elements are NaN and not what a NaN carries;
 *   - and a sum of floats that rounds.
 * No long double: its format is the architecture's. Every PE hashes each
 * result's bits, and the team requires its members' hashes alike; PE 0 prints
 * one line naming the hash of them all, which fixed-shape.sh compares across
 * separate launches. And each case whose result the order can turn - infinite
 * or not, -0.0 or +0.0, NaN or not - must turn it in some of its results and
 * not in all, so that its inputs still depend on the order: inputs that did
 * not would hash alike in every run whatever the order, and pass for a shape
 * that repeats.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

#include "check.h"

/* The sizes, in elements: the small class's, a few pieces of 16 KiB, a MiB. */
static const size_t sizes[] = { 8, 4096, 131072 };
enum { MOST = 131072 };

static uint64_t fold(uint64_t h, const void *bytes, size_t len)
{
    const unsigned char *p = bytes;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

/* A result's bits, its NaNs made one. */
static uint64_t hash_doubles(const double *v, size_t n)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < n; i++) {
        double x = isnan(v[i]) ? (double)NAN : v[i];
        h = fold(h, &x, sizeof x);
    }
    return h;
}

static uint64_t hash_floats(const float *v, size_t n)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < n; i++) {
        float x = isnan(v[i]) ? (float)NAN : v[i];
        h = fold(h, &x, sizeof x);
    }
    return h;
}

static int me, npes, disagree, order_unseen;
static uint64_t *hash_in, *hash_most, *hash_least, all;

/* A case the order can turn must show it in some of its results and not in
 * all (the file's comment): `shown` of `results` are `what`. */
static void shows_the_order(const char *reduction, const char *what, size_t shown,
                            size_t results)
{
    if (shown != 0 && shown != results)
        return;
    if (me == 0)
        fprintf(stderr, "%s: %zu of %zu results %s, where its inputs should make some and not "
                "all - they no longer depend on the order\n", reduction, shown, results, what);
    order_unseen++;
}

/* The team's members must hold one result: their hashes' maximum and minimum
 * alike. Then into the hash of everything. */
static void agree(uint64_t h, const char *what, size_t n)
{
    *hash_in = h;
    shmem_barrier_all();
    shmem_uint64_max_reduce(SHMEM_TEAM_WORLD, hash_most, hash_in, 1);
    shmem_uint64_min_reduce(SHMEM_TEAM_WORLD, hash_least, hash_in, 1);
    if (*hash_most != *hash_least) {
        if (me == 0)
            fprintf(stderr, "%s of %zu elements: the members' results differ\n", what, n);
        disagree++;
    }
    all = fold(all, &h, sizeof h);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    double *source = shmem_malloc(MOST * sizeof *source);
    double *dest = shmem_malloc(MOST * sizeof *dest);
    float *source_float = shmem_malloc(MOST * sizeof *source_float);
    float *dest_float = shmem_malloc(MOST * sizeof *dest_float);
    hash_in = shmem_malloc(3 * sizeof *hash_in);
    CHECK_FATAL(source && dest && source_float && dest_float && hash_in);
    hash_most = hash_in + 1;
    hash_least = hash_in + 2;
    all = UINT64_C(0xcbf29ce484222325);
    size_t infinities = 0, negative_zeros = 0, nans = 0, results = 0;

    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s];
        results += n;
        for (size_t i = 0; i < n; i++)
            source[i] = 1.0 / (double)(3 + (size_t)me * 7 + i % 101) * (me % 2 ? 1e-9 : 1e9);
        shmem_barrier_all();
        shmem_double_sum_reduce(SHMEM_TEAM_WORLD, dest, source, n);
        agree(hash_doubles(dest, n), "a sum that rounds", n);

        for (size_t i = 0; i < n; i++) {
            size_t k = ((size_t)me + (size_t)npes - i % (size_t)npes) % (size_t)npes;
            source[i] = k == 0 || k == 1 ? 1e308 : k == 2 ? -1e308 : 0.0;
        }
        shmem_barrier_all();
        shmem_double_sum_reduce(SHMEM_TEAM_WORLD, dest, source, n);
        agree(hash_doubles(dest, n), "a sum that overflows", n);
        for (size_t i = 0; i < n; i++)
            infinities += isinf(dest[i]) != 0;

        for (size_t i = 0; i < n; i++)
            source[i] = (i + (size_t)me) % 2 ? -0.0 : 0.0;
        shmem_barrier_all();
        shmem_double_max_reduce(SHMEM_TEAM_WORLD, dest, source, n);
        agree(hash_doubles(dest, n), "a maximum of signed zeros", n);
        for (size_t i = 0; i < n; i++)
            negative_zeros += signbit(dest[i]) != 0;
        shmem_barrier_all();
        shmem_double_min_reduce(SHMEM_TEAM_WORLD, dest, source, n);
        agree(hash_doubles(dest, n), "a minimum of signed zeros", n);

        for (size_t i = 0; i < n; i++)
            source[i] = i % (size_t)npes == (size_t)me ? (double)NAN : (double)(me + 1);
        shmem_barrier_all();
        shmem_double_max_reduce(SHMEM_TEAM_WORLD, dest, source, n);
        agree(hash_doubles(dest, n), "a maximum with a NaN", n);
        for (size_t i = 0; i < n; i++)
            nans += isnan(dest[i]) != 0;

        for (size_t i = 0; i < n; i++)
            source_float[i] =
                1.0f / (float)(3 + (size_t)me * 5 + i % 89) * (me % 2 ? 1e-4f : 1e4f);
        shmem_barrier_all();
        shmem_float_sum_reduce(SHMEM_TEAM_WORLD, dest_float, source_float, n);
        agree(hash_floats(dest_float, n), "a sum of floats that rounds", n);
    }

    shows_the_order("a sum that overflows", "infinite", infinities, results);
    shows_the_order("a maximum of signed zeros", "-0.0", negative_zeros, results);
    shows_the_order("a maximum with a NaN", "NaN", nans, results);
    CHECK(disagree == 0);
    CHECK(order_unseen == 0);
    if (me == 0)
        printf("FIXED SHAPE: hash %016llx of %zu reductions on %d PEs (of each case's %zu "
               "results, %zu infinite, %zu -0.0, %zu NaN)\n", (unsigned long long)all,
               6 * (sizeof sizes / sizeof sizes[0]), npes, results, infinities, negative_zeros,
               nans);
    shmem_free(hash_in);
    shmem_free(dest_float);
    shmem_free(source_float);
    shmem_free(dest);
    shmem_free(source);
    shmem_finalize();
    return CHECK_DONE();
}
