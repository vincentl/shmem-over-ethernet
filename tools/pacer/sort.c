/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * sort - a streaming bucket sort, ISx's exchange (proposal §4.1; the brief's
 * decision 7): every round, every PE generates R keys, sends each to the PE
 * that owns its share of the 64-bit key space, and checks what it received.
 *
 * **Ownership is node-major.** The team's PEs are put in node order - nodes in
 * the order of their leads' world ranks, then each node's PEs by local rank -
 * and the PE at index (key N) >> 64 of that order, a 128-bit product, owns the
 * key. So a node's keys are one contiguous range whatever Slurm's
 * distribution was; the 0,1,0,1 map is where that shows.
 *
 * **Keys** come from splitmix64, seeded by a mix of (seed, "sort", PE,
 * round); the generating PE adds each to its count, sum and xor as it goes.
 *
 * **The push exchange** (--exchange fetch-add, ISx's): keys go into private
 * staging, a bin a destination, each bin R/N plus an eighth plus 64 keys; a
 * bin that fills is sent early and refilled, so no key is dropped and the
 * slack stays small. Sending a bin is a shmem_uint64_atomic_fetch_add on the
 * destination's cursor - both cursors are one shmem_malloc_with_hints with
 * SHMEM_MALLOC_ATOMICS_REMOTE - then the keys at the offset it returned:
 * plain stores through shmem_ptr to a node-mate (unless --no-ptr), a
 * shmem_putmem_nbi otherwise, or a blocking put for an early send, whose bin
 * is refilled at once. A reservation past the receive capacity is a failure,
 * never a write; the capacity is the expectation plus an eighth plus 4,096
 * keys. After every bin, shmem_quiet - which completes stores through
 * shmem_ptr too (§9.12) - then shmem_team_sync.
 *
 * **--sort rounds**, bounded memory for an unbounded run: two receive
 * buffers and two cursors, alternated by round parity, so that one sync a
 * round suffices. After round r's sync each PE checks buffer r % 2 and resets
 * its cursor with shmem_uint64_atomic_set on itself - never a plain store to a
 * word others update atomically - while round r + 1 fills the other buffer.
 * No sender can reach round r + 2's reservation before round r + 1's sync,
 * which every PE enters only after its reset, and after the quiet that
 * completes it.
 *
 * **--sort accumulate**, the large-memory run: one buffer, filled round after
 * round with the cursor never reset, until the next round could overflow the
 * budget's capacity; then everything is sorted and checked at once, the
 * cursor reset, a quiet and a sync, and the next cycle starts. The sort is in
 * place - an MSD radix sort of 8-bit digits - so that the budget holds keys
 * rather than a scratch copy of them. Within a cycle nothing is checked: a
 * fast PE's next round may already be reserving on a PE's cursor, so no
 * round's own end can be read from it.
 *
 * **The pull exchange** (--exchange alltoall): staging is symmetric, the bins
 * contiguous in destination order, sized exactly - a first pass over the
 * round's keys counts them, a second, regenerating the same stream, places
 * them - so its generate time is two passes. A shmem_uint64_alltoall of one
 * {count, offset} a pair tells every receiver where each sender's bin for it
 * lies; the receiver gets them all with shmem_getmem_nbi at the running sum of
 * the counts, one shmem_quiet, checks, and a team sync keeps every sender
 * from refilling its staging while another PE may still be reading it.
 *
 * **The checks.** A batch: every key in this PE's range; its count, sum and
 * xor accumulated as it arrived; sorted - LSD radix with 16-bit digits into a
 * scratch in rounds mode - and non-decreasing afterwards, with the count, sum
 * and xor taken again so that a fault of this sort's own shows as the tool's.
 * **The oracle**, at every checkpoint: the sums over the team of the
 * generated counts and sums (shmem_uint64_sum_reduce) and the xor of their
 * xors (shmem_uint64_xor_reduce: a sum of per-PE xors is not the same for two
 * partitions of one set of keys) equal those received, or a failure line
 * says which differed. In rounds mode every round closes within its step; in
 * accumulate mode a cycle's keys join the oracle when the cycle closes.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { CHUNK = 4096, MINIMUM_KEYS = 1024, RADIX = 65536, SMALL_SORT = 48 };
enum phase { GENERATE, BIN, EXCHANGE, SORT, CHECK };
static const char *const phase_names[] = { "generate", "bin", "exchange", "sort", "check", NULL };

struct triple {
    uint64_t count, sum, xor;
};

static void triple_add(struct triple *t, uint64_t key)
{
    t->count++;
    t->sum += key;
    t->xor ^= key;
}

static struct {
    /* the plan, from memory() */
    uint64_t keys;                  /* R, a PE a round */
    uint64_t capacity;              /* keys a receive buffer holds */
    uint64_t bin_capacity;          /* push: keys a bin holds */
    long cycle;                     /* accumulate: rounds a cycle */
    int accumulate, push, use_ptr;
    uint64_t seed;
    int plant_key, plant_order, plant_range, plant_capacity;
    /* the team */
    shmem_team_t team;
    int n, me, me_world, my_index;
    int *world;                     /* team rank to world PE */
    int *order;                     /* node-major index to team rank */
    int *index_of;                  /* team rank to node-major index */
    /* buffers */
    uint64_t *receive[2];           /* push: symmetric; pull: private, one */
    uint64_t *cursors;              /* push: symmetric, two */
    uint64_t *staging;              /* push: private bins; pull: symmetric */
    uint64_t *fill;                 /* push: a bin's keys; pull: a destination's count */
    uint64_t *next;                 /* pull: where a destination's next key goes */
    uint64_t *pairs_out, *pairs_in; /* pull: symmetric, {count, offset} a pair */
    uint64_t *scratch;              /* rounds: the LSD sort's */
    size_t (*digits)[RADIX];        /* rounds: the LSD sort's four histograms */
    uint64_t *chunk;
    uint64_t *reduce_source, *reduce_result;   /* symmetric: 4 sums, then 2 xors */
    /* progress */
    long round;                     /* rounds done */
    long cycle_rounds;              /* accumulate: rounds in the open cycle */
    uint64_t received;              /* pull accumulate: keys in the buffer */
    struct triple open_generated;   /* accumulate: this cycle's, not yet checked */
    struct triple generated, arrived;   /* closed, since the checkpoint */
    double work, batch;
    double phase[5];
    double send_seconds;            /* inside the binning loop */
} sort;

/* ---- the plan ------------------------------------------------------------ */

static uint64_t sort_memory(int npes, const struct pacer_options *options, char *fields,
                            size_t size)
{
    uint64_t n = (uint64_t)npes, budget = options->memory;
    int accumulate = options->sort_mode == PACER_SORT_ACCUMULATE;
    int push = options->exchange == PACER_EXCHANGE_FETCH_ADD;
    long fixed_rounds = options->rounds ? options->rounds : options->quick ? 3 : 0;
    uint64_t asked = options->quick ? 65536 : accumulate ? UINT64_C(1) << 20 : UINT64_C(4) << 20;
    uint64_t small = 8 * 8 + (push ? 16 : 2 * 2 * n * 8);
    uint64_t fixed = CHUNK * 8 + 3 * n * 8 + 3 * n * sizeof(int) +
                     (accumulate ? 0 : 4 * (uint64_t)RADIX * sizeof(size_t));
    uint64_t keys = asked, capacity = 0, bin_capacity = 0, symmetric = 0, private_bytes = 0;
    long cycle = 0;
    for (;; keys /= 2) {
        if (keys < MINIMUM_KEYS) {
            snprintf(fields, size, "not even %d keys a round fit", MINIMUM_KEYS);
            return UINT64_MAX;
        }
        bin_capacity = keys / n + keys / (8 * n) + 64;
        uint64_t staging = push ? n * bin_capacity : keys;
        uint64_t slack = keys + keys / 8;
        if (!accumulate) {
            capacity = slack + 4096;
            symmetric = small + (push ? 2 * capacity * 8 : staging * 8);
            private_bytes = fixed + (push ? staging * 8 : capacity * 8) + capacity * 8;
        } else {
            uint64_t room = push ? budget - (small < budget ? small : budget)
                                 : budget - (fixed < budget ? fixed : budget);
            capacity = room / 8;
            cycle = capacity > 4096 ? (long)((capacity - 4096) / slack) : 0;
            if (fixed_rounds && cycle > fixed_rounds)
                cycle = fixed_rounds;
            if (cycle < 1)
                continue;
            capacity = (uint64_t)cycle * slack + 4096;
            symmetric = small + (push ? capacity * 8 : staging * 8);
            private_bytes = fixed + (push ? staging * 8 : capacity * 8);
        }
        if (symmetric <= budget && private_bytes <= budget)
            break;
    }
    sort.keys = keys;
    sort.capacity = capacity;
    sort.bin_capacity = bin_capacity;
    sort.cycle = cycle;
    sort.accumulate = accumulate;
    sort.push = push;
    sort.use_ptr = options->use_ptr;
    sort.seed = options->seed;
    sort.plant_key = options->plant == PACER_PLANT_SORT_KEY;
    sort.plant_order = options->plant == PACER_PLANT_SORT_ORDER;
    sort.plant_range = options->plant == PACER_PLANT_SORT_RANGE;
    sort.plant_capacity = options->plant == PACER_PLANT_SORT_CAPACITY;

    char private_text[32];
    pacer_size(private_text, sizeof private_text, private_bytes);
    size_t used = (size_t)snprintf(fields, size, "mode=%s exchange=%s ptr=%s keys=%llu",
                                   accumulate ? "accumulate" : "rounds",
                                   push ? "fetch-add" : "alltoall",
                                   push && options->use_ptr ? "yes" : "no",
                                   (unsigned long long)keys);
    if (keys != asked && used < size)
        used += (size_t)snprintf(fields + used, size - used, " asked=%llu",
                                 (unsigned long long)asked);
    if (accumulate && used < size)
        used += (size_t)snprintf(fields + used, size - used, " cycle=%ld", cycle);
    if (used < size)
        snprintf(fields + used, size - used, " capacity=%llu local_sort=%s private=%s",
                 (unsigned long long)capacity, accumulate ? "msd-in-place" : "lsd-scratch",
                 private_text);
    return symmetric;
}

/* ---- setup --------------------------------------------------------------- */

static const struct pacer_layout *order_layout;

static int node_major(const void *a, const void *b)
{
    int x = sort.world[*(const int *)a], y = sort.world[*(const int *)b];
    int nx = order_layout->node[x], ny = order_layout->node[y];
    if (nx != ny)
        return nx < ny ? -1 : 1;
    int lx = order_layout->local[x], ly = order_layout->local[y];
    return (lx > ly) - (lx < ly);
}

static int sort_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    int n = shmem_team_n_pes(team);
    sort.team = team;
    sort.n = n;
    sort.me = shmem_team_my_pe(team);
    sort.me_world = shmem_my_pe();
    sort.world = malloc(sizeof *sort.world * (size_t)n);
    sort.order = malloc(sizeof *sort.order * (size_t)n);
    sort.index_of = malloc(sizeof *sort.index_of * (size_t)n);
    if (!sort.world || !sort.order || !sort.index_of)
        return -1;
    for (int j = 0; j < n; j++) {
        sort.world[j] = shmem_team_translate_pe(team, j, SHMEM_TEAM_WORLD);
        sort.order[j] = j;
    }
    order_layout = pacer_layout();
    qsort(sort.order, (size_t)n, sizeof *sort.order, node_major);
    for (int k = 0; k < n; k++)
        sort.index_of[sort.order[k]] = k;
    sort.my_index = sort.index_of[sort.me];

    size_t bytes = sort.capacity * sizeof(uint64_t);
    sort.receive[0] = sort.receive[1] = NULL;
    sort.cursors = sort.pairs_out = sort.pairs_in = NULL;
    sort.scratch = NULL;
    sort.digits = NULL;
    if (sort.push) {
        sort.receive[0] = shmem_malloc(bytes);
        if (!sort.accumulate)
            sort.receive[1] = shmem_malloc(bytes);
        sort.cursors = shmem_malloc_with_hints(2 * sizeof *sort.cursors,
                                               SHMEM_MALLOC_ATOMICS_REMOTE);
        sort.staging = malloc((size_t)n * sort.bin_capacity * sizeof *sort.staging);
        if (!sort.receive[0] || (!sort.accumulate && !sort.receive[1]) || !sort.cursors)
            return -1;
        shmem_uint64_atomic_set(&sort.cursors[0], 0, sort.me_world);
        shmem_uint64_atomic_set(&sort.cursors[1], 0, sort.me_world);
    } else {
        sort.staging = shmem_malloc(sort.keys * sizeof *sort.staging);
        sort.pairs_out = shmem_malloc(2 * (size_t)n * sizeof *sort.pairs_out);
        sort.pairs_in = shmem_malloc(2 * (size_t)n * sizeof *sort.pairs_in);
        sort.receive[0] = malloc(bytes);
        if (!sort.pairs_out || !sort.pairs_in || !sort.receive[0])
            return -1;
    }
    sort.fill = calloc((size_t)n, sizeof *sort.fill);
    sort.next = calloc((size_t)n, sizeof *sort.next);
    sort.chunk = malloc(CHUNK * sizeof *sort.chunk);
    sort.reduce_source = shmem_malloc(4 * sizeof *sort.reduce_source);
    sort.reduce_result = shmem_malloc(4 * sizeof *sort.reduce_result);
    if (!sort.staging || !sort.fill || !sort.next || !sort.chunk || !sort.reduce_source ||
        !sort.reduce_result)
        return -1;
    if (!sort.accumulate) {
        sort.scratch = malloc(bytes);
        sort.digits = malloc(4 * sizeof *sort.digits);
        if (!sort.scratch || !sort.digits)
            return -1;
    }
    sort.round = 0;
    sort.cycle_rounds = 0;
    sort.received = 0;
    memset(&sort.open_generated, 0, sizeof sort.open_generated);
    memset(&sort.generated, 0, sizeof sort.generated);
    memset(&sort.arrived, 0, sizeof sort.arrived);
    sort.work = sort.batch = 0;
    memset(sort.phase, 0, sizeof sort.phase);
    shmem_quiet();                  /* the cursors' zeros, before anyone reserves */
    shmem_team_sync(team);
    return 0;
}

