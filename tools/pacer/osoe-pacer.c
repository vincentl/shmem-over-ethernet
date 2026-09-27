/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * osoe-pacer - puts the library through its paces: whole-job patterns from
 * real OpenSHMEM codes, every result checked against an oracle, reported as
 * it goes (docs/design/osoe-pacer-proposal.md; phase 1's brief is
 * docs/prompts/task-stress-phase1.md).
 *
 *   srun -N4 --ntasks-per-node=4 osoe-pacer [OPTION]... [EXERCISE]...
 *   osoe-launch -n 4 -m 0,0,1,1 -- osoe-pacer --quick
 *
 * It is not osoe-bench, which measures what one PE's operations cost with
 * every other PE idle. Here every PE is in motion at once, for as long as it
 * is told, and the main product is confidence: a wrong value found by any PE
 * is a failure line, a count, and the exit status.
 *
 * The output is for a person watching and for a script afterwards. PE 0
 * prints a header - the library, the job's shape, the budget, the seed, and a
 * line an exercise with its parameters and memory - then each exercise
 * prints a line a checkpoint of key=value fields after its name:
 *
 *   sort t=35.2 round=7 keys=117440512 keys_per_s=41200000 mib_per_s=314.3 ...
 *
 * t, round and the work count from the exercise's start; the rates are the
 * checkpoint's own: the job's work over the slowest PE's time. Failures go
 * through parallel_print, so the same failure on many PEs is one line under
 * their numbers. At the end, a summary line an exercise and last
 *
 *   osoe-pacer failures=N seconds=S
 *
 * with the exit status 1 when N is not 0. Every line is one write, flushed,
 * so that a launcher interleaving several PEs' output cannot split it.
 *
 * The driver owns the loop (exercise.h): it times each step, reduces the
 * counters over the exercise's team, prints, and sizes the next step so that
 * a checkpoint comes about every --report seconds. Every choice it makes
 * after a step - how many units next, whether to stop - comes from reduced
 * values, identical on every PE, so no PE can decide differently.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "base/size.h"
#include "exercise.h"
#include "library-counters.h"
#include "parallel-print.h"
#include "random.h"

/* The library's default heap, shmem/src/bootstrap.h's
 * OSOE_DEFAULT_SYMMETRIC_SIZE, which is internal. */
#define DEFAULT_HEAP (UINT64_C(256) << 20)

enum { FAILURE_LINES = 8, FAILURE_LINE = 320, DISTINCT_FAILURES = 16 };
#define MOST_UNITS (1L << 24)

static const struct pacer_exercise *const known[] = { &pacer_sort, &pacer_alltoall, &pacer_mailbox,
                                                      &pacer_teams, &pacer_atomics, &pacer_rma,
                                                      &pacer_locks, &pacer_memory,
                                                      &pacer_collectives, &pacer_threads,
                                                      &pacer_print };
enum { KNOWN = sizeof known / sizeof *known };

static const char *const plant_names[PACER_PLANTS] = {
    "", "sort-key", "sort-order", "sort-range", "sort-capacity", "alltoall-byte", "alltoall-gap",
    "print-hash", "mailbox-payload", "mailbox-signal", "mailbox-count", "mailbox-round-trip",
    "threads-row", "threads-table", "threads-count", "teams-query", "teams-collective",
    "teams-exhaust", "teams-member", "teams-ring", "atomics-add", "atomics-fetch",
    "atomics-claim", "atomics-swap", "atomics-set", "atomics-bitwise", "atomics-fetched-bit",
    "rma-halo", "rma-put", "rma-gap", "rma-get", "rma-get-gap", "rma-ptr", "locks-count",
    "locks-holder", "memory-free", "memory-calloc", "memory-align", "memory-accessible",
    "memory-agree", "collectives-reduce", "collectives-scan", "collectives-data",
    "collectives-gap", "atomics-fetch-count", "atomics-attempts", "rma-flag", "rma-single",
    "rma-ptr-null", "memory-realloc", "memory-refused", "collectives-status",
    "collectives-timed", "collectives-timed-data", "memory-granted"
};
static const char *const plant_exercise[PACER_PLANTS] = {
    "", "sort", "sort", "sort", "sort", "alltoall", "alltoall", "print", "mailbox", "mailbox",
    "mailbox", "mailbox", "threads", "threads", "threads", "teams", "teams", "teams", "teams",
    "teams", "atomics", "atomics", "atomics", "atomics", "atomics", "atomics", "atomics", "rma",
    "rma", "rma", "rma", "rma", "rma", "locks", "locks", "memory", "memory", "memory", "memory",
    "memory", "collectives", "collectives", "collectives", "collectives", "atomics", "atomics",
    "rma", "rma", "rma", "memory", "memory", "collectives", "collectives", "collectives",
    "memory"
};
static const char *const plant_descriptions[PACER_PLANTS] = {
    "",
    "PE 1 changes a key after counting it",
    "PE 1 skips its first local sort",
    "PE 1 sends a key to the PE after its owner",
    "PE 1 reserves, or declares, more than a PE holds, once",
    "PE 1 flips a byte of one block in every variant",
    "PE 1 writes a word into a gap of the next PE's strided dest",
    "PE 1 hashes other text than it prints",
    "PE 1 flips a byte of its first message",
    "PE 1 signals one message twice over",
    "PE 1 counts one message it read twice",
    "PE 1 flips a byte of one round-trip message and signals it one too many",
    "PE 1's thread 0 puts one word other than it checks",
    "PE 1 writes one word of its table wrong",
    "PE 1's thread 0 counts one add short",
    "PE 1 expects every answer about one team off by one",
    "PE 1 gives its row a wrong input once in every collective",
    "PE 1 claims one split more than it made",
    "PE 1 misjudges its membership of one team",
    "PE 1 misjudges one context round trip",
    "PE 1 adds one more than it drew, once",
    "PE 1 misreads one value a fetch_add returned",
    "PE 1 believes one claim it never made",
    "PE 1 changes one token in hand",
    "PE 1 sets one slot wrong",
    "PE 1 leaves out one xor",
    "PE 1 misjudges one fetched bit",
    "PE 1 sends one halo byte wrong",
    "PE 1 puts one element wrong",
    "PE 1 writes a byte into a strided put's gap",
    "PE 1 changes a byte of the source others read",
    "PE 1 writes a byte into its own strided get's gap",
    "PE 1 stores one word wrong through shmem_ptr",
    "PE 1 counts one acquisition twice",
    "PE 1 writes a holder word not its own",
    "PE 1 overwrites a byte of a block it frees",
    "PE 1 writes a byte into a fresh calloc",
    "PE 1 misreads an aligned address",
    "PE 1 misreads one addr_accessible",
    "PE 1 misrecords one allocation",
    "PE 1 gives one reduction an input its result cannot absorb",
    "PE 1 gives one scan an input one too many",
    "PE 1 offers one byte wrong to a gather",
    "PE 1 writes a byte into a gap of its own alltoalls dest",
    "PE 1 increments one slot once more than its stream says",
    "PE 1 gives up claiming after its first claim",
    "PE 1 misreads its halo flag once",
    "PE 1 sends one value wrong by p",
    "PE 1 takes one shmem_ptr to a node-mate for NULL",
    "PE 1 overwrites a byte of a block it is about to realloc",
    "PE 1 overwrites a byte of the block whose realloc is refused",
    "PE 1 misreads one collective's status",
    "PE 1 gives one timed reduction a wrong input",
    "PE 1 offers one byte wrong to a timed gather",
    "PE 1 takes a refused realloc for granted",
};

