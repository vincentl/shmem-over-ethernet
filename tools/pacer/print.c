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
 * and, folded to nodes (parallel_print_fold_nodes), whatever --fold-nodes says,
 * the earlier cases being printed by PE numbers alike:
 *
 *   hosts     every PE its host's name: a line a host, the name its label
 *   partial   each host's last PE apart from the rest: a host of two PEs or
 *             more is whole in neither group, which keep their PE numbers
 *   folded    every PE the same text: one line, labeled by every host
 *   shared-folded
 *             on SHMEM_TEAM_SHARED: shared@host where a node has its host to
 *             itself, shared[...] where logical nodes share one
 *
 * and, on every PE, the fold itself: host lists written out here by hand,
 * folded, against their folds written out by hand.
 *
 * Every case runs twice a round, by each of parallel_print's routes - the
 * library's fcollect, then the tool's own dissemination by put-with-signal -
 * against the one prediction: a differential test of the two, the signal
 * route's failures saying so.
 *
 * The first round shows what it printed; later rounds only check. A world
 * sync separates the cases: each has a team a PE of the last may not have
 * been in (parallel-print.c says why that needs one).
 *
 * The expected labels are written here from a membership array scanned in
 * world order - deliberately not parallel_print's way, a sorted list folded
 * into runs - so that the two cannot share a mistake; a host's PEs, from a
 * sort of the PEs by name, not from parallel_print's tally. The work counted
 * is the lines printed, by each team's rank 0.
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

/* The fold's own oracle. */
static const struct {
    int count;
    const char *names[5];
    const char *folded;
} fold_cases[] = {
    { 4, { "pi-001", "pi-002", "pi-003", "pi-004" }, "pi-[001-004]" },
    { 4, { "pi-004", "pi-001", "pi-002", "pi-001" }, "pi-[001-002,004]" },
    { 3, { "node9", "node10", "node11" }, "node[9-11]" },
    { 5, { "pi-003", "login", "pi-001", "gpu08", "gpu07" }, "gpu[07-08],login,pi-[001,003]" },
    { 1, { "login" }, "login" },
};

static struct {
    shmem_team_t team, even, strided;
    int me, n;
    long round;
    unsigned char *member;          /* [world]: who a line's label should hold */
    int *host_first, *host_last;    /* [world]: the lowest and highest PE of one's host */
    const char **names;             /* [world]: every PE's host name */
    char *label;                    /* every host, folded */
    size_t label_size;
    char *capture, *expected;
    size_t size;
    char *text;                     /* LONG_TEXT + 1 */
    double lines;                   /* printed as a team's rank 0, since the checkpoint */
    const char *route_name;         /* in a failure's line: "" or ", signal route" */
} print;

static uint64_t print_memory(int npes, const struct pacer_options *options, char *fields,
                             size_t size)
{
    (void)npes;
    (void)options;
    snprintf(fields, size, "cases=same,parity,silent,distinct,even,shared,strided,windows,long,"
             "hosts,partial,folded,shared-folded,fold routes=fcollect,signal window=%d "
             "outside_budget=yes", WINDOW);
    return 0;                       /* parallel_print is outside the budget */
}

/* World PEs by host name, then by number. */
static int by_host(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    int c = strcmp(parallel_print_host(x), parallel_print_host(y));
    return c ? c : (x > y) - (x < y);
}

