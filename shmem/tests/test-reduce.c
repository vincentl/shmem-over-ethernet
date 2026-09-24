/*
 * The team-based reductions (spec §9.10.9; design §5.5, phase 1 step 5): every
 * operator family on types of each kind, one element, a few, and more than one
 * internal-region slot (so the leads' tree carries it in pieces); in place;
 * nreduce 0; SHMEM_TEAM_SHARED; the C11 generics; and the same reduction many
 * times over, so that the epoch counters and the leads' words go round.
 *
 * Every PE's source is a function of its rank and the index, so every PE knows
 * the answer without talking. The one thing no single PE can check alone -
 * that every PE ends with the *same bits* when the operator is not associative
 * (§5.8 rule 3) - is checked by a second pair of reductions: the maximum and
 * the minimum of the result's bits across the team must agree.
 */
#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { SMALL = 7, LARGE = 5000 };    /* LARGE doubles: three 16 KiB slots' worth */

static int me, npes;

/* PE p's element i, for the integer checks: small enough that a product over
 * the job fits, and different per PE and index. */
static long element(int p, int i)
{
    return (long)((p + 1) * 3 + i % 5);
}

static void integer_reductions(void)
{
    long *src = shmem_malloc(LARGE * sizeof *src);
    long *dst = shmem_malloc(LARGE * sizeof *dst);
    CHECK_FATAL(src && dst);  /* NULL would be dereferenced just below */
    size_t sizes[] = { 1, SMALL, LARGE };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s];
        for (size_t i = 0; i < n; i++)
            src[i] = element(me, (int)i);

        CHECK(shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dst, src, n) == 0);
        int wrong = -1;
        for (size_t i = 0; i < n && wrong < 0; i++) {
            long want = 0;
            for (int p = 0; p < npes; p++)
                want += element(p, (int)i);
            if (dst[i] != want)
                wrong = (int)i;
        }
        if (wrong >= 0)
            fprintf(stderr, "PE %d: sum of %zu longs, element %d is %ld\n", me, n, wrong,
                    dst[wrong]);
        CHECK(wrong < 0);

        CHECK(shmem_long_max_reduce(SHMEM_TEAM_WORLD, dst, src, n) == 0);
        for (size_t i = 0; i < n; i++)
            CHECK(dst[i] == element(npes - 1, (int)i));
        CHECK(shmem_long_min_reduce(SHMEM_TEAM_WORLD, dst, src, n) == 0);
        for (size_t i = 0; i < n; i++)
            CHECK(dst[i] == element(0, (int)i));
    }

    /* Products: of twos, which overflow only past 31 PEs; of 65537s, which
     * overflow from two PEs on; and of ones. Then a sum that overflows. Each
     * wrapped value is the same on every PE (combine.h), and computed here in
     * unsigned arithmetic, so the test is defined at any PE count. */
    int *isrc = shmem_malloc(SMALL * sizeof *isrc);
    int *idst = shmem_malloc(SMALL * sizeof *idst);
    CHECK_FATAL(isrc && idst);
    for (int i = 0; i < SMALL; i++)
        isrc[i] = i == 0 ? 2 : i == 1 ? 65537 : 1;
    CHECK(shmem_int_prod_reduce(SHMEM_TEAM_WORLD, idst, isrc, SMALL) == 0);
    uint32_t twos = 1, big = 1;
    for (int p = 0; p < npes; p++) {
        twos *= 2u;
        big *= 65537u;
    }
    CHECK(idst[0] == (int32_t)twos);
    CHECK(idst[1] == (int32_t)big);
    for (int i = 2; i < SMALL; i++)
        CHECK(idst[i] == 1);
    for (int i = 0; i < SMALL; i++)
        isrc[i] = INT32_MAX;
    CHECK(shmem_int_sum_reduce(SHMEM_TEAM_WORLD, idst, isrc, SMALL) == 0);
    int32_t wrapped = (int32_t)((uint32_t)INT32_MAX * (uint32_t)npes);
    CHECK(idst[0] == wrapped);

    /* The bitwise operators, on an unsigned and an exact-width type. */
    unsigned char *usrc = shmem_malloc(SMALL);
    unsigned char *udst = shmem_malloc(SMALL);
    CHECK_FATAL(usrc && udst);
    for (int i = 0; i < SMALL; i++)
        usrc[i] = (unsigned char)(1u << ((me + i) % 8));
    unsigned char want_or[SMALL], want_and[SMALL], want_xor[SMALL];
    for (int i = 0; i < SMALL; i++) {
        want_or[i] = 0;
        want_and[i] = 0xff;
        want_xor[i] = 0;
        for (int p = 0; p < npes; p++) {
            unsigned char v = (unsigned char)(1u << ((p + i) % 8));
            want_or[i] |= v;
            want_and[i] &= v;
            want_xor[i] ^= v;
        }
    }
    CHECK(shmem_uchar_or_reduce(SHMEM_TEAM_WORLD, udst, usrc, SMALL) == 0);
    CHECK(memcmp(udst, want_or, SMALL) == 0);
    CHECK(shmem_uchar_and_reduce(SHMEM_TEAM_WORLD, udst, usrc, SMALL) == 0);
    CHECK(memcmp(udst, want_and, SMALL) == 0);
    CHECK(shmem_uchar_xor_reduce(SHMEM_TEAM_WORLD, udst, usrc, SMALL) == 0);
    CHECK(memcmp(udst, want_xor, SMALL) == 0);

    int32_t *esrc = shmem_malloc(sizeof *esrc);
    int32_t *edst = shmem_malloc(sizeof *edst);
    CHECK_FATAL(esrc && edst);
    *esrc = (int32_t)(1u << (me % 31));
    CHECK(shmem_int32_or_reduce(SHMEM_TEAM_WORLD, edst, esrc, 1) == 0);
    int32_t want_bits = 0;
    for (int p = 0; p < npes; p++)
        want_bits |= (int32_t)(1u << (p % 31));
    CHECK(*edst == want_bits);

    /* In place: source and dest the same symmetric array. */
    for (int i = 0; i < SMALL; i++)
        src[i] = element(me, i);
    CHECK(shmem_long_sum_reduce(SHMEM_TEAM_WORLD, src, src, SMALL) == 0);
    for (int i = 0; i < SMALL; i++) {
        long want = 0;
        for (int p = 0; p < npes; p++)
            want += element(p, i);
        CHECK(src[i] == want);
    }

    /* nreduce 0: returns success and touches nothing. */
    dst[0] = -12345;
    CHECK(shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dst, src, 0) == 0);
    CHECK(dst[0] == -12345);

    shmem_free(esrc);
    shmem_free(edst);
    shmem_free(usrc);
    shmem_free(udst);
    shmem_free(isrc);
    shmem_free(idst);
    shmem_free(src);
    shmem_free(dst);
}

