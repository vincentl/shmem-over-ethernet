/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * relay - a quiet's promise, kept to a third node (T26; the release review's
 * pacer paragraph, docs/reviews/archive/phase1-release-correctness.md).
 *
 * The review planted eight defects in the library, each in a copy of its own,
 * and the pacer's oracles caught seven. The eighth, a shmem_quiet that skips the
 * last connection's acknowledgement, passed --quick on four maps and ninety
 * seconds of mix, because in every other exercise the step that follows a quiet
 * travels on the same ordered stream as the data it follows - the sync, the
 * flag, the atomic, the lock's release - and cannot overtake it. Only a reader
 * that reaches the data by another path can see a quiet return early, and this
 * exercise is that reader.
 *
 * **A round** takes three nodes, a, b and c - the writer's, the block's home and
 * the reader's - and relays a block between a PE of each (lanes, below):
 *
 *   a   puts a block of 4 MiB to b - sixty-four of the default 64 KiB messages,
 *       eight times the two buffers its connection to b holds - then calls
 *       shmem_quiet, or shmem_pe_quiet naming b, and sets a flag at c with an
 *       atomic set
 *   c   waits for the flag, and gets the block from b - the last words of it
 *       first, since they are the last to land - by shmem_getmem, or by two
 *       shmem_getmem_nbi and a quiet, into a buffer of its own that the round
 *       poisoned, and checks every word
 *   b   does nothing until the round is over, and then checks its own block
 *
 * a's data reaches b by the connection of the pair a and b, and the flag reaches
 * c by the connection of the pair a and c, so that nothing but the quiet orders
 * the one before the other; and c's get reaches b by a third. A quiet that
 * returns before b has acknowledged the block lets the flag arrive first, and c
 * reads a block whose last words are still the previous round's. The block's
 * words are base + word * STEP, the base drawn from the seed, the round and the
 * writer, so that each round's differs from every other's in every word, and a
 * stale word is known for what it is: the failure line says how many of the
 * wrong words are what the block held before.
 *
 * **The rotation.** Round r's nodes are the r-th ordered triple of different
 * nodes (triple, below), the writer's node turning first: in any N rounds each
 * node writes once, is the home once and reads once, and in N * (N - 1) * (N - 2)
 * rounds - a cycle - every ordered triple comes up once: six rounds on three
 * nodes, whose map is 0,1,2. The forms of the quiet and of the get turn once a
 * cycle, so that each triple meets each of the four.
 *
 * **Lanes.** A node with more PEs than one gives each a lane: the PE of rank k
 * on a, the one of rank k on b and the one on c are a relay, if all three nodes
 * have a rank k, and all the lanes relay at once over the connections the nodes
 * share. A PE with no lane in a round has no part in it but the syncs.
 *
 * **Two syncs a round**: one before it, so that no PE is still reading the last
 * round's block or flag, and none is poisoned, filled or waiting early; one
 * after it, so that the home may read what the round left in its block. A
 * library that keeps a quiet's promise has delivered the block by the second -
 * the writer quieted before it - which is what the home checks.
 *
 * It needs three nodes at least (exercise.h, nodes_needed), is not in the
 * default set, and runs on the world alone: it has no rebind, and mix does not
 * choose it.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum {
    BLOCK_BYTES = 4 << 20,          /* the block, lowered to fit --memory */
    LEAST_BLOCK_BYTES = 256 << 10,  /* four default messages: less is no relay */
    TAIL_WORDS = 4096,              /* what c reads first: the last 32 KiB */
    POISON = 0xa5,
    FORMS = 4                       /* the quiet's two spellings and the get's two */
};
#define STEP UINT64_C(0x9e3779b97f4a7c15)

enum role { IDLE, WRITER, HOME, READER };
enum { PHASE_PUT, PHASE_WAIT, PHASE_GET, PHASE_CHECK, PHASES_USED };
static const char *const phase_names[] = { "put", "wait", "get", "check", NULL };