static int print_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    print.team = team;
    print.me = shmem_team_my_pe(team);
    print.n = shmem_team_n_pes(team);
    print.round = 0;
    print.lines = 0;
    print.route_name = "";
    int pes = pacer_layout()->pes;
    /* What one case prints, at most: N one-PE lines of a label and a text
     * under 128 bytes (distinct, windows); a line a host of two names and a
     * text (hosts), 200 bytes; or labels holding every PE, 12 characters
     * each, or every host, 65, on a few lines (same, parity, folded); or one
     * text of the whole capacity (long). O(N), 280 bytes a PE. */
    print.size = (size_t)pes * 280 + 2 * PACER_MESSAGE_CAPACITY + 4096;
    print.label_size = (size_t)pes * 65 + 64;
    print.member = malloc((size_t)pes);
    print.capture = malloc(print.size);
    print.expected = malloc(print.size);
    print.text = malloc(LONG_TEXT + 1);
    print.host_first = malloc(sizeof *print.host_first * (size_t)pes);
    print.host_last = malloc(sizeof *print.host_last * (size_t)pes);
    print.names = malloc(sizeof *print.names * (size_t)pes);
    print.label = malloc(print.label_size);
    if (!print.member || !print.capture || !print.expected || !print.text ||
        !print.host_first || !print.host_last || !print.names || !print.label)
        return -1;
    /* Each PE's host's lowest and highest PE, from the PEs sorted by name:
     * a host is a run, its first entry the lowest, its last the highest. */
    int *order = malloc(sizeof *order * (size_t)pes);
    if (!order)
        return -1;
    for (int p = 0; p < pes; p++)
        order[p] = p;
    qsort(order, (size_t)pes, sizeof *order, by_host);
    for (int i = 0; i < pes;) {
        int j = i;
        while (j + 1 < pes &&
               strcmp(parallel_print_host(order[j + 1]), parallel_print_host(order[i])) == 0)
            j++;
        for (int m = i; m <= j; m++) {
            print.host_first[order[m]] = order[i];
            print.host_last[order[m]] = order[j];
        }
        i = j + 1;
    }
    free(order);
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

/* One line of the prediction under a label written out whole. */
static void expect_labeled(size_t *used, const char *label, const char *text)
{
    if (*used < print.size)
        *used += (size_t)snprintf(print.expected + *used, print.size - *used, "%s %s\n", label,
                                  text);
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
        pacer_failure("print round=%ld %s%s: returned %d lines, expected %d", print.round, name,
                      print.route_name, returned, lines);
    if (shmem_team_my_pe(team) != 0)
        return;
    print.lines += returned > 0 ? returned : 0;
    if (strcmp(print.capture, print.expected) != 0) {
        char got[96], wanted[96];
        first_difference(print.capture, print.expected, got, wanted, sizeof got);
        pacer_failure("print round=%ld %s%s: printed \"%s\", expected \"%s\"", print.round,
                      name, print.route_name, got, wanted);
    }
    if (print.round == 0 && !*print.route_name) {   /* show it once */
        fputs(print.capture, stdout);
        fflush(stdout);
    }
}