/* Floating and complex: exact sums of small integers, and the same bits
 * everywhere for a sum that rounds differently in different orders. */
static void floating_reductions(void)
{
    double *src = shmem_malloc(LARGE * sizeof *src);
    double *dst = shmem_malloc(LARGE * sizeof *dst);
    CHECK_FATAL(src && dst);
    for (int i = 0; i < LARGE; i++)
        src[i] = (double)element(me, i);
    CHECK(shmem_double_sum_reduce(SHMEM_TEAM_WORLD, dst, src, LARGE) == 0);
    int wrong = -1;
    for (int i = 0; i < LARGE && wrong < 0; i++) {
        double want = 0;
        for (int p = 0; p < npes; p++)
            want += (double)element(p, i);
        if (dst[i] != want)
            wrong = i;
    }
    CHECK(wrong < 0);
    CHECK(shmem_double_max_reduce(SHMEM_TEAM_WORLD, dst, src, SMALL) == 0);
    CHECK(dst[0] == (double)element(npes - 1, 0));

    /* Values whose sum depends on the order: 1e16 on one PE, ones elsewhere. */
    src[0] = me == 0 ? 1e16 : 1.0;
    CHECK(shmem_double_sum_reduce(SHMEM_TEAM_WORLD, dst, src, 1) == 0);
    uint64_t *bits = shmem_malloc(2 * sizeof *bits);
    CHECK_FATAL(bits != NULL);
    memcpy(&bits[0], &dst[0], sizeof bits[0]);
    CHECK(shmem_uint64_max_reduce(SHMEM_TEAM_WORLD, &bits[1], &bits[0], 1) == 0);
    uint64_t highest = bits[1];
    CHECK(shmem_uint64_min_reduce(SHMEM_TEAM_WORLD, &bits[1], &bits[0], 1) == 0);
    if (highest != bits[1])
        fprintf(stderr, "PE %d: the PEs' sums differ in their bits\n", me);
    CHECK(highest == bits[1]);

    float *fsrc = shmem_malloc(SMALL * sizeof *fsrc);
    float *fdst = shmem_malloc(SMALL * sizeof *fdst);
    CHECK_FATAL(fsrc && fdst);
    for (int i = 0; i < SMALL; i++)
        fsrc[i] = (float)(me + i);
    CHECK(shmem_float_min_reduce(SHMEM_TEAM_WORLD, fdst, fsrc, SMALL) == 0);
    for (int i = 0; i < SMALL; i++)
        CHECK(fdst[i] == (float)i);

    double _Complex *csrc = shmem_malloc(2 * sizeof *csrc);
    double _Complex *cdst = shmem_malloc(2 * sizeof *cdst);
    CHECK_FATAL(csrc && cdst);
    csrc[0] = (double)(me + 1) + (double)me * I;
    csrc[1] = I;
    CHECK(shmem_complexd_sum_reduce(SHMEM_TEAM_WORLD, cdst, csrc, 2) == 0);
    double real = 0, imaginary = 0;
    for (int p = 0; p < npes; p++) {
        real += (double)(p + 1);
        imaginary += (double)p;
    }
    CHECK(creal(cdst[0]) == real && cimag(cdst[0]) == imaginary);
    CHECK(shmem_complexd_prod_reduce(SHMEM_TEAM_WORLD, cdst, csrc, 2) == 0);
    double _Complex power = 1;
    for (int p = 0; p < npes; p++)
        power *= I;
    CHECK(creal(cdst[1]) == creal(power) && cimag(cdst[1]) == cimag(power));

    shmem_free(csrc);
    shmem_free(cdst);
    shmem_free(fsrc);
    shmem_free(fdst);
    shmem_free(bits);
    shmem_free(src);
    shmem_free(dst);
}