static struct {
    uint64_t seed;
    int plant_block, plant_stale, plant_quiet;
    shmem_team_t team;
    int me;
    int nodes, lanes;               /* the job's nodes, and the most PEs a node has */
    long cycle;                     /* rounds to an ordered triple of nodes each */
    int *world_of;                  /* [nodes][lanes]: the PE of a rank on a node, or -1 */
    size_t words;                   /* the block's size, in words */
    /* symmetric */
    uint64_t *block;                /* [words]: this PE's, which others put to */
    uint64_t *flag;                 /* [1]: the token of the round whose block is there */
    /* private */
    uint64_t *source;               /* [words]: what this PE puts when it writes */
    uint64_t *fetched;              /* [words]: what this PE gets when it reads */
    long round;
    double relays, bytes, phase[PHASES_USED];
} relay;

static uint64_t relay_memory(int npes, const struct pacer_options *options, char *fields,
                             size_t size)
{
    (void)npes;
    const struct pacer_layout *layout = pacer_layout();
    relay.nodes = layout->nodes;
    relay.lanes = 1;
    for (int p = 0; p < layout->pes; p++)
        if (layout->local[p] + 1 > relay.lanes)
            relay.lanes = layout->local[p] + 1;
    relay.cycle = (long)relay.nodes * (relay.nodes - 1) * (relay.nodes - 2);
    /* This PE's block is symmetric; the block it puts and the one it gets are
     * private, beside it: within the budget each. */
    uint64_t block = BLOCK_BYTES;
    while (block > LEAST_BLOCK_BYTES && 2 * block + 64 > options->memory)
        block /= 2;
    if (2 * block + 64 > options->memory) {
        snprintf(fields, size, "a block of %llu bytes, three times over, needs %llu bytes",
                 (unsigned long long)block, (unsigned long long)(3 * block + 64));
        return UINT64_MAX;
    }
    relay.words = (size_t)(block / 8);
    relay.seed = options->seed;
    relay.plant_block = options->plant == PACER_PLANT_RELAY_BLOCK;
    relay.plant_stale = options->plant == PACER_PLANT_RELAY_STALE;
    relay.plant_quiet = options->plant == PACER_PLANT_RELAY_QUIET;
    char block_text[32];
    pacer_size(block_text, sizeof block_text, block);
    snprintf(fields, size, "block=%s nodes=%d lanes=%d cycle=%ld", block_text, relay.nodes,
             relay.lanes, relay.cycle);
    return block + 8;
}

/* What a block is made of: word w of the round's writer's is base + w * STEP,
 * the base from the seed, the round and the writer. Round -1 is the block as
 * setup left it, the home's own. */
static uint64_t block_base(long round, int writer)
{
    return random_mix(relay.seed ^ random_name("relay"), (uint64_t)round, (uint64_t)writer, 0);
}

/*
 * Round r's nodes, an ordered triple of different ones: the writer's node turns
 * through all N in N rounds, and each turn takes the next pair of gaps, b = a +
 * d1 and c = a + d2 mod N, of the (N - 1) * (N - 2) there are - so that the
 * first N rounds already make each node the writer, the home and the reader.
 */
static void triple(long round, int *a, int *b, int *c)
{
    int nodes = relay.nodes;
    int pairs = (nodes - 1) * (nodes - 2);
    int pair = (int)((round / nodes) % pairs);
    int first = 1 + pair / (nodes - 2), second = 1 + pair % (nodes - 2);
    if (second >= first)
        second++;
    *a = (int)(round % nodes);
    *b = (*a + first) % nodes;
    *c = (*a + second) % nodes;
}

/* Whether `pe` has a part in `round`, and the PEs of the lane it is in. */
static enum role role_in_round(long round, int pe, int *writer, int *home, int *reader)
{
    const struct pacer_layout *layout = pacer_layout();
    int a, b, c, lane = layout->local[pe], node = layout->node[pe];
    triple(round, &a, &b, &c);
    int w = relay.world_of[a * relay.lanes + lane];
    int h = relay.world_of[b * relay.lanes + lane];
    int r = relay.world_of[c * relay.lanes + lane];
    if (w < 0 || h < 0 || r < 0)
        return IDLE;
    if (writer)
        *writer = w;
    if (home)
        *home = h;
    if (reader)
        *reader = r;
    return node == a ? WRITER : node == b ? HOME : node == c ? READER : IDLE;
}

/* The base of the words `home`'s block held before `round`: those of the last
 * round that made it the home, within a cycle, or else the ones setup left. */
