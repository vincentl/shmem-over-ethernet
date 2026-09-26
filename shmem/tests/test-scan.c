/*
 * The prefix scans (spec §9.10.10; design §5.8; phase 1 step 10 slice 1,
 * docs/prompts/phase1-step10-slice1-prompt.md), checked by value: team rank
 * r's source element j is distinct per (r, j), so a result missing a rank's
 * contribution, or holding one it should not, shows.
 *
 * Inclusive and exclusive; in place and not; one element, a few, and more
 * than a scan slot of the internal region holds (a scan in pieces); long
 * through its typed name and the C11 generic, and one scan of each other type
 * SUM takes but the exact-width ones whose base type is tested - unsigned
 * char, unsigned int and uint64_t with sums that wrap; on the world, the
 * world's evens (a strided split), the world reversed (a stride of -1),
 * SHMEM_TEAM_SHARED - a table of ranks on 0,1,2,0,1,0 - and a team of one.
 * Then scans back to back on teams whose members differ, with no barrier
 * between, each team on its own slots: a team of the first and last PE and
 * then the world (the model's CA); and three teams of two - {0, 3}, {0, 1},
 * {2, 1} - in that order (the model's ZXY), which span nodes on a map of one
 * PE a node.
 */
#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { LARGE = 2048 + 3 };   /* past a 4 KiB scan slot of longs, four times over */

static int me, npes;
static unsigned char *first, *second;   /* symmetric, LARGE x 16 bytes each */

/* One type's scan over `team`, `n` elements, checked on every member. */
#define SCAN_CHECK(T, NAME, VALUE)                                                    \
    static void check_##NAME(shmem_team_t team, const char *label, size_t n,          \
                             bool inclusive, bool in_place, bool generic)             \
    {                                                                                 \
        int rank = shmem_team_my_pe(team);                                            \
        if (rank < 0)                                                                 \
            return;                                                                   \
        T *source = (T *)first, *dest = in_place ? (T *)first : (T *)second;          \
        for (size_t j = 0; j < n; j++)                                                \
            source[j] = VALUE(rank, j);                                               \
        int status;                                                                   \
        if (generic)                                                                  \
            status = inclusive ? shmem_sum_inscan(team, dest, source, n)              \
                               : shmem_sum_exscan(team, dest, source, n);             \
        else                                                                          \
            status = inclusive ? shmem_##NAME##_sum_inscan(team, dest, source, n)     \
                               : shmem_##NAME##_sum_exscan(team, dest, source, n);    \
        CHECK(status == 0);                                                           \
        size_t bad = n;                                                               \
        for (size_t j = 0; j < n && bad == n; j++) {                                  \
            T want = (T)0;                                                            \
            for (int q = 0; q < rank + (inclusive ? 1 : 0); q++)                      \
                want = (T)(want + VALUE(q, j));                                       \
            if (dest[j] != want)                                                      \
                bad = j;                                                              \
        }                                                                             \
        if (bad < n)                                                                  \
            fprintf(stderr, "PE %d: %s %s of %zu %s on %s%s: element %zu wrong at "  \
                    "team rank %d\n", me, #NAME, inclusive ? "inscan" : "exscan", n,  \
                    in_place ? "in place" : "apart", label,                           \
                    generic ? " (generic)" : "", bad, rank);                          \
        CHECK(bad == n);                                                              \
    }

#define LONG_VALUE(r, j) ((long)((r) + 1) * 1000003L + (long)(j) * 7L)
#define INT_VALUE(r, j) ((int)((r) + 1) * 1009 + (int)(j) % 97)
#define CHAR_VALUE(r, j) ((char)((r) + 1 + (int)((j) % 3)))
#define UCHAR_VALUE(r, j) ((unsigned char)((r) * 97 + 61 + (int)(j)))
#define FLOAT_VALUE(r, j) ((float)((r) + 1) * 8.0f + (float)((j) % 5))
#define DOUBLE_VALUE(r, j) ((double)((r) + 1) * 1024.0 + (double)(j))
#define LONGDOUBLE_VALUE(r, j) ((long double)((r) + 1) * 4096.0L + (long double)(j))
#define COMPLEX_VALUE(r, j) ((double)((r) + 1) + (double)(j) * I)