/* The C11 interfaces, the shared team, and many rounds with barriers between
 * some of them - the team's epochs are the barrier's too. */
static void generics_teams_and_rounds(void)
{
    int *src = shmem_malloc(SMALL * sizeof *src);
    int *dst = shmem_malloc(SMALL * sizeof *dst);
    CHECK_FATAL(src && dst);
    for (int i = 0; i < SMALL; i++)
        src[i] = me + i;
    CHECK(shmem_sum_reduce(SHMEM_TEAM_WORLD, dst, src, SMALL) == 0);
    CHECK(dst[0] == npes * (npes - 1) / 2);
    CHECK(shmem_max_reduce(SHMEM_TEAM_WORLD, dst, src, SMALL) == 0);
    CHECK(dst[0] == npes - 1);
    unsigned int *usrc = shmem_malloc(sizeof *usrc);
    unsigned int *udst = shmem_malloc(sizeof *udst);
    CHECK_FATAL(usrc && udst);
    *usrc = 1u << (me % 32);
    CHECK(shmem_or_reduce(SHMEM_TEAM_WORLD, udst, usrc, 1) == 0);
    unsigned int want_bits = 0;
    for (int p = 0; p < npes; p++)
        want_bits |= 1u << (p % 32);
    CHECK(*udst == want_bits);

    /* The shared team: this node's PEs only, numbered by local position. */
    int local = shmem_team_n_pes(SHMEM_TEAM_SHARED);
    CHECK(shmem_int_sum_reduce(SHMEM_TEAM_SHARED, dst, src, SMALL) == 0);
    int want = 0;
    for (int p = 0; p < npes; p++)
        if (shmem_team_translate_pe(SHMEM_TEAM_WORLD, p, SHMEM_TEAM_SHARED) >= 0)
            want += p;
    CHECK(dst[0] == want);
    CHECK(local >= 1);

    /* Round after round, with a barrier in between every third. */
    for (int round = 0; round < 40; round++) {
        src[0] = me * round;
        CHECK(shmem_int_sum_reduce(SHMEM_TEAM_WORLD, dst, src, 1) == 0);
        CHECK(dst[0] == round * npes * (npes - 1) / 2);
        if (round % 3 == 0)
            shmem_barrier_all();
    }

    shmem_free(usrc);
    shmem_free(udst);
    shmem_free(src);
    shmem_free(dst);
}