static uint64_t previous_base(long round, int home)
{
    for (long earlier = round - 1; earlier >= 0 && earlier >= round - relay.cycle; earlier--) {
        int writer = -1;
        if (role_in_round(earlier, home, &writer, NULL, NULL) == HOME)
            return block_base(earlier, writer);
    }
    return block_base(-1, home);
}

static void fill(uint64_t *words, uint64_t base)
{
    for (size_t w = 0; w < relay.words; w++)
        words[w] = base + w * STEP;
}

static int relay_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    const struct pacer_layout *layout = pacer_layout();
    relay.team = team;
    relay.me = shmem_my_pe();
    relay.world_of = malloc(sizeof *relay.world_of * (size_t)relay.nodes * (size_t)relay.lanes);
    relay.source = malloc(relay.words * sizeof *relay.source);
    relay.fetched = malloc(relay.words * sizeof *relay.fetched);
    relay.block = shmem_calloc(relay.words, sizeof *relay.block);
    relay.flag = shmem_calloc(1, sizeof *relay.flag);
    if (!relay.world_of || !relay.source || !relay.fetched || !relay.block || !relay.flag)
        return -1;
    for (int k = 0; k < relay.nodes * relay.lanes; k++)
        relay.world_of[k] = -1;
    for (int p = 0; p < layout->pes; p++)
        relay.world_of[layout->node[p] * relay.lanes + layout->local[p]] = p;
    fill(relay.block, block_base(-1, relay.me));
    relay.round = 0;
    shmem_team_sync(team);
    return 0;
}

/* How words differ from the pattern they should hold: how many, the first and
 * the last, and how many are what the block held before `round` made `home` its
 * home - looked up once, at the first wrong word, since a run that finds none
 * has no use for it. */
struct difference {
    size_t wrong, first, last, stale;
};

static struct difference compare(const uint64_t *have, uint64_t base, long round, int home)
{
    struct difference d = { 0, 0, 0, 0 };
    uint64_t old_base = 0;
    for (size_t w = 0; w < relay.words; w++) {
        uint64_t want = base + w * STEP;
        if (have[w] == want)
            continue;
        if (d.wrong++ == 0) {
            d.first = w;
            old_base = previous_base(round, home);
        }
        d.last = w;
        d.stale += have[w] == old_base + w * STEP;
    }
    return d;
}

/* Round r's forms: the quiet and the get, turning once a cycle. */
static int quiet_by_pe(long round)
{
    return (int)((round / relay.cycle) % FORMS) % 2;
}

static int get_nonblocking(long round)
{
    return (int)((round / relay.cycle) % FORMS) / 2;
}

static void write_block(long round, int home, int reader)
{
    double started = pacer_now();
    size_t bytes = relay.words * sizeof *relay.block;
    shmem_putmem(relay.block, relay.source, bytes, home);
    if (relay.plant_quiet && relay.me == 1) {
        /* The planted defect, every time PE 1 writes: no quiet at all, which is
         * what a quiet that returns early comes to when only one connection
         * has data to wait for. */
    } else if (quiet_by_pe(round)) {
        shmem_pe_quiet(&home, 1);
    } else {
        shmem_quiet();
    }
    shmem_uint64_atomic_set(relay.flag, (uint64_t)round + 1, reader);
    relay.phase[PHASE_PUT] += pacer_now() - started;
    relay.bytes += (double)bytes;
}

