/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Detecting a program's own deadlock, by proof (deadlock.h, §3.15).
 *
 * The count of each PE's waits (osoe_wait_sleeps, osoe_wait_ends) runs on
 * whichever of the PE's threads waits. Everything else runs on a node lead's
 * agent thread - the samples and the root's rounds from its loop
 * (osoe_deadlock_tick), the answers to the root from its engine's pass
 * (osoe_deadlock_message) - so the state below it has one thread and no locks.
 */
#define _GNU_SOURCE
#include "deadlock.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/deadline.h"
#include "base/environment.h"
#include "base/sweep.h"
#include "blocked.h"
#include "fatal.h"
#include "node.h"
#include "state.h"
#include "transport.h"

enum { DEFAULT_SECONDS = 10, MAXIMUM_SECONDS = 86400 };

/* How long the root's own node must look quiet before a round begins. A round
 * costs every node a sample, so a root that waits often - in a barrier while
 * other nodes compute - begins one at most this often; and after each round
 * that fails, twice as long, up to OSOE_DEADLOCK_SECONDS, until the root's
 * node is seen to move. A job whose root's PEs each keep a thread waiting
 * while another computes looks quiet at every look, and pays a round each
 * OSOE_DEADLOCK_SECONDS at most. */
#define WATCH_NS (UINT64_C(1) * 1000000000)
/* How long a half of a sample waits for a thread to fall asleep or an
 * acknowledgement to come, looking each millisecond: a sweep's wake lasts
 * microseconds and a round trip across Ethernet a fraction of a millisecond.
 * A thread that computes does not stop, and a longer wait would only hold up
 * the node's engine, which sleeps between the looks. */
#define SAMPLE_PATIENCE_NS (UINT64_C(5) * 1000000)
/* How long the root waits for every node to say where its PEs wait before it
 * ends the job regardless: a node that cannot answer cannot be helped by more. */
#define DECLARE_GRACE_NS (UINT64_C(3) * 1000000000)
/* How long the root waits for every node's answer to a probe before it gives
 * the round up and watches again (P25): a node drops a probe it cannot take - it
 * is still answering the last, or its control wire is closing - and a
 * collection that waited for that answer waited for the job's life. Given up,
 * it costs a round, as a node seen to move does; a late report is the old
 * round's, and dropped. */
#define COLLECTION_GRACE_NS (UINT64_C(3) * 1000000000)
/* OSOE_TEST_DEADLOCK_DROPS's most: probes a node drops, for a test. */
#define MOST_PROBES_DROPPED 16

/* A PE slot's `waits` word (node.h): threads asleep in the low half, changes
 * in the high. */
#define WAITS_CHANGE (UINT64_C(1) << 32)

static uint64_t quiet_needed_ns;        /* OSOE_DEADLOCK_SECONDS; 0: off */
static uint32_t probes_to_drop;         /* OSOE_TEST_DEADLOCK_DROPS, still to drop */

static void start_afresh(void);

void osoe_deadlock_init(void)
{
    /* A program may finalize and initialize again: nothing of the last
     * session's rounds - a probe half answered, a collection half heard -
     * survives into this one's, whose connections and engine are new. */
    start_afresh();
    uint32_t seconds = DEFAULT_SECONDS;
    enum osoe_environment_status status = osoe_environment_u32("OSOE_DEADLOCK_SECONDS", &seconds);
    if (status == OSOE_ENVIRONMENT_INVALID || seconds > MAXIMUM_SECONDS)
        osoe_fatal("OSOE_DEADLOCK_SECONDS=%s: 0 to turn deadlock detection off, or seconds "
                   "up to %d", getenv("OSOE_DEADLOCK_SECONDS"), MAXIMUM_SECONDS);
    quiet_needed_ns = (uint64_t)seconds * 1000000000;
    probes_to_drop = 0;
    status = osoe_environment_u32("OSOE_TEST_DEADLOCK_DROPS", &probes_to_drop);
    if (status == OSOE_ENVIRONMENT_INVALID || probes_to_drop > MOST_PROBES_DROPPED)
        osoe_fatal("OSOE_TEST_DEADLOCK_DROPS='%s' is not a number of probes from 0 to %d",
                   getenv("OSOE_TEST_DEADLOCK_DROPS"), MOST_PROBES_DROPPED);
}

/* ---- Each PE's waits (base/sweep.h) --------------------------------------- */

static _Thread_local bool counted;      /* this thread is counted as asleep in a wait */

