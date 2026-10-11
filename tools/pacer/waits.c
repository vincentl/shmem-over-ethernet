/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * waits - every point-to-point synchronization routine on every type it takes
 * (docs/prompts/done/task-stress-phase4.md item 5). mailbox waits with every form
 * on uint64_t words; this takes every form - wait_until, its _all, _any and
 * _some, their _vector forms, and the six tests - over every standard AMO
 * type of Table 6, which since 1.5 are the types a wait takes (§9.11), by the
 * type's own name and by the C11 generic in turn.
 *
 * **A round** is one case: a type, a form and a comparison. Every member has
 * four words of the type, each written by another member - word j of rank r
 * by rank r + j + 1, mod the team's size - with the type's atomic set, the
 * only update 1.5 allows a waited-for word (change #17). A round: every
 * member sets its own words to their start, the team syncs, every member
 * writes its four words elsewhere and quiets, then waits for its own four by
 * the round's form, and checks what they hold.
 *
 * **The comparisons.** Values written are 1 to 100. EQ, NE, GT and GE start
 * the words at 0, LT and LE at 101; a form with one comparison value waits
 * for EQ the round's common value - every writer writes it - NE 0, GT 0,
 * GE 1, LT 101 or LE 100, and a _vector form for each word's own value: EQ
 * it, NE the start, GT one less, GE it, LT one more, LE it.
 *
 * **The checks.** Every word, once its wait is over, holds what its writer
 * wrote; an index an _any or _some form returns is one the status array
 * leaves in, and its word meets the comparison; with every word excluded, an
 * _any form returns SIZE_MAX and a _some form 0.
 *
 * Case r is type r mod 12 and form (r mod 168) / 12, so that each form meets
 * every type, and the comparison moves with the form, the type and the pass,
 * so that it meets every comparison. Even passes spell each routine by its
 * name, odd passes by the generic, which reaches the routine of the C type a
 * typedef names: 336 rounds call every name and every generic selection.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { WORDS = 4, START_LOW = 0, START_HIGH = 101, LARGEST = 100 };

enum form {
    WAIT_UNTIL, WAIT_ALL, WAIT_ANY, WAIT_SOME, WAIT_ALL_VECTOR, WAIT_ANY_VECTOR,
    WAIT_SOME_VECTOR, TEST, TEST_ALL, TEST_ANY, TEST_SOME, TEST_ALL_VECTOR, TEST_ANY_VECTOR,
    TEST_SOME_VECTOR, FORMS
};
static const char *const form_names[FORMS] = {
    "wait_until", "wait_until_all", "wait_until_any", "wait_until_some", "wait_until_all_vector",
    "wait_until_any_vector", "wait_until_some_vector", "test", "test_all", "test_any",
    "test_some", "test_all_vector", "test_any_vector", "test_some_vector"
};
static const int comparisons[6] = { SHMEM_CMP_EQ, SHMEM_CMP_NE, SHMEM_CMP_GT, SHMEM_CMP_GE,
                                    SHMEM_CMP_LT, SHMEM_CMP_LE };
static const char *const comparison_names[6] = { "eq", "ne", "gt", "ge", "lt", "le" };

/*
 * A type by its routines. Words are void and values whole numbers 0 to 101,
 * which every type holds; `generic` picks the C11 generic over the type's
 * name. The many-word forms take the status array and, for _some, the
 * indices; the _vector forms their comparison values as uint64_t, made the
 * type's here.
 */
struct wait_type {
    const char *name;
    int size;
    void (*set)(void *word, uint64_t value, int pe);
    uint64_t (*load)(const void *word);
    void (*wait_until)(void *word, int cmp, uint64_t value, int generic);
    int (*test)(void *word, int cmp, uint64_t value, int generic);
    void (*wait_all)(void *words, const int *status, int cmp, uint64_t value, int generic);
    size_t (*wait_any)(void *words, const int *status, int cmp, uint64_t value, int generic);
    size_t (*wait_some)(void *words, size_t *indices, const int *status, int cmp,
                        uint64_t value, int generic);
    void (*wait_all_vector)(void *words, const int *status, int cmp, const uint64_t *values,
                            int generic);
    size_t (*wait_any_vector)(void *words, const int *status, int cmp, const uint64_t *values,
                              int generic);
    size_t (*wait_some_vector)(void *words, size_t *indices, const int *status, int cmp,
                               const uint64_t *values, int generic);
    int (*test_all)(void *words, const int *status, int cmp, uint64_t value, int generic);
    size_t (*test_any)(void *words, const int *status, int cmp, uint64_t value, int generic);
    size_t (*test_some)(void *words, size_t *indices, const int *status, int cmp,
                        uint64_t value, int generic);
    int (*test_all_vector)(void *words, const int *status, int cmp, const uint64_t *values,
                           int generic);
    size_t (*test_any_vector)(void *words, const int *status, int cmp, const uint64_t *values,
                              int generic);
    size_t (*test_some_vector)(void *words, size_t *indices, const int *status, int cmp,
                               const uint64_t *values, int generic);
};

#define WAIT_TYPE(T, NAME)                                                          \
    static void set_##NAME(void *word, uint64_t value, int pe)                     \
    {                                                                               \
        shmem_##NAME##_atomic_set((T *)word, (T)value, pe);                        \
    }                                                                               \
    static uint64_t load_##NAME(const void *word)                                   \
    {                                                                               \
        return (uint64_t)*(const T *)word;                                          \
    }                                                                               \
    static void wait_until_##NAME(void *word, int cmp, uint64_t value, int generic) \
    {                                                                               \
        if (generic)                                                                \
            shmem_wait_until((T *)word, cmp, (T)value);                             \
        else                                                                        \
            shmem_##NAME##_wait_until((T *)word, cmp, (T)value);                    \
    }                                                                               \
    static int test_##NAME(void *word, int cmp, uint64_t value, int generic)        \
    {                                                                               \
        return generic ? shmem_test((T *)word, cmp, (T)value)                       \
                       : shmem_##NAME##_test((T *)word, cmp, (T)value);             \
    }                                                                               \
    static void wait_all_##NAME(void *words, const int *status, int cmp, uint64_t value, \
                                int generic)                                        \
    {                                                                               \
        if (generic)                                                                \
            shmem_wait_until_all((T *)words, WORDS, status, cmp, (T)value);         \
        else                                                                        \
            shmem_##NAME##_wait_until_all((T *)words, WORDS, status, cmp, (T)value); \
    }                                                                               \
    static size_t wait_any_##NAME(void *words, const int *status, int cmp,          \
                                  uint64_t value, int generic)                      \
    {                                                                               \
        return generic ? shmem_wait_until_any((T *)words, WORDS, status, cmp, (T)value) \
                       : shmem_##NAME##_wait_until_any((T *)words, WORDS, status, cmp, \
                                                       (T)value);                   \
    }                                                                               \
    static size_t wait_some_##NAME(void *words, size_t *indices, const int *status, \
                                   int cmp, uint64_t value, int generic)            \
    {                                                                               \
        return generic ? shmem_wait_until_some((T *)words, WORDS, indices, status, cmp, \
                                               (T)value)                            \
                       : shmem_##NAME##_wait_until_some((T *)words, WORDS, indices, status, \
                                                        cmp, (T)value);             \
    }                                                                               \
    static void wait_all_vector_##NAME(void *words, const int *status, int cmp,     \
                                       const uint64_t *values, int generic)         \
    {                                                                               \
        T typed[WORDS];                                                             \
        for (int j = 0; j < WORDS; j++)                                             \
            typed[j] = (T)values[j];                                                \
        if (generic)                                                                \
            shmem_wait_until_all_vector((T *)words, WORDS, status, cmp, typed);     \
        else                                                                        \
            shmem_##NAME##_wait_until_all_vector((T *)words, WORDS, status, cmp, typed); \
    }                                                                               \
    static size_t wait_any_vector_##NAME(void *words, const int *status, int cmp,   \
                                         const uint64_t *values, int generic)       \
    {                                                                               \
        T typed[WORDS];                                                             \
        for (int j = 0; j < WORDS; j++)                                             \
            typed[j] = (T)values[j];                                                \
        return generic ? shmem_wait_until_any_vector((T *)words, WORDS, status, cmp, typed) \
                       : shmem_##NAME##_wait_until_any_vector((T *)words, WORDS, status, \
                                                              cmp, typed);          \
    }                                                                               \
    static size_t wait_some_vector_##NAME(void *words, size_t *indices, const int *status, \
                                          int cmp, const uint64_t *values, int generic) \
    {                                                                               \
        T typed[WORDS];                                                             \
        for (int j = 0; j < WORDS; j++)                                             \
            typed[j] = (T)values[j];                                                \
        return generic ? shmem_wait_until_some_vector((T *)words, WORDS, indices, status, \
                                                      cmp, typed)                   \
                       : shmem_##NAME##_wait_until_some_vector((T *)words, WORDS, indices, \
                                                               status, cmp, typed); \
    }                                                                               \
    static int test_all_##NAME(void *words, const int *status, int cmp, uint64_t value, \
                               int generic)                                         \
    {                                                                               \
        return generic ? shmem_test_all((T *)words, WORDS, status, cmp, (T)value)   \
                       : shmem_##NAME##_test_all((T *)words, WORDS, status, cmp, (T)value); \
    }                                                                               \
    static size_t test_any_##NAME(void *words, const int *status, int cmp, uint64_t value, \
                                  int generic)                                      \
    {                                                                               \
        return generic ? shmem_test_any((T *)words, WORDS, status, cmp, (T)value)   \
                       : shmem_##NAME##_test_any((T *)words, WORDS, status, cmp, (T)value); \
    }                                                                               \
    static size_t test_some_##NAME(void *words, size_t *indices, const int *status, \
                                   int cmp, uint64_t value, int generic)            \
    {                                                                               \
        return generic ? shmem_test_some((T *)words, WORDS, indices, status, cmp, (T)value) \
                       : shmem_##NAME##_test_some((T *)words, WORDS, indices, status, cmp, \
                                                  (T)value);                        \
    }                                                                               \
    static int test_all_vector_##NAME(void *words, const int *status, int cmp,      \
                                      const uint64_t *values, int generic)          \
    {                                                                               \
        T typed[WORDS];                                                             \
        for (int j = 0; j < WORDS; j++)                                             \
            typed[j] = (T)values[j];                                                \
        return generic ? shmem_test_all_vector((T *)words, WORDS, status, cmp, typed) \
                       : shmem_##NAME##_test_all_vector((T *)words, WORDS, status, cmp, \
                                                        typed);                     \
    }                                                                               \
    static size_t test_any_vector_##NAME(void *words, const int *status, int cmp,   \
                                         const uint64_t *values, int generic)       \
    {                                                                               \
        T typed[WORDS];                                                             \
        for (int j = 0; j < WORDS; j++)                                             \
            typed[j] = (T)values[j];                                                \
        return generic ? shmem_test_any_vector((T *)words, WORDS, status, cmp, typed) \
                       : shmem_##NAME##_test_any_vector((T *)words, WORDS, status, cmp, \
                                                        typed);                     \
    }                                                                               \
    static size_t test_some_vector_##NAME(void *words, size_t *indices, const int *status, \
                                          int cmp, const uint64_t *values, int generic) \
    {                                                                               \
        T typed[WORDS];                                                             \
        for (int j = 0; j < WORDS; j++)                                             \
            typed[j] = (T)values[j];                                                \
        return generic ? shmem_test_some_vector((T *)words, WORDS, indices, status, cmp, \
                                                typed)                              \
                       : shmem_##NAME##_test_some_vector((T *)words, WORDS, indices, status, \
                                                         cmp, typed);               \
    }                                                                               \
    static const struct wait_type type_##NAME = {                                   \
        #NAME, (int)sizeof(T), set_##NAME, load_##NAME, wait_until_##NAME, test_##NAME, \
        wait_all_##NAME, wait_any_##NAME, wait_some_##NAME, wait_all_vector_##NAME,  \
        wait_any_vector_##NAME, wait_some_vector_##NAME, test_all_##NAME, test_any_##NAME, \
        test_some_##NAME, test_all_vector_##NAME, test_any_vector_##NAME,            \
        test_some_vector_##NAME };