static void usage(FILE *out)
{
    fprintf(out,
"usage: osoe-pacer [OPTION]... [EXERCISE]...\n"
"Puts the library through its paces: whole-job patterns from real OpenSHMEM\n"
"codes, every result checked against an oracle, a line a checkpoint.\n"
"\n"
"Exercises, all but threads and print when none is named:\n"
"  sort      a streaming bucket sort, ISx's exchange: every key to its owner\n"
"  alltoall  every-to-every exchange eight ways, on the world and on `even'\n"
"  mailbox   messages with signals, waited for by every wait and test form\n"
"  teams     teams of every shape, their queries, collectives on rows and\n"
"            columns at once\n"
"  atomics   every atomic operation on every type, on three contexts\n"
"  rma       a halo put and fenced to a flag, and every transfer of every\n"
"            type and size, strided, blocked, nonblocking, and by shmem_ptr\n"
"  locks     distributed locks guarding counts that are not atomic\n"
"  memory    the heap's routines, collective and checked, up to the budget\n"
"  collectives\n"
"            every collective on every type, and each timed from 8 B to\n"
"            1 MiB on the world and on a strided team\n"
"  threads   worker threads on private, serialized, default and team contexts;\n"
"            only when named, since it runs under SHMEM_THREAD_MULTIPLE\n"
"  print     parallel_print checking its own lines; only when named\n"
"\n"
"Options:\n"
"  --seconds S         each exercise about S seconds (default 60)\n"
"  --rounds R          exactly R rounds each instead, a checkpoint a round\n"
"  --report S          seconds between checkpoints (default 5)\n"
"  --memory SIZE|P%%    what the exercises may use a PE, symmetric and private\n"
"                      each: a size as SHMEM_SYMMETRIC_SIZE spells one, or a\n"
"                      share of the heap (default 50%%)\n"
"  --seed N            the seed every random choice derives from (default 1)\n"
"  --quick             the smoke test's sizes and rounds\n"
"  --sort rounds|accumulate\n"
"                      recycle two buffers a round (default), or fill the\n"
"                      budget and sort everything at once\n"
"  --exchange fetch-add|alltoall\n"
"                      how the sort moves keys: reserve by fetch-add and push\n"
"                      (default), or exchange counts and pull\n"
"  --no-ptr            put to node-mates rather than store through shmem_ptr\n"
"  --threads T         the threads exercise's workers a PE (default 4); T + 1\n"
"                      at most OSOE_MAX_THREADS (the library's, default 16)\n"
"  --teams-exhaust     the teams exercise splits until a split fails, and\n"
"                      stops; teams must be the only exercise named\n"
"  --stop-on-failure   end the job at the first checkpoint with a failure\n"
"  --coverage FILE     osoe-pacer-counted only: which routines were called, how\n"
"                      often, and which never were, into FILE\n"
"  --plant DEFECT      plant a defect a check must catch, from the list below;\n"
"                      always on PE 1, and named in the header\n"
"  --help              this, before shmem_init\n"
"\n"
"The last line is \"osoe-pacer failures=N seconds=S\"; the exit status is 1\n"
"when N is not 0, and 2 when the command line or the budget is refused.\n"
"\n"
"Defects --plant takes, and the exercise that must be named with each:\n");
    for (int k = 1; k < PACER_PLANTS; k++)
        fprintf(out, "  %-20s %-9s %s\n", plant_names[k], plant_exercise[k], plant_descriptions[k]);
}

/* ---- What the exercises are given (exercise.h) ------------------------- */

static struct {
    int pes, nodes;
    int *node, *local, *partner;
    int *count;                     /* [nodes]: PEs a node */
} layout;

static struct pacer_layout layout_view;

const struct pacer_layout *pacer_layout(void)
{
    return &layout_view;
}

double pacer_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Worker threads report too: one lock around the lines and counts. */
static pthread_mutex_t failures_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    long count;                     /* since the last checkpoint */
    long total;                     /* this PE's, over the run */
    int lines;
    char text[FAILURE_LINES][FAILURE_LINE];
} failures;

void pacer_failure(const char *format, ...)
{
    pthread_mutex_lock(&failures_lock);
    failures.count++;
    failures.total++;
    if (failures.lines < FAILURE_LINES) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(failures.text[failures.lines++], FAILURE_LINE, format, arguments);
        va_end(arguments);
    }
    pthread_mutex_unlock(&failures_lock);
}

/* Lines for the end: the exercise's own wait here until the driver has put
 * its summary line first. */
static struct {
    char **lines;
    int count, room;
    int pending;                    /* the running exercise's, not yet placed */
} summaries;

static void keep_line(char *line)
{
    if (!line)
        return;
    if (summaries.count == summaries.room) {
        int room = summaries.room ? 2 * summaries.room : 16;
        char **lines = realloc(summaries.lines, sizeof *lines * (size_t)room);
        if (!lines) {
            free(line);
            return;
        }
        summaries.lines = lines;
        summaries.room = room;
    }
    summaries.lines[summaries.count++] = line;
}

void pacer_summary(const char *format, ...)
{
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    keep_line(strdup(text));
    summaries.pending++;
}

/* The driver's own summary line goes in before the exercise's pending ones. */
static void keep_summary_first(const char *text)
{
    char *line = strdup(text);
    int before = summaries.count;
    keep_line(line);
    if (summaries.count == before)
        return;
    int start = summaries.count - 1 - summaries.pending;
    memmove(&summaries.lines[start + 1], &summaries.lines[start],
            sizeof *summaries.lines * (size_t)summaries.pending);
    summaries.lines[start] = line;
    summaries.pending = 0;
}

void pacer_size(char *text, size_t size, uint64_t bytes)
{
    static const struct {
        char unit;
        unsigned shift;
    } units[] = { { 't', 40 }, { 'g', 30 }, { 'm', 20 }, { 'k', 10 } };
    for (size_t i = 0; i < sizeof units / sizeof *units; i++) {
        uint64_t one = UINT64_C(1) << units[i].shift;
        if (bytes >= one && bytes % one == 0) {
            snprintf(text, size, "%llu%c", (unsigned long long)(bytes / one), units[i].unit);
            return;
        }
    }
    /* Not a whole number of any unit: a tenth of the largest that fits,
     * rounded up, so that the spelling is never less than the bytes. */
    for (size_t i = 0; i < sizeof units / sizeof *units; i++) {
        uint64_t one = UINT64_C(1) << units[i].shift;
        if (bytes >= one) {
            uint64_t tenths = (bytes * 10 + one - 1) / one;
            snprintf(text, size, "%llu.%llu%c", (unsigned long long)(tenths / 10),
                     (unsigned long long)(tenths % 10), units[i].unit);
            return;
        }
    }
    snprintf(text, size, "%llu", (unsigned long long)bytes);
}

/* ---- The command line --------------------------------------------------- */

#ifdef PACER_COUNTED
/* Where osoe-pacer-counted writes which routines were called (--coverage). */
static const char *coverage;
void pacer_shim_report(const char *path);   /* the generated shim's, collective */
#endif

