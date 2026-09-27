/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * mailbox - point-to-point synchronization, the U4 idiom (proposal §4.4;
 * docs/prompts/task-stress-phase2.md): every PE has a slot for every sender, a
 * payload area and two signal words; every round each PE sends K messages and
 * receives K, waiting for them with one of the spec's waiting and testing
 * forms - another every round, so that twenty rounds run every form.
 *
 * **Who sends to whom.** Round t draws K distinct offsets from the seed, alike
 * on every PE; PE s sends to s + o and receives from r - o, mod N, for each
 * offset o. So every PE knows whom it waits for without being told, and sends
 * and receives exactly K a round.
 *
 * **A slot holds one message at a time.** A sender sends to a slot again only
 * once its receiver has acknowledged the last message there: an
 * shmem_uint64_atomic_set of the round into the sender's acknowledgement word
 * for that receiver, which the sender waits on - a wait may watch only words
 * updated atomically (§9.11). Every PE sends round t, then receives it; a send
 * of round t waits only for acknowledgements of earlier rounds, which every
 * PE gave before starting round t. So the rounds pipeline, with no sync
 * between them, and cannot deadlock.
 *
 * **Two signal words a slot**, since six comparisons need a word that grows
 * and one that shrinks. The "up" word: set to the round plus one
 * (SHMEM_SIGNAL_SET) or added one (SHMEM_SIGNAL_ADD), as the round's form
 * wants; the receiver takes its value and resets it to 0 in one atomic swap,
 * which completes before the acknowledgement that lets the next message come.
 * Equality, inequality, greater and greater-or-equal wait on it. The "down"
 * word: started at 2^62, added minus one by every message, never reset; less
 * and less-or-equal wait on it against the last value seen. Once a wait
 * returns, the word must hold exactly what one message makes it hold.
 *
 * **The forms**, one a round in turn: wait_until_all, _any, _some and their
 * _vector forms; test_all, _any, _some and theirs, polled with work between
 * polls; signal_wait_until; signal_fetch polled; and wait_until with each of
 * the six comparisons. The many-word forms go over all N up words, the status
 * array excluding every slot but the round's senders'. Odd rounds send with
 * shmem_putmem_signal_nbi and quiet after the round's sends.
 *
 * **The checks.** A payload is (sender, receiver, round)-determined in every
 * word but its first, which carries the sender's clock: checked whole on wake,
 * it proves the payload landed before its signal. The signal word's exact
 * value, as above. And at the last checkpoint, an alltoall of every PE's
 * counts sent to each PE against the counts each received from each: equal,
 * pair by pair.
 *
 * **It reports** messages a second, and the time from a node-mate's send to
 * this PE's reading of the message - one clock on one node; across nodes no
 * time is claimed - its median and 99th percentile, the slowest PE's. The
 * receiver is sending its own round meanwhile, so this is a message's time
 * in the mailbox as an application would see it, not a bare wake.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { MAXIMUM_SENDS = 4, SAMPLES = 1024, WORK_BETWEEN_POLLS = 64 };
#define DOWN_START (UINT64_C(1) << 62)

/* Prefixed: WAIT_ANY is <sys/wait.h>'s, which <stdlib.h> may bring in. */
enum form {
    FORM_WAIT_ALL, FORM_WAIT_ANY, FORM_WAIT_SOME,
    FORM_WAIT_ALL_VECTOR, FORM_WAIT_ANY_VECTOR, FORM_WAIT_SOME_VECTOR,
    FORM_TEST_ALL, FORM_TEST_ANY, FORM_TEST_SOME,
    FORM_TEST_ALL_VECTOR, FORM_TEST_ANY_VECTOR, FORM_TEST_SOME_VECTOR,
    FORM_SIGNAL_WAIT, FORM_SIGNAL_FETCH,
    FORM_WAIT_EQ, FORM_WAIT_NE, FORM_WAIT_GT, FORM_WAIT_GE, FORM_WAIT_LT, FORM_WAIT_LE,
    FORM_COUNT
};