/* ---- keys ------------------------------------------------------------------ */

/* The node-major index of a key's owner: (key N) >> 64. */
static int owner(uint64_t key)
{
    return (int)(((unsigned __int128)key * (unsigned __int128)sort.n) >> 64);
}

static uint64_t stream(long round)
{
    return random_mix(sort.seed, random_name("sort"), (uint64_t)sort.me_world, (uint64_t)round);
}

/* The keys of one chunk; counted when `count` is given. */
static void generate(uint64_t *state, size_t m, struct triple *count)
{
    double started = pacer_now();
    for (size_t i = 0; i < m; i++)
        sort.chunk[i] = random_next(state);
    if (count)
        for (size_t i = 0; i < m; i++)
            triple_add(count, sort.chunk[i]);
    sort.phase[GENERATE] += pacer_now() - started;
}

/* The planted key: PE 1's first key of round 0, changed after it was counted. */
static int planted_key(long round, uint64_t first)
{
    return sort.plant_key && sort.me_world == 1 && round == 0 && first == 0;
}

/* The node-major index a key goes to: its owner's - but for the planted range
 * defect, PE 1's first key of round 0 goes to the PE after its owner, where
 * it is still counted, so that the range check alone can see it. */
static int destination(uint64_t key, long round, uint64_t first, size_t i)
{
    int index = owner(key);
    if (sort.plant_range && sort.me_world == 1 && round == 0 && first == 0 && i == 0)
        index = (index + 1) % sort.n;
    return index;
}

