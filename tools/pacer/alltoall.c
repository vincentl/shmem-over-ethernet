/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * alltoall - every PE's block to every PE, eight ways (proposal §4.2; the
 * brief's decision 8, which calls them seven and lists these eight):
 *
 *   alltoall     shmem_uint64_alltoall, through the C11 generic
 *   alltoallmem  shmem_alltoallmem
 *   alltoalls    shmem_uint64_alltoalls, dst 3 and sst 2: the gaps of dest
 *                hold a canary that must be there afterwards
 *   put          puts to every peer, a quiet, a team sync
 *   get          a team sync, a nonblocking get from every peer, a quiet
 *   signal       shmem_putmem_signal to every peer, setting one word a sender
 *                to the exchange's number, then shmem_uint64_wait_until_all
 *                over those words
 *   iput         shmem_uint64_iput from a peer-minor source, element e of
 *                peer j at e N + j: the transpose PRK's kernel does by hand
 *   ibput        shmem_uint64_ibput from a [chunk][peer][bsize] source: a
 *                peer's block as four chunks, in one call
 *
 * at blocks of 8 B to 64 KiB a pair, on the exercise's team and on `even`,
 * its even ranks (on the odd PEs the split gives SHMEM_TEAM_INVALID, and they
 * wait at the team sync that follows). A unit is one block size, every
 * variant, on both teams; the sizes take turns. A size is skipped alike by
 * every PE when N blocks of it, five times over (a source of two blocks a
 * pair for the strided one, a dest of three), do not fit the budget -
 * osoe-bench's rule.
 *
 * **Every word names its origin.** Word e of the block from PE s to PE d in
 * exchange x is a mix of (seed, s, d, e, x), and dest is filled with a canary
 * first, so a wrong word is traced back: the canary means nothing arrived; a
 * word of this exchange or one of the eight before, from which PE and index,
 * means a misplaced or stale write; nothing matching means corruption. The
 * exchange number goes up every variant, so a variant that writes nothing
 * cannot pass on the previous one's data.
 *
 * **Timing** runs from just before a variant's first call to the moment its
 * data is known complete on this PE - the collective's return, the put
 * form's sync, the get form's quiet, the signal form's wait - and the checks
 * come after. An untimed team sync before each exchange makes dest ready on
 * every member and every source filled (§9.10: dest must be ready on every PE
 * before any PE calls), and one after the get form, so that no peer is still
 * reading this PE's source when the next variant refills it. Each PE keeps
 * its own times; a line gives, a variant, the slowest PE's median.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "parallel-print.h"
#include "random.h"

enum { SIZES = 5, VARIANTS = 8, TEAMS = 2, CHUNKS = 4, SEARCH_BACK = 8, DIAGNOSED = 4 };
enum { CHECKPOINT_SAMPLES = 256, RUN_SAMPLES = 1024 };
enum variant { COLLECTIVE, COLLECTIVE_BYTES, COLLECTIVE_STRIDED, PUT, GET, SIGNAL, IPUT, IBPUT };

static const char *const variant_names[VARIANTS] = { "alltoall", "alltoallmem", "alltoalls", "put",
                                                     "get", "signal", "iput", "ibput" };
static const size_t all_sizes[SIZES] = { 8, 64, 512, 4096, 65536 };
static const size_t quick_sizes[] = { 8, 4096, 65536 };
static const char *const phase_names[] = { "fill", "exchange", "check", NULL };

#define CANARY UINT64_C(0xdeadbeefcafef00d)
#define GOLDEN UINT64_C(0x9e3779b97f4a7c15)

/* A reservoir of times: the first `capacity`, then each new one replacing a
 * random earlier one, so that a median stands for the whole period. */
struct samples {
    double *values;
    int capacity, count;
    long seen;
    uint64_t state;
};