/* How a form's messages signal and how its receiver waits: which word, the
 * signal operation, the comparison. */
static const struct {
    const char *name;
    int down, add, cmp;
} forms[FORM_COUNT] = {
    [FORM_WAIT_ALL] = { "wait_until_all", 0, 0, SHMEM_CMP_EQ },
    [FORM_WAIT_ANY] = { "wait_until_any", 0, 1, SHMEM_CMP_EQ },
    [FORM_WAIT_SOME] = { "wait_until_some", 0, 0, SHMEM_CMP_GE },
    [FORM_WAIT_ALL_VECTOR] = { "wait_until_all_vector", 0, 1, SHMEM_CMP_GE },
    [FORM_WAIT_ANY_VECTOR] = { "wait_until_any_vector", 0, 0, SHMEM_CMP_EQ },
    [FORM_WAIT_SOME_VECTOR] = { "wait_until_some_vector", 0, 1, SHMEM_CMP_NE },
    [FORM_TEST_ALL] = { "test_all", 0, 0, SHMEM_CMP_EQ },
    [FORM_TEST_ANY] = { "test_any", 0, 1, SHMEM_CMP_GT },
    [FORM_TEST_SOME] = { "test_some", 0, 0, SHMEM_CMP_GE },
    [FORM_TEST_ALL_VECTOR] = { "test_all_vector", 0, 1, SHMEM_CMP_EQ },
    [FORM_TEST_ANY_VECTOR] = { "test_any_vector", 0, 0, SHMEM_CMP_NE },
    [FORM_TEST_SOME_VECTOR] = { "test_some_vector", 0, 1, SHMEM_CMP_GE },
    [FORM_SIGNAL_WAIT] = { "signal_wait_until", 0, 0, SHMEM_CMP_EQ },
    [FORM_SIGNAL_FETCH] = { "signal_fetch", 0, 1, SHMEM_CMP_GE },
    [FORM_WAIT_EQ] = { "wait_until_eq", 0, 0, SHMEM_CMP_EQ },
    [FORM_WAIT_NE] = { "wait_until_ne", 0, 1, SHMEM_CMP_NE },
    [FORM_WAIT_GT] = { "wait_until_gt", 0, 0, SHMEM_CMP_GT },
    [FORM_WAIT_GE] = { "wait_until_ge", 0, 1, SHMEM_CMP_GE },
    [FORM_WAIT_LT] = { "wait_until_lt", 1, 1, SHMEM_CMP_LT },
    [FORM_WAIT_LE] = { "wait_until_le", 1, 1, SHMEM_CMP_LE },
};

static const char *const phase_names[] = { "send", "wait", "check", NULL };

static struct {
    /* the plan */
    int sends;                      /* K */
    size_t words;                   /* a slot's payload, in words */
    uint64_t seed;
    int plant_payload, plant_signal, plant_count;
    /* the team */
    shmem_team_t team;
    int n, me, me_world;
    int *world;                     /* team rank to world PE */
    int *node_mate;                 /* team rank: on this PE's node */
    /* symmetric */
    uint64_t *slots;                /* [n][words]: sender s's slot at s */
    uint64_t *up, *down;            /* [n]: sender s's signal words */
    uint64_t *acknowledged;         /* [n]: the last round receiver r read, plus one */
    uint64_t *sent_to, *claimed;    /* [n]: for the pairwise count */
    double *latency;                /* [4]: medians and 99th percentiles, reduced */
    /* private */
    uint64_t *message[MAXIMUM_SENDS];
    int *status;                    /* [n]: 0 while a slot is awaited */
    uint64_t *values;               /* [n]: the vector forms' comparison values */
    size_t *indices;                /* [n] */
    int64_t *last_sent;             /* [n]: the round of the last message to r, or -1 */
    uint64_t *last_down;            /* [n]: sender s's down word as last read */
    uint64_t *received;             /* [n]: messages read from s */
    int expected[MAXIMUM_SENDS];
    double samples[SAMPLES];
    long sample_count, sample_seen;
    uint64_t sample_state;
    long round;
    int last;
    double messages, bytes, phase[3];
    uint64_t work_state;
} mailbox;