/* ---- the local sorts -------------------------------------------------------- */

/* LSD radix, four passes of 16-bit digits between keys and scratch, ending in
 * keys. */
static void sort_lsd(uint64_t *keys, uint64_t *scratch, size_t count)
{
    memset(sort.digits, 0, 4 * sizeof *sort.digits);
    for (size_t i = 0; i < count; i++) {
        uint64_t k = keys[i];
        sort.digits[0][k & 0xffff]++;
        sort.digits[1][(k >> 16) & 0xffff]++;
        sort.digits[2][(k >> 32) & 0xffff]++;
        sort.digits[3][k >> 48]++;
    }
    uint64_t *from = keys, *to = scratch;
    for (int d = 0; d < 4; d++) {
        size_t total = 0;
        for (size_t b = 0; b < RADIX; b++) {
            size_t c = sort.digits[d][b];
            sort.digits[d][b] = total;
            total += c;
        }
        unsigned shift = 16 * (unsigned)d;
        for (size_t i = 0; i < count; i++) {
            uint64_t k = from[i];
            to[sort.digits[d][(k >> shift) & 0xffff]++] = k;
        }
        uint64_t *swap = from;
        from = to;
        to = swap;
    }
}

/* MSD radix in place (American flag sort), 8-bit digits from the top, each
 * bucket permuted into place by following cycles; small buckets by insertion. */
