/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * print - parallel_print checking its own lines (the brief's decision 12).
 * Not run unless named. A round calls it once a case, and for each case the
 * team's rank 0 compares the lines it assembled with the ones the job's map
 * predicts, and every member checks the count returned:
 *
 *   same      every PE the same text: one line, [0-N-1]
 *   parity    two lines, the even PEs' and the odd PEs'
 *   silent    the odd PEs say nothing: one line
 *   distinct  every PE its own text: N lines, in rank order
 *   even      on the team of even PEs, named `even`: even[0,2,...]
 *   shared    on SHMEM_TEAM_SHARED: one line a node, shared[...]
 *   strided   an unnamed team of three, from the last PE backwards:
 *             team(N-1:-1:3)[N-3-N-1], the labels sorted though the team is not
 *   windows   every PE its own text of 25 to 60 bytes, into windows of 64
 *             bytes: about one window a line
 *   long      a text past the capacity: cut, ending in "..."
 *
 * The first round shows what it printed; later rounds only check. A world
 * sync separates the cases: each has a team a PE of the last may not have
 * been in (parallel-print.c says why that needs one).
 *
 * The expected labels are written here from a membership array scanned in
 * world order - deliberately not parallel_print's way, a sorted list folded
 * into runs - so that the two cannot share a mistake. The work counted is the
 * lines printed, by each team's rank 0.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "parallel-print.h"

enum { LONG_TEXT = 1500, WINDOW = 64 };

static struct {
    shmem_team_t team, even, strided;
    int me, n;
    long round;
    unsigned char *member;          /* [world]: who a line's label should hold */
    char *capture, *expected;
    size_t size;
    char *text;                     /* LONG_TEXT + 1 */
    double lines;                   /* printed as a team's rank 0, since the checkpoint */
} print;

static uint64_t print_memory(int npes, const struct pacer_options *options, char *fields,
                             size_t size)
{
    (void)npes;
    (void)options;
    snprintf(fields, size, "cases=same,parity,silent,distinct,even,shared,strided,windows,long "
             "window=%d outside_budget=yes", WINDOW);
    return 0;                       /* parallel_print is outside the budget */
}

static int print_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    print.team = team;
    print.me = shmem_team_my_pe(team);
    print.n = shmem_team_n_pes(team);
    print.round = 0;
    print.lines = 0;
    int pes = pacer_layout()->pes;
    /* A line holds a label of at most 12 characters a PE and a text of at most
     * the capacity; N lines at most. */
    print.size = (size_t)print.n * (PACER_MESSAGE_CAPACITY + 64) + (size_t)pes * 12 + 4096;
    print.member = malloc((size_t)pes);
    print.capture = malloc(print.size);
    print.expected = malloc(print.size);
    print.text = malloc(LONG_TEXT + 1);
    if (!print.member || !print.capture || !print.expected || !print.text)
        return -1;
    if (shmem_team_split_strided(team, 0, 2, (print.n + 1) / 2, NULL, 0, &print.even) != 0)
        return -1;
    if (print.even != SHMEM_TEAM_INVALID)
        parallel_print_name_team(print.even, "even");
    print.strided = SHMEM_TEAM_INVALID;
    if (print.n >= 3 &&
        shmem_team_split_strided(team, print.n - 1, -1, 3, NULL, 0, &print.strided) != 0)
        return -1;
    return 0;
}

/* One line of the prediction: prefix, the members' world PEs, the text. */
static void expect(size_t *used, const char *prefix, const char *text)
{
    int pes = pacer_layout()->pes;
    char *out = print.expected;
    size_t size = print.size;
    *used += (size_t)snprintf(out + *used, size - *used, "%s[", prefix);
    int first = 1;
    for (int p = 0; p < pes && *used < size; p++) {
        if (!print.member[p] || (p > 0 && print.member[p - 1]))
            continue;               /* not where a run starts */
        int end = p;
        while (end + 1 < pes && print.member[end + 1])
            end++;
        *used += (size_t)snprintf(out + *used, size - *used, first ? "%d" : ",%d", p);
        if (end > p && *used < size)
            *used += (size_t)snprintf(out + *used, size - *used, "-%d", end);
        first = 0;
    }
    if (*used < size)
        *used += (size_t)snprintf(out + *used, size - *used, "] %s\n", text);
}