static struct {
    /* the plan, from memory() */
    size_t sizes[SIZES];            /* block bytes that fit, ascending */
    int size_count;
    size_t largest;                 /* elements a block, at most */
    uint64_t seed;
    int plant_byte, plant_gap;
    int mixing;                     /* under mix: no run summary, a period each */
    /* the teams: the exercise's and even */
    shmem_team_t teams[TEAMS];
    int *world[TEAMS];              /* team rank to world PE */
    int me_world;
    uint64_t exchange[TEAMS];       /* the last exchange's number, alike on every member */
    uint64_t *source, *dest, *signals;
    size_t dest_words;              /* allocated */
    double *medians;                /* symmetric: [TEAMS][3][SIZES * VARIANTS] */
    struct samples checkpoint[TEAMS][SIZES][VARIANTS], run[TEAMS][SIZES][VARIANTS];
    long unit;                      /* units done */
    int last;
    int diagnosed;                  /* failures traced since the checkpoint */
    double exchanges;               /* counted by each team's rank 0 */
    double phase[3];
} alltoall;

static void samples_add(struct samples *s, double value)
{
    s->seen++;
    if (s->count < s->capacity) {
        s->values[s->count++] = value;
        return;
    }
    uint64_t k = random_next(&s->state) % (uint64_t)s->seen;
    if (k < (uint64_t)s->capacity)
        s->values[k] = value;
}

static int by_value(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double samples_median(struct samples *s)
{
    if (s->count == 0)
        return -1;
    qsort(s->values, (size_t)s->count, sizeof *s->values, by_value);
    return s->values[s->count / 2];
}

/* The words: one base a block, a finalize a word. */
static uint64_t block_base(int from, int to, uint64_t exchange)
{
    return random_mix(alltoall.seed ^ random_name("alltoall"), (uint64_t)from, (uint64_t)to,
                      exchange);
}

static uint64_t word(uint64_t base, size_t e)
{
    return random_finalize(base + (uint64_t)e * GOLDEN);
}

/* Where word e of the block for team rank j sits in the source, and where
 * word e of the block from team rank i sits in dest, per variant. */
static size_t source_index(enum variant v, int j, size_t e, size_t words, int n)
{
    switch (v) {
    case COLLECTIVE_STRIDED:
        return ((size_t)j * words + e) * 2;
    case IPUT:
        return e * (size_t)n + (size_t)j;
    case IBPUT: {
        size_t chunks = words < CHUNKS ? words : CHUNKS, chunk = words / chunks;
        return (e / chunk * (size_t)n + (size_t)j) * chunk + e % chunk;
    }
    default:
        return (size_t)j * words + e;
    }
}

static size_t dest_index(enum variant v, int i, size_t e, size_t words)
{
    size_t k = (size_t)i * words + e;
    return v == COLLECTIVE_STRIDED ? k * 3 : k;
}

static uint64_t alltoall_memory(int npes, const struct pacer_options *options, char *fields,
                                size_t size)
{
    const size_t *sizes = options->quick ? quick_sizes : all_sizes;
    int count = options->quick ? (int)(sizeof quick_sizes / sizeof *quick_sizes) : SIZES;
    uint64_t fixed = UINT64_C(8) * (2 * (uint64_t)npes + TEAMS * 3 * SIZES * VARIANTS);
    alltoall.size_count = 0;
    size_t used = (size_t)snprintf(fields, size, "teams=world,even blocks=");
    uint64_t bytes = 0;
    for (int s = 0; s < count; s++) {
        uint64_t need = 5 * (uint64_t)npes * sizes[s] + fixed;
        if (need > options->memory)
            continue;               /* and every larger one */
        alltoall.sizes[alltoall.size_count++] = sizes[s];
        bytes = need;
        if (used < size)
            used += (size_t)snprintf(fields + used, size - used, "%s%zu",
                                     alltoall.size_count > 1 ? "," : "", sizes[s]);
    }
    if (alltoall.size_count == 0) {
        snprintf(fields, size, "%d PEs' blocks of 8 bytes, five times over, need %llu bytes",
                 npes, (unsigned long long)(5 * (uint64_t)npes * 8 + fixed));
        return UINT64_MAX;
    }
    if (alltoall.size_count < count && used < size)
        used += (size_t)snprintf(fields + used, size - used, " skipped=%d", count -
                                 alltoall.size_count);
    if (used < size)
        snprintf(fields + used, size - used,
                 " variants=alltoall,alltoallmem,alltoalls,put,get,signal,iput,ibput");
    alltoall.largest = alltoall.sizes[alltoall.size_count - 1] / 8;
    alltoall.seed = options->seed;
    alltoall.plant_byte = options->plant == PACER_PLANT_ALLTOALL_BYTE;
    alltoall.plant_gap = options->plant == PACER_PLANT_ALLTOALL_GAP;
    return bytes;
}

static int alltoall_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    int n = shmem_team_n_pes(team);
    alltoall.me_world = shmem_my_pe();
    alltoall.teams[0] = team;
    if (shmem_team_split_strided(team, 0, 2, (n + 1) / 2, NULL, 0, &alltoall.teams[1]) != 0)
        return -1;
    if (alltoall.teams[1] != SHMEM_TEAM_INVALID)
        parallel_print_name_team(alltoall.teams[1], "even");
    for (int t = 0; t < TEAMS; t++) {
        alltoall.exchange[t] = 0;
        alltoall.world[t] = NULL;
        if (alltoall.teams[t] == SHMEM_TEAM_INVALID)
            continue;
        int members = shmem_team_n_pes(alltoall.teams[t]);
        alltoall.world[t] = malloc(sizeof *alltoall.world[t] * (size_t)members);
        if (!alltoall.world[t])
            return -1;
        for (int j = 0; j < members; j++)
            alltoall.world[t][j] = shmem_team_translate_pe(alltoall.teams[t], j, SHMEM_TEAM_WORLD);
    }
    size_t words = (size_t)n * alltoall.largest;
    alltoall.dest_words = 3 * words;
    alltoall.source = shmem_malloc(2 * words * sizeof *alltoall.source);
    alltoall.dest = shmem_malloc(alltoall.dest_words * sizeof *alltoall.dest);
    alltoall.signals = shmem_calloc(2 * (size_t)n, sizeof *alltoall.signals);
    alltoall.medians = shmem_malloc(TEAMS * 3 * SIZES * VARIANTS * sizeof *alltoall.medians);
    if (!alltoall.source || !alltoall.dest || !alltoall.signals || !alltoall.medians)
        return -1;
    for (int t = 0; t < TEAMS; t++)
        for (int s = 0; s < SIZES; s++)
            for (int v = 0; v < VARIANTS; v++) {
                struct samples *c = &alltoall.checkpoint[t][s][v], *r = &alltoall.run[t][s][v];
                *c = (struct samples){ malloc(sizeof(double) * CHECKPOINT_SAMPLES),
                                       CHECKPOINT_SAMPLES, 0, 0, (uint64_t)(t * 97 + s * 13 + v) };
                *r = (struct samples){ malloc(sizeof(double) * RUN_SAMPLES), RUN_SAMPLES, 0, 0,
                                       (uint64_t)(t * 97 + s * 13 + v) + 7 };
                if (!c->values || !r->values)
                    return -1;
            }
    alltoall.unit = 0;
    alltoall.exchanges = 0;
    alltoall.diagnosed = 0;
    memset(alltoall.phase, 0, sizeof alltoall.phase);
    return 0;
}