static void sort_in_place(uint64_t *keys, size_t count, unsigned shift)
{
    if (count < SMALL_SORT) {
        for (size_t i = 1; i < count; i++) {
            uint64_t k = keys[i];
            size_t j = i;
            while (j > 0 && keys[j - 1] > k) {
                keys[j] = keys[j - 1];
                j--;
            }
            keys[j] = k;
        }
        return;
    }
    size_t counts[256] = { 0 }, next[256], end[256];
    for (size_t i = 0; i < count; i++)
        counts[(keys[i] >> shift) & 0xff]++;
    size_t total = 0;
    for (int b = 0; b < 256; b++) {
        next[b] = total;
        total += counts[b];
        end[b] = total;
    }
    for (unsigned b = 0; b < 256; b++)
        while (next[b] < end[b]) {
            uint64_t value = keys[next[b]];
            unsigned digit = (unsigned)(value >> shift) & 0xff;
            while (digit != b) {
                uint64_t displaced = keys[next[digit]];
                keys[next[digit]++] = value;
                value = displaced;
                digit = (unsigned)(value >> shift) & 0xff;
            }
            keys[next[b]++] = value;
        }
    if (shift == 0)
        return;
    size_t start = 0;
    for (int b = 0; b < 256; b++) {
        if (counts[b] > 1)
            sort_in_place(keys + start, counts[b], shift - 8);
        start += counts[b];
    }
}