void osoe_wait_sleeps(void)
{
    if (counted || !osoe.node.slots)
        return;
    counted = true;
    atomic_fetch_add_explicit(&osoe.node.slots[osoe.bootstrap.local_id].waits,
                              WAITS_CHANGE + 1, memory_order_seq_cst);
}

void osoe_wait_ends(void)
{
    if (!counted)
        return;
    counted = false;
    atomic_fetch_add_explicit(&osoe.node.slots[osoe.bootstrap.local_id].waits,
                              WAITS_CHANGE - 1, memory_order_seq_cst);
}

/* ---- A node's sample -------------------------------------------------------- */

/*
 * Whether thread `tid` of PE `s` is one of its node's progress helpers (v0.2
 * step 4, Part 3): threads of the lead's process, each naming itself in its
 * owner record (node.h, layout 36) before its first turn. One not yet named is
 * counted as the program's, and a sample that sees it awake fails, as it does
 * for any thread of the program's that runs.
 */
static bool progress_helper(const struct osoe_pe_slot *s, int32_t tid)
{
    if (s != &osoe.node.slots[0])
        return false;
    for (uint32_t o = 1; o < osoe.node.owner_count; o++)
        if (atomic_load_explicit(&osoe.node.owner_metrics[o].tid, memory_order_acquire) == tid)
            return true;
    return false;
}

/*
 * How many of PE `s`'s threads are the application's, if every one of them
 * sleeps; -1 when one does not - it runs, it was woken and has not run yet, it
 * is stopped or traced, it waits on a disk - or /proc cannot say. The
 * application's threads are every task but the PE's agent, the lead's progress
 * helpers, and io_uring's workers, which the kernel runs as threads of the
 * process that submits and marks PF_IO_WORKER in the flags /proc gives: by the
 * flag, not by the "iou-" name, which a thread of the program's may also take.
 */
#define TASK_IO_WORKER 0x10u           /* the kernel's PF_IO_WORKER */
static int threads_asleep(const struct osoe_pe_slot *s)
{
    int32_t agent = atomic_load_explicit(&s->agent_tid, memory_order_acquire);
    char path[64];
    snprintf(path, sizeof path, "/proc/%" PRId32 "/task", s->pid);
    DIR *tasks = opendir(path);
    if (!tasks)
        return -1;
    int count = 0;
    for (struct dirent *e; count >= 0 && (e = readdir(tasks));) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || atoi(e->d_name) == agent ||
            progress_helper(s, atoi(e->d_name)))
            continue;
        char stat_path[320], line[512];     /* d_name is at most 255 bytes */
        snprintf(stat_path, sizeof stat_path, "/proc/%" PRId32 "/task/%s/stat", s->pid,
                 e->d_name);
        FILE *f = fopen(stat_path, "r");
        if (!f) {
            count = -1;                 /* a thread that has just ended: something runs */
            break;
        }
        size_t n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        line[n] = '\0';
        /* "tid (comm) S ppid pgrp session tty tpgid flags ...": the command may
         * hold spaces and parentheses, so the fields are read after its last. */
        const char *close = strrchr(line, ')');
        char state;
        unsigned flags;
        if (!close || sscanf(close + 1, " %c %*d %*d %*d %*d %*d %u", &state, &flags) != 2)
            count = -1;
        else if (flags & TASK_IO_WORKER)
            continue;
        else if (state != 'S')
            count = -1;
        else
            count++;
    }
    closedir(tasks);
    return count;
}

/*
 * A sample in progress. Its first half has read the words; `seen` says it has
 * also seen, through /proc, every thread asleep in a wait, and that the pass
 * after it has run. A half that sees a thread awake, or the network owed an
 * acknowledgement, looks again a millisecond later - the thread may be a
 * sweep's, awake for microseconds, and the acknowledgement a round trip away -
 * until `deadline`, SAMPLE_PATIENCE_NS on, as long as nothing moves; so the
 * interval grows, and the argument, which asks only that nothing moved within
 * it, is unchanged.
 */
static struct {
    bool taken;                 /* a sample is in progress */
    bool seen;                  /* its first half is done: the second is next */
    uint64_t activity;          /* the node's activity when it began */
    uint64_t deadline_ns;
} half;

static uint64_t *words;         /* each local PE's `waits`, as a half last read them */
static int *asleep;             /* and its threads as /proc listed them */
/* Each local PE's `sweeps` when this node's first sample of a round held. */
static uint64_t *first_sweeps;

