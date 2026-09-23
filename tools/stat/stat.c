/*
 * osoe-stat - read an osoe job's control segment from outside the job (§3.14).
 *
 * > The library writes introspection state into shared memory; an external tool
 * > reads it. The library serves nothing and writes no files.
 *
 * This is that tool, and the case it is built for is a hung job: it maps the
 * control segment read-only, needs no cooperation from the job, and works as
 * well on a job that is wedged as on one that is running. It is installed,
 * unlike everything else under tools/, because the thing it is for happens on
 * someone else's machine at three in the morning.
 *
 * It is written against the *segment*, not the library: segment.h explains why,
 * and enforces it by including nothing from shmem/src. Every value here is
 * found through the metric table by name. The two exceptions are marked: the
 * frozen directory, and the §4.9 watch word, whose encoding is part of what the
 * directory's `layout` version covers - so it is decoded only for the layout
 * this tool knows, and printed raw otherwise.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "segment.h"

enum mode { MODE_LIST, MODE_COUNTERS, MODE_THREADS, MODE_DUMP, MODE_GC, MODE_SUMMARY };

static bool opt_json;
static bool opt_nonzero;
static unsigned opt_interval;           /* seconds; 0 is once */

/* ---- small helpers ---------------------------------------------------- */

static const char *phase_name(uint64_t p)
{
    switch (p) {
    case 0: return "empty";
    case 1: return "joined";
    case 2: return "mapped";
    case 3: return "finalized";
    default: return "?";
    }
}

/* A NUL-terminated copy of a fixed-width name from the table. */
static void metric_name(const struct osoe_metric_descriptor *m, char *buf, size_t len)
{
    size_t n = sizeof m->name < len - 1 ? sizeof m->name : len - 1;
    memcpy(buf, m->name, n);
    buf[n] = '\0';
}

static void print_duration(char *buf, size_t len, uint64_t ns)
{
    if (ns < 1000)
        snprintf(buf, len, "%" PRIu64 " ns", ns);
    else if (ns < 1000 * 1000)
        snprintf(buf, len, "%.1f us", (double)ns / 1e3);
    else if (ns < UINT64_C(1000) * 1000 * 1000)
        snprintf(buf, len, "%.1f ms", (double)ns / 1e6);
    else
        snprintf(buf, len, "%.3f s", (double)ns / 1e9);
}

/* JSON string escaping, enough for metric names, routine names and unit
 * strings; control characters are escaped rather than emitted raw. */
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc((char)c, f);
    }
    fputc('"', f);
}

/* ---- the §4.9 watch word ---------------------------------------------- */

/*
 * The one field whose *encoding* this tool knows, rather than finding it by
 * name. The directory's `layout` version covers the encoding of every word in
 * the segment (§10), so decoding is gated on the layout this tool was built
 * against; any other layout prints the raw word and says nothing it cannot
 * support. Kept deliberately small, and a copy rather than a shared header,
 * because sharing the header would be sharing shmem/src.
 */
static void describe_watch(char *buf, size_t len, uint32_t layout, uint64_t word)
{
    if (word == 0) {
        snprintf(buf, len, "-");
        return;
    }
    if (layout != OSOE_SEGMENT_KNOWN_LAYOUT) {
        snprintf(buf, len, "%#" PRIx64 " (layout %u: encoding unknown)", word, layout);
        return;
    }
    enum { LEN_BITS = 22, OFF_BITS = 40, GRAIN = 4 };
    uint64_t len_max = (UINT64_C(1) << LEN_BITS) - 1;
    unsigned region = (unsigned)(word >> (OFF_BITS + LEN_BITS));
    uint64_t grains = word & len_max;
    uint64_t first = (word >> LEN_BITS) & ((UINT64_C(1) << OFF_BITS) - 1);
    const char *rname = region == 0 ? "heap" : region == 1 ? "internal" : "?";

    if (grains == len_max)
        snprintf(buf, len, "%s +%#" PRIx64 "..end", rname, first * GRAIN);
    else
        snprintf(buf, len, "%s +%#" PRIx64 "..+%#" PRIx64, rname, first * GRAIN,
                 first * GRAIN + grains * GRAIN);
}

/* ---- what the kernel already tracks (§3.14) --------------------------- */

/*
 * Per-thread CPU time and involuntary context switches are read here, from
 * /proc/<pid>/task/<tid>/, and not maintained by the job: §3.14's rule. The tid
 * comes from the `thread.tid` counter each block records, and the pid from the
 * owning PE's slot, so this needs nothing the segment does not already say.
 *
 * Rising involuntary switches on an agent's thread mean application threads are
 * starving it - the cost §3.12 accepts, made visible.
 */
struct thread_kernel {
    bool found;
    uint64_t cpu_ns;
    uint64_t involuntary;
};

