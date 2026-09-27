/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * osoe-pacer-start-pes - Annex F's routines without a prefix, in a program of
 * their own (docs/prompts/task-stress-phase4.md item 5): start_pes starts the
 * library in place of shmem_init, and finalizes it at exit, so it cannot share
 * osoe-pacer's process.
 *
 *   start_pes(0), then _my_pe and _num_pes against shmem_my_pe and shmem_n_pes;
 *   words shmalloc'd, put to the next PE and read there after a barrier;
 *   shrealloc growing them to four times, the words kept;
 *   shmemalign's address a multiple of what it was asked; shfree of both.
 *
 * A failure is a line "start-pes: PE n: ..."; the last line is PE 0's
 * "start-pes failures=N", and the exit status 1 when N is not 0. --plant
 * my-pe|put|realloc|align plants on PE 1 a defect a check must catch;
 * --coverage FILE, in the counted build, writes which routines were called.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

enum { WORDS = 64, ALIGNMENT = 64 };

static int me_known = -1, failures;

static void fail(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *format, ...)
{
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    printf("start-pes: PE %d: %s\n", me_known, line);     /* one write: whole among PEs' */
    fflush(stdout);
    failures++;
}

static long word_of(int from, int k)
{
    return (long)from * 1000003L + k * 7919L + 1;
}

#ifdef PACER_COUNTED
void pacer_shim_report(const char *path);   /* the generated shim's, collective */
#endif

int main(int argc, char **argv)
{
    const char *plant = "", *coverage = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--plant") == 0 && i + 1 < argc)
            plant = argv[++i];
#ifdef PACER_COUNTED
        else if (strcmp(argv[i], "--coverage") == 0 && i + 1 < argc)
            coverage = argv[++i];
#endif
        else {
            fprintf(stderr, "usage: osoe-pacer-start-pes [--plant my-pe|put|realloc|align]"
#ifdef PACER_COUNTED
                            " [--coverage FILE]"
#endif
                            "\n");
            return 2;
        }
    }

    start_pes(0);
    int me = shmem_my_pe(), n = shmem_n_pes(), planted = me == 1;
    me_known = me;
    if (planted && *plant)
        printf("start-pes: PE 1 planted %s\n", plant);

    int my_pe = _my_pe(), num_pes = _num_pes();
    if (planted && strcmp(plant, "my-pe") == 0)
        my_pe++;                    /* the planted defect: _my_pe misread */
    if (my_pe != me || num_pes != n)
        fail("_my_pe says %d and _num_pes %d, where shmem_my_pe says %d and shmem_n_pes %d",
             my_pe, num_pes, me, n);

    /* shmalloc: words every PE has, the next PE's written by this one; and
     * the job's count of failures, a symmetric pair - this program is not
     * linked by oshcc, so its statics are not symmetric. */
    long *words = shmalloc(WORDS * sizeof *words);
    int *tally = shmalloc(2 * sizeof *tally);
    long source[WORDS];
    if (!words || !tally) {         /* a refusal, as osoe-pacer's at start: alike on every PE */
        fprintf(stderr, "osoe-pacer-start-pes: PE %d: out of symmetric memory at start\n", me);
        return 2;
    }
    int next = (me + 1) % n, previous = (me + n - 1) % n;
    for (int k = 0; k < WORDS; k++)
        source[k] = word_of(me, k);
    if (planted && strcmp(plant, "put") == 0)
        source[WORDS / 2]++;        /* the planted defect: one word sent wrong */
    shmem_long_put(words, source, WORDS, next);
    shmem_barrier_all();
    for (int k = 0; k < WORDS; k++)
        if (words[k] != word_of(previous, k)) {
            fail("word %d of the shmalloc'd block holds %ld from PE %d, expected %ld", k,
                 words[k], previous, word_of(previous, k));
            break;
        }

    /* shrealloc: four times as large, the first words kept. */
    if (planted && strcmp(plant, "realloc") == 0)
        words[0]++;                 /* the planted defect: a word changed before the realloc */
    /* One check: the block grew, and its words are the ones it held. */
    long *grown = shrealloc(words, 4 * WORDS * sizeof *grown);
    int lost = grown ? -1 : WORDS;
    if (grown)
        words = grown;
    for (int k = 0; k < WORDS && grown && lost < 0; k++)
        if (words[k] != word_of(previous, k))
            lost = k;
    if (lost >= 0) {
        char what[96] = "returned NULL";
        if (grown)
            snprintf(what, sizeof what, "changed word %d, %ld, expected %ld", lost, words[lost],
                     word_of(previous, lost));
        fail("shrealloc to %zu bytes %s", 4 * WORDS * sizeof *grown, what);
    }

    /* shmemalign: an address a multiple of the alignment asked. */
    void *aligned = shmemalign(ALIGNMENT, 100);
    uintptr_t address = (uintptr_t)aligned;
    if (planted && strcmp(plant, "align") == 0)
        address++;                  /* the planted defect: the address misread */
    if (!aligned || address % ALIGNMENT != 0)
        fail("shmemalign(%d, 100) returned %#lx, not a multiple of %d", ALIGNMENT,
             (unsigned long)address, ALIGNMENT);
    shfree(aligned);
    shfree(words);

    /* Every PE's failures, summed: the job's line. */
    tally[0] = failures;
    shmem_barrier_all();
    shmem_int_sum_reduce(SHMEM_TEAM_WORLD, tally + 1, tally, 1);
    int total = tally[1];
    shfree(tally);
    if (me == 0) {
        printf("start-pes failures=%d%s%s\n", total, *plant ? " plant=" : "", plant);
        fflush(stdout);
    }
#ifdef PACER_COUNTED
    if (coverage)
        pacer_shim_report(coverage);
#else
    (void)coverage;
#endif
    return total ? 1 : 0;           /* start_pes finalizes at exit */
}