static struct pacer_options options = {
    .seconds = 60, .report = 5, .seed = 1, .use_ptr = 1, .threads = 4, .period = 10,
};
static double memory_share = 50;    /* percent of the heap; 0 when a size was given */
static uint64_t memory_size;
static const char *memory_text = "50%";
static const struct pacer_exercise *chosen[KNOWN];
static int chosen_count;

static int refuse(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int refuse(const char *format, ...)
{
    /* Every PE refuses, before init, and a line in one fprintf is one write on
     * the unbuffered stderr: PEs refusing at once do not splice their lines. */
    char message[640];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    fprintf(stderr, "osoe-pacer: %s (osoe-pacer --help)\n", message);
    return 2;
}

static int positive_number(const char *text, double *out)
{
    char *end;
    double value = strtod(text, &end);
    if (end == text || *end || !(value > 0) || value > 1e9)
        return -1;
    *out = value;
    return 0;
}

/* 0, 1 for --help, or 2 when refused. */
static int parse(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        if (strncmp(argument, "--", 2) != 0) {
            if (strcmp(argument, "mix") == 0) {
                options.mixing = 1;
                continue;
            }
            int k = 0;
            while (k < KNOWN && strcmp(argument, known[k]->name) != 0)
                k++;
            if (k == KNOWN)
                return refuse("no exercise '%s'; --help lists them", argument);
            int again = 0;
            for (int j = 0; j < chosen_count; j++)
                again |= chosen[j] == known[k];
            if (!again)
                chosen[chosen_count++] = known[k];
            continue;
        }
        char name[32];
        const char *value = NULL;
        const char *equals = strchr(argument, '=');
        size_t length = equals ? (size_t)(equals - argument) : strlen(argument);
        if (length >= sizeof name)
            return refuse("no option '%s'", argument);
        memcpy(name, argument, length);
        name[length] = '\0';

        static const char *const flags[] = { "--help", "--quick", "--no-ptr",
                                             "--stop-on-failure", "--teams-exhaust" };
        int flag = -1;
        for (int k = 0; k < 5; k++)
            if (strcmp(name, flags[k]) == 0)
                flag = k;
        if (flag >= 0) {
            if (equals)
                return refuse("%s takes no value", name);
            if (flag == 0)
                return 1;
            if (flag == 1)
                options.quick = 1;
            if (flag == 2)
                options.use_ptr = 0;
            if (flag == 3)
                options.stop_on_failure = 1;
            if (flag == 4)
                options.teams_exhaust = 1;
            continue;
        }
        static const char *const valued[] = { "--seconds", "--report", "--rounds", "--seed",
                                              "--memory", "--sort", "--exchange", "--plant",
                                              "--threads", "--coverage", "--mix-teams",
                                              "--period" };
        int takes = 0;
        for (size_t k = 0; k < sizeof valued / sizeof *valued; k++)
            takes |= strcmp(name, valued[k]) == 0;
        if (!takes)
            return refuse("no option '%s'", name);
        if (equals)
            value = equals + 1;
        else if (i + 1 < argc)
            value = argv[++i];
        else
            return refuse("%s needs a value", name);

        if (strcmp(name, "--seconds") == 0) {
            if (positive_number(value, &options.seconds) != 0)
                return refuse("--seconds '%s': a number of seconds above 0", value);
        } else if (strcmp(name, "--report") == 0) {
            if (positive_number(value, &options.report) != 0)
                return refuse("--report '%s': a number of seconds above 0", value);
        } else if (strcmp(name, "--rounds") == 0) {
            char *end;
            long rounds = strtol(value, &end, 10);
            if (end == value || *end || rounds < 1)
                return refuse("--rounds '%s': a whole number above 0", value);
            options.rounds = rounds;
        } else if (strcmp(name, "--threads") == 0) {
            char *end;
            long count = strtol(value, &end, 10);
            if (end == value || *end || count < 1 || count > 1023)
                return refuse("--threads '%s': a whole number from 1 to 1023", value);
            options.threads = (int)count;
        } else if (strcmp(name, "--mix-teams") == 0) {
            char *end;
            long count = strtol(value, &end, 10);
            if (end == value || *end || count < 1 || count > 5)
                return refuse("--mix-teams '%s': a whole number from 1 to 5, one a team exercise",
                              value);
            options.mix_teams = (int)count;
        } else if (strcmp(name, "--period") == 0) {
            if (positive_number(value, &options.period) != 0)
                return refuse("--period '%s': a number of seconds above 0", value);
        } else if (strcmp(name, "--coverage") == 0) {
#ifdef PACER_COUNTED
            coverage = value;
#else
            return refuse("--coverage: this build counts nothing; osoe-pacer-counted does");
#endif
        } else if (strcmp(name, "--seed") == 0) {
            char *end;
            unsigned long long seed = strtoull(value, &end, 0);
            if (end == value || *end || value[0] == '-')
                return refuse("--seed '%s': a whole number", value);
            options.seed = seed;
        } else if (strcmp(name, "--memory") == 0) {
            size_t n = strlen(value);
            memory_text = value;
            if (n > 1 && value[n - 1] == '%') {
                char share[32];
                if (n >= sizeof share)
                    return refuse("--memory '%s': a size or a share of the heap", value);
                memcpy(share, value, n - 1);
                share[n - 1] = '\0';
                if (positive_number(share, &memory_share) != 0)
                    return refuse("--memory '%s': a share above 0%%", value);
                memory_size = 0;
            } else {
                if (osoe_parse_size(value, &memory_size) != 0 || memory_size == 0)
                    return refuse("--memory '%s': a size as SHMEM_SYMMETRIC_SIZE spells one, "
                                  "or a share of the heap as P%%", value);
                memory_share = 0;
            }
        } else if (strcmp(name, "--sort") == 0) {
            if (strcmp(value, "rounds") == 0)
                options.sort_mode = PACER_SORT_ROUNDS;
            else if (strcmp(value, "accumulate") == 0)
                options.sort_mode = PACER_SORT_ACCUMULATE;
            else
                return refuse("--sort '%s': rounds or accumulate", value);
        } else if (strcmp(name, "--exchange") == 0) {
            if (strcmp(value, "fetch-add") == 0)
                options.exchange = PACER_EXCHANGE_FETCH_ADD;
            else if (strcmp(value, "alltoall") == 0)
                options.exchange = PACER_EXCHANGE_ALLTOALL;
            else
                return refuse("--exchange '%s': fetch-add or alltoall", value);
        } else if (strcmp(name, "--plant") == 0) {
            int k = 1;
            while (k < PACER_PLANTS && strcmp(value, plant_names[k]) != 0)
                k++;
            if (k == PACER_PLANTS)
                return refuse("--plant '%s': no such defect; --help lists them", value);
            options.plant = (enum pacer_plant)k;
        } else {
            return refuse("no option '%s'", name);
        }
    }
    /* mix runs its own: the five team exercises, then the two of the world. */
    if (options.mixing) {
        if (chosen_count)
            return refuse("mix runs its own exercises: name it alone");
        chosen[chosen_count++] = &pacer_mailbox;
        chosen[chosen_count++] = &pacer_atomics;
        chosen[chosen_count++] = &pacer_rma;
        chosen[chosen_count++] = &pacer_locks;
        chosen[chosen_count++] = &pacer_collectives;
        chosen[chosen_count++] = &pacer_sort;
        chosen[chosen_count++] = &pacer_alltoall;
    }
    if (chosen_count == 0) {        /* every exercise but threads and print */
        chosen[chosen_count++] = &pacer_sort;
        chosen[chosen_count++] = &pacer_alltoall;
        chosen[chosen_count++] = &pacer_mailbox;
        chosen[chosen_count++] = &pacer_teams;
        chosen[chosen_count++] = &pacer_atomics;
        chosen[chosen_count++] = &pacer_rma;
        chosen[chosen_count++] = &pacer_locks;
        chosen[chosen_count++] = &pacer_memory;
        chosen[chosen_count++] = &pacer_collectives;
    }
    /* Exhausting the job's team numbers leaves none for any exercise after. */
    if (options.teams_exhaust && (chosen_count != 1 || chosen[0] != &pacer_teams))
        return refuse("--teams-exhaust uses up the job's team numbers: name teams alone");
    if (options.plant != PACER_PLANT_NONE) {
        int present = 0;
        for (int j = 0; j < chosen_count; j++)
            present |= strcmp(chosen[j]->name, plant_exercise[options.plant]) == 0;
        if (!present)
            return refuse("--plant %s is caught by the %s exercise, which is not named",
                          plant_names[options.plant], plant_exercise[options.plant]);
    }
    /* The library admits OSOE_MAX_THREADS threads of a PE at once, the main
     * one among them (default 16, README); one more ends the job. */
    for (int k = 0; k < chosen_count; k++)
        if (chosen[k] == &pacer_threads) {
            const char *text = getenv("OSOE_MAX_THREADS");
            char *end = NULL;
            long most = text ? strtol(text, &end, 10) : 16;
            if ((!text || (end != text && !*end)) && options.threads + 1 > most)
                return refuse("--threads %d: with the main thread, more than OSOE_MAX_THREADS=%ld "
                              "admits; raise it, to 1023 at most", options.threads, most);
        }
    return 0;
}

