/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * threads - SHMEM_THREAD_MULTIPLE (proposal §4.7;
 * docs/prompts/done/task-stress-phase2.md): T worker threads a PE, each working on
 * four contexts in turn and checking every result it can see:
 *
 *   private     its own shmem_ctx_create(SHMEM_CTX_PRIVATE), each slice inside
 *               a session (SHMEM_CTX_SESSION_BATCH)
 *   serialized  its own shmem_ctx_create(SHMEM_CTX_SERIALIZED)
 *   default     SHMEM_CTX_DEFAULT, every thread of the PE issuing and
 *               quieting on it at once - the lock-free shared path
 *   team        a context of the world reversed, which the PE's threads share,
 *               so that its PE numbers go through a team's numbering
 *
 * and on each, a slice's work:
 *
 *   a row      its own words to the next PE, the same to the previous one,
 *              a quiet - or on odd slices a pe_quiet naming only the next,
 *              which leaves the other put outstanding - and a get of the row
 *              back: a quiet on a shared context must complete this thread's
 *              puts whatever the others issue and quiet meanwhile, the
 *              promise OSOE_CHECK_QUIET checks inside the library;
 *   a large    one blocking get of LARGE_WORDS words - 20 KiB, three pieces at
 *   get        the tests' landing zone of 8 KiB (OSOE_GET_LANDING_BYTES) - of
 *              an array the next PE wrote once, from a start of this thread's
 *              own, into a buffer of its own, poisoned first and checked word
 *              by word, the last word first. A blocking get's pieces go out
 *              together, and any thread of the PE that finds no room for its
 *              own copies a landed one out, into the waiting thread's buffer:
 *              a get that returns before that copy is done - a piece counted
 *              before it is copied - shows as stale words at the buffer's end
 *              (the adversarial read of v0.2's blocking gets, C9-4: `mix` ran
 *              60 s clean with that defect built in);
 *   a table    nonblocking gets of a table the next PE wrote once, completed
 *              by one quiet, checked word by word;
 *   counters   fetch-adds on counters spread over every PE, each thread
 *              keeping what it added where: at each checkpoint the sums over
 *              the team of what was added must be what the counters hold.
 *
 * and, after the four contexts, a fifth phase that is not a context:
 *
 *   collectives  a team of the thread's own. Each slice a thread splits its own
 *              copy of the exercise's team, made at setup, into a team of a
 *              shape of its own - all the PEs, all but the first, the even
 *              ones, the order reversed - and on it makes a sum, an inclusive
 *              scan, a broadcast from a turning root and an exclusive scan;
 *              then on the copy a large sum and a large inclusive and
 *              exclusive scan, and on the team a second sum, and destroys the
 *              team. The sizes, 64 to 40,000 bytes, lie either side of the
 *              library's class line, so that a thread's collectives are small
 *              and large, and a large one goes through the lane its thread
 *              lends (design §5.8). Each result is poisoned first and checked
 *              against the sum of the members' words. No other phase, and no
 *              other exercise, has two threads of a PE lead collectives at
 *              once, so that a lane shared by threads - which the library's
 *              shmem.lanes.threads sees - was invisible to `mix`,
 *              `collectives` and `teams` (round six's correctness H1, T25).
 *
 * The driver stays on the main thread: step releases the workers for `units`
 * slices each and waits for all of them; checkpoint, the oracle's reduction
 * and parallel_print run with the workers parked. Initialized with
 * SHMEM_THREAD_MULTIPLE, so named only, as osoe-bench's threads is; the debug
 * and asan tests run it with OSOE_CHECK_QUIET=1 and OSOE_TEST_PERTURB=1.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

/* LARGE_WORDS longs are 20 KiB: three pieces at a landing zone of 8 KiB, as
 * shmem.multiple's blocking get of several pieces is. A worker's start in the
 * array is LARGE_STARTS of its own, moving one word a slice, so that no two
 * workers of a slice read the same words and a piece copied into another
 * thread's buffer reads wrong there. OPERATIONS is a slice's on one context:
 * two puts, the row's get, the large get, the table's gets, the adds. */
enum { ROW = 16, TABLE = 16, GETS = 4, COUNTERS = 8, ADDS = 4, CONTEXTS = 4,
       LARGE_WORDS = 2560, LARGE_STARTS = 8, OPERATIONS = 4 + GETS + ADDS };
enum kind { PRIVATE, SERIALIZED, DEFAULT, TEAM };
static const char *const context_names[CONTEXTS] = { "private", "serialized", "default", "team" };

/*
 * The collectives phase. Its sizes, in 8-byte words, either side of the
 * library's class line (OSOE_SMALL_COLLECTIVE_BYTES, 1 KiB by default): 8 and
 * 128 are small; 129 is large in one piece, 2,048 (16 KiB) in two and 5,000
 * (40,000 bytes) in three. A broadcast of a size's words takes 3 bytes fewer,
 * so that its end is not a whole word. COLLECTIVE_WORDS is the largest, and a
 * phase is timed beside the contexts' four, so PHASES.
 */
enum { COLLECTIVES_PHASE = CONTEXTS, PHASES, COLLECTIVE_WORDS = 5000, COLLECTIVE_SIZES = 5,
       COLLECTIVE_SHAPES = 4 };
static const size_t collective_sizes[COLLECTIVE_SIZES] = { 8, 128, 129, 2048, COLLECTIVE_WORDS };
static const char *const phase_names[] = { "private", "serialized", "default", "team",
                                           "collectives", NULL };
enum collective_kind { SUM, INSCAN, BROADCAST, EXSCAN, COLLECTIVE_KINDS };
static const char *const collective_names[COLLECTIVE_KINDS] = {
    "sum_reduce", "sum_inscan", "broadcast", "sum_exscan"
};

struct worker {
    pthread_t thread;
    int index;
    shmem_ctx_t own[2];             /* private, serialized */
    shmem_team_t parent;            /* its own copy of the team, which its collectives split */
    uint64_t *added;                /* [n][COUNTERS]: what this thread added where */
    uint64_t *large_buffer;         /* [LARGE_WORDS]: where its large get lands */
    uint64_t *source, *dest;        /* symmetric, [COLLECTIVE_WORDS]: a sum's or scan's */
    unsigned char *broadcast_source, *broadcast_dest;   /* symmetric, 8 * COLLECTIVE_WORDS */
    uint64_t *expected;             /* [COLLECTIVE_WORDS]: what `dest` must hold, or as bytes */
    double operations, seconds[PHASES];
    int failed_to_start;
};

static struct {
    /* the plan */
    int count;                      /* T */
    uint64_t seed;
    int plant_row, plant_table, plant_count, plant_pieces;
    int plant_collective[COLLECTIVE_KINDS];     /* a result still to be changed, by kind */
    /* the team, and the world reversed for the team context */
    shmem_team_t team, reversed;
    shmem_ctx_t team_context;
    int n, me, me_world;
    int next, previous;             /* team ranks */
    int next_world, previous_world;
    int next_reversed, previous_reversed;
    int *world;                     /* team rank to world PE */
    int *reversed_of;               /* team rank to its number in `reversed` */
    /* symmetric */
    uint64_t *rows;                 /* [T][CONTEXTS][ROW], written by the previous PE */
    uint64_t *rows_back;            /* the same, written by the next PE */
    uint64_t *table;                /* [T][TABLE], written once */
    uint64_t *large;                /* [LARGE_WORDS + T * LARGE_STARTS], written once */
    uint64_t *counters;             /* [COUNTERS] */
    uint64_t *reduce_source, *reduce_total;   /* [n][COUNTERS] */
    /* private */
    uint64_t *large_expected;       /* the next PE's `large`, as it wrote it */
    uint64_t *added;                /* [n][COUNTERS], every thread's since the start */
    struct worker *workers;
    pthread_mutex_t lock;
    pthread_cond_t start, done;
    long generation, units, slice;
    int finished, stop;
} threads;

static uint64_t threads_memory(int npes, const struct pacer_options *options, char *fields,
                               size_t size)
{
    uint64_t n = (uint64_t)npes, t = (uint64_t)options->threads;
    uint64_t large = 8 * (LARGE_WORDS + t * LARGE_STARTS);  /* the array, and its mirror here */
    /* A thread's collectives: a source and a result of words, and the same of
     * bytes for a broadcast, all symmetric; privately, the words that the result
     * must hold. */
    uint64_t collectives = 8 * t * 4 * COLLECTIVE_WORDS;
    uint64_t symmetric = 8 * (2 * t * CONTEXTS * ROW + t * TABLE + COUNTERS + 2 * n * COUNTERS) +
                         large + collectives;
    uint64_t private_bytes = 8 * (t + 1) * n * COUNTERS + 8 * n + large + 8 * t * LARGE_WORDS +
                             8 * t * COLLECTIVE_WORDS;
    if (symmetric > options->memory || private_bytes > options->memory) {
        snprintf(fields, size, "%d threads on %d PEs need %llu bytes", options->threads, npes,
                 (unsigned long long)(symmetric > private_bytes ? symmetric : private_bytes));
        return UINT64_MAX;
    }
    threads.count = options->threads;
    threads.seed = options->seed;
    threads.plant_row = options->plant == PACER_PLANT_THREADS_ROW;
    threads.plant_table = options->plant == PACER_PLANT_THREADS_TABLE;
    threads.plant_count = options->plant == PACER_PLANT_THREADS_COUNT;
    threads.plant_pieces = options->plant == PACER_PLANT_THREADS_PIECES;
    for (int k = 0; k < COLLECTIVE_KINDS; k++)
        threads.plant_collective[k] = options->plant == PACER_PLANT_THREADS_COLLECTIVE;
    snprintf(fields, size, "threads=%d contexts=private,serialized,default,team row=%d "
             "large_get=%d gets=%d counters=%d collectives=sum,inscan,broadcast,exscan "
             "collective_bytes=%d..%d", threads.count, ROW * 8, LARGE_WORDS * 8, GETS, COUNTERS,
             (int)(8 * collective_sizes[0]), 8 * COLLECTIVE_WORDS);
    return symmetric;
}

static uint64_t table_word(int pe, int thread, int i)
{
    return random_mix(threads.seed ^ random_name("threads"), (uint64_t)pe, (uint64_t)thread,
                      (uint64_t)i);
}

static uint64_t row_word(int pe, int thread, int context, long slice, int i)
{
    return random_mix(threads.seed ^ random_name("rows"), (uint64_t)pe,
                      ((uint64_t)thread << 32) | (uint64_t)context,
                      ((uint64_t)slice << 8) | (uint64_t)i);
}

static uint64_t large_word(int pe, size_t i)
{
    return random_mix(threads.seed ^ random_name("pieces"), (uint64_t)pe, (uint64_t)i, 0);
}

/* ---- one slice of one worker, on one context -------------------------------- */

/*
 * The large get: LARGE_WORDS words of the next PE's array into this worker's
 * buffer, from a start of its own for the slice, on the default context by
 * both spellings in turn and on the others by the context form. The buffer is
 * poisoned first, so that a word the get did not deliver cannot pass for one
 * it did, and checked from its last word back against the mirror of what the
 * next PE wrote - not words made on the spot, which would cost the time that
 * a get returning too early leaves the check: a piece another thread copies
 * out is counted last, and the buffer's end is where a get that returns before
 * the copy is done has not been written yet.
 */
static void large_get(struct worker *w, enum kind k, shmem_ctx_t ctx, int next, long slice)
{
    size_t start = (size_t)w->index * LARGE_STARTS + (size_t)(slice % LARGE_STARTS);
    uint64_t *buffer = w->large_buffer;
    memset(buffer, 0xa5, sizeof *buffer * LARGE_WORDS);
    if (k == DEFAULT && slice % 2)
        shmem_getmem(buffer, threads.large + start, sizeof *buffer * LARGE_WORDS, next);
    else
        shmem_ctx_getmem(ctx, buffer, threads.large + start, sizeof *buffer * LARGE_WORDS, next);
    const uint64_t *expected = threads.large_expected + start;
    size_t wrong = 0, lowest = 0, highest = 0;
    for (size_t i = LARGE_WORDS; i-- > 0;)
        if (buffer[i] != expected[i]) {
            if (wrong++ == 0)
                highest = i;
            lowest = i;
        }
    if (wrong)
        pacer_failure("threads round=%ld %s pieces: thread %d read %zu of %d words of PE %d's "
                      "array wrong, words %zu to %zu of the get, the last %016llx, not %016llx",
                      slice, context_names[k], w->index, wrong, LARGE_WORDS, threads.next_world,
                      lowest, highest, (unsigned long long)buffer[highest],
                      (unsigned long long)expected[highest]);
}

static void on_context(struct worker *w, enum kind k, shmem_ctx_t ctx, long slice,
                       uint64_t *state)
{
    int t = w->index, team_numbering = k == TEAM;
    int next = team_numbering ? threads.next_reversed : threads.next_world;
    int previous = team_numbering ? threads.previous_reversed : threads.previous_world;
    size_t place = ((size_t)t * CONTEXTS + (size_t)k) * ROW;
    double started = pacer_now();

    /* The row: out to both neighbours, a quiet or a pe_quiet naming only the
     * next, and back from the next. */
    uint64_t row[ROW], back[ROW];
    for (int i = 0; i < ROW; i++)
        row[i] = row_word(threads.me_world, t, (int)k, slice, i);
    uint64_t sent = row[3];
    if (t == 0 && threads.me_world == 1 && threads.plant_row) {    /* thread 0's flag alone */
        row[3] ^= 1;                /* the planted defect: one word not what it checks */
        threads.plant_row = 0;
    }
    shmem_ctx_putmem(ctx, threads.rows + place, row, sizeof row, next);
    shmem_ctx_fence(ctx);
    shmem_ctx_putmem(ctx, threads.rows_back + place, row, sizeof row, previous);
    if (slice % 2)
        shmem_ctx_pe_quiet(ctx, &next, 1);
    else
        shmem_ctx_quiet(ctx);
    shmem_ctx_getmem(ctx, back, threads.rows + place, sizeof back, next);
    row[3] = sent;
    for (int i = 0; i < ROW; i++)
        if (back[i] != row[i]) {
            pacer_failure("threads round=%ld %s row: thread %d read back word %d as %016llx, "
                          "not %016llx", slice, context_names[k], t, i,
                          (unsigned long long)back[i], (unsigned long long)row[i]);
            break;
        }

    /* The large get: a blocking get of several pieces, whose landed pieces any
     * thread of the PE may copy out into this one's buffer - one whose own
     * issue, the table's gets below among them, finds no room in the zone. */
    large_get(w, k, ctx, next, slice);

    /* The table: several nonblocking gets, one quiet. The table never changes,
     * so what lands is cleared first: a get the quiet left in flight must not
     * pass on the last slice's words. */
    uint64_t got[GETS][TABLE];
    memset(got, 0xa5, sizeof got);
    for (int g = 0; g < GETS; g++)
        shmem_ctx_getmem_nbi(ctx, got[g], threads.table + (size_t)((t + g) % threads.count) * TABLE,
                             sizeof got[g], next);
    shmem_ctx_quiet(ctx);
    for (int g = 0; g < GETS; g++) {
        int of = (t + g) % threads.count;
        for (int i = 0; i < TABLE; i++)
            if (got[g][i] != table_word(threads.next_world, of, i)) {
                pacer_failure("threads round=%ld %s table: thread %d got word %d of PE %d's row "
                              "%d as %016llx", slice, context_names[k], t, i, threads.next_world,
                              of, (unsigned long long)got[g][i]);
                g = GETS;
                break;
            }
    }

    /* Counters: fetch-adds anywhere, each remembered. */
    for (int a = 0; a < ADDS; a++) {
        uint64_t r = random_next(state);
        int target = (int)(r % (uint64_t)threads.n);
        int counter = (int)((r >> 32) % COUNTERS);
        uint64_t amount = 1 + (r >> 48) % 7;
        int pe = team_numbering ? threads.reversed_of[target] : threads.world[target];
        (void)shmem_ctx_uint64_atomic_fetch_add(ctx, threads.counters + counter, amount, pe);
        if (t == 0 && threads.me_world == 1 && threads.plant_count) {
            amount--;               /* the planted defect: one add counted short */
            threads.plant_count = 0;
        }
        w->added[(size_t)target * COUNTERS + (size_t)counter] += amount;
    }
    w->operations += (double)OPERATIONS;
    w->seconds[k] += pacer_now() - started;
}

/* ---- one slice of one worker: collectives on a team of its own --------------- */

/*
 * A team a thread splits from its own copy of the exercise's team: a
 * progression of the copy's ranks, so that the oracle knows its members without
 * asking the library.
 */
struct collective_team {
    shmem_team_t handle;            /* SHMEM_TEAM_INVALID where this PE is not a member */
    const char *shape;              /* named in a failure line */
    int start, stride, size;        /* member j is the copy's rank start + j * stride */
    int rank;                       /* this PE's place in it, or -1 */
};

/*
 * The shape of thread `thread`'s team in slice `slice`: all the PEs; all but the
 * first; the even ones; the order reversed. Every PE works it out alike, since
 * the split is a call that every PE of the copy makes. All but the first is
 * empty on one PE, and is then all.
 */
static struct collective_team shape_of(int thread, long slice, int n, int me)
{
    int shape = (int)((slice + thread) % COLLECTIVE_SHAPES);
    struct collective_team team = { SHMEM_TEAM_INVALID, "all", 0, 1, n, -1 };
    if (shape == 1 && n > 1) {
        team.shape = "all-but-first";
        team.start = 1;
        team.size = n - 1;
    } else if (shape == 2) {
        team.shape = "even";
        team.stride = 2;
        team.size = (n + 1) / 2;
    } else if (shape == 3) {
        team.shape = "reversed";
        team.start = n - 1;
        team.stride = -1;
    }
    int offset = me - team.start;
    if (offset % team.stride == 0 && offset / team.stride >= 0 && offset / team.stride < team.size)
        team.rank = offset / team.stride;
    return team;
}

/*
 * What a PE gives stage `stage` of a slice: word i is the finalized sum of a
 * base, which only the PE, the thread, the slice and the stage determine, and i.
 * A stage's words are its own, so that a piece of another stage's collective, or
 * another thread's, cannot pass for this one's.
 */
static uint64_t collective_base(int pe, int thread, long slice, int stage)
{
    return random_mix(threads.seed ^ random_name("collectives"), (uint64_t)pe,
                      ((uint64_t)thread << 48) | ((uint64_t)stage << 40) | (uint64_t)slice, 0);
}

static uint64_t collective_word(uint64_t base, size_t i)
{
    return random_finalize(base + (uint64_t)i * UINT64_C(0x9e3779b97f4a7c15));
}

/* `bytes` bytes of those words, a word's lowest byte first. */
static void collective_bytes(unsigned char *out, size_t bytes, uint64_t base)
{
    for (size_t i = 0; i < bytes; i += 8) {
        uint64_t word = collective_word(base, i / 8);
        for (size_t b = 0; b < 8 && i + b < bytes; b++)
            out[i + b] = (unsigned char)(word >> (8 * b));
    }
}

/* The planted defect, once for each kind: thread 0 of PE 1 reads a bit of the
 * first result of that kind it checks wrong. */
static int plant_result(const struct worker *w, enum collective_kind kind)
{
    if (w->index != 0 || threads.me_world != 1 || !threads.plant_collective[kind])
        return 0;
    threads.plant_collective[kind] = 0;
    return 1;
}

/*
 * One sum or scan of `count` words on `team`: the result poisoned first, so that
 * a word the call did not write cannot pass for one it did, the status checked,
 * and then every word against the sum of the members' words that it must hold -
 * all the members', those up to this rank and this rank's, or those before it.
 */
static void words_stage(struct worker *w, long slice, int stage, enum collective_kind kind,
                        const struct collective_team *team, size_t count)
{
    uint64_t mine = collective_base(threads.me_world, w->index, slice, stage);
    for (size_t i = 0; i < count; i++)
        w->source[i] = collective_word(mine, i);
    memset(w->dest, 0xa5, sizeof *w->dest * count);
    int status = kind == SUM ? shmem_uint64_sum_reduce(team->handle, w->dest, w->source, count)
                 : kind == INSCAN ? shmem_uint64_sum_inscan(team->handle, w->dest, w->source, count)
                 : shmem_uint64_sum_exscan(team->handle, w->dest, w->source, count);
    w->operations += 1;
    if (plant_result(w, kind))
        w->dest[0] ^= 1;            /* the planted defect: a bit of the result wrong */
    if (status != 0) {
        pacer_failure("threads round=%ld collectives: thread %d %s of %zu words on team %s of %d "
                      "returned %d", slice, w->index, collective_names[kind], count, team->shape,
                      team->size, status);
        return;
    }
    int last = kind == SUM ? team->size - 1 : kind == INSCAN ? team->rank : team->rank - 1;
    memset(w->expected, 0, sizeof *w->expected * count);
    for (int j = 0; j <= last; j++) {
        uint64_t base = collective_base(threads.world[team->start + j * team->stride], w->index,
                                        slice, stage);
        for (size_t i = 0; i < count; i++)
            w->expected[i] += collective_word(base, i);
    }
    for (size_t i = 0; i < count; i++)
        if (w->dest[i] != w->expected[i]) {
            pacer_failure("threads round=%ld collectives: thread %d %s of %zu words on team %s of "
                          "%d: element %zu is %016llx, expected %016llx", slice, w->index,
                          collective_names[kind], count, team->shape, team->size, i,
                          (unsigned long long)w->dest[i], (unsigned long long)w->expected[i]);
            return;
        }
}

/*
 * One broadcast of `bytes` bytes on `team`, from the rank the slice turns to:
 * every member's, the root's included, must hold the root's bytes.
 */
static void broadcast_stage(struct worker *w, long slice, int stage,
                            const struct collective_team *team, size_t bytes)
{
    int root = (int)(slice % team->size);
    collective_bytes(w->broadcast_source, bytes,
                     collective_base(threads.me_world, w->index, slice, stage));
    memset(w->broadcast_dest, 0xee, bytes);
    int status = shmem_broadcastmem(team->handle, w->broadcast_dest, w->broadcast_source, bytes,
                                    root);
    w->operations += 1;
    if (plant_result(w, BROADCAST))
        w->broadcast_dest[0] ^= 1;  /* the planted defect: a bit of the result wrong */
    if (status != 0) {
        pacer_failure("threads round=%ld collectives: thread %d broadcast of %zu bytes on team %s "
                      "of %d returned %d", slice, w->index, bytes, team->shape, team->size, status);
        return;
    }
    unsigned char *expected = (unsigned char *)w->expected;
    int root_pe = threads.world[team->start + root * team->stride];
    collective_bytes(expected, bytes, collective_base(root_pe, w->index, slice, stage));
    for (size_t i = 0; i < bytes; i++)
        if (w->broadcast_dest[i] != expected[i]) {
            pacer_failure("threads round=%ld collectives: thread %d broadcast of %zu bytes on team "
                          "%s of %d from rank %d: byte %zu is %02x, expected %02x", slice,
                          w->index, bytes, team->shape, team->size, root, i, w->broadcast_dest[i],
                          expected[i]);
            return;
        }
}

/*
 * The slice's collectives: a split of the thread's copy into a team of this
 * slice's shape; on it a sum, an inclusive scan, a broadcast and an exclusive
 * scan; on the copy a large sum and a large inclusive and exclusive scan; a
 * second sum on the team; the team's destroy. So the thread's one lane serves
 * reductions, broadcasts and scans of two teams in turn while the PE's other
 * threads use theirs. The team's two sizes turn with the slice and the thread
 * through collective_sizes, two places apart, so that a thread's first and
 * second are on either side of the class line in turn. The copy's are always
 * large, and every thread's copy is the same team: in every slice the PE's
 * threads have large scans of one tree in flight at once. The teams' scans, whose
 * shapes differ from thread to thread, meet by chance, and a scan through a lane
 * that another thread's scan shares fails only when they do (T25: the teams'
 * scans alone missed that mutant in 3 runs of 25).
 */
static void collectives_round(struct worker *w, long slice)
{
    double started = pacer_now();
    int n = threads.n;
    struct collective_team team = shape_of(w->index, slice, n, threads.me);
    size_t first = collective_sizes[(slice + w->index) % COLLECTIVE_SIZES];
    size_t second = collective_sizes[(slice + w->index + 2) % COLLECTIVE_SIZES];

    int status = shmem_team_split_strided(w->parent, team.start, team.stride, team.size, NULL, 0,
                                          &team.handle);
    w->operations += 1;
    int usable = 0;
    if (status != 0) {
        pacer_failure("threads round=%ld collectives: thread %d split of team %s returned %d",
                      slice, w->index, team.shape, status);
    } else if ((team.handle != SHMEM_TEAM_INVALID) != (team.rank >= 0)) {
        pacer_failure("threads round=%ld collectives: thread %d split of team %s %s", slice,
                      w->index, team.shape, team.rank >= 0 ? "gave this member no team"
                                                           : "gave a team to a PE outside it");
    } else if (team.rank >= 0) {
        usable = 1;
        int size = shmem_team_n_pes(team.handle), rank = shmem_team_my_pe(team.handle);
        if (size != team.size || rank != team.rank)
            pacer_failure("threads round=%ld collectives: thread %d team %s: team_n_pes %d and "
                          "team_my_pe %d, expected %d and %d", slice, w->index, team.shape, size,
                          rank, team.size, team.rank);
    }
    if (usable) {
        words_stage(w, slice, 0, SUM, &team, first);
        words_stage(w, slice, 1, INSCAN, &team, second);
        broadcast_stage(w, slice, 2, &team, second * 8 - 3);
        words_stage(w, slice, 3, EXSCAN, &team, first);
    }
    struct collective_team copy = { w->parent, "copy", 0, 1, n, threads.me };
    words_stage(w, slice, 4, SUM, &copy, 2048 + (size_t)(slice % 3));
    words_stage(w, slice, 5, INSCAN, &copy, COLLECTIVE_WORDS);
    words_stage(w, slice, 6, EXSCAN, &copy, 2048 + (size_t)((slice + 1) % 3));
    if (usable)
        words_stage(w, slice, 7, SUM, &team, second);
    if (team.handle != SHMEM_TEAM_INVALID) {
        shmem_team_destroy(team.handle);
        w->operations += 1;
    }
    w->seconds[COLLECTIVES_PHASE] += pacer_now() - started;
}

static void slice_of(struct worker *w, long slice)
{
    uint64_t state = random_mix(threads.seed, random_name("threads"),
                                (uint64_t)threads.me_world, ((uint64_t)w->index << 40) | (uint64_t)slice);
    shmem_ctx_session_config_t config = { .total_ops = OPERATIONS };
    shmem_ctx_session_start(w->own[0], SHMEM_CTX_SESSION_BATCH, &config,
                            SHMEM_CTX_SESSION_TOTAL_OPS);
    on_context(w, PRIVATE, w->own[0], slice, &state);
    shmem_ctx_session_stop(w->own[0]);
    on_context(w, SERIALIZED, w->own[1], slice, &state);
    on_context(w, DEFAULT, SHMEM_CTX_DEFAULT, slice, &state);
    on_context(w, TEAM, threads.team_context, slice, &state);
    collectives_round(w, slice);
}

static void *worker_main(void *argument)
{
    struct worker *w = argument;
    int created = shmem_ctx_create(SHMEM_CTX_PRIVATE, &w->own[0]) == 0;
    created = created && shmem_ctx_create(SHMEM_CTX_SERIALIZED, &w->own[1]) == 0;
    long seen = 0;
    pthread_mutex_lock(&threads.lock);
    if (!created)
        w->failed_to_start = 1;
    for (;;) {
        while (threads.generation == seen && !threads.stop)
            pthread_cond_wait(&threads.start, &threads.lock);
        if (threads.stop)
            break;
        seen = threads.generation;
        long first = threads.slice, units = threads.units;
        pthread_mutex_unlock(&threads.lock);
        if (!w->failed_to_start)
            for (long u = 0; u < units; u++)
                slice_of(w, first + u);
        pthread_mutex_lock(&threads.lock);
        if (++threads.finished == threads.count)
            pthread_cond_signal(&threads.done);
    }
    pthread_mutex_unlock(&threads.lock);
    if (created) {
        shmem_ctx_destroy(w->own[1]);
        shmem_ctx_destroy(w->own[0]);
    }
    return NULL;
}

/* ---- the exercise ------------------------------------------------------------ */

static int threads_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    int n = shmem_team_n_pes(team), t = threads.count;
    threads.team = team;
    threads.n = n;
    threads.me = shmem_team_my_pe(team);
    threads.me_world = shmem_my_pe();
    threads.next = (threads.me + 1) % n;
    threads.previous = (threads.me + n - 1) % n;
    if (shmem_team_split_strided(team, n - 1, -1, n, NULL, 0, &threads.reversed) != 0 ||
        shmem_team_create_ctx(threads.reversed, 0, &threads.team_context) != 0)
        return -1;
    threads.world = malloc(sizeof *threads.world * (size_t)n);
    threads.reversed_of = malloc(sizeof *threads.reversed_of * (size_t)n);
    threads.added = calloc((size_t)n * COUNTERS, sizeof *threads.added);
    threads.workers = calloc((size_t)t, sizeof *threads.workers);
    if (!threads.world || !threads.reversed_of || !threads.added || !threads.workers)
        return -1;
    for (int j = 0; j < n; j++) {
        threads.world[j] = shmem_team_translate_pe(team, j, SHMEM_TEAM_WORLD);
        threads.reversed_of[j] = shmem_team_translate_pe(team, j, threads.reversed);
    }
    threads.next_world = threads.world[threads.next];
    threads.previous_world = threads.world[threads.previous];
    threads.next_reversed = threads.reversed_of[threads.next];
    threads.previous_reversed = threads.reversed_of[threads.previous];

    size_t rows = (size_t)t * CONTEXTS * ROW;
    threads.rows = shmem_calloc(rows, sizeof *threads.rows);
    threads.rows_back = shmem_calloc(rows, sizeof *threads.rows_back);
    threads.table = shmem_malloc(sizeof *threads.table * (size_t)t * TABLE);
    size_t large_words = LARGE_WORDS + (size_t)t * LARGE_STARTS;
    threads.large = shmem_malloc(sizeof *threads.large * large_words);
    threads.large_expected = malloc(sizeof *threads.large_expected * large_words);
    threads.counters = shmem_calloc(COUNTERS, sizeof *threads.counters);
    threads.reduce_source = shmem_malloc(sizeof *threads.reduce_source * (size_t)n * COUNTERS);
    threads.reduce_total = shmem_malloc(sizeof *threads.reduce_total * (size_t)n * COUNTERS);
    if (!threads.rows || !threads.rows_back || !threads.table || !threads.large ||
        !threads.large_expected || !threads.counters || !threads.reduce_source ||
        !threads.reduce_total)
        return -1;
    for (int th = 0; th < t; th++)
        for (int i = 0; i < TABLE; i++)
            threads.table[(size_t)th * TABLE + i] = table_word(threads.me_world, th, i);
    if (threads.plant_table && threads.me_world == 1)
        threads.table[5] ^= 1;      /* the planted defect: one word of the table */
    for (size_t i = 0; i < large_words; i++) {
        threads.large[i] = large_word(threads.me_world, i);
        threads.large_expected[i] = large_word(threads.next_world, i);
    }
    if (threads.plant_pieces && threads.me_world == 1)
        threads.large[LARGE_WORDS - 1] ^= 1;    /* the planted defect: a word at the end of
                                                 * every large get's last piece */
    /* Each thread's collectives: buffers of its own, and its own copy of the
     * team to split. Two threads may not make calls on one team at once, so each
     * splits a copy, made here with the symmetric allocations, on the main
     * thread. */
    for (int th = 0; th < t; th++) {
        struct worker *w = &threads.workers[th];
        w->source = shmem_malloc(sizeof *w->source * COLLECTIVE_WORDS);
        w->dest = shmem_malloc(sizeof *w->dest * COLLECTIVE_WORDS);
        w->broadcast_source = shmem_malloc(8 * COLLECTIVE_WORDS);
        w->broadcast_dest = shmem_malloc(8 * COLLECTIVE_WORDS);
        w->expected = malloc(sizeof *w->expected * COLLECTIVE_WORDS);
        if (!w->source || !w->dest || !w->broadcast_source || !w->broadcast_dest || !w->expected ||
            shmem_team_split_strided(team, 0, 1, n, NULL, 0, &w->parent) != 0)
            return -1;
    }
    shmem_quiet();
    shmem_team_sync(team);          /* every array written before any worker reads one */

    pthread_mutex_init(&threads.lock, NULL);
    pthread_cond_init(&threads.start, NULL);
    pthread_cond_init(&threads.done, NULL);
    threads.generation = threads.slice = 0;
    threads.stop = 0;
    for (int th = 0; th < t; th++) {
        struct worker *w = &threads.workers[th];
        w->index = th;
        w->added = calloc((size_t)n * COUNTERS, sizeof *w->added);
        w->large_buffer = malloc(sizeof *w->large_buffer * LARGE_WORDS);
        if (!w->added || !w->large_buffer || pthread_create(&w->thread, NULL, worker_main, w) != 0)
            return -1;
    }
    return 0;
}