SCAN_CHECK(long, long, LONG_VALUE)
SCAN_CHECK(int, int, INT_VALUE)
SCAN_CHECK(char, char, CHAR_VALUE)
SCAN_CHECK(unsigned char, uchar, UCHAR_VALUE)
SCAN_CHECK(float, float, FLOAT_VALUE)
SCAN_CHECK(double, double, DOUBLE_VALUE)
SCAN_CHECK(long double, longdouble, LONGDOUBLE_VALUE)
SCAN_CHECK(double _Complex, complexd, COMPLEX_VALUE)
#define SHORT_VALUE(r, j) ((short)(((r) + 1) * 31 + (int)(j) % 11))
#define LONGLONG_VALUE(r, j) ((long long)((r) + 1) * 1000000007LL + (long long)(j))
#define UINT_VALUE(r, j) ((unsigned int)((r) + 1) * 4000000000u + (unsigned int)(j))
#define UINT64_VALUE(r, j) ((uint64_t)((r) + 1) * UINT64_C(0x9e3779b97f4a7c15) + (uint64_t)(j))
#define SIZE_VALUE(r, j) ((size_t)((r) + 1) * 77u + (size_t)(j))
#define PTRDIFF_VALUE(r, j) ((ptrdiff_t)((r) + 1) * -5 + (ptrdiff_t)(j))
#define COMPLEXF_VALUE(r, j) ((float)((r) + 1) + (float)(j) * I)
SCAN_CHECK(short, short, SHORT_VALUE)
SCAN_CHECK(long long, longlong, LONGLONG_VALUE)
SCAN_CHECK(unsigned int, uint, UINT_VALUE)
SCAN_CHECK(uint64_t, uint64, UINT64_VALUE)
SCAN_CHECK(size_t, size, SIZE_VALUE)
SCAN_CHECK(ptrdiff_t, ptrdiff, PTRDIFF_VALUE)
SCAN_CHECK(float _Complex, complexf, COMPLEXF_VALUE)

/* Every shape of long's, and one of every other type's, on `team`. */
static void check_team(shmem_team_t team, const char *label)
{
    size_t sizes[] = { 1, 5, LARGE };
    for (int s = 0; s < 3; s++)
        for (int inclusive = 0; inclusive < 2; inclusive++)
            for (int in_place = 0; in_place < 2; in_place++)
                check_long(team, label, sizes[s], inclusive, in_place, false);
    check_long(team, label, 9, true, false, true);
    check_long(team, label, 9, false, true, true);
    check_int(team, label, 13, true, false, false);
    check_char(team, label, 13, false, false, false);
    check_uchar(team, label, 13, true, true, false);
    check_float(team, label, 7, true, false, false);
    check_double(team, label, LARGE / 2, false, false, false);
    check_longdouble(team, label, 7, true, false, false);
    check_complexd(team, label, 7, false, true, false);
    /* The rest of the types SUM takes, once each; unsigned int and uint64_t
     * wrap. */
    check_short(team, label, 7, true, false, false);
    check_longlong(team, label, 7, false, false, false);
    check_uint(team, label, 7, true, false, false);
    check_uint64(team, label, 7, false, true, false);
    check_size(team, label, 7, true, false, false);
    check_ptrdiff(team, label, 7, false, false, false);
    check_complexf(team, label, 7, true, false, false);
}

static shmem_team_t split(int start, int stride, int size)
{
    shmem_team_t t = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, start, stride, size, NULL, 0, &t) ==
                0);
    return t;
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    first = shmem_malloc((size_t)LARGE * 16);
    second = shmem_malloc((size_t)LARGE * 16);
    CHECK_FATAL(first != NULL && second != NULL);

    shmem_team_t evens = split(0, 2, (npes + 1) / 2);
    shmem_team_t reversed = split(npes - 1, -1, npes);
    shmem_team_t one = split(0, 1, 1);
    shmem_team_t ends = npes > 1 ? split(0, npes - 1, 2) : SHMEM_TEAM_INVALID;
    shmem_team_t z = SHMEM_TEAM_INVALID, x = SHMEM_TEAM_INVALID, y = SHMEM_TEAM_INVALID;
    if (npes >= 4) {
        z = split(0, 3, 2);
        x = split(0, 1, 2);
        y = split(2, -1, 2);
    }

    check_team(SHMEM_TEAM_WORLD, "the world");
    shmem_barrier_all();
    check_team(evens, "the evens");
    shmem_barrier_all();
    check_team(reversed, "the world reversed");
    shmem_barrier_all();
    check_team(SHMEM_TEAM_SHARED, "SHMEM_TEAM_SHARED");
    shmem_barrier_all();
    check_team(one, "a team of one");
    shmem_barrier_all();

    /* Back to back, no barrier between: members of the second team that were
     * not in the first reach it while the first's members are still in it. */
    for (int round = 0; round < 3; round++) {
        if (ends != SHMEM_TEAM_INVALID)
            check_long(ends, "the first and last PE", 5, true, false, false);
        check_long(SHMEM_TEAM_WORLD, "the world, after them", 5, false, false, false);
    }
    shmem_barrier_all();
    if (npes >= 4)
        for (int round = 0; round < 3; round++) {
            check_long(z, "{0, 3}", LARGE, true, false, false);
            check_long(x, "{0, 1}", LARGE, false, false, false);
            check_long(y, "{2, 1}", LARGE, true, true, false);
        }
    shmem_barrier_all();

    /* SHMEM_TEAM_INVALID is answered nonzero (the spec leaves it undefined). */
    long word = 0;
    CHECK(shmem_long_sum_inscan(SHMEM_TEAM_INVALID, &word, &word, 1) != 0);

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