/* Table 6, the standard AMO types, as the specification lists them. */
#define WAIT_TYPES(X)                                                               \
    X(int, int) X(long, long) X(long long, longlong) X(unsigned int, uint)          \
    X(unsigned long, ulong) X(unsigned long long, ulonglong) X(int32_t, int32)       \
    X(int64_t, int64) X(uint32_t, uint32) X(uint64_t, uint64) X(size_t, size)      \
    X(ptrdiff_t, ptrdiff)

WAIT_TYPES(WAIT_TYPE)

#define WAIT_ENTRY(T, NAME) &type_##NAME,
static const struct wait_type *const types[] = { WAIT_TYPES(WAIT_ENTRY) };
enum { TYPES = sizeof types / sizeof *types, CASES = TYPES * FORMS };

static const char *const phase_names[] = { "write", "wait", "check", NULL };

static struct {
    uint64_t seed;
    shmem_team_t team;
    int n, me, me_world;
    int *world;                     /* [n]: members' world PEs */
    unsigned char *words;           /* symmetric, room for WORDS of the widest type */
    long round;
    int plant_value, plant_index, plant_excluded, plant_count;
    double checked, phase[3];
} waits;

/* What word j of `owner` holds once written this round: its writer's value,
 * or the round's common one when every writer writes one. */