static void threads_step(long units, int last)
{
    (void)last;
    pthread_mutex_lock(&threads.lock);
    threads.units = units;
    threads.finished = 0;
    threads.generation++;
    pthread_cond_broadcast(&threads.start);
    while (threads.finished < threads.count)
        pthread_cond_wait(&threads.done, &threads.lock);
    threads.slice += units;
    pthread_mutex_unlock(&threads.lock);
    for (int th = 0; th < threads.count; th++)
        if (threads.workers[th].failed_to_start) {
            pacer_failure("threads: thread %d could not create its contexts", th);
            threads.workers[th].failed_to_start = 0;
        }
}

/* With the workers parked: the counters against the sums over the team of
 * what every thread added to each. */
static void threads_checkpoint(struct pacer_counters *counters)
{
    int n = threads.n;
    size_t cells = (size_t)n * COUNTERS;
    double seconds[PHASES] = { 0 }, operations = 0;
    for (int th = 0; th < threads.count; th++) {
        struct worker *w = &threads.workers[th];
        for (size_t c = 0; c < cells; c++) {
            threads.added[c] += w->added[c];
            w->added[c] = 0;
        }
        for (int k = 0; k < PHASES; k++) {
            seconds[k] += w->seconds[k];
            w->seconds[k] = 0;
        }
        operations += w->operations;
        w->operations = 0;
    }
    memcpy(threads.reduce_source, threads.added, sizeof *threads.added * cells);
    shmem_uint64_sum_reduce(threads.team, threads.reduce_total, threads.reduce_source, cells);
    for (int k = 0; k < COUNTERS; k++) {
        uint64_t want = threads.reduce_total[(size_t)threads.me * COUNTERS + (size_t)k];
        uint64_t have = shmem_uint64_atomic_fetch(threads.counters + k, threads.me_world);
        if (have != want)
            pacer_failure("threads round=%ld counter %d: holds %llu, and %llu were added",
                          threads.slice - 1, k, (unsigned long long)have,
                          (unsigned long long)want);
    }
    counters->work = operations;
    for (int k = 0; k < PHASES; k++)
        counters->phase_seconds[k] = seconds[k];
}