/*
 * Whether every waiting thread has swept since the first sample: each PE's
 * sweep count (node.h) risen by at least the threads it has waiting, which a
 * sweep interval's sleeps make certain many times over within
 * OSOE_DEADLOCK_SECONDS. The argument's premise, checked rather than assumed:
 * a condition made true with no wake is found by its waiter at its next sweep
 * - unless the waiter's timer is so slack, or the PE so frozen, that it never
 * sweeps, and then nothing is declared.
 */
static bool record_sweeps(void)
{
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
        first_sweeps[i] = atomic_load_explicit(&osoe.node.slots[i].sweeps, memory_order_seq_cst);
    return true;
}

static bool every_waiter_swept(void)
{
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++) {
        uint64_t now = atomic_load_explicit(&osoe.node.slots[i].sweeps, memory_order_seq_cst);
        if (now - first_sweeps[i] < (uint32_t)words[i])
            return false;
    }
    return true;
}

/* Every local PE's `waits` word, its changes summed: one load each. */
static uint64_t read_waits(void)
{
    uint64_t changes = 0;
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++) {
        words[i] = atomic_load_explicit(&osoe.node.slots[i].waits, memory_order_seq_cst);
        changes += words[i] >> 32;
    }
    return changes;
}

/* Whether every local PE has a thread asleep in a wait, by the words alone. */
static bool every_pe_waits(void)
{
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
        if ((uint32_t)words[i] == 0)
            return false;
    return true;
}

/* Every local PE's application threads through /proc, all asleep. */
static bool read_threads(void)
{
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
        if ((asleep[i] = threads_asleep(&osoe.node.slots[i])) < 1)
            return false;
    return true;
}

/* Whether /proc's threads and the words' waits are the same threads. */
static bool threads_are_waits(void)
{
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
        if ((uint32_t)asleep[i] != (uint32_t)words[i])
            return false;
    return true;
}

/*
 * Begin a sample: the node's activity, from the words, which are read first
 * here and last in the second half, so the interval the halves bound runs from
 * this read to that one. False when some PE has no thread waiting at all, and
 * the sample has failed at once.
 */
static bool first_words(void)
{
    uint64_t changes = read_waits();
    if (!every_pe_waits())
        return false;
    uint64_t applied;
    /* Asks the peers for the acknowledgements quiet needs, which the pass sends. */
    (void)osoe_transport_agent_node_quiet(&applied);
    half.activity = changes + applied;
    half.taken = true;
    half.seen = false;
    half.deadline_ns = osoe_now_ns() + SAMPLE_PATIENCE_NS;
    return true;
}

/*
 * Go on with the sample in progress, on a turn of the agent's loop: the first
 * half's look through /proc - every PE's threads asleep, and as many as its
 * word counts in waits, so that none the words cannot see exists - and, a pass
 * later, the second half: /proc again, the network quiet, and the activity as
 * it began, read last. SAMPLE_GOING while it must look again.
 */
enum sample_result { SAMPLE_GOING, SAMPLE_HOLDS, SAMPLE_FAILS };

static enum sample_result go_on(void)
{
    /* A thread awake may be a sweep's; one asleep outside every wait is a
     * thread the library cannot see, and stays so. */
    if (!half.seen) {
        if (read_threads()) {
            if (!threads_are_waits())
                return SAMPLE_FAILS;
            half.seen = true;           /* the pass follows this turn's tick */
            return SAMPLE_GOING;
        }
    } else {
        bool threads = read_threads();
        uint64_t applied;
        bool quiet = osoe_transport_agent_node_quiet(&applied);
        uint64_t now = read_waits() + applied;
        if (now != half.activity || (threads && !threads_are_waits()))
            return SAMPLE_FAILS;
        if (threads && quiet)
            return SAMPLE_HOLDS;
    }
    if (osoe_now_ns() > half.deadline_ns)
        return SAMPLE_FAILS;
    struct timespec pause = { 0, 1000000 };
    nanosleep(&pause, NULL);
    return SAMPLE_GOING;
}

/* ---- Saying where each PE waits -------------------------------------------- */