static uint64_t mailbox_memory(int npes, const struct pacer_options *options, char *fields,
                               size_t size)
{
    uint64_t n = (uint64_t)npes;
    size_t words = 32;              /* 256 bytes */
    uint64_t symmetric = 0;
    for (;; words /= 2) {
        if (words < 4) {
            snprintf(fields, size, "%d PEs' slots of 32 bytes do not fit", npes);
            return UINT64_MAX;
        }
        symmetric = n * (words * 8 + 5 * 8) + 4 * 8;
        uint64_t private_bytes = n * (4 + 8 + 8 + 8 + 8 + 8 + 4 + 4) + MAXIMUM_SENDS * words * 8;
        if (symmetric <= options->memory && private_bytes <= options->memory)
            break;
    }
    mailbox.sends = npes < MAXIMUM_SENDS ? npes : MAXIMUM_SENDS;
    mailbox.words = words;
    mailbox.seed = options->seed;
    mailbox.plant_payload = options->plant == PACER_PLANT_MAILBOX_PAYLOAD;
    mailbox.plant_signal = options->plant == PACER_PLANT_MAILBOX_SIGNAL;
    mailbox.plant_count = options->plant == PACER_PLANT_MAILBOX_COUNT;
    snprintf(fields, size, "sends=%d slot=%zu forms=%d", mailbox.sends, words * 8, FORM_COUNT);
    return symmetric;
}

static int mailbox_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    int n = shmem_team_n_pes(team);
    mailbox.team = team;
    mailbox.n = n;
    mailbox.me = shmem_team_my_pe(team);
    mailbox.me_world = shmem_my_pe();
    const struct pacer_layout *layout = pacer_layout();
    mailbox.world = malloc(sizeof *mailbox.world * (size_t)n);
    mailbox.node_mate = malloc(sizeof *mailbox.node_mate * (size_t)n);
    if (!mailbox.world || !mailbox.node_mate)
        return -1;
    for (int j = 0; j < n; j++) {
        mailbox.world[j] = shmem_team_translate_pe(team, j, SHMEM_TEAM_WORLD);
        mailbox.node_mate[j] = layout->node[mailbox.world[j]] == layout->node[mailbox.me_world];
    }
    size_t words = mailbox.words;
    mailbox.slots = shmem_malloc(sizeof *mailbox.slots * (size_t)n * words);
    mailbox.up = shmem_calloc((size_t)n, sizeof *mailbox.up);
    mailbox.down = shmem_malloc(sizeof *mailbox.down * (size_t)n);
    mailbox.acknowledged = shmem_calloc((size_t)n, sizeof *mailbox.acknowledged);
    mailbox.sent_to = shmem_calloc((size_t)n, sizeof *mailbox.sent_to);
    mailbox.claimed = shmem_calloc((size_t)n, sizeof *mailbox.claimed);
    mailbox.latency = shmem_calloc(4, sizeof *mailbox.latency);
    mailbox.status = malloc(sizeof *mailbox.status * (size_t)n);
    mailbox.values = calloc((size_t)n, sizeof *mailbox.values);
    mailbox.indices = malloc(sizeof *mailbox.indices * (size_t)n);
    mailbox.last_sent = malloc(sizeof *mailbox.last_sent * (size_t)n);
    mailbox.last_down = malloc(sizeof *mailbox.last_down * (size_t)n);
    mailbox.received = calloc((size_t)n, sizeof *mailbox.received);
    if (!mailbox.slots || !mailbox.up || !mailbox.down || !mailbox.acknowledged ||
        !mailbox.sent_to || !mailbox.claimed || !mailbox.latency || !mailbox.status ||
        !mailbox.values || !mailbox.indices || !mailbox.last_sent || !mailbox.last_down ||
        !mailbox.received)
        return -1;
    for (int k = 0; k < MAXIMUM_SENDS; k++)
        if (!(mailbox.message[k] = malloc(sizeof(uint64_t) * words)))
            return -1;
    for (int j = 0; j < n; j++) {
        mailbox.status[j] = 1;
        mailbox.last_sent[j] = -1;
        mailbox.last_down[j] = DOWN_START;
        shmem_uint64_atomic_set(&mailbox.down[j], DOWN_START, mailbox.me_world);
    }
    mailbox.round = 0;
    mailbox.sample_count = mailbox.sample_seen = 0;
    mailbox.sample_state = 1;
    mailbox.work_state = (uint64_t)mailbox.me_world;
    mailbox.messages = mailbox.bytes = 0;
    memset(mailbox.phase, 0, sizeof mailbox.phase);
    shmem_quiet();                  /* the down words' starts, before anyone signals */
    shmem_team_sync(team);
    return 0;
}