static void read_thread_kernel(struct thread_kernel *k, int64_t pid, int64_t tid)
{
    memset(k, 0, sizeof *k);
    if (pid <= 0 || tid <= 0)
        return;

    char path[128], buf[4096];
    snprintf(path, sizeof path, "/proc/%lld/task/%lld/stat", (long long)pid, (long long)tid);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    /*
     * `comm` is parenthesised and may itself contain spaces and parentheses, so
     * proc(5) says to find the last ')' and count from there. After it come the
     * state character and then fields 4 to 15; utime and stime are the last two
     * of those twelve numbers.
     */
    char *p = strrchr(buf, ')');
    if (!p)
        return;
    p++;
    while (*p == ' ')
        p++;
    while (*p && *p != ' ')             /* the state character */
        p++;

    long long v[12];
    int got = 0;
    while (got < 12 && *p) {
        char *end;
        long long x = strtoll(p, &end, 10);
        if (end == p)
            break;
        v[got++] = x;
        p = end;
        while (*p == ' ')
            p++;
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (got == 12 && hz > 0)
        k->cpu_ns = (uint64_t)(v[10] + v[11]) * (UINT64_C(1000000000) / (uint64_t)hz);
    else
        return;

    snprintf(path, sizeof path, "/proc/%lld/task/%lld/status", (long long)pid, (long long)tid);
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(buf, sizeof buf, f)) {
        static const char key[] = "nonvoluntary_ctxt_switches:";
        if (strncmp(buf, key, sizeof key - 1) == 0) {
            k->involuntary = strtoull(buf + sizeof key - 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    k->found = true;
}

/* ---- aggregation ------------------------------------------------------ */

/*
 * A PE's value for a thread-scope metric is the sum of its blocks for anything
 * that counts and the maximum for anything that does not: adding two threads'
 * last-wake timestamps or thread ids would produce a number that means nothing.
 * Kind is exactly the distinction, which is why §3.14 puts it in the table.
 */
static bool metric_sums(const struct osoe_metric_descriptor *m)
{
    return m->kind == OSOE_MKIND_COUNTER || m->kind == OSOE_MKIND_INVARIANT;
}

static uint64_t pe_thread_value(const struct osoe_segment *s, const struct osoe_metric_descriptor *m,
                                uint32_t pe)
{
    /* Clamped like local_count and block_count (D-7): more per-PE blocks than
     * the segment has blocks at all is nonsense, and blocks_per_pe drives a
     * loop the same way those two do. */
    uint32_t per_pe = s->dir->blocks_per_pe;
    if (per_pe > s->block_count)
        per_pe = s->block_count;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < per_pe; i++) {
        uint64_t v;
        uint32_t block = pe * per_pe + i;
        if (block >= s->block_count || !osoe_segment_read(s, m, block, &v))
            continue;
        if (metric_sums(m))
            acc += v;
        else if (v > acc)
            acc = v;
    }
    return acc;
}

/* ---- headers ---------------------------------------------------------- */

static void print_identity(const struct osoe_segment *s)
{
    uint64_t job = 0, step = 0, node = 0, session = 0, world = 0;
    osoe_segment_read_name(s, "node.job", 0, &job);
    osoe_segment_read_name(s, "node.step", 0, &step);
    osoe_segment_read_name(s, "node.id", 0, &node);
    osoe_segment_read_name(s, "node.session", 0, &session);
    osoe_segment_read_name(s, "node.world_size", 0, &world);

    printf("%s  layout %u  %u local PE%s of %" PRIu64 "  (job %" PRIu64 " step %" PRIu64
           " node %" PRIu64 " session %" PRIu64 ")\n",
           s->name, s->dir->layout, s->dir->local_count,
           s->dir->local_count == 1 ? "" : "s", world, job, step, node, session);
    printf("  %" PRIu64 " bytes, %u metrics (checksum %#010x), %u blocks of %u bytes\n",
           s->dir->segment_bytes, s->dir->metrics_count, s->dir->metrics_checksum,
           s->dir->block_count, s->dir->block_bytes);

    uint64_t aborting = 0, abort_pe = 0, failed = 0;
    osoe_segment_read_name(s, "node.aborting", 0, &aborting);
    osoe_segment_read_name(s, "node.abort_pe", 0, &abort_pe);
    osoe_segment_read_name(s, "node.failed", 0, &failed);
    if (aborting)
        printf("  ABORTING on behalf of PE %" PRIu64 "\n", abort_pe);
    if (failed)
        printf("  a PE failed during rendezvous\n");
}

/* One line per PE: who it is, whether it is alive, and where it is blocked. */
static void print_pe_lines(const struct osoe_segment *s)
{
    uint64_t now = osoe_segment_now_ns();
    printf("\n%3s %5s %8s %-10s %6s %8s  %s\n",
           "PE", "rank", "pid", "phase", "epoch", "sweeps", "blocked in");
    for (uint32_t i = 0; i < s->local_count; i++) {
        uint64_t rank = 0, pid = 0, phase = 0, epoch = 0, sweeps = 0;
        osoe_segment_read_name(s, "pe.rank", i, &rank);
        osoe_segment_read_name(s, "pe.pid", i, &pid);
        osoe_segment_read_name(s, "pe.phase", i, &phase);
        osoe_segment_read_name(s, "pe.world_epoch", i, &epoch);
        osoe_segment_read_name(s, "pe.sweeps", i, &sweeps);

        bool alive = (int64_t)pid > 0 && !(kill((pid_t)(int64_t)pid, 0) != 0 && errno == ESRCH);
        char where[160] = "-";
        struct osoe_segment_blocked b;
        if (osoe_segment_blocked(s, i, &b)) {
            if (!b.valid)
                snprintf(where, sizeof where, "(republishing: not blocked)");
            else if (b.blocked) {
                char age[32] = "";
                if (b.since_ns && now > b.since_ns)
                    print_duration(age, sizeof age, now - b.since_ns);
                char what[48];
                osoe_segment_blocked_detail(&b, what, sizeof what);
                snprintf(where, sizeof where, "%s (%s) for %s", b.routine, what, age);
            }
        }
        printf("%3u %5" PRIu64 " %8" PRIu64 " %-10s %6" PRIu64 " %8" PRIu64 "  %s%s\n",
               i, rank, pid, phase_name(phase), epoch, sweeps, where,
               alive || phase == 0 || phase == 3 ? "" : "   [pid is gone]");
    }
}

/*
 * One line per connection toward a peer node (§3.14's connection scope).
 * Printed after the PE lines, because "which peer" is the first question about
 * a job whose traffic is uneven or whose one link has stopped: `acked` far
 * behind `sent` is a peer that is not applying, and `wanted` ahead of `acked`
 * is a quiet still waiting for it.
 */
static void print_connection_lines(const struct osoe_segment *s)
{
    if (s->connection_count == 0)
        return;
    printf("\n%5s %10s %10s %10s %10s %7s %7s %10s %10s %10s\n",
           "peer", "seg sent", "bytes", "seg appl", "bytes", "ctrlq", "bulkq",
           "sent thru", "acked", "wanted");
    for (uint32_t c = 0; c < s->connection_count; c++) {
        uint64_t peer = 0, ss = 0, bs = 0, sa = 0, ba = 0, cq = 0, bq = 0, st = 0, ack = 0, want = 0;
        osoe_segment_read_name(s, "connection.peer", c, &peer);
        osoe_segment_read_name(s, "connection.segments_sent", c, &ss);
        osoe_segment_read_name(s, "connection.bytes_sent", c, &bs);
        osoe_segment_read_name(s, "connection.segments_applied", c, &sa);
        osoe_segment_read_name(s, "connection.bytes_applied", c, &ba);
        osoe_segment_read_name(s, "connection.control_queue_depth", c, &cq);
        osoe_segment_read_name(s, "connection.bulk_queue_depth", c, &bq);
        osoe_segment_read_name(s, "connection.sent_through", c, &st);
        osoe_segment_read_name(s, "connection.acked_through", c, &ack);
        osoe_segment_read_name(s, "connection.wanted_through", c, &want);
        printf("%5" PRIu64 " %10" PRIu64 " %10" PRIu64 " %10" PRIu64 " %10" PRIu64
               " %7" PRIu64 " %7" PRIu64 " %10" PRIu64 " %10" PRIu64 " %10" PRIu64 "\n",
               peer, ss, bs, sa, ba, cq, bq, st, ack, want);
    }
}

/* ---- the summary view -------------------------------------------------- */

/*
 * One line per node, for walking a hung collective down the tree (§3.14, §5.3).
 *
 * It answers, for each team with an internode phase: which epoch this node has
 * open, whether its own members are in, how many children have arrived, and
 * **the lowest child that has not**. That last is the next node to look at, and
 * `host` says where. A node with `own` no is one whose own PEs have not all
 * called the collective, and the PE lines of *that* node say which.
 */
static void summary_line(const struct osoe_segment *s)
{
    char host[72] = "?";
    osoe_segment_read_str(s, "node.hostname", 0, host, sizeof host);
    uint64_t node = 0, live = 0;
    osoe_segment_read_name(s, "node.id", 0, &node);
    for (uint32_t i = 0; i < s->local_count; i++) {
        uint64_t pid = 0;
        osoe_segment_read_name(s, "pe.pid", i, &pid);
        if ((int64_t)pid > 0 && !(kill((pid_t)(int64_t)pid, 0) != 0 && errno == ESRCH))
            live++;
    }
    /* Which PE of this node is blocked longest, and in what. */
    uint64_t now = osoe_segment_now_ns(), worst = 0;
    char where[192] = "-";
    for (uint32_t i = 0; i < s->local_count; i++) {
        struct osoe_segment_blocked b;
        if (!osoe_segment_blocked(s, i, &b) || !b.valid || !b.blocked)
            continue;
        uint64_t age = b.since_ns && now > b.since_ns ? now - b.since_ns : 0;
        if (age < worst)
            continue;
        worst = age;
        char what[48], d[32] = "?";
        osoe_segment_blocked_detail(&b, what, sizeof what);
        if (age)
            print_duration(d, sizeof d, age);
        snprintf(where, sizeof where, "PE%u %s (%s) %s", i, b.routine, what, d);
    }

    for (unsigned t = 0; t < 2; t++) {
        const char *team = t == 0 ? "world" : "finalize";
        char name[64];
        uint64_t open = 0, closed = 0, own = 0, kids = 0, arrived = 0, missing = 0, parent = 0;
        snprintf(name, sizeof name, "team.%s.tree.epoch_open", team);
        if (!osoe_segment_read_name(s, name, 0, &open))
            return;                     /* older layout: no tree state to show */
        snprintf(name, sizeof name, "team.%s.tree.epoch_closed", team);
        osoe_segment_read_name(s, name, 0, &closed);
        snprintf(name, sizeof name, "team.%s.tree.own_arrived", team);
        osoe_segment_read_name(s, name, 0, &own);
        snprintf(name, sizeof name, "team.%s.tree.children", team);
        osoe_segment_read_name(s, name, 0, &kids);
        snprintf(name, sizeof name, "team.%s.tree.children_arrived", team);
        osoe_segment_read_name(s, name, 0, &arrived);
        snprintf(name, sizeof name, "team.%s.tree.lowest_missing", team);
        osoe_segment_read_name(s, name, 0, &missing);
        snprintf(name, sizeof name, "team.%s.tree.parent", team);
        osoe_segment_read_name(s, name, 0, &parent);
        /* A team that has not run says nothing: the finalize team is
         * registered from init and only moves at shmem_finalize. */
        if (open == 0 && closed == 0 && t == 1)
            continue;
        char missing_s[16] = "-", parent_s[16] = "root";
        if (missing != UINT32_MAX)
            snprintf(missing_s, sizeof missing_s, "%" PRIu64, missing);
        if (parent != UINT32_MAX)
            snprintf(parent_s, sizeof parent_s, "%" PRIu64, parent);
        printf("node %-3" PRIu64 " %-16s %2" PRIu64 "/%-2u live  %-8s epoch %" PRIu64
               "/%-3" PRIu64 " own %-3s kids %" PRIu64 "/%-2" PRIu64
               " next %-4s parent %-4s  %s\n",
               node, host, live, s->local_count, team, open, closed, own ? "yes" : "no",
               arrived, kids, missing_s, parent_s, where);
    }
}

/* ---- the counter view ------------------------------------------------- */

static void counters_human(const struct osoe_segment *s)
{
    print_identity(s);
    print_pe_lines(s);
    print_connection_lines(s);

    uint32_t npe = s->local_count;
    printf("\n%-40s", "counter");
    for (uint32_t p = 0; p < npe; p++)
        printf("%12s%u", "PE", p);
    printf("%14s\n", "total");

    for (uint32_t i = 0; i < s->nmetrics; i++) {
        const struct osoe_metric_descriptor *m = &s->metrics[i];
        if (m->scope != OSOE_MSCOPE_THREAD)
            continue;
        uint64_t per[64], total = 0;
        bool any = false;
        for (uint32_t p = 0; p < npe && p < 64; p++) {
            per[p] = pe_thread_value(s, m, p);
            if (per[p])
                any = true;
            if (metric_sums(m))
                total += per[p];
            else if (per[p] > total)
                total = per[p];
        }
        if (opt_nonzero && !any)
            continue;

        char name[48];
        metric_name(m, name, sizeof name);
        printf("%-40s", name);
        for (uint32_t p = 0; p < npe && p < 64; p++)
            printf("%13" PRIu64, per[p]);
        printf("%14" PRIu64 "%s\n", total,
               m->kind == OSOE_MKIND_INVARIANT && total ? "   <-- INVARIANT" : "");
    }

    /* §3.14: the whole point of a separate kind. Say it plainly rather than
     * leaving it to be spotted in a column of forty numbers. */
    for (uint32_t i = 0; i < s->nmetrics; i++) {
        const struct osoe_metric_descriptor *m = &s->metrics[i];
        if (m->scope != OSOE_MSCOPE_THREAD || m->kind != OSOE_MKIND_INVARIANT)
            continue;
        uint64_t total = 0;
        for (uint32_t p = 0; p < npe; p++)
            total += pe_thread_value(s, m, p);
        if (total) {
            char name[48];
            metric_name(m, name, sizeof name);
            printf("\ninvariant %s is %" PRIu64 ", not zero (§8.2)\n", name, total);
        }
    }
}

static void counters_json(const struct osoe_segment *s)
{
    uint32_t npe = s->local_count;
    printf("{\"segment\":");
    json_str(stdout, s->name);
    printf(",\"layout\":%u,\"local_count\":%u,\"counters\":{", s->dir->layout, npe);
    bool first = true;
    for (uint32_t i = 0; i < s->nmetrics; i++) {
        const struct osoe_metric_descriptor *m = &s->metrics[i];
        if (m->scope != OSOE_MSCOPE_THREAD)
            continue;
        char name[48];
        metric_name(m, name, sizeof name);
        if (!first)
            printf(",");
        first = false;
        json_str(stdout, name);
        printf(":[");
        for (uint32_t p = 0; p < npe; p++)
            printf("%s%" PRIu64, p ? "," : "", pe_thread_value(s, m, p));
        printf("]");
    }
    printf("}}\n");
}

/* ---- the per-thread view ---------------------------------------------- */

static void threads_human(const struct osoe_segment *s)
{
    print_identity(s);
    printf("\n%5s %3s %8s %8s %12s %12s  %s\n",
           "block", "PE", "pid", "tid", "cpu", "invol.csw", "kind");
    for (uint32_t b = 0; b < s->block_count; b++) {
        uint64_t claimed = 0, tid = 0, loops = 0, pid = 0;
        if (!osoe_segment_read_name(s, "thread.claimed", b, &claimed) || !claimed)
            continue;
        osoe_segment_read_name(s, "thread.tid", b, &tid);
        osoe_segment_read_name(s, "agent.loops", b, &loops);
        uint32_t pe = osoe_segment_block_pe(s, b);
        osoe_segment_read_name(s, "pe.pid", pe, &pid);

        struct thread_kernel k;
        read_thread_kernel(&k, (int64_t)pid, (int64_t)tid);
        char cpu[32] = "-", csw[32] = "-";
        if (k.found) {
            print_duration(cpu, sizeof cpu, k.cpu_ns);
            snprintf(csw, sizeof csw, "%" PRIu64, k.involuntary);
        }
        printf("%5u %3u %8" PRIu64 " %8" PRIu64 " %12s %12s  %s\n",
               b, pe, pid, tid, cpu, csw, loops ? "agent" : "application");
    }
}

/* ---- the state dump --------------------------------------------------- */

static void print_value(const struct osoe_segment *s, const struct osoe_metric_descriptor *m,
                        uint32_t index)
{
    char name[48];
    metric_name(m, name, sizeof name);
    if (m->type == OSOE_MTYPE_STRING) {
        char buf[128];
        osoe_segment_read_str(s, name, index, buf, sizeof buf);
        printf("    %-32s %s\n", name, buf[0] ? buf : "-");
        return;
    }
    uint64_t v;
    if (!osoe_segment_read(s, m, index, &v)) {
        printf("    %-32s (unreadable)\n", name);
        return;
    }
    if (m->type == OSOE_MTYPE_I32)
        printf("    %-32s %" PRId64 "\n", name, (int64_t)v);
    /* A STATE metric in nanoseconds is an instant on CLOCK_MONOTONIC, not an
     * elapsed time: printing "90596.181 s" beside it would read as a duration
     * and mean nothing. Only an accumulating kind is a duration. */
    else if (strncmp(m->unit, "ns", sizeof m->unit) == 0 && v &&
             m->kind != OSOE_MKIND_STATE) {
        char d[32];
        print_duration(d, sizeof d, v);
        printf("    %-32s %" PRIu64 "  (%s)\n", name, v, d);
    } else
        printf("    %-32s %" PRIu64 "\n", name, v);
}

static void dump_human(const struct osoe_segment *s)
{
    print_identity(s);

    printf("\n  directory\n");
    printf("    %-32s %u bytes (this reader knows %zu)\n", "directory_bytes",
           s->dir->directory_bytes, sizeof *s->dir);
    printf("    %-32s %" PRIu64 " + %u * %u\n", "slots", s->dir->slots_offset,
           s->dir->local_count, s->dir->slot_bytes);
    printf("    %-32s %" PRIu64 " + %u * %u\n", "metrics", s->dir->metrics_offset,
           s->dir->metrics_count, s->dir->metric_descriptor_bytes);
    printf("    %-32s %" PRIu64 " + %u * %u (%u per PE)\n", "blocks", s->dir->blocks_offset,
           s->dir->block_count, s->dir->block_bytes, s->dir->blocks_per_pe);
    if (OSOE_SEGMENT_HAS(s->dir, connection_count))
        printf("    %-32s %" PRIu64 " + %u * %u\n", "connections", s->dir->connections_offset,
               s->dir->connection_count, s->dir->connection_bytes);

    printf("\n  node\n");
    for (uint32_t i = 0; i < s->nmetrics; i++)
        if (s->metrics[i].scope == OSOE_MSCOPE_NODE)
            print_value(s, &s->metrics[i], 0);

    uint64_t now = osoe_segment_now_ns();
    for (uint32_t p = 0; p < s->local_count; p++) {
        printf("\n  PE %u\n", p);
        for (uint32_t i = 0; i < s->nmetrics; i++)
            if (s->metrics[i].scope == OSOE_MSCOPE_PE)
                print_value(s, &s->metrics[i], p);

        uint64_t rng = 0;
        if (osoe_segment_read_name(s, "pe.watch_rng", p, &rng)) {
            char w[96];
            describe_watch(w, sizeof w, s->dir->layout, rng);
            printf("    %-32s %s\n", "pe.watch (decoded)", w);
        }
        struct osoe_segment_blocked b;
        if (osoe_segment_blocked(s, p, &b)) {
            if (!b.valid)
                printf("    %-32s (republishing; not blocked)\n", "pe.blocked (read)");
            else if (!b.blocked)
                printf("    %-32s not blocked\n", "pe.blocked (read)");
            else {
                char age[32] = "?";
                if (b.since_ns && now > b.since_ns)
                    print_duration(age, sizeof age, now - b.since_ns);
                char what[48];
                osoe_segment_blocked_detail(&b, what, sizeof what);
                printf("    %-32s %s (%s) for %s\n", "pe.blocked (read)", b.routine, what,
                       age);
            }
        }
    }

    for (uint32_t c = 0; c < s->connection_count; c++) {
        printf("\n  connection %u\n", c);
        for (uint32_t i = 0; i < s->nmetrics; i++)
            if (s->metrics[i].scope == OSOE_MSCOPE_CONNECTION)
                print_value(s, &s->metrics[i], c);
    }

    printf("\n  counter blocks\n");
    for (uint32_t b = 0; b < s->block_count; b++) {
        uint64_t claimed = 0;
        if (!osoe_segment_read_name(s, "thread.claimed", b, &claimed) || !claimed)
            continue;
        uint64_t tid = 0, pid = 0;
        osoe_segment_read_name(s, "thread.tid", b, &tid);
        uint32_t pe = osoe_segment_block_pe(s, b);
        osoe_segment_read_name(s, "pe.pid", pe, &pid);
        struct thread_kernel k;
        read_thread_kernel(&k, (int64_t)pid, (int64_t)tid);
        char cpu[32] = "?";
        if (k.found)
            print_duration(cpu, sizeof cpu, k.cpu_ns);
        printf("    block %u (PE %u, tid %" PRIu64 "): cpu %s, involuntary switches %"
               PRIu64 "\n", b, pe, tid, cpu, k.involuntary);
        for (uint32_t i = 0; i < s->nmetrics; i++) {
            const struct osoe_metric_descriptor *m = &s->metrics[i];
            uint64_t v;
            if (m->scope != OSOE_MSCOPE_THREAD || !osoe_segment_read(s, m, b, &v))
                continue;
            if (opt_nonzero && !v)
                continue;
            char name[48];
            metric_name(m, name, sizeof name);
            printf("      %-38s %" PRIu64 "\n", name, v);
        }
    }
}

static void dump_json(const struct osoe_segment *s)
{
    printf("{\"segment\":");
    json_str(stdout, s->name);
    printf(",\"layout\":%u,\"segment_bytes\":%" PRIu64 ",\"metrics_checksum\":%u",
           s->dir->layout, s->dir->segment_bytes, s->dir->metrics_checksum);

    printf(",\"node\":{");
    bool first = true;
    for (uint32_t i = 0; i < s->nmetrics; i++) {
        const struct osoe_metric_descriptor *m = &s->metrics[i];
        if (m->scope != OSOE_MSCOPE_NODE)
            continue;
        uint64_t v;
        if (!osoe_segment_read(s, m, 0, &v))
            continue;
        char name[48];
        metric_name(m, name, sizeof name);
        if (!first)
            printf(",");
        first = false;
        json_str(stdout, name);
        printf(":%" PRIu64, v);
    }
    printf("}");

    printf(",\"pes\":[");
    for (uint32_t p = 0; p < s->local_count; p++) {
        printf("%s{", p ? "," : "");
        first = true;
        for (uint32_t i = 0; i < s->nmetrics; i++) {
            const struct osoe_metric_descriptor *m = &s->metrics[i];
            if (m->scope != OSOE_MSCOPE_PE)
                continue;
            char name[48];
            metric_name(m, name, sizeof name);
            if (!first)
                printf(",");
            first = false;
            json_str(stdout, name);
            if (m->type == OSOE_MTYPE_STRING) {
                char buf[128];
                osoe_segment_read_str(s, name, p, buf, sizeof buf);
                printf(":");
                json_str(stdout, buf);
            } else {
                uint64_t v = 0;
                osoe_segment_read(s, m, p, &v);
                printf(":%" PRIu64, v);
            }
        }
        struct osoe_segment_blocked b;
        if (osoe_segment_blocked(s, p, &b)) {
            printf(",\"blocked\":");
            if (b.valid && b.blocked) {
                printf("{\"routine\":");
                json_str(stdout, b.routine);
                printf(",\"detail\":%" PRIu64 ",\"detail_kind\":%u,\"since_ns\":%" PRIu64
                       "}", b.detail, b.detail_kind, b.since_ns);
            } else
                printf("null");
        }
        printf(",\"counters\":{");
        first = true;
        for (uint32_t i = 0; i < s->nmetrics; i++) {
            const struct osoe_metric_descriptor *m = &s->metrics[i];
            if (m->scope != OSOE_MSCOPE_THREAD)
                continue;
            char name[48];
            metric_name(m, name, sizeof name);
            if (!first)
                printf(",");
            first = false;
            json_str(stdout, name);
            printf(":%" PRIu64, pe_thread_value(s, m, p));
        }
        printf("}}");
    }
    printf("]}\n");
}

/* ---- listing and garbage collection ----------------------------------- */

/*
 * A segment is collectable when every PE that ever joined it is gone. A segment
 * where no PE has joined yet is left alone: it is a job in bootstrap, not a
 * corpse. A recycled pid can only make a dead job look alive, which loses a few
 * megabytes of /dev/shm rather than removing a running job's segment.
 */
static bool all_pes_gone(const struct osoe_segment *s, bool *any_joined)
{
    *any_joined = false;
    for (uint32_t i = 0; i < s->local_count; i++) {
        uint64_t phase = 0, pid = 0;
        osoe_segment_read_name(s, "pe.phase", i, &phase);
        osoe_segment_read_name(s, "pe.pid", i, &pid);
        if (phase == 0)
            continue;
        *any_joined = true;
        if ((int64_t)pid > 0 && !(kill((pid_t)(int64_t)pid, 0) != 0 && errno == ESRCH))
            return false;
    }
    return true;
}

static int gc_one(const char *name)
{
    struct osoe_segment s;
    char err[256];
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "osoe-stat: %s\n", err);
        return 1;
    }
    if (!s.table_usable) {
        fprintf(stderr, "osoe-stat: %s has no readable metric table; not removing it\n",
                s.name);
        osoe_segment_close(&s);
        return 1;
    }
    bool any_joined = false;
    bool gone = all_pes_gone(&s, &any_joined);
    /* The slots are the job's word for who it is; the kernel's is who maps the
     * object. A segment still mapped by a live process is never removed,
     * whatever its slots say - a recycled pid or a PE that died mid-join could
     * otherwise make a live job look dead. */
    bool mapped = gone && any_joined && osoe_segment_mapped(s.name);
    if (!gone || !any_joined || mapped) {
        printf("%s: %s\n", s.name, !any_joined ? "no PE has joined yet; left alone"
                                  : mapped ? "a live process still maps it; left alone"
                                           : "still has live PEs; left alone");
        osoe_segment_close(&s);
        return 0;
    }

    /* Heaps first: their names are in the segment, so removing the segment
     * first would strand them. */
    for (uint32_t i = 0; i < s.local_count; i++) {
        char heap[128];
        if (!osoe_segment_read_str(&s, "pe.heap_name", i, heap, sizeof heap) || !heap[0])
            continue;
        if (shm_unlink(heap) == 0)
            printf("%s: removed\n", heap);
        else if (errno != ENOENT)
            fprintf(stderr, "osoe-stat: cannot remove %s: %s\n", heap, strerror(errno));
    }
    char name_copy[OSOE_SEGMENT_NAME_MAX];
    snprintf(name_copy, sizeof name_copy, "%s", s.name);
    osoe_segment_close(&s);

    if (shm_unlink(name_copy) != 0) {
        fprintf(stderr, "osoe-stat: cannot remove %s: %s\n", name_copy, strerror(errno));
        return 1;
    }
    printf("%s: removed\n", name_copy);
    return 0;
}