static void describe(const struct osoe_blocked_copy *w, char *out, size_t size)
{
    switch (w->kind) {
    case OSOE_BLOCKED_DETAIL_NODE:
        snprintf(out, size, " (node %" PRIu64 ")", w->detail);
        break;
    case OSOE_BLOCKED_DETAIL_EPOCH:
        snprintf(out, size, " (epoch %" PRIu64 ")", w->detail);
        break;
    case OSOE_BLOCKED_DETAIL_OFFSET:
        snprintf(out, size, " (heap +%#" PRIx64 ")", w->detail);
        break;
    case OSOE_BLOCKED_DETAIL_STATICS_OFFSET:
        snprintf(out, size, " (statics +%#" PRIx64 ")", w->detail);
        break;
    case OSOE_BLOCKED_DETAIL_PE:
        snprintf(out, size, " (PE %" PRIu64 ")", w->detail);
        break;
    case OSOE_BLOCKED_DETAIL_NONE:
        out[0] = '\0';
        break;
    default:
        snprintf(out, size, " (detail %" PRIu64 ", kind %" PRIu32 ")", w->detail, w->kind);
        break;
    }
}

/*
 * Every local PE, those waiting at the same place on one line: O(L^2) for L
 * local PEs, once, as the job ends. One write a line, so lines from different
 * processes do not interleave within one.
 */
static void report_local(void)
{
    uint32_t count = osoe.bootstrap.local_count;
    struct osoe_blocked_copy *w = calloc(count, sizeof *w);
    bool *said = calloc(count, sizeof *said);
    if (!w || !said) {
        free(w);
        free(said);
        return;
    }
    const char *host = osoe.node.hdr ? osoe.node.hdr->hostname : "?";
    uint64_t now = osoe_now_ns();
    for (uint32_t i = 0; i < count; i++)
        if (!osoe_blocked_read(&osoe.node.slots[i], &w[i]) || !w[i].routine[0])
            snprintf(w[i].routine, sizeof w[i].routine, "a wait it did not record");
    for (uint32_t i = 0; i < count; i++) {
        if (said[i])
            continue;
        char line[1024], detail[96];
        describe(&w[i], detail, sizeof detail);
        int used = snprintf(line, sizeof line, "osoe: deadlock: %s: PE %" PRIu32, host,
                            osoe.node.slots[i].rank);
        for (uint32_t j = i + 1; j < count; j++)
            if (!said[j] && w[j].kind == w[i].kind && w[j].detail == w[i].detail &&
                strcmp(w[j].routine, w[i].routine) == 0) {
                said[j] = true;
                if (used > 0 && (size_t)used < sizeof line - 32)
                    used += snprintf(line + used, sizeof line - (size_t)used, ", %" PRIu32,
                                     osoe.node.slots[j].rank);
            }
        uint64_t seconds = w[i].since_ns && now > w[i].since_ns
                               ? (now - w[i].since_ns) / 1000000000 : 0;
        if (used > 0 && (size_t)used < sizeof line)
            snprintf(line + used, sizeof line - (size_t)used, " in %s%s, for %" PRIu64 " s\n",
                     w[i].routine, detail, seconds);
        fputs(line, stderr);
    }
    fflush(stderr);
    free(w);
    free(said);
}

/* ---- The root's rounds, and every node's answers -------------------------- */

enum stage {
    WATCHING,                   /* the root looks at its own node once a sweep interval */
    FIRST_SAMPLE,               /* its first sample is half taken */
    FIRST_COLLECTION,           /* every other node's first sample is asked for */
    BETWEEN,                    /* every node was quiet: OSOE_DEADLOCK_SECONDS pass */
    SECOND_SAMPLE,
    SECOND_COLLECTION,
    DECLARING,                  /* deadlocked: every node says where its PEs wait */
    ENDED,
};

static enum stage stage;
static uint32_t round_number, reports, declared;
static bool all_ok;
static uint64_t next_look_ns, quiet_since_ns, first_activity, between_since_ns, declared_at_ns;
static uint64_t collecting_since_ns;    /* when the collection out was asked for */
static uint32_t rounds_failed;          /* since the root's node was last seen to move */

/* A node answering the root: the probe it answers, and its round's first sample. */
static bool answering;
static struct osoe_deadlock_message probe;
static uint32_t probe_from, sampled_round;
static uint64_t sampled_first;

static void start_afresh(void)
{
    stage = WATCHING;
    round_number = reports = declared = 0;
    all_ok = false;
    next_look_ns = quiet_since_ns = first_activity = between_since_ns = declared_at_ns = 0;
    collecting_since_ns = 0;
    rounds_failed = 0;
    answering = false;
    probe = (struct osoe_deadlock_message){ 0 };
    probe_from = sampled_round = 0;
    sampled_first = 0;
    half.taken = half.seen = false;
    free(words);
    free(asleep);
    free(first_sweeps);
    words = NULL;
    asleep = NULL;
    first_sweeps = NULL;
}