/* ---- checking a batch ------------------------------------------------------- */

static void check_batch(uint64_t *keys, uint64_t count, long round)
{
    double started = pacer_now();
    struct triple before = { 0 };
    uint64_t outside = 0, first = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t k = keys[i];
        triple_add(&before, k);
        if (owner(k) != sort.my_index && outside++ == 0)
            first = k;
    }
    if (outside)
        pacer_failure("sort round=%ld range: %llu of %llu keys belong elsewhere; the first, "
                      "%016llx, is PE %d's", round, (unsigned long long)outside,
                      (unsigned long long)count, (unsigned long long)first,
                      sort.world[sort.order[owner(first)]]);
    double sorting = pacer_now();
    sort.phase[CHECK] += sorting - started;

    if (sort.plant_order && sort.me_world == 1)
        sort.plant_order = 0;       /* the planted defect: PE 1 skips its first sort */
    else {
        if (sort.accumulate)
            sort_in_place(keys, count, 56);
        else
            sort_lsd(keys, sort.scratch, count);
    }
    double sorted = pacer_now();
    sort.phase[SORT] += sorted - sorting;

    struct triple after = { 0 };
    uint64_t descents = 0, at = 0;
    for (uint64_t i = 0; i < count; i++) {
        triple_add(&after, keys[i]);
        if (i > 0 && keys[i] < keys[i - 1] && descents++ == 0)
            at = i;
    }
    if (descents)
        pacer_failure("sort round=%ld order: %llu keys follow a larger one; the first at %llu of "
                      "%llu", round, (unsigned long long)descents, (unsigned long long)at,
                      (unsigned long long)count);
    if (after.count != before.count || after.sum != before.sum || after.xor != before.xor)
        pacer_failure("sort round=%ld local sort: the batch changed while it was sorted - the "
                      "tool's own fault", round);
    sort.arrived.count += before.count;
    sort.arrived.sum += before.sum;
    sort.arrived.xor ^= before.xor;
    sort.batch += (double)count;
    sort.phase[CHECK] += pacer_now() - sorted;
}