/* Round t's offsets: K distinct values in [0, n), alike on every PE. */
static void offsets(long round, int *out)
{
    uint64_t state = random_mix(mailbox.seed, random_name("mailbox"), (uint64_t)round, 0);
    for (int k = 0; k < mailbox.sends;) {
        int o = (int)(random_next(&state) % (uint64_t)mailbox.n), seen = 0;
        for (int j = 0; j < k; j++)
            seen |= out[j] == o;
        if (!seen)
            out[k++] = o;
    }
}

static uint64_t message_base(int from, int to, long round)
{
    return random_mix(mailbox.seed ^ random_name("mailbox"), (uint64_t)from, (uint64_t)to,
                      (uint64_t)round);
}

/* A message's words: 2 to `words`, from its base. */
static size_t message_words(uint64_t base)
{
    return 2 + (size_t)(base % (uint64_t)(mailbox.words - 1));
}

static uint64_t message_word(uint64_t base, size_t e)
{
    return random_finalize(base + (uint64_t)e * UINT64_C(0x9e3779b97f4a7c15));
}

static uint64_t now_ns(void)
{
    return (uint64_t)(pacer_now() * 1e9);
}

static void work(void)
{
    for (int i = 0; i < WORK_BETWEEN_POLLS; i++)
        (void)random_next(&mailbox.work_state);
}

static void send_round(long round, const int *offset)
{
    double started = pacer_now();
    enum form f = (enum form)(round % FORM_COUNT);
    int n = mailbox.n, me = mailbox.me;
    for (int k = 0; k < mailbox.sends; k++) {
        int r = (me + offset[k]) % n;
        if (mailbox.last_sent[r] >= 0)
            shmem_uint64_wait_until(&mailbox.acknowledged[r], SHMEM_CMP_GE,
                                    (uint64_t)mailbox.last_sent[r] + 1);
        uint64_t base = message_base(mailbox.me_world, mailbox.world[r], round);
        size_t words = message_words(base);
        uint64_t *message = mailbox.message[k];
        for (size_t e = 1; e < words; e++)
            message[e] = message_word(base, e);
        if (mailbox.plant_payload && mailbox.me_world == 1) {
            ((unsigned char *)&message[1])[0] ^= 1;    /* the planted defect, once */
            mailbox.plant_payload = 0;
        }
        uint64_t signal = forms[f].down ? UINT64_MAX            /* minus one */
                          : forms[f].add ? 1 : (uint64_t)round + 1;
        if (mailbox.plant_signal && mailbox.me_world == 1 && f == FORM_WAIT_ALL_VECTOR) {
            signal = 2;             /* the planted defect: one too many, once */
            mailbox.plant_signal = 0;
        }
        int operation = forms[f].down || forms[f].add ? SHMEM_SIGNAL_ADD : SHMEM_SIGNAL_SET;
        uint64_t *word = (forms[f].down ? mailbox.down : mailbox.up) + me;
        message[0] = now_ns();
        if (round % 2)
            shmem_putmem_signal_nbi(mailbox.slots + (size_t)me * mailbox.words, message,
                                    words * sizeof *message, word, signal, operation,
                                    mailbox.world[r]);
        else
            shmem_putmem_signal(mailbox.slots + (size_t)me * mailbox.words, message,
                                words * sizeof *message, word, signal, operation,
                                mailbox.world[r]);
        mailbox.last_sent[r] = round;
        mailbox.sent_to[r]++;
    }
    shmem_quiet();                  /* the nonblocking round's sources are free again */
    mailbox.phase[0] += pacer_now() - started;
}