static uint32_t peers(void)
{
    return osoe.job.node_count > 0 ? osoe.job.node_count - 1 : 0;
}

static void send_all(uint16_t kind, uint16_t collection)
{
    struct osoe_deadlock_message m = { .round = round_number, .kind = kind,
                                       .phase = collection };
    for (uint32_t node = 1; node < osoe.job.node_count; node++)
        osoe_transport_agent_deadlock_send(node, &m);
}

static void watch_again(void)
{
    stage = WATCHING;
    quiet_since_ns = 0;
    if (rounds_failed < 32)
        rounds_failed++;
}

/* How long the root's node must look quiet before the next round (WATCH_NS). */
static uint64_t watch_ns(void)
{
    uint64_t most = quiet_needed_ns > WATCH_NS ? quiet_needed_ns : WATCH_NS;
    uint64_t wait = WATCH_NS;
    for (uint32_t i = 0; i < rounds_failed && wait < most; i++)
        wait *= 2;
    return wait < most ? wait : most;
}

static void end_job(void)
{
    stage = ENDED;
    osoe_node_declare_abort(&osoe.node, osoe.bootstrap.rank, 1, OSOE_ABORT_FATAL);
}

static void declare(uint64_t now)
{
    stage = DECLARING;
    declared = 0;
    declared_at_ns = now;
    char header[512];
    snprintf(header, sizeof header,
             "osoe: deadlock: every PE of the job waits in the library, and nothing has "
             "moved on any of its %" PRIu32 " node%s for %" PRIu64 " s, nor is anything "
             "in flight between them: no PE can ever be woken, so the job ends "
             "(OSOE_DEADLOCK_SECONDS=0 turns this off). Where each PE waits:\n",
             osoe.job.node_count, osoe.job.node_count == 1 ? "" : "s",
             quiet_needed_ns / 1000000000);
    fputs(header, stderr);
    report_local();
    if (peers() == 0)
        end_job();
    else
        send_all(OSOE_DEADLOCK_DECLARE, 0);
}

/* A sample is taken: the answer to the root, or the root's own next step. */
static void sampled(bool ok, uint64_t activity)
{
    if (answering) {
        answering = false;
        struct osoe_deadlock_message report = { .round = probe.round,
                                                .kind = OSOE_DEADLOCK_REPORT,
                                                .phase = probe.phase };
        if (probe.phase == 1) {
            sampled_round = probe.round;
            sampled_first = activity;
            report.ok = ok && record_sweeps();
        } else {
            report.ok = ok && sampled_round == probe.round && activity == sampled_first &&
                        every_waiter_swept();
        }
        osoe_transport_agent_deadlock_send(probe_from, &report);
        return;
    }
    if (stage == FIRST_SAMPLE) {
        if (!ok) {
            watch_again();
            return;
        }
        first_activity = activity;
        (void)record_sweeps();
        round_number++;
        if (peers() == 0) {
            stage = BETWEEN;
            between_since_ns = osoe_now_ns();
            return;
        }
        stage = FIRST_COLLECTION;
        reports = 0;
        all_ok = true;
        collecting_since_ns = osoe_now_ns();
        send_all(OSOE_DEADLOCK_PROBE, 1);
    } else if (stage == SECOND_SAMPLE) {
        if (!ok || activity != first_activity || !every_waiter_swept()) {
            watch_again();
            return;
        }
        if (peers() == 0) {
            declare(osoe_now_ns());
            return;
        }
        stage = SECOND_COLLECTION;
        reports = 0;
        all_ok = true;
        collecting_since_ns = osoe_now_ns();
        send_all(OSOE_DEADLOCK_PROBE, 2);
    }
}

/* Begin a sample; true while it is in progress. */
static bool begin_sample(void)
{
    if (first_words())
        return true;
    sampled(false, 0);
    return false;
}