/* Every case of a round, printed by the route parallel_print is set to. */
static void cases(long r)
{
    const struct pacer_layout *layout = pacer_layout();
    int pes = layout->pes, world = shmem_my_pe();
    char text[160], label[96];
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

    /* Folded to nodes, from here to the round's end. */
    parallel_print_fold_nodes(1);

    /* hosts: a line a host, labeled by its name alone */
    used = 0;
    int lines = 0;
    for (int p = 0; p < pes; p++)
        if (print.host_first[p] == p) {
            snprintf(text, sizeof text, "print round=%ld hosts: %s", r,
                     parallel_print_host(p));
            expect_labeled(&used, parallel_print_host(p), text);
            lines++;
        }
    snprintf(text, sizeof text, "print round=%ld hosts: %s", r, parallel_print_host(world));
    returned = parallel_print(print.team, "%s", text);
    check("hosts", print.team, returned, lines);
    shmem_team_sync(print.team);

    /* partial: each host's last PE apart from the rest, each group's line
     * at its lowest PE; a host of one PE is whole, and named */
    used = 0;
    lines = 0;
    for (int p = 0; p < pes; p++) {
        const char *host = parallel_print_host(p);
        int lowest = print.host_first[p], highest = print.host_last[p];
        if (p == lowest && lowest == highest) {
            snprintf(text, sizeof text, "print round=%ld partial: %s, last", r, host);
            expect_labeled(&used, host, text);
            lines++;
        } else if (p == lowest) {
            for (int q = 0; q < pes; q++)
                print.member[q] = print.host_first[q] == lowest && q != highest;
            snprintf(text, sizeof text, "print round=%ld partial: %s", r, host);
            expect(&used, "", text);
            lines++;
        }
        if (p == highest && lowest != highest) {
            memset(print.member, 0, (size_t)pes);
            print.member[p] = 1;
            snprintf(text, sizeof text, "print round=%ld partial: %s, last", r, host);
            expect(&used, "", text);
            lines++;
        }
    }
    snprintf(text, sizeof text, "print round=%ld partial: %s%s", r,
             parallel_print_host(world), print.host_last[world] == world ? ", last" : "");
    returned = parallel_print(print.team, "%s", text);
    check("partial", print.team, returned, lines);
    shmem_team_sync(print.team);

    /* folded: every PE the same text, every host in the label - or the
     * PEs, where the hosts' names outgrow the room PE numbers would take
     * (parallel-print.c's label_room, 80 + 12 N) */
    for (int p = 0; p < pes; p++)
        print.names[p] = parallel_print_host(p);
    size_t needed = parallel_print_fold_names(print.names, pes, print.label,
                                              print.label_size);
    snprintf(text, sizeof text, "print round=%ld folded: every PE says this", r);
    used = 0;
    if (needed < 80 + 12 * (size_t)pes) {
        expect_labeled(&used, print.label, text);
    } else {
        memset(print.member, 1, (size_t)pes);
        expect(&used, "", text);
    }
    returned = parallel_print(print.team, "%s", text);
    check("folded", print.team, returned, 1);
    shmem_team_sync(print.team);

    /* shared-folded: a node alone on its host is named for it */
    int alone = 1;              /* node: this PE's, as in shared */
    for (int p = 0; p < pes && alone; p++)
        alone = (layout->node[p] == node) == (print.host_first[p] == print.host_first[world]);
    snprintf(text, sizeof text, "print round=%ld shared-folded: node %d", r, node);
    used = 0;
    if (alone) {
        snprintf(label, sizeof label, "shared@%s", parallel_print_host(world));
        expect_labeled(&used, label, text);
    } else {
        for (int p = 0; p < pes; p++)
            print.member[p] = layout->node[p] == node;
        expect(&used, "shared", text);
    }
    returned = parallel_print(SHMEM_TEAM_SHARED, "%s", text);
    check("shared-folded", SHMEM_TEAM_SHARED, returned, 1);
    shmem_team_sync(print.team);
    parallel_print_fold_nodes(0);
}

static void print_step(long units, int last)
{
    (void)last;
    int folding = parallel_print_fold_nodes(0);
    enum parallel_print_route routing = parallel_print_route(PARALLEL_PRINT_FCOLLECT);
    parallel_print_capture(print.capture, print.size);
    for (long u = 0; u < units; u++, print.round++) {
        long r = print.round;
        /* Every case by each route, against one prediction: the signal
         * route's lines must be the fcollect's. */
        for (int route = 0; route < 2; route++) {
            parallel_print_route(route ? PARALLEL_PRINT_SIGNAL : PARALLEL_PRINT_FCOLLECT);
            print.route_name = route ? ", signal route" : "";
            cases(r);
        }
        parallel_print_route(routing);

        /* the fold itself, here on every PE */
        for (size_t c = 0; c < sizeof fold_cases / sizeof *fold_cases; c++) {
            char folded[64], listed[96];
            size_t at = 0;
            parallel_print_fold_names(fold_cases[c].names, fold_cases[c].count, folded,
                                      sizeof folded);
            for (int k = 0; k < fold_cases[c].count && at < sizeof listed; k++)
                at += (size_t)snprintf(listed + at, sizeof listed - at, k ? ",%s" : "%s",
                                       fold_cases[c].names[k]);
            if (strcmp(folded, fold_cases[c].folded) != 0)
                pacer_failure("print round=%ld fold: %s folded to %s, expected %s", r, listed,
                              folded, fold_cases[c].folded);
        }
    }
    parallel_print_capture(NULL, 0);
    parallel_print_fold_nodes(folding);
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
    free(print.label);
    free(print.names);
    free(print.host_last);
    free(print.host_first);
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