/* The push exchange's batch, from its cursor. A cursor past the capacity
 * means reservations were refused, and their ranges never written: the buffer
 * has holes, and checking it would only report its garbage. So the overflow
 * is said once, and the batch left out - which the oracle then sees. */
static void check_received(uint64_t *keys, uint64_t cursor, long round)
{
    if (cursor <= sort.capacity) {
        check_batch(keys, cursor, round);
        return;
    }
    pacer_failure("sort round=%ld capacity: reservations here reached %llu keys, past the %llu "
                  "this PE holds; its batch goes unchecked", round, (unsigned long long)cursor,
                  (unsigned long long)sort.capacity);
}

/* ---- the push exchange -------------------------------------------------------- */

static void send_bin(int index, int parity, long round, int early)
{
    double started = pacer_now();
    uint64_t count = sort.fill[index];
    uint64_t *bin = sort.staging + (size_t)index * sort.bin_capacity;
    int pe = sort.world[sort.order[index]];
    uint64_t *receive = sort.receive[parity];
    uint64_t reserve = count;
    if (sort.plant_capacity && sort.me_world == 1) {
        reserve += sort.capacity;   /* the planted defect: one reservation too many */
        sort.plant_capacity = 0;
    }
    uint64_t offset = shmem_uint64_atomic_fetch_add(&sort.cursors[parity], reserve, pe);
    if (offset + reserve > sort.capacity) {
        pacer_failure("sort round=%ld capacity: PE %d holds %llu keys; a reservation of %llu "
                      "reached %llu", round, pe, (unsigned long long)sort.capacity,
                      (unsigned long long)reserve, (unsigned long long)(offset + reserve));
    } else {
        uint64_t *remote = sort.use_ptr ? shmem_ptr(receive, pe) : NULL;
        if (remote)
            memcpy(remote + offset, bin, count * sizeof *bin);
        else if (early)
            shmem_putmem(receive + offset, bin, count * sizeof *bin, pe);
        else
            shmem_putmem_nbi(receive + offset, bin, count * sizeof *bin, pe);
    }
    sort.fill[index] = 0;
    double seconds = pacer_now() - started;
    sort.phase[EXCHANGE] += seconds;
    if (early)
        sort.send_seconds += seconds;
}

static void round_push(long round)
{
    int parity = sort.accumulate ? 0 : (int)(round % 2);
    uint64_t state = stream(round);
    struct triple *counted = sort.accumulate ? &sort.open_generated : &sort.generated;
    for (uint64_t first = 0; first < sort.keys; first += CHUNK) {
        size_t m = sort.keys - first < CHUNK ? (size_t)(sort.keys - first) : CHUNK;
        generate(&state, m, counted);
        if (planted_key(round, first))
            sort.chunk[0] ^= 1;
        double binning = pacer_now();
        sort.send_seconds = 0;
        for (size_t i = 0; i < m; i++) {
            uint64_t k = sort.chunk[i];
            int index = destination(k, round, first, i);
            sort.staging[(size_t)index * sort.bin_capacity + sort.fill[index]++] = k;
            if (sort.fill[index] == sort.bin_capacity)
                send_bin(index, parity, round, 1);
        }
        sort.phase[BIN] += pacer_now() - binning - sort.send_seconds;
    }
    /* From this PE's right neighbour round, as ISx does, so that no PE is
     * every PE's first target. */
    for (int k = 1; k <= sort.n; k++) {
        int index = (sort.my_index + k) % sort.n;
        if (sort.fill[index])
            send_bin(index, parity, round, 0);
    }
    double syncing = pacer_now();
    shmem_quiet();
    shmem_team_sync(sort.team);
    sort.phase[EXCHANGE] += pacer_now() - syncing;
    sort.work += (double)sort.keys;

    if (!sort.accumulate) {
        uint64_t cursor = shmem_uint64_atomic_fetch(&sort.cursors[parity], sort.me_world);
        check_received(sort.receive[parity], cursor, round);
        /* Completed by the next round's quiet, before its sync. */
        shmem_uint64_atomic_set(&sort.cursors[parity], 0, sort.me_world);
    }
}