bool osoe_deadlock_tick(void)
{
    if (!quiet_needed_ns || stage == ENDED)
        return false;
    if (!words) {
        words = calloc(osoe.bootstrap.local_count, sizeof *words);
        asleep = calloc(osoe.bootstrap.local_count, sizeof *asleep);
        first_sweeps = calloc(osoe.bootstrap.local_count, sizeof *first_sweeps);
        if (!words || !asleep || !first_sweeps)
            osoe_fatal("the deadlock detector: out of memory for %" PRIu32 " local PEs",
                       osoe.bootstrap.local_count);
    }
    /* A sample in progress: each look on its own turn, a pass between. */
    if (half.taken) {
        enum sample_result r = go_on();
        if (r == SAMPLE_GOING)
            return true;
        half.taken = false;
        sampled(r == SAMPLE_HOLDS, half.activity);
        return false;
    }
    if (answering)
        return begin_sample();
    if (osoe.job.node != 0)
        return false;

    uint64_t now = osoe_now_ns();
    if (now < next_look_ns)
        return false;
    next_look_ns = now + OSOE_SWEEP_NS;
    uint64_t applied;
    switch (stage) {
    case WATCHING:
        /* Cheap: the words and the engine's idleness, no /proc and no asking. */
        (void)read_waits();
        if (!every_pe_waits() || !osoe_transport_agent_node_idle(&applied)) {
            quiet_since_ns = 0;
            rounds_failed = 0;              /* seen to move */
            return false;
        }
        if (!quiet_since_ns)
            quiet_since_ns = now;
        if (now - quiet_since_ns < watch_ns())
            return false;
        stage = FIRST_SAMPLE;
        return begin_sample();
    case BETWEEN:
        /* Give up early once the root's own node has moved: seen to move. */
        (void)osoe_transport_agent_node_idle(&applied);
        if (read_waits() + applied != first_activity) {
            watch_again();
            rounds_failed = 0;
            return false;
        }
        if (now - between_since_ns < quiet_needed_ns)
            return false;
        stage = SECOND_SAMPLE;
        return begin_sample();
    case FIRST_COLLECTION:
    case SECOND_COLLECTION:
        /* Its reports move it - unless one never comes (COLLECTION_GRACE_NS). */
        if (now - collecting_since_ns > COLLECTION_GRACE_NS)
            watch_again();
        return false;
    case DECLARING:
        if (declared >= peers() || now - declared_at_ns > DECLARE_GRACE_NS)
            end_job();
        return false;
    default:
        return false;
    }
}

void osoe_deadlock_message(uint32_t node, const struct osoe_deadlock_message *m)
{
    switch (m->kind) {
    /*
     * What the protocol never sends - a probe to the root or to a node still
     * answering the last, a report the root did not ask for - is dropped, not
     * fatal: the detector may fail to find a deadlock, and must never end a
     * job that is not one. The tests that ask for a declaration are what
     * would see such a message lose one.
     */
    case OSOE_DEADLOCK_PROBE:
        /* Sampled from the agent's loop: its first half on the next turn. */
        if (osoe.job.node == 0 || answering || half.taken)
            break;
        if (probes_to_drop > 0) {
            probes_to_drop--;           /* OSOE_TEST_DEADLOCK_DROPS: as a busy node does */
            break;
        }
        if (!quiet_needed_ns) {
            struct osoe_deadlock_message veto = { .round = m->round,
                                                  .kind = OSOE_DEADLOCK_REPORT,
                                                  .phase = m->phase };
            osoe_transport_agent_deadlock_send(node, &veto);
            return;
        }
        answering = true;
        probe = *m;
        probe_from = node;
        break;
    case OSOE_DEADLOCK_REPORT: {
        enum stage collecting = m->phase == 1 ? FIRST_COLLECTION : SECOND_COLLECTION;
        if (osoe.job.node != 0 || m->round != round_number || stage != collecting)
            break;
        reports++;
        all_ok = all_ok && m->ok;
        if (reports < peers())
            break;
        if (!all_ok)
            watch_again();
        else if (stage == FIRST_COLLECTION) {
            stage = BETWEEN;
            between_since_ns = osoe_now_ns();
        } else {
            declare(osoe_now_ns());
        }
        break;
    }
    case OSOE_DEADLOCK_DECLARE: {
        report_local();
        struct osoe_deadlock_message answer = { .round = m->round,
                                                .kind = OSOE_DEADLOCK_DECLARED };
        osoe_transport_agent_deadlock_send(node, &answer);
        break;
    }
    case OSOE_DEADLOCK_DECLARED:
        if (osoe.job.node == 0 && stage == DECLARING)
            declared++;
        break;
    default:
        osoe_fatal("a deadlock detector message of kind %u from node %" PRIu32, m->kind, node);
    }
}