static void threads_teardown(void)
{
    pthread_mutex_lock(&threads.lock);
    threads.stop = 1;
    pthread_cond_broadcast(&threads.start);
    pthread_mutex_unlock(&threads.lock);
    for (int th = 0; th < threads.count; th++) {
        pthread_join(threads.workers[th].thread, NULL);
        free(threads.workers[th].expected);
        free(threads.workers[th].large_buffer);
        free(threads.workers[th].added);
    }
    pthread_cond_destroy(&threads.done);
    pthread_cond_destroy(&threads.start);
    pthread_mutex_destroy(&threads.lock);
    shmem_quiet();
    shmem_team_sync(threads.team);
    shmem_ctx_destroy(threads.team_context);
    shmem_team_destroy(threads.reversed);
    for (int th = 0; th < threads.count; th++)
        shmem_team_destroy(threads.workers[th].parent);
    for (int th = threads.count; th-- > 0;) {
        struct worker *w = &threads.workers[th];
        shmem_free(w->broadcast_dest);
        shmem_free(w->broadcast_source);
        shmem_free(w->dest);
        shmem_free(w->source);
    }
    shmem_free(threads.reduce_total);
    shmem_free(threads.reduce_source);
    shmem_free(threads.counters);
    shmem_free(threads.large);
    shmem_free(threads.table);
    shmem_free(threads.rows_back);
    shmem_free(threads.rows);
    free(threads.workers);
    free(threads.large_expected);
    free(threads.added);
    free(threads.reversed_of);
    free(threads.world);
}

const struct pacer_exercise pacer_threads = {
    .name = "threads",
    .work = "operations",
    .phases = phase_names,
    .quick_rounds = 8,
    .multiple = 1,
    .memory = threads_memory,
    .setup = threads_setup,
    .step = threads_step,
    .checkpoint = threads_checkpoint,
    .teardown = threads_teardown,
};