static int list_segments(void)
{
    char names[64][OSOE_SEGMENT_NAME_MAX];
    int found = osoe_segment_list(names, 64);
    int shown = found < 64 ? found : 64;

    if (opt_json)
        printf("[");
    for (int i = 0; i < shown; i++) {
        struct osoe_segment s;
        char err[256];
        if (osoe_segment_open(&s, names[i], err, sizeof err) != 0) {
            if (!opt_json)
                printf("%-28s %s\n", names[i], err);
            continue;
        }
        uint64_t live = 0;
        for (uint32_t p = 0; p < s.local_count; p++) {
            uint64_t pid = 0, phase = 0;
            osoe_segment_read_name(&s, "pe.pid", p, &pid);
            osoe_segment_read_name(&s, "pe.phase", p, &phase);
            if (phase != 0 && (int64_t)pid > 0 &&
                !(kill((pid_t)(int64_t)pid, 0) != 0 && errno == ESRCH))
                live++;
        }
        if (opt_json) {
            printf("%s{\"segment\":", i ? "," : "");
            json_str(stdout, s.name);
            printf(",\"layout\":%u,\"local_count\":%u,\"live\":%" PRIu64 "}",
                   s.dir->layout, s.dir->local_count, live);
        } else {
            printf("%-28s layout %u  %u PE%s, %" PRIu64 " live  %" PRIu64 " bytes\n",
                   s.name, s.dir->layout, s.dir->local_count,
                   s.dir->local_count == 1 ? " " : "s", live, s.dir->segment_bytes);
        }
        osoe_segment_close(&s);
    }
    if (opt_json)
        printf("]\n");
    else if (found == 0)
        printf("no osoe control segments in /dev/shm\n");
    else if (found > shown)
        printf("(%d more not shown)\n", found - shown);
    return 0;
}