/*
 * One reduction per element of the combine table that the tests above leave
 * alone, each with values that tell a wrong element apart: a char maximum over
 * 0x7f and 0x80, whose answer is the platform's char's (unsigned on the Pis);
 * signed minima over negatives, which an unsigned element gets wrong; one- and
 * two-byte sums and products that wrap, which a wider element does not; size_t
 * and ptrdiff_t; a long double sum and a complex float product. Every expected
 * value is folded here in the same C type, so it is the element's own answer.
 */
#define ELEMENT_CHECK(T, NAME, OPERATOR, VALUE, FOLD)                                 \
    do {                                                                              \
        T *src = shmem_malloc(sizeof *src), *dst = shmem_malloc(sizeof *dst);         \
        CHECK_FATAL(src && dst);                                                      \
        T want = 0;                                                                   \
        for (int p = 0; p < npes; p++) {                                              \
            T v = (T)(VALUE);                                                         \
            want = p == 0 ? v : (T)(FOLD);                                            \
        }                                                                             \
        { int p = me; (void)p; *src = (T)(VALUE); }                                  \
        CHECK(shmem_##NAME##_##OPERATOR##_reduce(SHMEM_TEAM_WORLD, dst, src, 1) == 0); \
        if (*dst != want)                                                             \
            fprintf(stderr, "PE %d: shmem_" #NAME "_" #OPERATOR "_reduce is wrong\n", me); \
        CHECK(*dst == want);                                                          \
        shmem_free(src);                                                              \
        shmem_free(dst);                                                              \
    } while (0)

static void every_element(void)
{
    ELEMENT_CHECK(char, char, max, p % 2 ? 0x80 : 0x7f, want > v ? want : v);
    ELEMENT_CHECK(signed char, schar, min, -(p + 1), want < v ? want : v);
    ELEMENT_CHECK(unsigned char, uchar, sum, 200, want + v);
    ELEMENT_CHECK(short, short, min, -(p * 1000) - 1, want < v ? want : v);
    ELEMENT_CHECK(short, short, sum, 30000, want + v);
    ELEMENT_CHECK(unsigned short, ushort, max, 65535 - p, want > v ? want : v);
    ELEMENT_CHECK(int8_t, int8, sum, 100, want + v);
    ELEMENT_CHECK(int16_t, int16, prod, 3 + p, want * v);
    ELEMENT_CHECK(uint16_t, uint16, sum, 40000, want + v);
    ELEMENT_CHECK(size_t, size, sum, (size_t)p * 1000000007u, want + v);
    ELEMENT_CHECK(ptrdiff_t, ptrdiff, min, -(ptrdiff_t)p * 1000, want < v ? want : v);
    ELEMENT_CHECK(long double, longdouble, sum, p + 0.5L, want + v);
#if !defined(__STDC_NO_COMPLEX__)
    ELEMENT_CHECK(float _Complex, complexf, prod, 1.0f + 1.0f * I, want * v);
#endif
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();

    integer_reductions();
    floating_reductions();
    generics_teams_and_rounds();
    every_element();

    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