static uint64_t value_of(long round, int owner, int j, int common)
{
    if (common)
        return 1 + random_mix(waits.seed ^ random_name("waits common"), (uint64_t)round, 0, 0) %
                       LARGEST;
    return 1 + random_mix(waits.seed ^ random_name("waits"), (uint64_t)round, (uint64_t)owner,
                          (uint64_t)j) % LARGEST;
}

static uint64_t waits_memory(int npes, const struct pacer_options *options, char *fields,
                             size_t size)
{
    waits.seed = options->seed;
    waits.plant_value = options->plant == PACER_PLANT_WAITS_VALUE;
    waits.plant_index = options->plant == PACER_PLANT_WAITS_INDEX;
    waits.plant_excluded = options->plant == PACER_PLANT_WAITS_EXCLUDED;
    waits.plant_count = options->plant == PACER_PLANT_WAITS_COUNT;
    snprintf(fields, size, "types=%d forms=%d comparisons=6 words=%d cases=%d", TYPES, FORMS,
             WORDS, CASES);
    return WORDS * 8 + (uint64_t)npes * sizeof(int);
}

/* Onto a team, afresh; the words are set to their start by each round. */
static int waits_rebind(shmem_team_t team, const struct pacer_options *options)
{
    int n = shmem_team_n_pes(team);
    waits.team = team;
    waits.n = n;
    waits.me = shmem_team_my_pe(team);
    waits.seed = options->seed;
    for (int j = 0; j < n; j++)
        waits.world[j] = shmem_team_translate_pe(team, j, SHMEM_TEAM_WORLD);
    waits.checked = 0;
    memset(waits.phase, 0, sizeof waits.phase);
    shmem_team_sync(team);
    return 0;
}