/* A message is read: its signal word's exact value, its payload, the reset,
 * the acknowledgement. */
static void read_message(int s, long round, enum form f)
{
    double started = pacer_now();
    uint64_t value, want;
    if (forms[f].down) {
        value = shmem_signal_fetch(&mailbox.down[s]);
        want = mailbox.last_down[s] - 1;
        mailbox.last_down[s] = want;
    } else {
        value = shmem_uint64_atomic_swap(&mailbox.up[s], 0, mailbox.me_world);
        want = forms[f].add ? 1 : (uint64_t)round + 1;
    }
    if (value != want)
        pacer_failure("mailbox round=%ld %s from PE %d: signal %llu, expected %llu", round,
                      forms[f].name, mailbox.world[s], (unsigned long long)value,
                      (unsigned long long)want);
    const uint64_t *slot = mailbox.slots + (size_t)s * mailbox.words;
    uint64_t base = message_base(mailbox.world[s], mailbox.me_world, round);
    size_t words = message_words(base), wrong = 0, first = 0;
    for (size_t e = 1; e < words; e++)
        if (slot[e] != message_word(base, e) && wrong++ == 0)
            first = e;
    if (wrong)
        pacer_failure("mailbox round=%ld %s from PE %d: %zu of %zu payload words wrong; the "
                      "first, word %zu, is %016llx", round, forms[f].name, mailbox.world[s],
                      wrong, words - 1, first, (unsigned long long)slot[first]);
    if (mailbox.node_mate[s]) {
        double us = ((double)now_ns() - (double)slot[0]) / 1e3;
        mailbox.sample_seen++;
        if (mailbox.sample_count < SAMPLES) {
            mailbox.samples[mailbox.sample_count++] = us;
        } else {
            uint64_t k = random_next(&mailbox.sample_state) % (uint64_t)mailbox.sample_seen;
            if (k < SAMPLES)
                mailbox.samples[k] = us;
        }
    }
    shmem_uint64_atomic_set(&mailbox.acknowledged[mailbox.me], (uint64_t)round + 1,
                            mailbox.world[s]);
    mailbox.received[s]++;
    if (mailbox.plant_count && mailbox.me_world == 1) {
        mailbox.received[s]++;      /* the planted defect: one counted twice */
        mailbox.plant_count = 0;
    }
    mailbox.status[s] = 1;
    mailbox.messages++;
    mailbox.bytes += (double)(words * 8);
    mailbox.phase[2] += pacer_now() - started;
}