/* ---- main ------------------------------------------------------------- */

static int show(const char *name, enum mode m)
{
    struct osoe_segment s;
    char err[256];
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "osoe-stat: %s\n", err);
        return 1;
    }
    if (!s.table_usable) {
        /* The directory is frozen so that exactly this case is still useful. */
        printf("%s: layout %u, %" PRIu64 " bytes, %u local PEs, but its metric table is "
               "not readable by this build (%u metrics of %u bytes, checksum %#010x).\n"
               "Nothing beyond the directory can be shown.\n",
               s.name, s.dir->layout, s.dir->segment_bytes, s.dir->local_count,
               s.dir->metrics_count, s.dir->metric_descriptor_bytes, s.dir->metrics_checksum);
        osoe_segment_close(&s);
        return 1;
    }

    for (;;) {
        switch (m) {
        case MODE_DUMP:     opt_json ? dump_json(&s) : dump_human(&s); break;
        case MODE_THREADS:  threads_human(&s); break;
        case MODE_SUMMARY:  summary_line(&s); break;
        default:            opt_json ? counters_json(&s) : counters_human(&s); break;
        }
        if (!opt_interval)
            break;
        fflush(stdout);
        sleep(opt_interval);
        printf("\n");
    }
    osoe_segment_close(&s);
    return 0;
}