/* The first line where two outputs differ, for the failure's text. */
static void first_difference(const char *printed, const char *expected, char *got, char *wanted,
                             size_t size)
{
    while (*printed && *printed == *expected) {
        const char *a = strchr(printed, '\n'), *b = strchr(expected, '\n');
        if (!a || !b || a - printed != b - expected || memcmp(printed, expected,
                                                              (size_t)(a - printed)) != 0)
            break;
        printed = a + 1;
        expected = b + 1;
    }
    snprintf(got, size, "%.*s", (int)strcspn(printed, "\n"), printed);
    snprintf(wanted, size, "%.*s", (int)strcspn(expected, "\n"), expected);
}

/* After a case: the count on every member, the lines on the team's rank 0. */
static void check(const char *name, shmem_team_t team, int returned, int lines)
{
    if (team == SHMEM_TEAM_INVALID)
        return;
    if (returned != lines)
        pacer_failure("print round=%ld %s: returned %d lines, expected %d", print.round, name,
                      returned, lines);
    if (shmem_team_my_pe(team) != 0)
        return;
    print.lines += returned > 0 ? returned : 0;
    if (strcmp(print.capture, print.expected) != 0) {
        char got[96], wanted[96];
        first_difference(print.capture, print.expected, got, wanted, sizeof got);
        pacer_failure("print round=%ld %s: printed \"%s\", expected \"%s\"", print.round, name,
                      got, wanted);
    }
    if (print.round == 0) {         /* show it once */
        fputs(print.capture, stdout);
        fflush(stdout);
    }
}