/* ---- The header --------------------------------------------------------- */

/* Every PE's node and local rank, from SHMEM_TEAM_SHARED: one fcollect of
 * {its node's lead, its rank there}, a lead being the shared team's rank 0. */
static int gather_layout(void)
{
    int n = shmem_n_pes();
    int32_t *mine = shmem_malloc(2 * sizeof *mine);
    int32_t *all = shmem_malloc(2 * sizeof *all * (size_t)n);
    layout.node = malloc(sizeof *layout.node * (size_t)n);
    layout.local = malloc(sizeof *layout.local * (size_t)n);
    layout.partner = malloc(sizeof *layout.partner * (size_t)n);
    int *number = malloc(sizeof *number * (size_t)n);
    if (!mine || !all || !layout.node || !layout.local || !layout.partner || !number)
        return -1;
    mine[0] = shmem_team_translate_pe(SHMEM_TEAM_SHARED, 0, SHMEM_TEAM_WORLD);
    mine[1] = shmem_team_my_pe(SHMEM_TEAM_SHARED);
    if (shmem_fcollectmem(SHMEM_TEAM_WORLD, all, mine, 2 * sizeof *mine) != 0)
        return -1;
    for (int p = 0; p < n; p++)
        number[p] = -1;
    for (int p = 0; p < n; p++)
        if (all[2 * p] >= 0 && all[2 * p] < n)
            number[all[2 * p]] = 0;
    int nodes = 0;
    for (int q = 0; q < n; q++)
        if (number[q] == 0)
            number[q] = ++nodes;    /* nodes in order of their leads' world ranks */
    layout.count = calloc((size_t)(nodes ? nodes : 1), sizeof *layout.count);
    if (!layout.count)
        return -1;
    for (int p = 0; p < n; p++) {
        int lead = all[2 * p];
        layout.node[p] = lead >= 0 && lead < n ? number[lead] - 1 : 0;
        layout.local[p] = all[2 * p + 1];
        layout.count[layout.node[p]]++;
    }
    /* Partners: each PE by its node and rank there, then the same rank on
     * the other node of the pair. `number` is reused, as each node's first
     * place in that order. */
    int *placed = malloc(sizeof *placed * (size_t)n);
    if (!placed)
        return -1;
    for (int k = 0, sum = 0; k < nodes; k++) {
        number[k] = sum;
        sum += layout.count[k];
    }
    for (int p = 0; p < n; p++)
        placed[p] = -1;
    for (int p = 0; p < n; p++)
        if (layout.local[p] >= 0 && layout.local[p] < layout.count[layout.node[p]])
            placed[number[layout.node[p]] + layout.local[p]] = p;
    for (int p = 0; p < n; p++) {
        int other = layout.node[p] ^ 1, rank = layout.local[p];
        layout.partner[p] = other < nodes && rank >= 0 && rank < layout.count[other]
                            ? placed[number[other] + rank] : -1;
    }
    free(placed);
    layout.pes = n;
    layout.nodes = nodes;
    layout_view = (struct pacer_layout){ n, nodes, layout.node, layout.local, layout.partner };
    free(number);
    shmem_free(all);
    shmem_free(mine);
    return 0;
}

static void release_layout(void)
{
    free(layout.count);
    free(layout.partner);
    free(layout.local);
    free(layout.node);
}

struct host {
    const char *name;
    size_t prefix;                  /* the name's length before its trailing digits */
    unsigned long long number;
    int width;                      /* digits kept with leading zeros; 0 when natural */
    int numbered;
};

static int host_order(const void *a, const void *b)
{
    const struct host *x = a, *y = b;
    size_t shorter = x->prefix < y->prefix ? x->prefix : y->prefix;
    int c = strncmp(x->name, y->name, shorter);
    if (c)
        return c;
    if (x->prefix != y->prefix)
        return x->prefix < y->prefix ? -1 : 1;
    if (x->numbered != y->numbered)
        return x->numbered - y->numbered;
    if (x->width != y->width)
        return x->width - y->width;
    return (x->number > y->number) - (x->number < y->number);
}

/* The nodes' host names, folded as Slurm folds a host list: pi-[001-004]. */
static void fold_hosts(char *out, size_t size)
{
    struct host *hosts = calloc((size_t)layout.nodes, sizeof *hosts);
    if (!hosts) {
        snprintf(out, size, "?");
        return;
    }
    int count = 0;
    for (int node = 0; node < layout.nodes; node++) {
        int pe = 0;
        while (pe < layout.pes && layout.node[pe] != node)
            pe++;
        struct host h = { .name = parallel_print_host(pe) };
        size_t length = strlen(h.name), start = length;
        while (start > 0 && h.name[start - 1] >= '0' && h.name[start - 1] <= '9')
            start--;
        h.prefix = start;
        h.numbered = start < length && length - start < 19;
        if (h.numbered) {
            h.number = strtoull(h.name + start, NULL, 10);
            h.width = h.name[start] == '0' && length - start > 1 ? (int)(length - start) : 0;
        } else {
            h.prefix = length;
        }
        int seen = 0;
        for (int j = 0; j < count && !seen; j++)
            seen = strcmp(hosts[j].name, h.name) == 0;
        if (!seen)
            hosts[count++] = h;
    }
    qsort(hosts, (size_t)count, sizeof *hosts, host_order);
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < count && used < size;) {
        int j = i;              /* one prefix and width: one bracket */
        while (j + 1 < count && hosts[i].numbered && hosts[j + 1].numbered &&
               hosts[j + 1].prefix == hosts[i].prefix && hosts[j + 1].width == hosts[i].width &&
               strncmp(hosts[j + 1].name, hosts[i].name, hosts[i].prefix) == 0)
            j++;
        if (used)
            used += (size_t)snprintf(out + used, size - used, ",");
        if (j == i) {
            used += (size_t)snprintf(out + used, size - used, "%s", hosts[i].name);
        } else {
            used += (size_t)snprintf(out + used, size - used, "%.*s[", (int)hosts[i].prefix,
                                     hosts[i].name);
            for (int k = i; k <= j && used < size;) {
                int m = k;
                while (m + 1 <= j && hosts[m + 1].number == hosts[m].number + 1)
                    m++;
                used += (size_t)snprintf(out + used, size - used, "%s%0*llu", k > i ? "," : "",
                                         hosts[k].width, hosts[k].number);
                if (m > k && used < size)
                    used += (size_t)snprintf(out + used, size - used, "-%0*llu", hosts[m].width,
                                             hosts[m].number);
                k = m + 1;
            }
            if (used < size)
                used += (size_t)snprintf(out + used, size - used, "]");
        }
        i = j + 1;
    }
    free(hosts);
}