/* A bare number is a job id: every control segment of that job, one per node. */
static bool is_job_id(const char *a)
{
    if (!*a)
        return false;
    for (; *a; a++)
        if (!isdigit((unsigned char)*a))
            return false;
    return true;
}

static int show_job(const char *job, enum mode m)
{
    char names[64][OSOE_SEGMENT_NAME_MAX], prefix[64];
    snprintf(prefix, sizeof prefix, "/osoe.%s.", job);
    int found = osoe_segment_list(names, 64);
    int rc = 0, shown = 0;
    for (int k = 0; k < found && k < 64; k++)
        if (strncmp(names[k], prefix, strlen(prefix)) == 0) {
            rc |= show(names[k], m);
            shown++;
        }
    if (!shown) {
        fprintf(stderr, "osoe-stat: job %s: no such job - no named segment and no process "
                        "of this user holds one\n", job);
        return 1;
    }
    return rc;
}

static void usage(FILE *f)
{
    fprintf(f,
"usage: osoe-stat [options] [job | segment | path ...]\n"
"\n"
"Reads an osoe job's control segment without the job's cooperation, so it\n"
"works on a hung job. A running job's segments have no name in /dev/shm - the\n"
"library removes it during init - and are found through /proc, by any live PE\n"
"of the node that holds one open. Name a job by its id, a segment as\n"
"/osoe.<job>.<step>.<node>.<session> (or without the slash, or its /dev/shm\n"
"path), or give any path to a segment file. With nothing named, lists them.\n"
"\n"
"  -c, --counters    per-PE counter totals and where each PE is blocked (default)\n"
"  -d, --dump        every field of every record: node, PE slots, counter blocks\n"
"  -T, --threads     one line per counter block, with its kernel CPU time\n"
"      --summary     one line per node and team: the collective epoch in\n"
"                    progress, whether this node's own members arrived, and the\n"
"                    lowest child that has not - walk a hung collective down\n"
"                    from the root by it (§5.3)\n"
"  -l, --list        list the control segments in /dev/shm\n"
"      --gc          remove named segments (OSOE_KEEP_CONTROL_SEGMENT) whose PEs\n"
"                    are all gone and that no live process maps\n"
"  -j, --json        JSON instead of a table\n"
"  -z, --nonzero     omit counters that are zero\n"
"  -n, --interval S  repeat every S seconds\n"
"  -h, --help\n");
}

