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
 * other nodes compute - begins one at most this often. */
#define WATCH_NS (UINT64_C(1) * 1000000000)
/* How long the root waits for every node to say where its PEs wait before it
 * ends the job regardless: a node that cannot answer cannot be helped by more. */
#define DECLARE_GRACE_NS (UINT64_C(3) * 1000000000)

/* A PE slot's `waits` word (node.h): threads asleep in the low half, changes
 * in the high. */
#define WAITS_CHANGE (UINT64_C(1) << 32)

static uint64_t quiet_needed_ns;        /* OSOE_DEADLOCK_SECONDS; 0: off */

void osoe_deadlock_init(void)
{
    uint32_t seconds = DEFAULT_SECONDS;
    enum osoe_environment_status status = osoe_environment_u32("OSOE_DEADLOCK_SECONDS", &seconds);
    if (status == OSOE_ENVIRONMENT_INVALID || seconds > MAXIMUM_SECONDS)
        osoe_fatal("OSOE_DEADLOCK_SECONDS=%s: 0 to turn deadlock detection off, or seconds "
                   "up to %d", getenv("OSOE_DEADLOCK_SECONDS"), MAXIMUM_SECONDS);
    quiet_needed_ns = (uint64_t)seconds * 1000000000;
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
 * How many of PE `s`'s threads are the application's, if every one of them
 * sleeps; -1 when one does not - it runs, it was woken and has not run yet, it
 * is stopped or traced, it waits on a disk - or /proc cannot say. The
 * application's threads are every task but the PE's agent and io_uring's
 * workers, which the kernel runs as threads of the process that submits
 * ("iou-...").
 */
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
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || atoi(e->d_name) == agent)
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
        /* "tid (comm) S ...": the command may hold spaces and parentheses. */
        const char *open = strchr(line, '('), *close = strrchr(line, ')');
        if (!open || !close || close[1] != ' ')
            count = -1;
        else if (strncmp(open + 1, "iou-", 4) == 0)
            continue;
        else if (close[2] != 'S')
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
 * until `deadline`, as long as nothing moves; so the interval grows, and the
 * argument, which asks only that nothing moved within it, is unchanged.
 */
static struct {
    bool taken;                 /* a sample is in progress */
    bool seen;                  /* its first half is done: the second is next */
    uint64_t activity;          /* the node's activity when it began */
    uint64_t deadline_ns;
} half;

static uint64_t *words;         /* each local PE's `waits`, as a half last read them */
static int *asleep;             /* and its threads as /proc listed them */

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
    uint64_t applied;
    /* Asks the peers for the acknowledgements quiet needs, which the pass sends. */
    (void)osoe_transport_agent_node_quiet(&applied);
    half.activity = read_waits() + applied;
    if (!every_pe_waits())
        return false;
    half.taken = true;
    half.seen = false;
    half.deadline_ns = osoe_now_ns() + OSOE_SWEEP_NS;
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

struct waiting {
    char routine[OSOE_BLOCKED_ROUTINE_MAX];
    uint32_t kind;
    uint64_t detail, since_ns;
};

/* A PE's "blocked in" record, read as osoe-stat reads it: the generation even
 * and unchanged around the fields (blocked.h). */
static bool read_waiting(const struct osoe_pe_slot *s, struct waiting *w)
{
    for (int attempt = 0; attempt < 100; attempt++) {
        uint64_t before = atomic_load_explicit(&s->blocked_gen, memory_order_seq_cst);
        if (before & 1)
            continue;
        memcpy(w->routine, s->blocked_routine, sizeof w->routine);
        w->routine[sizeof w->routine - 1] = '\0';
        w->kind = atomic_load_explicit(&s->blocked_detail_kind, memory_order_acquire);
        w->detail = atomic_load_explicit(&s->blocked_detail, memory_order_acquire);
        w->since_ns = atomic_load_explicit(&s->blocked_since_ns, memory_order_acquire);
        if (atomic_load_explicit(&s->blocked_gen, memory_order_seq_cst) == before)
            return true;
    }
    return false;
}

static void describe(const struct waiting *w, char *out, size_t size)
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
    struct waiting *w = calloc(count, sizeof *w);
    bool *said = calloc(count, sizeof *said);
    if (!w || !said) {
        free(w);
        free(said);
        return;
    }
    const char *host = osoe.node.hdr ? osoe.node.hdr->hostname : "?";
    uint64_t now = osoe_now_ns();
    for (uint32_t i = 0; i < count; i++)
        if (!read_waiting(&osoe.node.slots[i], &w[i]) || !w[i].routine[0])
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

/* A node answering the root: the probe it answers, and its round's first sample. */
static bool answering;
static struct osoe_deadlock_message probe;
static uint32_t probe_from, sampled_round;
static uint64_t sampled_first;

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
            report.ok = ok;
        } else {
            report.ok = ok && sampled_round == probe.round && activity == sampled_first;
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
        round_number++;
        if (peers() == 0) {
            stage = BETWEEN;
            between_since_ns = osoe_now_ns();
            return;
        }
        stage = FIRST_COLLECTION;
        reports = 0;
        all_ok = true;
        send_all(OSOE_DEADLOCK_PROBE, 1);
    } else if (stage == SECOND_SAMPLE) {
        if (!ok || activity != first_activity) {
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
        if (!words || !asleep)
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
            return false;
        }
        if (!quiet_since_ns)
            quiet_since_ns = now;
        if (now - quiet_since_ns < WATCH_NS)
            return false;
        stage = FIRST_SAMPLE;
        return begin_sample();
    case BETWEEN:
        /* Give up early once the root's own node has moved. */
        (void)osoe_transport_agent_node_idle(&applied);
        if (read_waits() + applied != first_activity) {
            watch_again();
            return false;
        }
        if (now - between_since_ns < quiet_needed_ns)
            return false;
        stage = SECOND_SAMPLE;
        return begin_sample();
    case DECLARING:
        if (declared >= peers() || now - declared_at_ns > DECLARE_GRACE_NS)
            end_job();
        return false;
    default:
        return false;           /* a collection is out: its reports move it */
    }
}

void osoe_deadlock_message(uint32_t node, const struct osoe_deadlock_message *m)
{
    switch (m->kind) {
    case OSOE_DEADLOCK_PROBE:
        /* Sampled from the agent's loop: its first half on the next turn. */
        if (osoe.job.node == 0 || answering || half.taken)
            osoe_fatal("a deadlock detector probe from node %" PRIu32 " (round %" PRIu32
                       ") while this node's lead %s", node, m->round,
                       osoe.job.node == 0 ? "is the root" : "is still answering the last");
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
            osoe_fatal("a deadlock detector report from node %" PRIu32 " for round %" PRIu32
                       ", collection %u, which this node did not ask for", node, m->round,
                       m->phase);
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