static uint64_t heap_size(void)
{
    const char *text = getenv("SHMEM_SYMMETRIC_SIZE");
    if (!text)
        text = getenv("SMA_SYMMETRIC_SIZE");
    uint64_t bytes = DEFAULT_HEAP;
    if (text && osoe_parse_size(text, &bytes) != 0)
        bytes = DEFAULT_HEAP;       /* the library has refused it already */
    return bytes;
}

static const char *thread_level(void)
{
    int provided = -1;
    shmem_query_thread(&provided);
    switch (provided) {
    case SHMEM_THREAD_SINGLE: return "single";
    case SHMEM_THREAD_FUNNELED: return "funneled";
    case SHMEM_THREAD_SERIALIZED: return "serialized";
    case SHMEM_THREAD_MULTIPLE: return "multiple";
    default: return "?";
    }
}

/* ---- The loop ----------------------------------------------------------- */

/* The counters' reduction, in two pairs of symmetric arrays. Two reductions a
 * checkpoint, alternating, and each result copied out before the PE enters
 * the other: a reduction completes on no PE before every PE has entered it,
 * so every PE has read an array's last result before any PE can reach the
 * next reduction that writes it - the spec's "dest ready on every PE"
 * (§9.10), with no sync spent on it. */
enum {
    SUM_WORK, SUM_BYTES, SUM_BATCH, SUM_FAILURES, SUM_LIBRARY_NODES, SUM_LIBRARY,
    SUMS = SUM_LIBRARY + PACER_LIBRARY_COUNTERS,
    MAX_STEP = 0, MAX_BATCH, MAX_ELAPSED, MAX_LINES, MAX_FINISHED, MAX_PHASE, MAXIMA =
        MAX_PHASE + PACER_PHASES
};
static double *sum_source, *sum_result, *max_source, *max_result;
static int library_counters_open;   /* this PE reads its node's counters */

struct totals {
    long rounds;
    double work, bytes, seconds, failures;
    double batch;                   /* this PE's, for the run's imbalance */
};

/*
 * A checkpoint's failure lines, by content: one fcollect of every member's
 * line hashes, then a parallel_print a distinct line - in the order of the
 * lowest rank holding each, then that rank's own order - in which every member
 * holding the line offers it. So a failure is one line under all its PEs,
 * however many others each recorded before it; printing each member's k-th
 * line in call k, the first way, split a failure recorded third on one PE and
 * second on another (the author's 16-PE run of 2026-09-27). At most
 * DISTINCT_FAILURES lines a checkpoint, and the number of the rest is said.
 */
static uint64_t *failure_mine, *failure_all;    /* symmetric: [FAILURE_LINES], [.. * world] */

struct failure_place {
    uint64_t hash;
    int rank, index;
};
static struct failure_place *failure_places;    /* [FAILURE_LINES * world] */

static uint64_t line_hash(const char *text)
{
    uint64_t h = random_finalize(random_name(text));
    return h ? h : 1;
}

static int by_hash_then_place(const void *a, const void *b)
{
    const struct failure_place *x = a, *y = b;
    if (x->hash != y->hash)
        return x->hash < y->hash ? -1 : 1;
    if (x->rank != y->rank)
        return x->rank < y->rank ? -1 : 1;
    return (x->index > y->index) - (x->index < y->index);
}

static int by_place(const void *a, const void *b)
{
    const struct failure_place *x = a, *y = b;
    if (x->rank != y->rank)
        return x->rank < y->rank ? -1 : 1;
    return (x->index > y->index) - (x->index < y->index);
}

static void flush_failures(shmem_team_t team, int any)
{
    if (any) {
        int n = shmem_team_n_pes(team);
        for (int k = 0; k < FAILURE_LINES; k++)
            failure_mine[k] = k < failures.lines ? line_hash(failures.text[k]) : 0;
        shmem_fcollectmem(team, failure_all, failure_mine, sizeof *failure_mine * FAILURE_LINES);
        /* Each hash at its first place: sorted by hash and place, a run's head. */
        size_t count = 0, distinct = 0;
        for (int r = 0; r < n; r++)
            for (int k = 0; k < FAILURE_LINES; k++) {
                uint64_t h = failure_all[(size_t)r * FAILURE_LINES + (size_t)k];
                if (h)
                    failure_places[count++] = (struct failure_place){ h, r, k };
            }
        qsort(failure_places, count, sizeof *failure_places, by_hash_then_place);
        for (size_t i = 0; i < count; i++)
            if (i == 0 || failure_places[i].hash != failure_places[i - 1].hash)
                failure_places[distinct++] = failure_places[i];
        qsort(failure_places, distinct, sizeof *failure_places, by_place);
        size_t shown = distinct < DISTINCT_FAILURES ? distinct : DISTINCT_FAILURES;
        for (size_t d = 0; d < shown; d++) {
            const char *text = "";
            for (int k = 0; k < failures.lines && !*text; k++)
                if (failure_mine[k] == failure_places[d].hash)
                    text = failures.text[k];
            parallel_print(team, "%s", text);
        }
        if (distinct > shown && shmem_team_my_pe(team) == 0) {
            printf("osoe-pacer: %zu more distinct failure lines at this checkpoint, not shown\n",
                   distinct - shown);
            fflush(stdout);
        }
    }
    failures.lines = 0;
    failures.count = 0;
}

static double failures_total;       /* the job's, reduced after each exercise */
static double run_start;

static void final_line(void)
{
    printf("osoe-pacer failures=%.0f seconds=%.1f%s%s\n", failures_total, pacer_now() - run_start,
           options.plant ? " plant=" : "", plant_names[options.plant]);
    fflush(stdout);
}

/*
 * One exercise's steps and checkpoints on a team, from the state setup or
 * rebind left it in: `total_rounds` of them, or about `seconds`. Its lines
 * start with `prefix` - mix's team name, or nothing. Its summary line is kept
 * for the end of the run, or under mix printed at once.
 */