/* What a wrong word is: the canary, a word of this exchange or one of the
 * eight before (from which PE, which index), or nothing sent. */
static void diagnose(char *out, size_t size, uint64_t found, int t, int n, size_t words,
                     uint64_t exchange)
{
    if (found == CANARY) {
        snprintf(out, size, "the canary: nothing arrived there");
        return;
    }
    for (uint64_t back = 0; back <= SEARCH_BACK && back < exchange; back++)
        for (int i = 0; i < n; i++) {
            uint64_t base = block_base(alltoall.world[t][i], alltoall.me_world, exchange - back);
            for (size_t e = 0; e < words; e++)
                if (word(base, e) == found) {
                    if (back)
                        snprintf(out, size, "it is PE %d's word %zu to this PE, %llu exchanges "
                                 "before", alltoall.world[t][i], e, (unsigned long long)back);
                    else
                        snprintf(out, size, "it is PE %d's word %zu to this PE",
                                 alltoall.world[t][i], e);
                    return;
                }
        }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            uint64_t base = block_base(alltoall.world[t][i], alltoall.world[t][j], exchange);
            for (size_t e = 0; e < words; e++)
                if (word(base, e) == found) {
                    snprintf(out, size, "it is PE %d's word %zu to PE %d", alltoall.world[t][i],
                             e, alltoall.world[t][j]);
                    return;
                }
        }
    snprintf(out, size, "it matches no word sent");
}