static void receive_round(long round, const int *offset)
{
    enum form f = (enum form)(round % FORM_COUNT);
    int n = mailbox.n, count = mailbox.sends;
    size_t waiting = (size_t)count;
    uint64_t value = forms[f].cmp == SHMEM_CMP_NE || forms[f].cmp == SHMEM_CMP_GT ? 0
                     : forms[f].add ? 1 : (uint64_t)round + 1;
    for (int k = 0; k < count; k++) {
        int s = (mailbox.me - offset[k] + n) % n;
        mailbox.expected[k] = s;
        mailbox.status[s] = 0;
        mailbox.values[s] = value;
    }
    uint64_t *up = mailbox.up;
    double started = pacer_now();
    double reading = mailbox.phase[2];
    switch (f) {
    case FORM_WAIT_ALL:
    case FORM_WAIT_ALL_VECTOR:
        if (f == FORM_WAIT_ALL)
            shmem_uint64_wait_until_all(up, (size_t)n, mailbox.status, forms[f].cmp, value);
        else
            shmem_uint64_wait_until_all_vector(up, (size_t)n, mailbox.status, forms[f].cmp,
                                               mailbox.values);
        for (int k = 0; k < count; k++)
            read_message(mailbox.expected[k], round, f);
        break;
    case FORM_TEST_ALL:
    case FORM_TEST_ALL_VECTOR:
        while (f == FORM_TEST_ALL ? !shmem_uint64_test_all(up, (size_t)n, mailbox.status,
                                                      forms[f].cmp, value)
                             : !shmem_uint64_test_all_vector(up, (size_t)n, mailbox.status,
                                                             forms[f].cmp, mailbox.values))
            work();
        for (int k = 0; k < count; k++)
            read_message(mailbox.expected[k], round, f);
        break;
    case FORM_WAIT_ANY:
    case FORM_WAIT_ANY_VECTOR:
    case FORM_TEST_ANY:
    case FORM_TEST_ANY_VECTOR:
        while (waiting) {
            size_t i;
            if (f == FORM_WAIT_ANY)
                i = shmem_uint64_wait_until_any(up, (size_t)n, mailbox.status, forms[f].cmp, value);
            else if (f == FORM_WAIT_ANY_VECTOR)
                i = shmem_uint64_wait_until_any_vector(up, (size_t)n, mailbox.status,
                                                       forms[f].cmp, mailbox.values);
            else if (f == FORM_TEST_ANY)
                i = shmem_uint64_test_any(up, (size_t)n, mailbox.status, forms[f].cmp, value);
            else
                i = shmem_uint64_test_any_vector(up, (size_t)n, mailbox.status, forms[f].cmp,
                                                 mailbox.values);
            if (i == SIZE_MAX) {
                work();
                continue;
            }
            read_message((int)i, round, f);
            waiting--;
        }
        break;
    case FORM_WAIT_SOME:
    case FORM_WAIT_SOME_VECTOR:
    case FORM_TEST_SOME:
    case FORM_TEST_SOME_VECTOR:
        while (waiting) {
            size_t got;
            if (f == FORM_WAIT_SOME)
                got = shmem_uint64_wait_until_some(up, (size_t)n, mailbox.indices, mailbox.status,
                                                   forms[f].cmp, value);
            else if (f == FORM_WAIT_SOME_VECTOR)
                got = shmem_uint64_wait_until_some_vector(up, (size_t)n, mailbox.indices,
                                                          mailbox.status, forms[f].cmp,
                                                          mailbox.values);
            else if (f == FORM_TEST_SOME)
                got = shmem_uint64_test_some(up, (size_t)n, mailbox.indices, mailbox.status,
                                             forms[f].cmp, value);
            else
                got = shmem_uint64_test_some_vector(up, (size_t)n, mailbox.indices,
                                                    mailbox.status, forms[f].cmp, mailbox.values);
            if (got == 0) {
                work();
                continue;
            }
            for (size_t j = 0; j < got; j++)
                read_message((int)mailbox.indices[j], round, f);
            waiting -= got;
        }
        break;
    default:                        /* one slot at a time, in order */
        for (int k = 0; k < count; k++) {
            int s = mailbox.expected[k];
            if (f == FORM_SIGNAL_WAIT)
                (void)shmem_signal_wait_until(&up[s], forms[f].cmp, value);
            else if (f == FORM_SIGNAL_FETCH)
                while (shmem_signal_fetch(&up[s]) < value)
                    work();
            else if (forms[f].down)
                shmem_uint64_wait_until(&mailbox.down[s], forms[f].cmp,
                                        forms[f].cmp == SHMEM_CMP_LT ? mailbox.last_down[s]
                                                                     : mailbox.last_down[s] - 1);
            else
                shmem_uint64_wait_until(&up[s], forms[f].cmp, value);
            read_message(s, round, f);
        }
        break;
    }
    /* Waiting is the phase's time less the reading inside it. */
    mailbox.phase[1] += pacer_now() - started - (mailbox.phase[2] - reading);
}