int main(int argc, char **argv)
{
    enum mode m = MODE_LIST;
    bool mode_set = false;
    int i = 1;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-c") || !strcmp(a, "--counters")) {
            m = MODE_COUNTERS; mode_set = true;
        } else if (!strcmp(a, "-d") || !strcmp(a, "--dump")) {
            m = MODE_DUMP; mode_set = true;
        } else if (!strcmp(a, "-T") || !strcmp(a, "--threads")) {
            m = MODE_THREADS; mode_set = true;
        } else if (!strcmp(a, "-l") || !strcmp(a, "--list")) {
            m = MODE_LIST; mode_set = true;
        } else if (!strcmp(a, "--summary")) {
            m = MODE_SUMMARY; mode_set = true;
        } else if (!strcmp(a, "--gc")) {
            m = MODE_GC; mode_set = true;
        } else if (!strcmp(a, "-j") || !strcmp(a, "--json")) {
            opt_json = true;
        } else if (!strcmp(a, "-z") || !strcmp(a, "--nonzero")) {
            opt_nonzero = true;
        } else if ((!strcmp(a, "-n") || !strcmp(a, "--interval")) && i + 1 < argc) {
            opt_interval = (unsigned)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "osoe-stat: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    if (m == MODE_GC) {
        int rc = 0;
        if (i < argc) {
            for (; i < argc; i++)
                rc |= gc_one(argv[i]);
            return rc;
        }
        /* Only named segments: one without a name is held open by a live job,
         * and has nothing to collect (§3.14). */
        char names[64][OSOE_SEGMENT_NAME_MAX];
        int found = osoe_segment_list_named(names, 64);
        for (int k = 0; k < found && k < 64; k++)
            rc |= gc_one(names[k]);
        if (found == 0)
            printf("no named osoe control segments in /dev/shm\n");
        return rc;
    }

    if (i >= argc)
        return m == MODE_LIST || !mode_set ? list_segments() : (usage(stderr), 2);

    if (!mode_set || m == MODE_LIST)
        m = MODE_COUNTERS;
    int rc = 0;
    for (; i < argc; i++)
        rc |= is_job_id(argv[i]) ? show_job(argv[i], m) : show(argv[i], m);
    return rc;
}