/* ---- the pull exchange -------------------------------------------------------- */

static void round_pull(long round)
{
    int n = sort.n;
    struct triple *counted = sort.accumulate ? &sort.open_generated : &sort.generated;
    uint64_t state = stream(round);
    memset(sort.fill, 0, (size_t)n * sizeof *sort.fill);
    for (uint64_t first = 0; first < sort.keys; first += CHUNK) {
        size_t m = sort.keys - first < CHUNK ? (size_t)(sort.keys - first) : CHUNK;
        generate(&state, m, counted);
        double binning = pacer_now();
        for (size_t i = 0; i < m; i++)
            sort.fill[destination(sort.chunk[i], round, first, i)]++;
        sort.phase[BIN] += pacer_now() - binning;
    }
    uint64_t offset = 0;
    for (int index = 0; index < n; index++) {
        sort.next[index] = offset;
        offset += sort.fill[index];
    }
    state = stream(round);
    for (uint64_t first = 0; first < sort.keys; first += CHUNK) {
        size_t m = sort.keys - first < CHUNK ? (size_t)(sort.keys - first) : CHUNK;
        generate(&state, m, NULL);
        double binning = pacer_now();
        for (size_t i = 0; i < m; i++) {
            uint64_t k = sort.chunk[i];
            int index = destination(k, round, first, i);
            if (i == 0 && planted_key(round, first))
                k ^= 1;             /* in the bin its original chose */
            sort.staging[sort.next[index]++] = k;
        }
        sort.phase[BIN] += pacer_now() - binning;
    }
    for (int j = 0; j < n; j++) {
        int index = sort.index_of[j];
        sort.pairs_out[2 * j] = sort.fill[index];
        sort.pairs_out[2 * j + 1] = sort.next[index] - sort.fill[index];
    }
    if (sort.plant_capacity && sort.me_world == 1) {
        /* The planted defect: one count declared larger than the receiver holds. */
        sort.pairs_out[2 * ((sort.me + 1) % n)] += sort.capacity;
        sort.plant_capacity = 0;
    }

    double exchanging = pacer_now();
    shmem_uint64_alltoall(sort.team, sort.pairs_in, sort.pairs_out, 2);
    uint64_t position = sort.accumulate ? sort.received : 0;
    uint64_t *receive = sort.receive[0];
    for (int s = 0; s < n; s++) {
        uint64_t count = sort.pairs_in[2 * s], from = sort.pairs_in[2 * s + 1];
        if (position + count > sort.capacity) {
            pacer_failure("sort round=%ld capacity: this PE holds %llu keys; PE %d's %llu "
                          "reached %llu", round, (unsigned long long)sort.capacity, sort.world[s],
                          (unsigned long long)count, (unsigned long long)(position + count));
            continue;
        }
        if (count)
            shmem_getmem_nbi(receive + position, sort.staging + from, count * sizeof *receive,
                             sort.world[s]);
        position += count;
    }
    shmem_quiet();
    sort.phase[EXCHANGE] += pacer_now() - exchanging;
    sort.work += (double)sort.keys;

    if (sort.accumulate)
        sort.received = position;
    else
        check_batch(receive, position, round);
    double syncing = pacer_now();
    shmem_team_sync(sort.team);     /* before any sender refills its staging */
    sort.phase[EXCHANGE] += pacer_now() - syncing;
}

/* ---- accumulate: a cycle closes ------------------------------------------------ */