static int waits_setup(shmem_team_t team, const struct pacer_options *options)
{
    waits.me_world = shmem_my_pe();
    waits.world = malloc(sizeof *waits.world * (size_t)shmem_team_n_pes(SHMEM_TEAM_WORLD));
    waits.words = shmem_calloc(WORDS, 8);
    if (!waits.world || !waits.words)
        return -1;
    waits.round = 0;     /* and on through every rebind: mix's periods */
    return waits_rebind(team, options);
}

/* Word j of this PE's: the words are an array of the round's type, as the
 * many-word forms take them. */
static void *word_at(const struct wait_type *type, size_t j)
{
    return waits.words + (size_t)type->size * j;
}

/* Whether `have` meets comparison `cmp` against `value`. */
static int meets(uint64_t have, int cmp, uint64_t value)
{
    switch (cmp) {
    case SHMEM_CMP_EQ: return have == value;
    case SHMEM_CMP_NE: return have != value;
    case SHMEM_CMP_GT: return have > value;
    case SHMEM_CMP_GE: return have >= value;
    case SHMEM_CMP_LT: return have < value;
    default: return have <= value;
    }
}

/* An index an _any or _some form returned: one of the words, left in by
 * the status, its word meeting the comparison - one check, which says which
 * of the three it is not. Marks it handled. */