static void run_exchange(int t, int s, enum variant v)
{
    shmem_team_t team = alltoall.teams[t];
    int n = shmem_team_n_pes(team), me = shmem_team_my_pe(team);
    size_t bytes = alltoall.sizes[s], words = bytes / 8;
    uint64_t exchange = ++alltoall.exchange[t];
    uint64_t *source = alltoall.source, *dest = alltoall.dest;
    uint64_t *signals = alltoall.signals + (size_t)t * (size_t)shmem_team_n_pes(alltoall.teams[0]);
    const int *world = alltoall.world[t];

    /* Fill: dest's span and one block past it with the canary, then the
     * source in this variant's layout, then the planted byte. */
    double started = pacer_now();
    size_t span = (v == COLLECTIVE_STRIDED ? 3 : 1) * (size_t)n * words;
    size_t guard = span + words <= alltoall.dest_words ? span + words : alltoall.dest_words;
    for (size_t k = 0; k < guard; k++)
        dest[k] = CANARY;
    for (int j = 0; j < n; j++) {
        uint64_t base = block_base(alltoall.me_world, world[j], exchange);
        for (size_t e = 0; e < words; e++)
            source[source_index(v, j, e, words, n)] = word(base, e);
    }
    if (alltoall.plant_byte && alltoall.me_world == 1)
        ((unsigned char *)&source[source_index(v, (me + 1) % n, 0, words, n)])[0] ^= 1;
    double filled = pacer_now();
    alltoall.phase[0] += filled - started;

    shmem_team_sync(team);
    if (v == COLLECTIVE_STRIDED && alltoall.plant_gap && alltoall.me_world == 1) {
        /* The planted defect: a word into a gap of the next PE's dest, put and
         * quieted before this PE enters the exchange that PE's return waits
         * on - so it is there, every time, when that PE checks. */
        shmem_uint64_p(dest + 1, ~CANARY, world[(me + 1) % n]);
        shmem_quiet();
    }
    double begin = pacer_now();
    switch (v) {
    case COLLECTIVE:
        shmem_alltoall(team, dest, source, words);
        break;
    case COLLECTIVE_BYTES:
        shmem_alltoallmem(team, dest, source, bytes);
        break;
    case COLLECTIVE_STRIDED:
        shmem_uint64_alltoalls(team, dest, source, 3, 2, words);
        break;
    case PUT:
        for (int k = 1; k <= n; k++) {
            int j = (me + k) % n;
            shmem_putmem(dest + (size_t)me * words, source + (size_t)j * words, bytes, world[j]);
        }
        shmem_quiet();
        shmem_team_sync(team);
        break;
    case GET:
        /* Nonblocking gets completed by a quiet, or, every other time this
         * variant runs, blocking ones: the exchange number goes up by the
         * eight variants between two of its turns, so its parity would not. */
        shmem_team_sync(team);
        for (int k = 1; k <= n; k++) {
            int j = (me + k) % n;
            if ((exchange / VARIANTS) % 2)
                shmem_getmem(dest + (size_t)j * words, source + (size_t)me * words, bytes,
                             world[j]);
            else
                shmem_getmem_nbi(dest + (size_t)j * words, source + (size_t)me * words, bytes,
                                 world[j]);
        }
        shmem_quiet();
        break;
    case SIGNAL:
        for (int k = 1; k <= n; k++) {
            int j = (me + k) % n;
            shmem_putmem_signal(dest + (size_t)me * words, source + (size_t)j * words, bytes,
                                &signals[me], exchange, SHMEM_SIGNAL_SET, world[j]);
        }
        shmem_uint64_wait_until_all(signals, (size_t)n, NULL, SHMEM_CMP_EQ, exchange);
        break;
    case IPUT:
        for (int k = 1; k <= n; k++) {
            int j = (me + k) % n;
            shmem_uint64_iput(dest + (size_t)me * words, source + j, 1, n, words, world[j]);
        }
        shmem_quiet();
        shmem_team_sync(team);
        break;
    case IBPUT: {
        size_t chunks = words < CHUNKS ? words : CHUNKS, chunk = words / chunks;
        for (int k = 1; k <= n; k++) {
            int j = (me + k) % n;
            shmem_uint64_ibput(dest + (size_t)me * words, source + (size_t)j * chunk,
                               (ptrdiff_t)chunk, (ptrdiff_t)(chunk * (size_t)n), chunk, chunks,
                               world[j]);
        }
        shmem_quiet();
        shmem_team_sync(team);
        break;
    }
    }
    double end = pacer_now();
    alltoall.phase[1] += end - filled;
    samples_add(&alltoall.checkpoint[t][s][v], (end - begin) * 1e6);
    samples_add(&alltoall.run[t][s][v], (end - begin) * 1e6);
    if (me == 0)
        alltoall.exchanges++;

    /* Check every word, then the gaps and the guard. */
    size_t wrong = 0, first_word = 0;
    int first_from = 0;
    uint64_t found = 0, expected = 0;
    for (int i = 0; i < n; i++) {
        uint64_t base = block_base(world[i], alltoall.me_world, exchange);
        for (size_t e = 0; e < words; e++) {
            uint64_t want = word(base, e), have = dest[dest_index(v, i, e, words)];
            if (have != want && wrong++ == 0) {
                first_from = i;
                first_word = e;
                found = have;
                expected = want;
            }
        }
    }
    size_t trampled = 0, first_gap = 0;
    for (size_t k = 0; k < guard; k++) {
        int data = k < span && (v != COLLECTIVE_STRIDED || k % 3 == 0);
        if (!data && dest[k] != CANARY && trampled++ == 0)
            first_gap = k;
    }
    const char *name = t == 0 ? "world" : "even";
    if (wrong) {
        char origin[128] = "not traced";
        if (alltoall.diagnosed++ < DIAGNOSED)
            diagnose(origin, sizeof origin, found, t, n, words, exchange);
        pacer_failure("alltoall round=%ld team=%s block=%zu %s: %zu of %zu words wrong; the "
                      "first, word %zu from PE %d, is %016llx, not %016llx: %s", alltoall.unit,
                      name, bytes, variant_names[v], wrong, (size_t)n * words, first_word,
                      world[first_from], (unsigned long long)found, (unsigned long long)expected,
                      origin);
    }
    if (trampled)
        pacer_failure("alltoall round=%ld team=%s block=%zu %s: %zu %s outside the blocks "
                      "written; the first at word %zu of dest, %016llx", alltoall.unit, name,
                      bytes, variant_names[v], trampled, trampled == 1 ? "word" : "words",
                      first_gap, (unsigned long long)dest[first_gap]);
    alltoall.phase[2] += pacer_now() - end;

    if (v == GET)
        shmem_team_sync(team);      /* no peer still reads this source */
}