static void run_on(const struct pacer_exercise *exercise, shmem_team_t team, long total_rounds,
                   double run_seconds, const char *prefix)
{
    int me = shmem_team_my_pe(team), n = shmem_team_n_pes(team);
    int phases = 0;
    while (exercise->phases && exercise->phases[phases] && phases < PACER_PHASES)
        phases++;

    struct totals totals = { 0 };
    summaries.pending = 0;
    double start = pacer_now();
    long units = 1;
    int last = total_rounds ? total_rounds == 1 : 0;
    for (;;) {
        double before = pacer_now();
        exercise->step(units, last);
        double seconds = pacer_now() - before;
        struct pacer_counters counters = { 0 };
        exercise->checkpoint(&counters);
        totals.rounds += units;

        sum_source[SUM_WORK] = counters.work;
        sum_source[SUM_BYTES] = counters.bytes;
        sum_source[SUM_BATCH] = counters.batch;
        sum_source[SUM_FAILURES] = (double)failures.count;
        /* The library's counters, on the world alone: a team's checkpoint
         * would sum only the nodes whose first PE is in it. They are read
         * only here, so a world line's growth covers everything since the
         * last - a mix's team periods between them included. */
        double growth[PACER_LIBRARY_COUNTERS] = { 0 };
        int library_here = team == SHMEM_TEAM_WORLD && library_counters_open;
        if (library_here)
            pacer_library_counters_growth(growth);
        sum_source[SUM_LIBRARY_NODES] = library_here;
        for (int k = 0; k < PACER_LIBRARY_COUNTERS; k++)
            sum_source[SUM_LIBRARY + k] = growth[k];
        shmem_double_sum_reduce(team, sum_result, sum_source, SUMS);
        double sums[SUMS], maxima[MAXIMA];
        memcpy(sums, sum_result, sizeof sums);
        max_source[MAX_STEP] = seconds;
        max_source[MAX_BATCH] = counters.batch;
        max_source[MAX_ELAPSED] = pacer_now() - start;
        max_source[MAX_LINES] = failures.lines;
        max_source[MAX_FINISHED] = counters.finished;
        for (int k = 0; k < PACER_PHASES; k++)
            max_source[MAX_PHASE + k] = counters.phase_seconds[k];
        shmem_double_max_reduce(team, max_result, max_source, MAXIMA);
        memcpy(maxima, max_result, sizeof maxima);

        double step = maxima[MAX_STEP], elapsed = maxima[MAX_ELAPSED];
        totals.work += sums[SUM_WORK];
        totals.bytes += sums[SUM_BYTES];
        totals.seconds += step;
        totals.failures += sums[SUM_FAILURES];
        totals.batch += counters.batch;

        if (me == 0) {
            char line[1024];
            size_t used = 0, size = sizeof line;
            double rate_seconds = step > 0 ? step : 1e-9;
            used += (size_t)snprintf(line + used, size - used, "%s%s t=%.1f round=%ld %s=%.0f %s_per_s=%.0f",
                                     prefix, exercise->name, elapsed, totals.rounds, exercise->work,
                                     totals.work, exercise->work,
                                     sums[SUM_WORK] / rate_seconds);
            if (sums[SUM_BYTES] > 0 && used < size)
                used += (size_t)snprintf(line + used, size - used, " mib_per_s=%.1f",
                                         sums[SUM_BYTES] / 1048576.0 / rate_seconds);
            if (sums[SUM_BATCH] > 0 && used < size)
                used += (size_t)snprintf(line + used, size - used, " imbalance=%.3f",
                                         maxima[MAX_BATCH] / (sums[SUM_BATCH] / n));
            for (int k = 0; k < phases && used < size; k++)
                used += (size_t)snprintf(line + used, size - used, " %s_s=%.3f",
                                         exercise->phases[k], maxima[MAX_PHASE + k]);
            if (used < size)
                snprintf(line + used, size - used, " step_s=%.3f failures=%.0f\n",
                         step, totals.failures);
            fputs(line, stdout);
            if (sums[SUM_LIBRARY_NODES] > 0) {
                printf("%slibrary nodes=%.0f", prefix, sums[SUM_LIBRARY_NODES]);
                for (int k = 0; k < PACER_LIBRARY_COUNTERS; k++)
                    printf(" %s=%.0f", pacer_library_field_names[k], sums[SUM_LIBRARY + k]);
                printf("\n");
            }
            fflush(stdout);
        }
        flush_failures(team, (int)maxima[MAX_LINES]);
        if (options.stop_on_failure && sums[SUM_FAILURES] > 0) {
            failures_total += totals.failures;
            if (me == 0) {
                printf("osoe-pacer stopped: %s failed at round %ld\n", exercise->name,
                       totals.rounds);
                final_line();
                shmem_global_exit(1);
            }
            shmem_team_sync(team);  /* the global exit ends this */
        }
        if (last || maxima[MAX_FINISHED] > 0)
            break;

        /* The next step: about --report seconds, its units from the slowest PE's
         * time a unit, never more than eight times the last so that one quick
         * start cannot overshoot; the last when it reaches the end. */
        if (total_rounds) {
            last = totals.rounds + 1 >= total_rounds;
            continue;
        }
        double per_unit = step / (double)units;
        if (!(per_unit > 0))
            per_unit = 1e-6;
        double wanted = options.report / per_unit;
        long next = wanted < 1 ? 1 : wanted > (double)units * 8 ? units * 8 : (long)wanted;
        if (next > MOST_UNITS)
            next = MOST_UNITS;      /* a step that costs nothing must not overflow the count */
        double remaining = run_seconds - elapsed;
        if (remaining <= (double)next * per_unit) {
            double fit = remaining / per_unit;
            next = fit < 1 ? 1 : (long)(fit + 0.5);
            last = 1;
        }
        units = next;
    }
    /* The run's imbalance: the largest PE's whole batch over the mean. */
    sum_source[0] = totals.batch;
    shmem_double_sum_reduce(team, sum_result, sum_source, 1);
    double batch_sum = sum_result[0];
    max_source[0] = totals.batch;
    shmem_double_max_reduce(team, max_result, max_source, 1);
    double batch_max = max_result[0];
    failures_total += totals.failures;

    if (me == 0) {
        char line[512];
        double seconds = totals.seconds > 0 ? totals.seconds : 1e-9;
        size_t used = (size_t)snprintf(line, sizeof line,
                                       "%ssummary %s rounds=%ld %s=%.0f seconds=%.1f %s_per_s=%.0f",
                                       prefix, exercise->name, totals.rounds, exercise->work,
                                       totals.work, totals.seconds, exercise->work,
                                       totals.work / seconds);
        if (totals.bytes > 0 && used < sizeof line)
            used += (size_t)snprintf(line + used, sizeof line - used, " mib_per_s=%.1f",
                                     totals.bytes / 1048576.0 / seconds);
        if (batch_sum > 0 && used < sizeof line)
            used += (size_t)snprintf(line + used, sizeof line - used, " imbalance=%.3f",
                                     batch_max / (batch_sum / n));
        if (used < sizeof line)
            snprintf(line + used, sizeof line - used, " failures=%.0f", totals.failures);
        if (options.mixing) {
            printf("%s\n", line);
            fflush(stdout);
        } else {
            keep_summary_first(line);
        }
    }
}

static void run(const struct pacer_exercise *exercise, shmem_team_t team)
{
    if (exercise->setup(team, &options) != 0) {
        fprintf(stderr, "osoe-pacer: PE %d: %s could not set up\n", shmem_my_pe(), exercise->name);
        shmem_global_exit(2);
    }
    long total_rounds = options.rounds ? options.rounds
                        : options.quick ? exercise->quick_rounds : 0;
    run_on(exercise, team, total_rounds, options.seconds, "");
    exercise->teardown();
}

/* ---- mix (proposal §4.9) ------------------------------------------------------ */