static void take(long round, const struct wait_type *type, enum form form, int c, size_t index,
                 int *status, const uint64_t *thresholds)
{
    char why[64] = "";
    if (index >= WORDS)
        snprintf(why, sizeof why, "past the %d words", WORDS);
    else if (status[index])
        snprintf(why, sizeof why, "which the status excludes");
    else if (!meets(type->load(word_at(type, index)), comparisons[c], thresholds[index]))
        snprintf(why, sizeof why, "whose word holds %llu",
                 (unsigned long long)type->load(word_at(type, index)));
    if (waits.plant_index && waits.me_world == 1) {
        snprintf(why, sizeof why, "which the status excludes");     /* the planted defect */
        waits.plant_index = 0;
    }
    if (why[0])
        pacer_failure("waits round=%ld %s %s %s: returned index %zu, %s", round, type->name,
                      form_names[form], comparison_names[c], index, why);
    if (index < WORDS)
        status[index] = 1;
}

/* With every word excluded, an _any form returns SIZE_MAX and a _some form 0,
 * and neither waits. */
static void excluded_all(long round, const struct wait_type *type, enum form form, int c,
                         const uint64_t *thresholds, int generic)
{
    int status[WORDS] = { 1, 1, 1, 1 };
    size_t indices[WORDS], got = 0, want = SIZE_MAX;
    uint64_t value = thresholds[0];
    int cmp = comparisons[c];
    switch (form) {
    case WAIT_ANY: got = type->wait_any(waits.words, status, cmp, value, generic); break;
    case WAIT_ANY_VECTOR:
        got = type->wait_any_vector(waits.words, status, cmp, thresholds, generic);
        break;
    case TEST_ANY: got = type->test_any(waits.words, status, cmp, value, generic); break;
    case TEST_ANY_VECTOR:
        got = type->test_any_vector(waits.words, status, cmp, thresholds, generic);
        break;
    case WAIT_SOME:
        got = type->wait_some(waits.words, indices, status, cmp, value, generic);
        want = 0;
        break;
    case WAIT_SOME_VECTOR:
        got = type->wait_some_vector(waits.words, indices, status, cmp, thresholds, generic);
        want = 0;
        break;
    case TEST_SOME:
        got = type->test_some(waits.words, indices, status, cmp, value, generic);
        want = 0;
        break;
    case TEST_SOME_VECTOR:
        got = type->test_some_vector(waits.words, indices, status, cmp, thresholds, generic);
        want = 0;
        break;
    default:
        return;
    }
    if (waits.plant_excluded && waits.me_world == 1) {
        got ^= 1;                   /* the planted defect: the return misread once */
        waits.plant_excluded = 0;
    }
    if (got != want)
        pacer_failure("waits round=%ld %s %s %s: returned %zu with every word excluded, not "
                      "%s", round, type->name, form_names[form], comparison_names[c], got,
                      want == 0 ? "0" : "SIZE_MAX");
}