static void alltoall_step(long units, int last)
{
    alltoall.last = last;
    for (long u = 0; u < units; u++, alltoall.unit++) {
        int s = (int)(alltoall.unit % alltoall.size_count);
        for (int t = 0; t < TEAMS; t++) {
            if (alltoall.teams[t] != SHMEM_TEAM_INVALID)
                for (int v = 0; v < VARIANTS; v++)
                    run_exchange(t, s, (enum variant)v);
            if (t > 0)
                shmem_team_sync(alltoall.teams[0]);   /* the odd PEs wait here */
        }
    }
}

/* A line a size that ran: each variant's median, the slowest member's. The
 * run's report follows the last checkpoint's with no other reduction between
 * them on a PE outside `even`, so it reduces into an array of its own. */
static void report(int run)
{
    enum { CELLS = SIZES * VARIANTS };
    for (int t = 0; t < TEAMS; t++) {
        shmem_team_t team = alltoall.teams[t];
        if (team == SHMEM_TEAM_INVALID)
            continue;
        double *mine = alltoall.medians + (size_t)t * 3 * CELLS;
        double *slowest = mine + (size_t)(1 + run) * CELLS;
        for (int s = 0; s < SIZES; s++)
            for (int v = 0; v < VARIANTS; v++)
                mine[s * VARIANTS + v] = s < alltoall.size_count
                    ? samples_median(run ? &alltoall.run[t][s][v] : &alltoall.checkpoint[t][s][v])
                    : -1;
        shmem_double_max_reduce(team, slowest, mine, CELLS);
        if (shmem_team_my_pe(team) == 0)
            for (int s = 0; s < alltoall.size_count; s++) {
                if (slowest[s * VARIANTS] < 0)
                    continue;       /* no exchange at this size since the last */
                long count = run ? alltoall.run[t][s][0].seen : alltoall.checkpoint[t][s][0].seen;
                char line[512];
                size_t used = (size_t)snprintf(line, sizeof line, "%salltoall team=%s block=%zu "
                                               "exchanges=%ld", run ? "summary " : "",
                                               t == 0 ? "world" : "even", alltoall.sizes[s], count);
                for (int v = 0; v < VARIANTS && used < sizeof line; v++)
                    used += (size_t)snprintf(line + used, sizeof line - used, " %s_us=%.1f",
                                             variant_names[v], slowest[s * VARIANTS + v]);
                if (run) {
                    if (!alltoall.mixing)
                        pacer_summary("%s", line);
                } else {
                    printf("%s%s\n", pacer_prefix(), line);
                    fflush(stdout);
                }
            }
        if (!run)
            for (int s = 0; s < SIZES; s++)
                for (int v = 0; v < VARIANTS; v++) {
                    alltoall.checkpoint[t][s][v].count = 0;
                    alltoall.checkpoint[t][s][v].seen = 0;
                }
    }
}