static void print_step(long units, int last)
{
    (void)last;
    const struct pacer_layout *layout = pacer_layout();
    int pes = layout->pes, world = shmem_my_pe();
    char text[160], label[64];
    parallel_print_capture(print.capture, print.size);
    for (long u = 0; u < units; u++, print.round++) {
        long r = print.round;
        size_t used;
        int returned;

        /* same */
        snprintf(text, sizeof text, "print round=%ld same: every PE says this", r);
        memset(print.member, 1, (size_t)pes);
        used = 0;
        expect(&used, "", text);
        returned = parallel_print(print.team, "%s", text);
        check("same", print.team, returned, 1);
        shmem_team_sync(print.team);

        /* parity */
        used = 0;
        for (int parity = 0; parity < 2 && parity < pes; parity++) {
            for (int p = 0; p < pes; p++)
                print.member[p] = p % 2 == parity;
            snprintf(text, sizeof text, "print round=%ld parity: %s", r, parity ? "odd" : "even");
            expect(&used, "", text);
        }
        snprintf(text, sizeof text, "print round=%ld parity: %s", r, world % 2 ? "odd" : "even");
        returned = parallel_print(print.team, "%s", text);
        check("parity", print.team, returned, pes > 1 ? 2 : 1);
        shmem_team_sync(print.team);

        /* silent */
        for (int p = 0; p < pes; p++)
            print.member[p] = p % 2 == 0;
        snprintf(text, sizeof text, "print round=%ld silent: the odd PEs say nothing", r);
        used = 0;
        expect(&used, "", text);
        returned = parallel_print(print.team, "%s", world % 2 ? "" : text);
        check("silent", print.team, returned, 1);
        shmem_team_sync(print.team);

        /* distinct */
        used = 0;
        for (int p = 0; p < pes; p++) {
            memset(print.member, 0, (size_t)pes);
            print.member[p] = 1;
            snprintf(text, sizeof text, "print round=%ld distinct: PE %d of %d", r, p, pes);
            expect(&used, "", text);
        }
        snprintf(text, sizeof text, "print round=%ld distinct: PE %d of %d", r, world, pes);
        returned = parallel_print(print.team, "%s", text);
        check("distinct", print.team, returned, pes);
        shmem_team_sync(print.team);

        /* even */
        for (int p = 0; p < pes; p++)
            print.member[p] = p % 2 == 0;
        snprintf(text, sizeof text, "print round=%ld even: the even team", r);
        used = 0;
        expect(&used, "even", text);
        returned = parallel_print(print.even, "%s", text);
        check("even", print.even, returned, 1);
        shmem_team_sync(print.team);

        /* shared: a line a node, the node's own */
        int node = layout->node[world];
        for (int p = 0; p < pes; p++)
            print.member[p] = layout->node[p] == node;
        snprintf(text, sizeof text, "print round=%ld shared: node %d", r, node);
        used = 0;
        expect(&used, "shared", text);
        returned = parallel_print(SHMEM_TEAM_SHARED, "%s", text);
        check("shared", SHMEM_TEAM_SHARED, returned, 1);
        shmem_team_sync(print.team);

        /* strided: team ranks 0, 1, 2 are world N-1, N-2, N-3 */
        if (print.n >= 3) {
            memset(print.member, 0, (size_t)pes);
            for (int k = 0; k < 3; k++)
                print.member[shmem_team_translate_pe(print.team, print.n - 1 - k,
                                                     SHMEM_TEAM_WORLD)] = 1;
            snprintf(label, sizeof label, "team(%d:-1:3)",
                     shmem_team_translate_pe(print.team, print.n - 1, SHMEM_TEAM_WORLD));
            snprintf(text, sizeof text, "print round=%ld strided: three from the last, backwards",
                     r);
            used = 0;
            expect(&used, label, text);
            returned = parallel_print(print.strided, "%s", text);
            check("strided", print.strided, returned, 1);
            shmem_team_sync(print.team);
        }

        /* windows: texts of 25 to 60 bytes, windows of 64 */
        used = 0;
        for (int p = 0; p < pes; p++) {
            memset(print.member, 0, (size_t)pes);
            print.member[p] = 1;
            int length = snprintf(text, sizeof text, "print round=%ld windows: PE %d ", r, p);
            int target = length + p * 7 % 23;
            while (length < target && length < 60)
                text[length++] = '.';
            text[length] = '\0';
            expect(&used, "", text);
        }
        int length = snprintf(text, sizeof text, "print round=%ld windows: PE %d ", r, world);
        int target = length + world * 7 % 23;
        while (length < target && length < 60)
            text[length++] = '.';
        text[length] = '\0';
        size_t window = parallel_print_window(WINDOW);
        returned = parallel_print(print.team, "%s", text);
        parallel_print_window(window);
        check("windows", print.team, returned, pes);
        shmem_team_sync(print.team);

        /* long: cut to the capacity, less its NUL, ending in "..." */
        length = snprintf(print.text, LONG_TEXT + 1, "print round=%ld long: ", r);
        memset(print.text + length, 'x', (size_t)(LONG_TEXT - length));
        print.text[LONG_TEXT] = '\0';
        memset(print.member, 1, (size_t)pes);
        char cut[PACER_MESSAGE_CAPACITY];
        memcpy(cut, print.text, PACER_MESSAGE_CAPACITY - 4);
        memcpy(cut + PACER_MESSAGE_CAPACITY - 4, "...", 4);
        used = 0;
        expect(&used, "", cut);
        returned = parallel_print(print.team, "%s", print.text);
        check("long", print.team, returned, 1);
        shmem_team_sync(print.team);
    }
    parallel_print_capture(NULL, 0);
}

static void print_checkpoint(struct pacer_counters *counters)
{
    counters->work = print.lines;
    print.lines = 0;
}

static void print_teardown(void)
{
    if (print.strided != SHMEM_TEAM_INVALID)
        shmem_team_destroy(print.strided);
    if (print.even != SHMEM_TEAM_INVALID) {
        parallel_print_name_team(print.even, NULL);
        shmem_team_destroy(print.even);
    }
    free(print.text);
    free(print.expected);
    free(print.capture);
    free(print.member);
}

const struct pacer_exercise pacer_print = {
    .name = "print",
    .work = "lines",
    .phases = NULL,
    .quick_rounds = 3,
    .memory = print_memory,
    .setup = print_setup,
    .step = print_step,
    .checkpoint = print_checkpoint,
    .teardown = print_teardown,
};