/* This PE's four words, waited for by the round's form. */
static void wait_for(long round, const struct wait_type *type, enum form form, int c,
                     const uint64_t *thresholds, int generic)
{
    int cmp = comparisons[c], status[WORDS] = { 0 };
    size_t indices[WORDS];
    uint64_t value = thresholds[0];     /* the forms with one comparison value */
    int left = WORDS;
    switch (form) {
    case WAIT_UNTIL:
        for (int j = 0; j < WORDS; j++)
            type->wait_until(word_at(type, j), cmp, thresholds[j], generic);
        break;
    case TEST:
        for (int j = 0; j < WORDS; j++)
            while (!type->test(word_at(type, j), cmp, thresholds[j], generic))
                ;
        break;
    case WAIT_ALL: type->wait_all(waits.words, status, cmp, value, generic); break;
    case WAIT_ALL_VECTOR:
        type->wait_all_vector(waits.words, status, cmp, thresholds, generic);
        break;
    case TEST_ALL:
        while (!type->test_all(waits.words, status, cmp, value, generic))
            ;
        break;
    case TEST_ALL_VECTOR:
        while (!type->test_all_vector(waits.words, status, cmp, thresholds, generic))
            ;
        break;
    case WAIT_ANY:
    case WAIT_ANY_VECTOR:
    case TEST_ANY:
    case TEST_ANY_VECTOR:
        while (left > 0) {
            size_t index;
            if (form == WAIT_ANY)
                index = type->wait_any(waits.words, status, cmp, value, generic);
            else if (form == WAIT_ANY_VECTOR)
                index = type->wait_any_vector(waits.words, status, cmp, thresholds, generic);
            else if (form == TEST_ANY)
                index = type->test_any(waits.words, status, cmp, value, generic);
            else
                index = type->test_any_vector(waits.words, status, cmp, thresholds, generic);
            if ((form == TEST_ANY || form == TEST_ANY_VECTOR) && index == SIZE_MAX)
                continue;           /* nothing yet */
            int was = status[index < WORDS ? index : 0];
            take(round, type, form, c, index, status, thresholds);
            if (index >= WORDS || was)
                break;              /* said; the rest cannot be trusted */
            left--;
        }
        excluded_all(round, type, form, c, thresholds, generic);
        break;
    case WAIT_SOME:
    case WAIT_SOME_VECTOR:
    case TEST_SOME:
    case TEST_SOME_VECTOR:
        while (left > 0) {
            size_t count;
            if (form == WAIT_SOME)
                count = type->wait_some(waits.words, indices, status, cmp, value, generic);
            else if (form == WAIT_SOME_VECTOR)
                count = type->wait_some_vector(waits.words, indices, status, cmp, thresholds,
                                               generic);
            else if (form == TEST_SOME)
                count = type->test_some(waits.words, indices, status, cmp, value, generic);
            else
                count = type->test_some_vector(waits.words, indices, status, cmp, thresholds,
                                               generic);
            if (count == 0 && (form == TEST_SOME || form == TEST_SOME_VECTOR))
                continue;           /* nothing yet */
            if (waits.plant_count && waits.me_world == 1) {
                count += WORDS;     /* the planted defect: a count misread once */
                waits.plant_count = 0;
            }
            if (count == 0 || count > (size_t)left) {
                pacer_failure("waits round=%ld %s %s %s: returned %zu indices with %d words "
                              "left", round, type->name, form_names[form], comparison_names[c],
                              count, left);
                break;
            }
            int wrong = 0;
            for (size_t k = 0; k < count && !wrong; k++) {
                int was = indices[k] < WORDS ? status[indices[k]] : 1;
                take(round, type, form, c, indices[k], status, thresholds);
                wrong = was;
            }
            if (wrong)
                break;
            left -= (int)count;
        }
        excluded_all(round, type, form, c, thresholds, generic);
        break;
    default:
        break;
    }
}