static void alltoall_checkpoint(struct pacer_counters *counters)
{
    report(0);
    if (alltoall.last)
        report(1);
    counters->work = alltoall.exchanges;
    for (int k = 0; k < 3; k++)
        counters->phase_seconds[k] = alltoall.phase[k];
    alltoall.exchanges = 0;
    alltoall.diagnosed = 0;
    memset(alltoall.phase, 0, sizeof alltoall.phase);
}

/* The world again, afresh - a period of mix: alltoall runs on the world and
 * its even PEs alone, both made at setup. */
static int alltoall_rebind(shmem_team_t team, const struct pacer_options *options)
{
    if (team != alltoall.teams[0])
        return -1;
    alltoall.seed = options->seed;
    alltoall.mixing = options->mixing;
    for (int t = 0; t < TEAMS; t++)
        for (int s = 0; s < SIZES; s++)
            for (int v = 0; v < VARIANTS; v++) {
                alltoall.checkpoint[t][s][v].count = alltoall.run[t][s][v].count = 0;
                alltoall.checkpoint[t][s][v].seen = alltoall.run[t][s][v].seen = 0;
            }
    alltoall.unit = 0;
    alltoall.exchanges = 0;
    alltoall.diagnosed = 0;
    memset(alltoall.phase, 0, sizeof alltoall.phase);
    shmem_team_sync(team);
    return 0;
}

static void alltoall_teardown(void)
{
    for (int t = 0; t < TEAMS; t++) {
        for (int s = 0; s < SIZES; s++)
            for (int v = 0; v < VARIANTS; v++) {
                free(alltoall.checkpoint[t][s][v].values);
                free(alltoall.run[t][s][v].values);
            }
        free(alltoall.world[t]);
    }
    shmem_free(alltoall.medians);
    shmem_free(alltoall.signals);
    shmem_free(alltoall.dest);
    shmem_free(alltoall.source);
    if (alltoall.teams[1] != SHMEM_TEAM_INVALID) {
        parallel_print_name_team(alltoall.teams[1], NULL);
        shmem_team_destroy(alltoall.teams[1]);
    }
}

const struct pacer_exercise pacer_alltoall = {
    .name = "alltoall",
    .work = "exchanges",
    .phases = phase_names,
    .quick_rounds = 12,             /* three sizes, four exchanges of every variant each */
    .memory = alltoall_memory,
    .setup = alltoall_setup,
    .step = alltoall_step,
    .checkpoint = alltoall_checkpoint,
    .teardown = alltoall_teardown,
    .rebind = alltoall_rebind,
};