static const char *line_prefix = "";

const char *pacer_prefix(void)
{
    return line_prefix;
}

/*
 * The world split into K strided teams, mix0 to mixK-1, and a sequence of
 * periods. In a team period each team runs one exercise - team k the one at
 * (k + q) mod 5 in the list below, q counting team periods - for its own
 * seeded length, so the teams run out of step and no exercise is on two teams
 * at once; every fourth period is sort or alltoall on the whole world, the
 * teams' exercises paused. Every exercise is set up once, on the world, its
 * symmetric memory sized there - allocation is collective over the world
 * (§9.3) - and moved onto a period's team by its rebind, afresh, with the
 * period's own seed. Between periods, a world sync.
 */
static const struct pacer_exercise *const mix_team_exercises[] = {
    &pacer_mailbox, &pacer_atomics, &pacer_rma, &pacer_locks, &pacer_collectives
};
static const struct pacer_exercise *const mix_world_exercises[] = { &pacer_sort, &pacer_alltoall };
enum { MIX_TEAM_EXERCISES = 5, MIX_WORLD_EXERCISES = 2, MIX_WORLD_EVERY = 4, MIX_QUICK_ROUNDS = 3 };

static int mix_teams(int npes)
{
    int teams = options.mix_teams ? options.mix_teams : npes >= 8 ? 4 : npes >= 4 ? 2 : 1;
    return teams < npes ? teams : npes;
}

/* The exercise team k runs in team period q, and whether period p is the
 * world's: the schedule every PE computes alike. */
static const struct pacer_exercise *mix_exercise(int k, long q)
{
    return mix_team_exercises[(k + q) % MIX_TEAM_EXERCISES];
}

static int mix_world_period(long p)
{
    return p % MIX_WORLD_EVERY == MIX_WORLD_EVERY - 1;
}

static void run_mix(int npes)
{
    int teams = mix_teams(npes), me_world = shmem_my_pe();
    static char names[8][16];
    shmem_team_t team = SHMEM_TEAM_INVALID;
    int mine = me_world % teams;
    for (int k = 0; k < teams; k++) {
        shmem_team_t made = SHMEM_TEAM_INVALID;
        if (shmem_team_split_strided(SHMEM_TEAM_WORLD, k, teams, (npes - k + teams - 1) / teams,
                                     NULL, 0, &made) != 0) {
            fprintf(stderr, "osoe-pacer: PE %d: mix could not make its teams\n", me_world);
            shmem_global_exit(2);
        }
        if (k == mine) {
            team = made;
            snprintf(names[k], sizeof names[k], "mix%d", k);
            parallel_print_name_team(team, names[k]);
        }
    }
    struct pacer_options setup_options = options;
    setup_options.memory = options.memory / (MIX_TEAM_EXERCISES + MIX_WORLD_EXERCISES);
    const struct pacer_exercise *all[MIX_TEAM_EXERCISES + MIX_WORLD_EXERCISES];
    for (int e = 0; e < MIX_TEAM_EXERCISES; e++)
        all[e] = mix_team_exercises[e];
    for (int e = 0; e < MIX_WORLD_EXERCISES; e++)
        all[MIX_TEAM_EXERCISES + e] = mix_world_exercises[e];
    for (int e = 0; e < MIX_TEAM_EXERCISES + MIX_WORLD_EXERCISES; e++)
        if (all[e]->setup(SHMEM_TEAM_WORLD, &setup_options) != 0) {
            fprintf(stderr, "osoe-pacer: PE %d: %s could not set up\n", me_world, all[e]->name);
            shmem_global_exit(2);
        }

    double start = pacer_now();
    long q = 0;
    /* Quick: six team periods, so that each team meets every team exercise,
     * and the two world periods among them. */
    long periods = options.rounds ? options.rounds : options.quick ? 2 * MIX_WORLD_EVERY : 0;
    for (long p = 0;; p++) {
        if (periods ? p >= periods : pacer_now() - start >= options.seconds)
            break;
        struct pacer_options period = setup_options;
        period.seed = random_mix(options.seed, random_name("mix"), (uint64_t)p, 0);
        const struct pacer_exercise *exercise;
        shmem_team_t on;
        char prefix[16];
        int world = mix_world_period(p);
        if (world) {
            exercise = mix_world_exercises[(p / MIX_WORLD_EVERY) % MIX_WORLD_EXERCISES];
            on = SHMEM_TEAM_WORLD;
            snprintf(prefix, sizeof prefix, "world ");
        } else {
            exercise = mix_exercise(mine, q);
            on = team;
            snprintf(prefix, sizeof prefix, "mix%d ", mine);
        }
        if (me_world == 0) {
            printf("mix period=%ld", p);
            if (world)
                printf(" world=%s", exercise->name);
            else
                for (int k = 0; k < teams; k++)
                    printf(" mix%d=%s", k, mix_exercise(k, q)->name);
            printf("\n");
            fflush(stdout);
        }
        if (exercise->rebind(on, &period) != 0) {
            fprintf(stderr, "osoe-pacer: PE %d: %s cannot run on %s\n", me_world,
                    exercise->name, prefix);
            shmem_global_exit(2);
        }
        /* A team period's own length: half to one and a half --period, by
         * the seed, so that the teams' steps and checkpoints fall out of step. */
        double length = options.period;
        if (!world)
            length *= 0.5 + (double)(random_mix(period.seed, (uint64_t)mine, 1, 0) % 1000) / 1000;
        line_prefix = prefix;
        run_on(exercise, on, periods ? MIX_QUICK_ROUNDS : 0, length, prefix);
        line_prefix = "";
        shmem_quiet();
        shmem_team_sync(SHMEM_TEAM_WORLD);      /* every team's period over */
        if (!world)
            q++;
    }

    /* Back to the world, whose members every teardown's syncs are. */
    struct pacer_options final = setup_options;
    for (int e = 0; e < MIX_TEAM_EXERCISES + MIX_WORLD_EXERCISES; e++) {
        all[e]->rebind(SHMEM_TEAM_WORLD, &final);
        all[e]->teardown();
    }
    parallel_print_name_team(team, NULL);
    shmem_team_destroy(team);
}