static void mailbox_step(long units, int last)
{
    int offset[MAXIMUM_SENDS];
    mailbox.last = last;
    for (long u = 0; u < units; u++, mailbox.round++) {
        offsets(mailbox.round, offset);
        send_round(mailbox.round, offset);
        receive_round(mailbox.round, offset);
    }
}

static int by_value(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void mailbox_checkpoint(struct pacer_counters *counters)
{
    /* The time in the mailbox, node-mates only: each PE's median and 99th
     * percentile, the slowest PE's; none claimed where no PE had a node-mate. */
    double *latency = mailbox.latency;
    latency[0] = latency[1] = -1;
    if (mailbox.sample_count) {
        qsort(mailbox.samples, (size_t)mailbox.sample_count, sizeof *mailbox.samples, by_value);
        latency[0] = mailbox.samples[mailbox.sample_count / 2];
        latency[1] = mailbox.samples[(mailbox.sample_count * 99) / 100];
    }
    shmem_double_max_reduce(mailbox.team, latency + 2, latency, 2);
    if (mailbox.me == 0 && latency[2] >= 0) {
        printf("mailbox node_mate_us median=%.1f p99=%.1f\n", latency[2], latency[3]);
        fflush(stdout);
    }
    mailbox.sample_count = mailbox.sample_seen = 0;

    /* The pairwise count, at the end: what each PE says it sent here, against
     * what arrived. */
    if (mailbox.last) {
        shmem_uint64_alltoall(mailbox.team, mailbox.claimed, mailbox.sent_to, 1);
        for (int s = 0; s < mailbox.n; s++)
            if (mailbox.claimed[s] != mailbox.received[s])
                pacer_failure("mailbox round=%ld count: PE %d sent %llu here, %llu were read",
                              mailbox.round - 1, mailbox.world[s],
                              (unsigned long long)mailbox.claimed[s],
                              (unsigned long long)mailbox.received[s]);
    }
    counters->work = mailbox.messages;
    counters->bytes = mailbox.bytes;
    for (int k = 0; k < 3; k++)
        counters->phase_seconds[k] = mailbox.phase[k];
    mailbox.messages = mailbox.bytes = 0;
    memset(mailbox.phase, 0, sizeof mailbox.phase);
}

static void mailbox_teardown(void)
{
    shmem_quiet();                  /* the last acknowledgements */
    shmem_team_sync(mailbox.team);
    shmem_free(mailbox.latency);
    shmem_free(mailbox.claimed);
    shmem_free(mailbox.sent_to);
    shmem_free(mailbox.acknowledged);
    shmem_free(mailbox.down);
    shmem_free(mailbox.up);
    shmem_free(mailbox.slots);
    for (int k = 0; k < MAXIMUM_SENDS; k++)
        free(mailbox.message[k]);
    free(mailbox.received);
    free(mailbox.last_down);
    free(mailbox.last_sent);
    free(mailbox.indices);
    free(mailbox.values);
    free(mailbox.status);
    free(mailbox.node_mate);
    free(mailbox.world);
}

const struct pacer_exercise pacer_mailbox = {
    .name = "mailbox",
    .work = "messages",
    .phases = phase_names,
    .quick_rounds = FORM_COUNT,          /* every form once */
    .memory = mailbox_memory,
    .setup = mailbox_setup,
    .step = mailbox_step,
    .checkpoint = mailbox_checkpoint,
    .teardown = mailbox_teardown,
};