static void read_block(long round, int writer, int home)
{
    size_t tail = TAIL_WORDS, head = relay.words - tail;
    double started = pacer_now();
    shmem_uint64_wait_until(relay.flag, SHMEM_CMP_EQ, (uint64_t)round + 1);
    relay.phase[PHASE_WAIT] += pacer_now() - started;

    /* The end of the block first: the data lands in order, so that the words a
     * quiet that returned early would leave behind are the last. */
    started = pacer_now();
    if (get_nonblocking(round)) {
        shmem_getmem_nbi(relay.fetched + head, relay.block + head, tail * sizeof *relay.block,
                         home);
        shmem_getmem_nbi(relay.fetched, relay.block, head * sizeof *relay.block, home);
        shmem_quiet();
    } else {
        shmem_getmem(relay.fetched + head, relay.block + head, tail * sizeof *relay.block, home);
        shmem_getmem(relay.fetched, relay.block, head * sizeof *relay.block, home);
    }
    relay.phase[PHASE_GET] += pacer_now() - started;
    relay.bytes += (double)(relay.words * sizeof *relay.block);

    started = pacer_now();
    if (relay.plant_block && relay.me == 1) {
        relay.fetched[relay.words - 1] ^= 1;    /* the planted defect: the last word misread */
        relay.plant_block = 0;
    }
    struct difference d = compare(relay.fetched, block_base(round, writer), round, home);
    if (d.wrong)
        pacer_failure("relay round=%ld get: %zu of %zu words of PE %d's block, put by PE %d, are "
                      "wrong when PE %d's flag said it had landed. the first, word %zu, is "
                      "%016llx, not %016llx. the last wrong word is %zu. %zu of them are what "
                      "the block held before",
                      round, d.wrong, relay.words, home, writer, relay.me, d.first,
                      (unsigned long long)relay.fetched[d.first],
                      (unsigned long long)(block_base(round, writer) + d.first * STEP), d.last,
                      d.stale);
    relay.relays++;
    relay.phase[PHASE_CHECK] += pacer_now() - started;
}

static void check_block(long round, int writer)
{
    double started = pacer_now();
    struct difference d = compare(relay.block, block_base(round, writer), round, relay.me);
    if (d.wrong)
        pacer_failure("relay round=%ld put: %zu of %zu words of the block PE %d put to PE %d "
                      "are wrong when the round is over. the first, word %zu, is %016llx, not "
                      "%016llx. the last wrong word is %zu. %zu of them are what the block "
                      "held before",
                      round, d.wrong, relay.words, writer, relay.me, d.first,
                      (unsigned long long)relay.block[d.first],
                      (unsigned long long)(block_base(round, writer) + d.first * STEP), d.last,
                      d.stale);
    relay.phase[PHASE_CHECK] += pacer_now() - started;
}

static void relay_round(long round)
{
    int writer = -1, home = -1, reader = -1;
    enum role role = role_in_round(round, relay.me, &writer, &home, &reader);

    /* The round's own work before anyone starts, off the clock of the relay: the
     * writer's block, and the reader's buffer made wrong everywhere. */
    if (role == WRITER) {
        fill(relay.source, block_base(round, relay.me));
        if (relay.plant_stale && relay.me == 1) {
            /* The planted defect, once: the last quarter of the block put as the
             * block held it before, as a quiet that returned early leaves it. */
            uint64_t old_base = previous_base(round, home);
            for (size_t w = relay.words - relay.words / 4; w < relay.words; w++)
                relay.source[w] = old_base + w * STEP;
            relay.plant_stale = 0;
        }
    } else if (role == READER) {
        memset(relay.fetched, POISON, relay.words * sizeof *relay.fetched);
    }
    shmem_team_sync(relay.team);

    if (role == WRITER)
        write_block(round, home, reader);
    else if (role == READER)
        read_block(round, writer, home);
    shmem_team_sync(relay.team);

    if (role == HOME)
        check_block(round, writer);
}

static void relay_step(long units, int last)
{
    (void)last;
    for (long u = 0; u < units; u++)
        relay_round(relay.round++);
}

static void relay_checkpoint(struct pacer_counters *counters)
{
    counters->work = relay.relays;
    counters->bytes = relay.bytes;
    for (int k = 0; k < PHASES_USED; k++)
        counters->phase_seconds[k] = relay.phase[k];
    relay.relays = relay.bytes = 0;
    memset(relay.phase, 0, sizeof relay.phase);
}

static void relay_teardown(void)
{
    shmem_team_sync(relay.team);
    shmem_free(relay.flag);
    shmem_free(relay.block);
    free(relay.fetched);
    free(relay.source);
    free(relay.world_of);
}

const struct pacer_exercise pacer_relay = {
    .name = "relay",
    .work = "relays",
    .phases = phase_names,
    .quick_rounds = 72,             /* twelve cycles of three nodes' triples: three of each form */
    .nodes_needed = 3,
    .memory = relay_memory,
    .setup = relay_setup,
    .step = relay_step,
    .checkpoint = relay_checkpoint,
    .teardown = relay_teardown,
};