int main(int argc, char **argv)
{
    int parsed = parse(argc, argv);
    if (parsed == 1) {
        usage(stdout);
        return 0;
    }
    if (parsed != 0)
        return parsed;

    /* SHMEM_THREAD_MULTIPLE only when an exercise needs it, as osoe-bench's
     * threads: the others should not pay for it. */
    int multiple = 0;
    for (int k = 0; k < chosen_count; k++)
        multiple |= chosen[k]->multiple;
    if (multiple) {
        int provided = -1;
        shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided);
        if (provided != SHMEM_THREAD_MULTIPLE) {
            if (shmem_my_pe() == 0)
                fprintf(stderr, "osoe-pacer: the threads exercise needs SHMEM_THREAD_MULTIPLE, "
                        "and the library gave %d\n", provided);
            shmem_finalize();
            return 2;
        }
    } else {
        shmem_init();
    }
    run_start = pacer_now();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    if (parallel_print_init(PACER_MESSAGE_CAPACITY) != 0 || gather_layout() != 0) {
        fprintf(stderr, "osoe-pacer: PE %d: out of memory at start\n", me);
        shmem_global_exit(2);
    }
    library_counters_open = pacer_library_counters_open();
    sum_source = shmem_malloc(sizeof *sum_source * SUMS);
    sum_result = shmem_malloc(sizeof *sum_result * SUMS);
    max_source = shmem_malloc(sizeof *max_source * MAXIMA);
    max_result = shmem_malloc(sizeof *max_result * MAXIMA);
    failure_mine = shmem_malloc(sizeof *failure_mine * FAILURE_LINES);
    failure_all = shmem_malloc(sizeof *failure_all * FAILURE_LINES * (size_t)npes);
    failure_places = malloc(sizeof *failure_places * FAILURE_LINES * (size_t)npes);
    if (!sum_source || !sum_result || !max_source || !max_result || !failure_mine ||
        !failure_all || !failure_places) {
        fprintf(stderr, "osoe-pacer: PE %d: out of memory at start\n", me);
        shmem_global_exit(2);
    }

    /* The budget, and what the tool keeps of the heap beside it: its own
     * arrays, which grow 48 bytes a PE (parallel_print's records and the
     * failure lines' hashes), and a margin for the small ones. */
    uint64_t heap = heap_size();
    options.memory = memory_share > 0 ? (uint64_t)((double)heap * memory_share / 100)
                                      : memory_size;
    uint64_t kept = UINT64_C(48) * (uint64_t)npes + (UINT64_C(128) << 10);

    char library[SHMEM_MAX_NAME_LEN] = "";
    shmem_info_get_name(library);
    for (char *c = library; *c; c++)
        if (*c == ' ' || *c == '=')
            *c = '_';
    int major = 0, minor = 0;
    shmem_info_get_version(&major, &minor);
    if (me == 0) {
        char line[2048], hosts[512], heap_text[32], memory[32];
        size_t used = 0, size = sizeof line;
        fold_hosts(hosts, sizeof hosts);
        pacer_size(heap_text, sizeof heap_text, heap);
        pacer_size(memory, sizeof memory, options.memory);
        used += (size_t)snprintf(line + used, size - used,
                                 "osoe-pacer library=%s version=%d.%d thread=%s pes=%d nodes=%d "
                                 "layout=", library, major, minor, thread_level(), npes,
                                 layout.nodes);
        for (int node = 0; node < layout.nodes && used < size; node++)
            used += (size_t)snprintf(line + used, size - used, node ? ",%d" : "%d",
                                     layout.count[node]);
        if (used < size)
            used += (size_t)snprintf(line + used, size - used,
                                     " hosts=%s heap=%s memory=%s seed=%llu report=%g exercises=",
                                     hosts, heap_text, memory, (unsigned long long)options.seed,
                                     options.report);
        for (int k = 0; k < chosen_count && used < size; k++)
            used += (size_t)snprintf(line + used, size - used, k ? ",%s" : "%s", chosen[k]->name);
        if (options.plant != PACER_PLANT_NONE && used < size)
            used += (size_t)snprintf(line + used, size - used, " plant=%s",
                                     plant_names[options.plant]);
        if (used < size)
            snprintf(line + used, size - used, "\n");
        fputs(line, stdout);
        if (options.plant != PACER_PLANT_NONE)
            printf("osoe-pacer planted %s: %s; checks must fail, and the exit status is 1\n",
                   plant_names[options.plant], plant_descriptions[options.plant]);
        fflush(stdout);
    }

    int refused = 0;
    if (options.memory + kept > heap) {
        refused = 1;
        if (me == 0) {
            char needed[32], heap_text[32], kept_text[32];
            uint64_t mib = (options.memory + kept + (UINT64_C(1) << 20) - 1) >> 20;
            pacer_size(needed, sizeof needed, mib << 20);
            pacer_size(heap_text, sizeof heap_text, heap);
            pacer_size(kept_text, sizeof kept_text, kept);
            fprintf(stderr, "osoe-pacer: --memory %s does not fit a heap of %s beside the "
                    "tool's own %s; set SHMEM_SYMMETRIC_SIZE=%s or more\n", memory_text,
                    heap_text, kept_text, needed);
        }
    }
    if (options.plant != PACER_PLANT_NONE && npes < 2) {
        refused = 1;
        if (me == 0)
            fprintf(stderr, "osoe-pacer: --plant %s is planted on PE 1: run 2 PEs or more\n",
                    plant_names[options.plant]);
    } else if (options.plant == PACER_PLANT_MAILBOX_ROUND_TRIP && layout.partner[1] < 0) {
        refused = 1;
        if (me == 0)
            fprintf(stderr, "osoe-pacer: --plant %s is planted in PE 1's round trips, and PE 1 "
                    "has no partner: its node's pair needs a PE of its rank there\n",
                    plant_names[options.plant]);
    }
    for (int k = 0; k < chosen_count && !refused; k++) {
        char fields[512] = "";
        /* Under mix every exercise is set up at once, each on a share. */
        struct pacer_options sizing = options;
        if (options.mixing)
            sizing.memory = options.memory / (uint64_t)chosen_count;
        uint64_t bytes = chosen[k]->memory(npes, &sizing, fields, sizeof fields);
        if (bytes > options.memory) {
            refused = 1;
            if (me == 0)
                fprintf(stderr, "osoe-pacer: %s does not fit --memory %s: %s\n",
                        chosen[k]->name, memory_text, fields);
            break;
        }
        if (me == 0) {
            char symmetric[32], schedule[64];
            pacer_size(symmetric, sizeof symmetric, bytes);
            long rounds = options.rounds ? options.rounds
                          : options.quick ? chosen[k]->quick_rounds : 0;
            if (rounds)
                snprintf(schedule, sizeof schedule, "rounds=%ld", rounds);
            else
                snprintf(schedule, sizeof schedule, "seconds=%g", options.seconds);
            printf("exercise %s %s %s memory=%s\n", chosen[k]->name, fields, schedule, symmetric);
            fflush(stdout);
        }
    }
    if (refused) {
        free(failure_places);
        release_layout();
        parallel_print_finalize();
        shmem_finalize();
        return 2;
    }

    if (options.plant == PACER_PLANT_PRINT_HASH && me == 1)
        parallel_print_plant_hash(1);
    if (options.mixing) {
        if (me == 0) {
            printf("mix teams=%d period=%g world_every=%d\n", mix_teams(npes), options.period,
                   MIX_WORLD_EVERY);
            fflush(stdout);
        }
        run_mix(npes);
    } else {
        for (int k = 0; k < chosen_count; k++)
            run(chosen[k], SHMEM_TEAM_WORLD);
    }

    /* Every PE's count, whatever teams the exercises ran on. */
    sum_source[0] = (double)failures.total;
    shmem_double_sum_reduce(SHMEM_TEAM_WORLD, sum_result, sum_source, 1);
    failures_total = sum_result[0];
    for (int k = 0; k < summaries.count; k++) {
        printf("%s\n", summaries.lines[k]);
        fflush(stdout);
        free(summaries.lines[k]);
    }
    free(summaries.lines);
    if (me == 0)
        final_line();
    fflush(stdout);

    free(failure_places);
    shmem_free(failure_all);
    shmem_free(failure_mine);
    shmem_free(max_result);
    shmem_free(max_source);
    shmem_free(sum_result);
    shmem_free(sum_source);
    pacer_library_counters_close();
    release_layout();
    parallel_print_finalize();
#ifdef PACER_COUNTED
    if (coverage)
        pacer_shim_report(coverage);
#endif
    shmem_finalize();
    return failures_total != 0;
}