static void close_cycle(long round)
{
    if (sort.push) {
        /* Nothing is sent until the sync below, so the cursor is still. */
        uint64_t cursor = shmem_uint64_atomic_fetch(&sort.cursors[0], sort.me_world);
        check_received(sort.receive[0], cursor, round);
    } else {
        check_batch(sort.receive[0], sort.received, round);
    }
    sort.generated.count += sort.open_generated.count;
    sort.generated.sum += sort.open_generated.sum;
    sort.generated.xor ^= sort.open_generated.xor;
    memset(&sort.open_generated, 0, sizeof sort.open_generated);
    sort.received = 0;
    sort.cycle_rounds = 0;
    if (sort.push) {
        double syncing = pacer_now();
        shmem_uint64_atomic_set(&sort.cursors[0], 0, sort.me_world);
        shmem_quiet();
        shmem_team_sync(sort.team);
        sort.phase[EXCHANGE] += pacer_now() - syncing;
    }
}

static void sort_step(long units, int last)
{
    for (long u = 0; u < units; u++) {
        long round = sort.round++;
        if (sort.push)
            round_push(round);
        else
            round_pull(round);
        if (sort.accumulate && ++sort.cycle_rounds == sort.cycle)
            close_cycle(round);
    }
    if (last && sort.accumulate && sort.cycle_rounds > 0)
        close_cycle(sort.round - 1);
}

/* ---- the oracle, a checkpoint ------------------------------------------------- */

static void sort_checkpoint(struct pacer_counters *counters)
{
    uint64_t *source = sort.reduce_source, *result = sort.reduce_result;
    uint64_t sums[4], xors[2];
    source[0] = sort.generated.count;
    source[1] = sort.generated.sum;
    source[2] = sort.arrived.count;
    source[3] = sort.arrived.sum;
    shmem_uint64_sum_reduce(sort.team, result, source, 4);
    memcpy(sums, result, sizeof sums);
    source[0] = sort.generated.xor;
    source[1] = sort.arrived.xor;
    shmem_uint64_xor_reduce(sort.team, result + 2, source, 2);
    memcpy(xors, result + 2, sizeof xors);
    if (sort.me == 0) {
        long round = sort.round - 1;
        if (sums[0] != sums[2])
            pacer_failure("sort round=%ld oracle: count generated %llu, received %llu", round,
                          (unsigned long long)sums[0], (unsigned long long)sums[2]);
        if (sums[1] != sums[3])
            pacer_failure("sort round=%ld oracle: sum generated %016llx, received %016llx", round,
                          (unsigned long long)sums[1], (unsigned long long)sums[3]);
        if (xors[0] != xors[1])
            pacer_failure("sort round=%ld oracle: xor generated %016llx, received %016llx", round,
                          (unsigned long long)xors[0], (unsigned long long)xors[1]);
    }
    memset(&sort.generated, 0, sizeof sort.generated);
    memset(&sort.arrived, 0, sizeof sort.arrived);

    counters->work = sort.work;
    counters->bytes = sort.work * 8;
    counters->batch = sort.batch;
    for (int k = 0; k < 5; k++)
        counters->phase_seconds[k] = sort.phase[k];
    sort.work = sort.batch = 0;
    memset(sort.phase, 0, sizeof sort.phase);
}

static void sort_teardown(void)
{
    shmem_quiet();
    shmem_team_sync(sort.team);
    shmem_free(sort.reduce_result);
    shmem_free(sort.reduce_source);
    if (sort.push) {
        shmem_free(sort.cursors);
        if (sort.receive[1])
            shmem_free(sort.receive[1]);
        shmem_free(sort.receive[0]);
        free(sort.staging);
    } else {
        shmem_free(sort.pairs_in);
        shmem_free(sort.pairs_out);
        shmem_free(sort.staging);
        free(sort.receive[0]);
    }
    free(sort.digits);
    free(sort.scratch);
    free(sort.chunk);
    free(sort.next);
    free(sort.fill);
    free(sort.index_of);
    free(sort.order);
    free(sort.world);
}

const struct pacer_exercise pacer_sort = {
    .name = "sort",
    .work = "keys",
    .phases = phase_names,
    .quick_rounds = 3,
    .memory = sort_memory,
    .setup = sort_setup,
    .step = sort_step,
    .checkpoint = sort_checkpoint,
    .teardown = sort_teardown,
};