static void waits_step(long units, int last)
{
    (void)last;
    int n = waits.n;
    for (long u = 0; u < units; u++, waits.round++) {
        long round = waits.round, pass = round / CASES;
        int index = (int)(round % CASES);
        const struct wait_type *type = types[index % TYPES];
        enum form form = (enum form)(index / TYPES);
        int c = (int)((index / TYPES + index % TYPES + pass) % 6);
        int cmp = comparisons[c], generic = (int)(pass % 2);
        int vector = form == WAIT_ALL_VECTOR || form == WAIT_ANY_VECTOR ||
                     form == WAIT_SOME_VECTOR || form == TEST_ALL_VECTOR ||
                     form == TEST_ANY_VECTOR || form == TEST_SOME_VECTOR;
        int scalar = form == WAIT_UNTIL || form == TEST;
        /* A form with one comparison value and EQ: every writer writes the
         * round's common value. */
        int common = cmp == SHMEM_CMP_EQ && !vector && !scalar;
        uint64_t start = cmp == SHMEM_CMP_LT || cmp == SHMEM_CMP_LE ? START_HIGH : START_LOW;

        /* The start, on this PE's own words, before anyone writes. */
        double started = pacer_now();
        for (int j = 0; j < WORDS; j++)
            type->set(word_at(type, j), start, waits.me_world);
        shmem_quiet();
        shmem_team_sync(waits.team);

        /* Word j of rank r is rank r + j + 1's: this PE writes word j of
         * rank me - j - 1. */
        for (int j = 0; j < WORDS; j++) {
            int owner = ((waits.me - j - 1) % n + n) % n;
            uint64_t value = value_of(round, waits.world[owner], j, common);
            if (waits.plant_value && waits.me_world == 1 &&
                (cmp == SHMEM_CMP_NE || cmp == SHMEM_CMP_GT || cmp == SHMEM_CMP_GE) &&
                !vector && value < LARGEST) {
                value++;            /* the planted defect: a value written wrong, once */
                waits.plant_value = 0;
            }
            type->set(word_at(type, j), value, waits.world[owner]);
        }
        shmem_quiet();
        waits.phase[0] += pacer_now() - started;

        /* What each of this PE's words is waited for against. */
        started = pacer_now();
        uint64_t thresholds[WORDS];
        for (int j = 0; j < WORDS; j++) {
            uint64_t v = value_of(round, waits.me_world, j, common);
            if (vector || scalar) {
                switch (cmp) {
                case SHMEM_CMP_EQ: thresholds[j] = v; break;
                case SHMEM_CMP_NE: thresholds[j] = start; break;
                case SHMEM_CMP_GT: thresholds[j] = v - 1; break;
                case SHMEM_CMP_GE: thresholds[j] = v; break;
                case SHMEM_CMP_LT: thresholds[j] = v + 1; break;
                default: thresholds[j] = v; break;
                }
            } else {
                switch (cmp) {
                case SHMEM_CMP_EQ: thresholds[j] = v; break;
                case SHMEM_CMP_NE: thresholds[j] = START_LOW; break;
                case SHMEM_CMP_GT: thresholds[j] = START_LOW; break;
                case SHMEM_CMP_GE: thresholds[j] = 1; break;
                case SHMEM_CMP_LT: thresholds[j] = START_HIGH; break;
                default: thresholds[j] = LARGEST; break;
                }
            }
        }
        wait_for(round, type, form, c, thresholds, generic);
        waits.phase[1] += pacer_now() - started;

        /* Every word holds what its writer wrote. */
        started = pacer_now();
        for (int j = 0; j < WORDS; j++) {
            uint64_t want = value_of(round, waits.me_world, j, common);
            uint64_t have = type->load(word_at(type, j));
            if (have != want) {
                int writer = waits.world[(waits.me + j + 1) % n];
                pacer_failure("waits round=%ld %s %s %s: word %d from PE %d holds %llu, "
                              "expected %llu", round, type->name, form_names[form],
                              comparison_names[c], j, writer, (unsigned long long)have,
                              (unsigned long long)want);
            }
        }
        waits.checked += WORDS;
        waits.phase[2] += pacer_now() - started;
        shmem_team_sync(waits.team);    /* every word read before the next round starts it */
    }
}

static void waits_checkpoint(struct pacer_counters *counters)
{
    counters->work = waits.checked;
    for (int k = 0; k < 3; k++)
        counters->phase_seconds[k] = waits.phase[k];
    waits.checked = 0;
    memset(waits.phase, 0, sizeof waits.phase);
}

static void waits_teardown(void)
{
    shmem_team_sync(waits.team);
    shmem_free(waits.words);
    free(waits.world);
}

const struct pacer_exercise pacer_waits = {
    .name = "waits",
    .work = "words",
    .phases = phase_names,
    .quick_rounds = CASES,          /* every type in every form, once */
    .memory = waits_memory,
    .setup = waits_setup,
    .step = waits_step,
    .checkpoint = waits_checkpoint,
    .teardown = waits_teardown,
    .rebind = waits_rebind,
};
