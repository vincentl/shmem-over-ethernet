/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "base/deadline.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "abort.h"
#include "fatal.h"
#include "shm-object.h"
#include "connection-area.h"

#include "metrics.h"
#include "internal-region.h"
#include "node.h"

/*
 * The writer's half of §3.14's frozen prefix. tools/stat declares the same
 * structure independently, so that the reader proves something about the
 * segment rather than about the library's own headers; these assertions are
 * what keep the two copies the same size. Both move together when a field is
 * appended, and never otherwise.
 */
_Static_assert(sizeof(struct osoe_metric_descriptor) == 64, "frozen by §3.14");
_Static_assert(sizeof(struct osoe_segment_directory) == 256,
               "frozen by §3.14: append only. 136 at layouts 6 and 7; 160 at layout 12, "
               "with the connections section; 208 at layout 18, with the team blocks and "
               "the published epochs; 232 at layout 21, with the context pools; 256 at "
               "layout 23, with the thread records");

/*
 * Candidate heap bases, tried in order by the lead; the first where its own
 * heap maps becomes B for the node (§4.1). No single address is free under
 * every build in the development VM (aarch64, 48-bit): AddressSanitizer and
 * ThreadSanitizer reserve disjoint ranges. The list covers a plain build, ASan,
 * TSan, and 39-bit address spaces, in that order. OSOE_HEAP_BASE replaces it.
 */
static const uint64_t heap_base_candidates[] = {
    UINT64_C(0x100000000000),
    UINT64_C(0x300000000000),
    UINT64_C(0x010000000000),
    UINT64_C(0x001000000000),
};

/* Sections of the control segment start on a cache line, so none shares one. */
static uint64_t align_up(uint64_t n)
{
    return (n + OSOE_CACHELINE - 1) / OSOE_CACHELINE * OSOE_CACHELINE;
}

static uint64_t page_round(uint64_t n)
{
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    if (n > UINT64_MAX - (page - 1))
        osoe_fatal("size %llu too large to map", (unsigned long long)n);
    return (n + page - 1) / page * page;
}

/*
 * The executable's symmetric statics (§4.3). oshcc's linker script gathers the
 * program's .data and .bss input sections into two output sections, each
 * beginning and ending on the linker's largest page, and brackets them with
 * these symbols. Weak, so a program linked without the script has none and
 * each is NULL; default visibility, because the library is built hidden, and a
 * hidden weak reference is settled when the library is linked - to nothing -
 * rather than bound to the executable's definition when the program is loaded.
 */
extern unsigned char __osoe_statics_data_start[] __attribute__((weak, visibility("default")));
extern unsigned char __osoe_statics_data_end[] __attribute__((weak, visibility("default")));
extern unsigned char __osoe_statics_bss_start[] __attribute__((weak, visibility("default")));
extern unsigned char __osoe_statics_bss_end[] __attribute__((weak, visibility("default")));

/*
 * The two ranges, or none. The script puts the data before the bss and both on
 * page boundaries; bounds that are not so come from some other script, and
 * their region could be neither mapped over nor made symmetric in full, so
 * they are fatal rather than half used.
 */
static void find_statics(struct osoe_node *n)
{
    unsigned char *data = __osoe_statics_data_start, *data_end = __osoe_statics_data_end;
    unsigned char *bss = __osoe_statics_bss_start, *bss_end = __osoe_statics_bss_end;
    n->statics_data = n->statics_bss = NULL;
    n->statics_data_bytes = n->statics_bss_bytes = n->statics_bytes = 0;
    if (!data && !data_end && !bss && !bss_end)
        return;
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    if (!data || !data_end || !bss || !bss_end || data_end < data || bss < data_end ||
        bss_end < bss ||
        ((uintptr_t)data | (uintptr_t)data_end | (uintptr_t)bss | (uintptr_t)bss_end) % page)
        osoe_fatal("the program's symmetric statics are bracketed at data [%p, %p) and bss "
                   "[%p, %p), which are not the page-aligned ranges, data first, that "
                   "oshcc's linker script makes; link the program with oshcc",
                   (void *)data, (void *)data_end, (void *)bss, (void *)bss_end);
    n->statics_data = data;
    n->statics_data_bytes = (uint64_t)(data_end - data);
    n->statics_bss = bss;
    n->statics_bss_bytes = (uint64_t)(bss_end - bss);
    n->statics_bytes = n->statics_data_bytes + n->statics_bss_bytes;
}

/*
 * The copy reads whole pages of the program's data, which under
 * AddressSanitizer include the poisoned redzones between its globals: this
 * scan is not instrumented - nor for ThreadSanitizer, whose shadow for a large
 * .bss it would commit - and copy_into writes by the system call itself rather
 * than through the sanitizer's pwrite, which checks its buffer. Neither is a
 * read of a variable, only of the pages that hold them.
 */
typedef uint64_t __attribute__((may_alias)) any_word;   /* reads bytes of any type */

__attribute__((no_sanitize_address, no_sanitize_thread))
static bool page_is_zero(const unsigned char *p, uint64_t len)
{
    const any_word *word = (const any_word *)(const void *)p;   /* page-aligned */
    for (uint64_t i = 0; i < len / sizeof *word; i++)
        if (word[i])
            return false;
    return true;
}

/*
 * Write `bytes` of this process's memory at `from` into the object `fd` at
 * `offset`, the pages holding a nonzero byte only: the object is zero where it
 * is not written, and an untouched .bss page reads as the kernel's zero page,
 * so a large .bss costs neither a copy nor a page of /dev/shm until the
 * program writes it. False, with errno set, if a write fails.
 */
static bool copy_into(int fd, const unsigned char *from, uint64_t bytes, uint64_t offset)
{
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    for (uint64_t at = 0; at < bytes; at += page) {
        if (page_is_zero(from + at, page))
            continue;
        uint64_t done = 0;
        while (done < page) {
            long w = syscall(SYS_pwrite64, fd, from + at + done, (size_t)(page - done),
                             (off_t)(offset + at + done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                return false;
            done += (uint64_t)w;
        }
    }
    return true;
}

/* Publish failure so waiting siblings stop at their next sweep: osoe_fatal_then's
 * last act, after the message is out (fatal.h). */
static void publish_failure(void *context)
{
    struct osoe_node *n = context;
    if (n->hdr) {
        /* release: checked by waiters at each wake and sweep (§3.16). */
        atomic_store_explicit(&n->hdr->failed, 1, memory_order_release);
        osoe_futex_wake(&n->hdr->ready, INT_MAX, OSOE_FUTEX_SHARED);
        osoe_futex_wake(&n->hdr->joined, INT_MAX, OSOE_FUTEX_SHARED);
        osoe_futex_wake(&n->hdr->mapped, INT_MAX, OSOE_FUTEX_SHARED);
    }
}

_Noreturn static void fail_rendezvous(struct osoe_node *n, const char *what)
{
    osoe_fatal_then(publish_failure, n, "%s", what);
}

static void check_failed(struct osoe_node *n)
{
    if (atomic_load_explicit(&n->hdr->failed, memory_order_acquire))
        osoe_fatal("another local PE failed during rendezvous");
}

/* The node's control segment's name: the job, step, node and session. */
static void control_name(char *out, size_t len, const struct osoe_bootstrap *b, uint32_t session)
{
    snprintf(out, len, "/osoe.%llu.%u.%u.%u", (unsigned long long)b->job, b->step, b->node,
             session);
}

/* ---- a refusal before the node has joined (node.h) --------------------- */

static struct {
    struct osoe_node *node;
    char name[sizeof ((struct osoe_node *)0)->control_name];
    uint64_t job;
    uint32_t step, node_id, session;
    bool lead;
} refusal;

/*
 * The lead's tale, when it has no control segment yet: one that is nothing but
 * a header, `failed` set and `ready` never, which the node's other PEs open by
 * name, as they open any, and end on (open_control). Its name stays: a failed
 * job's leftover, which osoe-stat --gc removes. No fatal here - this runs on
 * the way out of one (fatal.h) - so whatever fails just tells nobody.
 */
static void tombstone(void)
{
    char err[256];
    int fd = osoe_shm_create(refusal.name, (long)offsetof(struct osoe_node_header, creator_pid),
                             err, sizeof err);
    if (fd < 0)
        return;
    size_t bytes = (size_t)page_round(sizeof(struct osoe_node_header));
    /* Allocated, not only sized: a full /dev/shm then refuses here rather than
     * with SIGBUS at the first store. */
    if (ftruncate(fd, (off_t)bytes) != 0 || posix_fallocate(fd, 0, (off_t)bytes) != 0) {
        close(fd);
        shm_unlink(refusal.name);
        return;
    }
    struct osoe_node_header *h = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (h == MAP_FAILED) {
        shm_unlink(refusal.name);
        return;
    }
    h->creator_pid = (int32_t)getpid();
    h->dir.magic = OSOE_NODE_MAGIC;
    h->dir.layout = OSOE_NODE_LAYOUT;
    h->dir.directory_bytes = sizeof h->dir;
    h->dir.header_bytes = (uint32_t)sizeof *h;
    h->dir.segment_bytes = bytes;
    h->job = refusal.job;
    h->step = refusal.step;
    h->node = refusal.node_id;
    h->session = refusal.session;
    /* release: read by every waiter with acquire (check_failed; §3.16). */
    atomic_store_explicit(&h->failed, 1, memory_order_release);
    osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
}

/*
 * Any other PE's tale: `failed` in the lead's segment, which the lead may not
 * have made yet - waited for, at most the abort grace, as open_control waits
 * for it, and given up on after that: the lead is then gone or never came, and
 * the node's notices say so.
 */
static void tell_the_lead(void)
{
    uint64_t until = 0;
    struct timespec t, back = { 0, 1000 * 1000 };
    clock_gettime(CLOCK_MONOTONIC, &t);
    until = (uint64_t)t.tv_sec + OSOE_ABORT_GRACE_SECONDS;
    for (;;) {
        char err[256];
        int fd = osoe_shm_open_ours(refusal.name, err, sizeof err);
        struct stat st;
        if (fd >= 0 && fstat(fd, &st) == 0 &&
            (uint64_t)st.st_size >= sizeof(struct osoe_node_header) && !osoe_shm_orphaned(fd)) {
            struct osoe_node_header *h = mmap(NULL, sizeof *h, PROT_READ | PROT_WRITE,
                                              MAP_SHARED, fd, 0);
            close(fd);
            if (h == MAP_FAILED)
                return;
            atomic_store_explicit(&h->failed, 1, memory_order_release);
            osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
            osoe_futex_wake(&h->joined, INT_MAX, OSOE_FUTEX_SHARED);
            osoe_futex_wake(&h->mapped, INT_MAX, OSOE_FUTEX_SHARED);
            return;
        }
        if (fd >= 0)
            close(fd);
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((uint64_t)t.tv_sec >= until)
            return;
        nanosleep(&back, NULL);
        if (back.tv_nsec < 64 * 1000 * 1000)
            back.tv_nsec *= 2;
    }
}

static void tell_refusal(void *unused)
{
    (void)unused;
    struct osoe_node *n = refusal.node;
    if (n && n->hdr)
        publish_failure(n);         /* joined far enough to hold the segment */
    else if (refusal.lead)
        tombstone();
    else
        tell_the_lead();
}

void osoe_node_refusal_arm(struct osoe_node *n, const struct osoe_bootstrap *b,
                           uint32_t session)
{
    refusal.node = n;
    control_name(refusal.name, sizeof refusal.name, b, session);
    refusal.job = b->job;
    refusal.step = b->step;
    refusal.node_id = b->node;
    refusal.session = session;
    refusal.lead = b->local_id == 0;
    osoe_fatal_last_word(tell_refusal, NULL);
}

void osoe_node_refusal_disarm(void)
{
    osoe_fatal_last_word(NULL, NULL);
}

/* The local PEs that have not reached `phase`, as " 3 (rank 7)" each. */
static void describe_missing(struct osoe_node *n, const struct osoe_bootstrap *b,
                             enum osoe_phase phase, char *buf, size_t len)
{
    size_t off = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < b->local_count && off + 32 < len; i++)
        if (atomic_load_explicit(&n->slots[i].phase, memory_order_acquire) < phase)
            off += (size_t)snprintf(buf + off, len - off, " %u (rank %u)", i, b->gtids[i]);
}

/*
 * Wait for a rendezvous count to reach its target: 0 then, -1 once it has
 * stopped moving for the no-progress limit (§3.15), which by default is never.
 * Ends at once if a local PE has published a failure; a failure's wake can be
 * missed (the failing PE stores `failed` and then wakes without bumping the
 * word), so it is noticed at the next sweep at worst. On the lead, a wait that
 * lasts says which local PEs have not reached `phase` - `doing` is what they
 * have not done - every minute from 30 s; the others leave the node's notice to
 * it (base/stall-watch.h).
 */
static int wait_count(struct osoe_node *n, const struct osoe_bootstrap *b, _Atomic uint32_t *word,
                      uint32_t target, enum osoe_phase phase, const char *doing)
{
    struct osoe_stall_watch watch;
    uint32_t start = atomic_load_explicit(word, memory_order_acquire);
    if (b->local_id == 0)
        osoe_stall_watch_begin_noticed(&watch, start, b->no_progress_ns);
    else
        osoe_stall_watch_begin(&watch, start, b->no_progress_ns);
    for (;;) {
        uint32_t v = atomic_load_explicit(word, memory_order_acquire);
        check_failed(n);
        if (v == target)
            return 0;
        if (osoe_stall_watch_stalled(&watch, v))
            return -1;
        if (osoe_stall_watch_notice_due(&watch)) {
            char missing[256];
            describe_missing(n, b, phase, missing, sizeof missing);
            osoe_notice("shmem_init on node %u has waited %llu s for local PEs to %s: %u of %u "
                        "have; still waiting for local id%s", b->node,
                        (unsigned long long)osoe_stall_watch_waited_seconds(&watch), doing, v,
                        target, missing);
        }
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(word, v, &sweep, OSOE_FUTEX_SHARED);
    }
}

/* Create this PE's heap object and map it. With base == 0, try the
 * candidates and return the one that worked; 0 if none did. */
static uint64_t create_own_heap(struct osoe_node *n, const struct osoe_bootstrap *b,
                                struct osoe_pe_slot *slot, uint64_t base)
{
    /* A heap has nowhere to record its creator; a leftover with this name
     * predates the control segment this node's lead has just created, and so
     * is a dead job's, reclaimed if it is this user's. */
    char err[256];
    int fd = osoe_shm_create(slot->heap_name, OSOE_SHM_NO_OWNER, err, sizeof err);
    if (fd < 0) {
        char msg[320];
        snprintf(msg, sizeof msg, "this PE's heap: %s", err);
        fail_rendezvous(n, msg);
    }
    if (ftruncate(fd, (off_t)n->heap_bytes) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "cannot size heap %s to %llu bytes: %s", slot->heap_name,
                 (unsigned long long)n->heap_bytes, strerror(errno));
        close(fd);
        fail_rendezvous(n, msg);
    }

    const uint64_t *candidates = heap_base_candidates;
    size_t ncandidates = sizeof heap_base_candidates / sizeof heap_base_candidates[0];
    if (base) {
        candidates = &base;
        ncandidates = 1;
    } else if (b->heap_base) {
        candidates = &b->heap_base;
        ncandidates = 1;
    }

    /* B is what is returned and published; the heap itself sits past it by
     * this PE's shift, which is 0 unless §8.2's test mode is on. */
    uint64_t shift = b->heap_shift * b->rank;
    uint64_t mapped_at = 0;
    for (size_t i = 0; i < ncandidates && !mapped_at; i++) {
        void *want = (void *)(uintptr_t)(candidates[i] + shift);
        void *p = mmap(want, n->heap_bytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (p == want)
            mapped_at = candidates[i];
        else if (p != MAP_FAILED)
            munmap(p, n->heap_bytes);   /* a kernel without NOREPLACE placed it elsewhere */
    }
    close(fd);
    return mapped_at;
}

/*
 * The object the program's statics are mapped from, once a session has made
 * one: kept open, since a later session copies from it, and closed when that
 * session's object replaces it. -1 before the first. A static like any other -
 * with the library linked statically it is among the statics - so it is read
 * and written only outside the window below.
 */
static int statics_mapped_from = -1;

/* Where object offset `offset` is in the program. */
static unsigned char *statics_address(const struct osoe_node *n, uint64_t offset)
{
    return offset < n->statics_data_bytes ? n->statics_data + offset
                                          : n->statics_bss + (offset - n->statics_data_bytes);
}

/*
 * Copy the program's statics into the object `fd`. The first session reads the
 * ranges whole: their pages are the program's own - file pages and, for an
 * untouched .bss, the kernel's zero page - so copy_into's scan costs reads and
 * nothing more. A later session's ranges are the earlier object, where reading
 * a hole would allocate it: tmpfs has no zero page to share. So only the parts
 * of `previous` that hold data are read, found by SEEK_DATA and SEEK_HOLE, and
 * the holes, zero, are left as the new object's. False, with errno, on a
 * failure.
 */
static bool copy_statics(int fd, int previous, const struct osoe_node *n)
{
    uint64_t data_bytes = n->statics_data_bytes, total = n->statics_bytes;
    if (previous < 0)
        return copy_into(fd, n->statics_data, data_bytes, 0) &&
               copy_into(fd, n->statics_bss, n->statics_bss_bytes, data_bytes);
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t at = 0;
    while (at < total) {
        off_t data = lseek(previous, (off_t)at, SEEK_DATA);
        if (data < 0)
            return errno == ENXIO;              /* nothing but a hole from here */
        off_t hole = lseek(previous, data, SEEK_HOLE);
        if (hole < 0)
            return false;
        uint64_t start = (uint64_t)data / page * page;
        uint64_t stop = ((uint64_t)hole + page - 1) / page * page;
        if (stop > total)
            stop = total;
        /* The two ranges are two runs of program addresses: split at the seam. */
        while (start < stop) {
            uint64_t end = start < data_bytes && stop > data_bytes ? data_bytes : stop;
            if (!copy_into(fd, statics_address(n, start), end - start, start))
                return false;
            start = end;
        }
        at = stop;
    }
    return true;
}

/*
 * Create this PE's statics object, copy the program's statics into it, and map
 * it over them (§4.3), with an alias of the whole object for node-mates' use
 * of it through the transport.
 *
 * Nothing between the copy and the mapping may write memory in the ranges: a
 * write there would land in the pages the mapping then hides. The program is
 * inside shmem_init, the library's own threads start after this, and signals
 * are blocked across it, so that a handler setting a global cannot lose its
 * write; a thread of the application's own, or of a library it linked
 * statically, is what remains (§4.3, known limits). With the library linked
 * statically its own globals are in the ranges too, so the code here keeps
 * what it computes in locals until the mapping is in place. The two mappings
 * over the ranges are made by the system call itself, not through a
 * sanitizer's interception of mmap, which would reset the shadow of the
 * program's globals - the redzones that let it find an overflow of one.
 */
static void create_own_statics(struct osoe_node *n, const struct osoe_bootstrap *b,
                               struct osoe_pe_slot *slot)
{
    int len = snprintf(slot->statics_name, sizeof slot->statics_name, "%s.statics.%u",
                       n->control_name, b->local_id);
    if (len < 0 || (size_t)len >= sizeof slot->statics_name)
        fail_rendezvous(n, "statics object name too long");
    char err[256];
    int fd = osoe_shm_create(slot->statics_name, OSOE_SHM_NO_OWNER, err, sizeof err);
    if (fd < 0) {
        char msg[320];
        snprintf(msg, sizeof msg, "this PE's statics: %s", err);
        fail_rendezvous(n, msg);
    }
    if (ftruncate(fd, (off_t)n->statics_bytes) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "cannot size statics %s to %llu bytes: %s",
                 slot->statics_name, (unsigned long long)n->statics_bytes, strerror(errno));
        close(fd);
        shm_unlink(slot->statics_name);
        fail_rendezvous(n, msg);
    }

    unsigned char *data = n->statics_data, *bss = n->statics_bss;
    uint64_t data_bytes = n->statics_data_bytes, bss_bytes = n->statics_bss_bytes;
    int previous = statics_mapped_from;
    sigset_t every, before;
    sigfillset(&every);
    pthread_sigmask(SIG_SETMASK, &every, &before);
    bool copied = copy_statics(fd, previous, n);
    int copy_error = copied ? 0 : errno;
    long over_data = copied && data_bytes
        ? syscall(SYS_mmap, data, data_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                  fd, (off_t)0)
        : (long)(uintptr_t)data;
    long over_bss = copied && over_data == (long)(uintptr_t)data && bss_bytes
        ? syscall(SYS_mmap, bss, bss_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                  fd, (off_t)data_bytes)
        : (long)(uintptr_t)bss;
    int map_error = errno;
    pthread_sigmask(SIG_SETMASK, &before, NULL);
    void *alias = mmap(NULL, n->statics_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (alias == MAP_FAILED)
        map_error = errno;

    if (!copied || over_data != (long)(uintptr_t)data || over_bss != (long)(uintptr_t)bss ||
        alias == MAP_FAILED) {
        char msg[320];
        snprintf(msg, sizeof msg, "cannot %s the program's statics (%llu bytes) %s %s: %s",
                 !copied ? "copy" : "map", (unsigned long long)n->statics_bytes,
                 !copied ? "into" : "from", slot->statics_name,
                 strerror(!copied ? copy_error : map_error));
        close(fd);
        shm_unlink(slot->statics_name);
        fail_rendezvous(n, msg);
    }
    if (previous >= 0)
        close(previous);                        /* its last mapping is gone */
    statics_mapped_from = fd;
    /* The ranges are this PE's statics in its core already. */
    madvise(alias, n->statics_bytes, MADV_DONTDUMP);
    n->statics_of[b->local_id] = alias;
}

static void map_siblings(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    for (uint32_t i = 0; i < b->local_count; i++) {
        if (i == b->local_id)
            continue;
        struct osoe_pe_slot *s = &n->slots[i];
        char err[256];
        int fd = osoe_shm_open_ours(s->heap_name, err, sizeof err);
        if (fd < 0) {
            char msg[512];
            snprintf(msg, sizeof msg, "local PE %u's heap: %s", i,
                     err[0] ? err : strerror(errno));
            fail_rendezvous(n, msg);
        }
        void *p = mmap(NULL, s->heap_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) {
            char msg[256];
            snprintf(msg, sizeof msg, "cannot map local PE %u's heap (%llu bytes): %s", i,
                     (unsigned long long)s->heap_bytes, strerror(errno));
            fail_rendezvous(n, msg);
        }
        /* A sibling's heap is in the sibling's own core; in this one it would
         * make every core L heaps large (review round two, omissions 4). */
        madvise(p, s->heap_bytes, MADV_DONTDUMP);
        n->heap_of[i] = p;

        if (!n->statics_bytes)
            continue;
        fd = osoe_shm_open_ours(s->statics_name, err, sizeof err);
        if (fd < 0) {
            char msg[512];
            snprintf(msg, sizeof msg, "local PE %u's statics: %s", i,
                     err[0] ? err : strerror(errno));
            fail_rendezvous(n, msg);
        }
        p = mmap(NULL, n->statics_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) {
            char msg[256];
            snprintf(msg, sizeof msg, "cannot map local PE %u's statics (%llu bytes): %s", i,
                     (unsigned long long)n->statics_bytes, strerror(errno));
            fail_rendezvous(n, msg);
        }
        madvise(p, n->statics_bytes, MADV_DONTDUMP);
        n->statics_of[i] = p;
    }
}

/* Every local PE must have parsed the same SHMEM_SYMMETRIC_SIZE (§4.1, §4.4).
 * Every PE runs this check on the same slots, so all of them fail together. */
static void check_agreement(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    for (uint32_t i = 0; i < b->local_count; i++) {
        if (n->slots[i].symmetric_size != b->symmetric_size) {
            char msg[256];
            snprintf(msg, sizeof msg, "SHMEM_SYMMETRIC_SIZE differs between PEs: local PE "
                     "%u has %llu bytes, this PE has %llu", i,
                     (unsigned long long)n->slots[i].symmetric_size,
                     (unsigned long long)b->symmetric_size);
            fail_rendezvous(n, msg);
        }
        /* A backstop. Every setting that sizes the heap object - the symmetric
         * size above, the fan-out, the pipeline's depth and the team count in
         * open_control - is agreed by name; this fires only for one that a
         * later change forgets to agree, before the progress thread reads a
         * node-mate's internal region by this PE's size of it. */
        if (n->slots[i].heap_bytes != n->heap_bytes) {
            char msg[256];
            snprintf(msg, sizeof msg, "local PE %u's heap object is %llu bytes and this PE's "
                     "%llu: the PEs size the library's internal region differently", i,
                     (unsigned long long)n->slots[i].heap_bytes,
                     (unsigned long long)n->heap_bytes);
            fail_rendezvous(n, msg);
        }
        if (n->slots[i].statics_bytes != n->statics_bytes) {
            char msg[256];
            snprintf(msg, sizeof msg, "the PEs run different programs: local PE %u's "
                     "symmetric statics are %llu bytes, this PE's %llu (§4.3)", i,
                     (unsigned long long)n->slots[i].statics_bytes,
                     (unsigned long long)n->statics_bytes);
            fail_rendezvous(n, msg);
        }
        if (n->slots[i].rank != b->gtids[i]) {
            char msg[256];
            snprintf(msg, sizeof msg, "local PE %u joined as rank %u, but %sGTIDS "
                     "names rank %u", i, n->slots[i].rank,
                     b->launcher ? b->launcher : "OSOE_", b->gtids[i]);
            fail_rendezvous(n, msg);
        }
    }
}

/* The lead creates and initializes the control segment. */
static void create_control(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session)
{
    /*
     * Four sections: the header, the PE slots, the §3.14 metric table, and the
     * counter blocks. Each starts on a cache line, so no two sections share
     * one. The header records where each begins, which is the whole directory
     * osoe-stat needs before it can read the rest by name.
     */
    uint32_t per_pe = b->max_threads + 1;      /* and the agent's (bootstrap.h) */
    uint32_t metrics_count;
    const struct osoe_metric_descriptor *metrics = osoe_metrics_table(&metrics_count);

    /*
     * The header ends with one block per team number (layout 18, §5.7):
     * OSOE_MAX_TEAMS of them, about 900 bytes each with 128-byte lines, so 56
     * KiB of the segment at the default of 64 and under a megabyte at the most.
     * Sized by the count every node agreed, never by the teams that exist.
     */
    uint64_t teams_offset = offsetof(struct osoe_node_header, team);
    uint64_t teams_bytes = (uint64_t)b->max_teams * sizeof(struct osoe_team_block);
    uint64_t header_bytes = teams_offset + teams_bytes;
    uint64_t slots_offset = align_up(header_bytes);
    uint64_t metrics_offset =
        align_up(slots_offset + (uint64_t)b->local_count * sizeof(struct osoe_pe_slot));
    uint64_t metrics_bytes = (uint64_t)metrics_count * sizeof *metrics;
    uint64_t blocks_offset = align_up(metrics_offset + metrics_bytes);
    uint64_t block_count = (uint64_t)b->local_count * per_pe;
    uint64_t blocks_bytes = block_count * sizeof(struct osoe_counter_block);

    /*
     * The §3.12 landing zones, one slot per local PE, each holding one AM chunk
     * (§3.2). Sized from the same OSOE_AM_CHUNK the AM layer uses, because a
     * reply chunk has to fit: a landing zone smaller than a chunk would make
     * the wire's maximum message undeliverable, which is a hang rather than a
     * short read.
     */
    struct osoe_am_config am;
    osoe_am_config_read(&am);
    /* Since layout 23 a zone is extents (node.h): its header and bitmap, a
     * record an extent, then the extents. A piece of a get is at most a run of
     * 64 extents in one word of the bitmap, and at most a chunk, which the
     * wire's largest message holds. Whole extents, which osoe_am_config_read
     * holds every PE to. */
    uint64_t extents = am.get_landing_bytes / OSOE_LANDING_EXTENT_BYTES;
    uint64_t landing_requests_offset =
        align_up(sizeof(struct osoe_landing_zone) + (extents + 63) / 64 * sizeof(uint64_t));
    uint64_t landing_arena_offset =
        align_up(landing_requests_offset + extents * sizeof(struct osoe_landing_record));
    uint64_t landing_arena_bytes = extents * OSOE_LANDING_EXTENT_BYTES;
    uint64_t landing_slot_bytes = align_up(landing_arena_offset + landing_arena_bytes);
    uint64_t largest_piece = (uint64_t)OSOE_LANDING_RUN_MAX * OSOE_LANDING_EXTENT_BYTES;
    if (largest_piece > landing_arena_bytes)
        largest_piece = landing_arena_bytes;
    if (largest_piece > am.chunk)
        largest_piece = am.chunk;
    uint64_t landing_offset = align_up(blocks_offset + blocks_bytes);
    uint64_t landing_bytes = (uint64_t)b->local_count * landing_slot_bytes;

    /*
     * One record per connection toward a peer node (§3.14's connection scope,
     * layout 12). Sized by node count, never by PE count (§1): 128 bytes per
     * peer, so 512 KiB per node at 4,096 nodes, in the segment every PE of the
     * node already maps. A job of one node has none.
     */
    uint64_t connections_offset = align_up(landing_offset + landing_bytes);
    uint64_t connection_count = b->node_count > 1 ? b->node_count - 1 : 0;
    uint64_t connections_bytes = connection_count * sizeof(struct osoe_am_connection_metrics);

    /*
     * Each local PE's published epochs (layout 18, §5.4): a row per PE of one
     * word per team number, indexed [local id][team], each row padded to a
     * cache line so that a PE publishing its count after every collective
     * shares no line with another. 128 bytes a PE at the default.
     */
    uint64_t epoch_row_bytes = align_up((uint64_t)b->max_teams * sizeof(uint32_t));
    uint64_t epochs_offset = align_up(connections_offset + connections_bytes);
    uint64_t epochs_bytes = (uint64_t)b->local_count * epoch_row_bytes;

    /*
     * Each local PE's pool of context blocks (layout 21; §4.6, §4.11): a
     * cache line per context, OSOE_MAX_CONTEXTS of them a PE, 8 KiB a PE at
     * the default. Last, so that nothing before it moved when it was added.
     */
    uint64_t contexts_offset = align_up(epochs_offset + epochs_bytes);
    uint64_t contexts_bytes = (uint64_t)b->local_count * b->max_contexts *
                              sizeof(struct osoe_context_block);
    /* Each local PE's thread records (layout 23; §4.11 item 3): a cache line a
     * thread, OSOE_MAX_THREADS of them a PE, one per counter block. */
    uint64_t threads_offset = align_up(contexts_offset + contexts_bytes);
    uint64_t threads_bytes = (uint64_t)b->local_count * per_pe *
                             sizeof(struct osoe_thread_record);
    n->control_bytes = page_round(threads_offset + threads_bytes);

    char err[256];
    int fd = osoe_shm_create(n->control_name,
                             (long)offsetof(struct osoe_node_header, creator_pid),
                             err, sizeof err);
    if (fd < 0)
        osoe_fatal("the control segment: %s", err);
    if (ftruncate(fd, (off_t)n->control_bytes) != 0) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot size control segment: %s", strerror(errno));
    }
    /* The pages the lead writes now - the header, the slots, the metric table -
     * allocated rather than only sized: a /dev/shm too full for even these
     * refuses here, by name, where a store into a page it cannot have would
     * end the lead by SIGBUS and say nothing. The rest is checked below, with
     * the segment there to say so through. */
    int short_of = posix_fallocate(fd, 0, (off_t)blocks_offset);
    if (short_of != 0) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot allocate the control segment's first %llu KiB in /dev/shm: %s",
                   (unsigned long long)(blocks_offset >> 10), strerror(short_of));
    }
    void *p = mmap(NULL, n->control_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot map control segment: %s", strerror(errno));
    }
    n->control_fd = fd;             /* kept: see struct osoe_node */

    /* ftruncate zeroed it: every atomic word starts at 0. */
    n->hdr = p;
    n->hdr->creator_pid = (int32_t)getpid();
    n->slots = (struct osoe_pe_slot *)((char *)p + slots_offset);
    struct osoe_node_header *h = n->hdr;
    h->dir.magic = OSOE_NODE_MAGIC;
    h->dir.layout = OSOE_NODE_LAYOUT;
    h->dir.header_bytes = (uint32_t)header_bytes;
    h->dir.segment_bytes = n->control_bytes;
    h->job = b->job;
    h->step = b->step;
    h->node = b->node;
    h->session = session;
    h->dir.local_count = b->local_count;
    h->world_size = b->world_size;
    h->collective_fanout = b->collective_fanout;
    h->collective_pipeline = b->collective_pipeline;
    h->dir.slot_bytes = sizeof(struct osoe_pe_slot);
    h->dir.slots_offset = slots_offset;
    h->dir.metrics_offset = metrics_offset;
    h->dir.metrics_bytes = metrics_bytes;
    h->dir.metrics_count = metrics_count;
    h->dir.metrics_checksum = osoe_metric_checksum(metrics, metrics_count);
    h->dir.directory_bytes = sizeof h->dir;
    h->dir.metric_descriptor_bytes = sizeof *metrics;
    h->dir.blocks_offset = blocks_offset;
    h->dir.blocks_bytes = blocks_bytes;
    h->dir.block_count = (uint32_t)block_count;
    h->dir.blocks_per_pe = per_pe;
    h->dir.landing_offset = landing_offset;
    h->dir.landing_bytes = landing_bytes;
    h->dir.landing_slot_bytes = (uint32_t)landing_slot_bytes;
    h->dir.landing_count = b->local_count;
    h->dir.connections_offset = connections_offset;
    h->dir.connections_bytes = connections_bytes;
    h->dir.connection_bytes = (uint32_t)sizeof(struct osoe_am_connection_metrics);
    h->dir.connection_count = (uint32_t)connection_count;
    h->dir.teams_offset = teams_offset;
    h->dir.teams_bytes = teams_bytes;
    h->dir.team_bytes = (uint32_t)sizeof(struct osoe_team_block);
    h->dir.team_count = b->max_teams;
    h->dir.epochs_offset = epochs_offset;
    h->dir.epochs_bytes = epochs_bytes;
    h->dir.epoch_row_bytes = (uint32_t)epoch_row_bytes;
    h->dir.epoch_row_count = b->local_count;
    h->dir.contexts_offset = contexts_offset;
    h->dir.contexts_bytes = contexts_bytes;
    h->dir.context_bytes = (uint32_t)sizeof(struct osoe_context_block);
    h->dir.contexts_per_pe = b->max_contexts;
    h->dir.threads_offset = threads_offset;
    h->dir.threads_bytes = threads_bytes;
    h->dir.thread_record_bytes = (uint32_t)sizeof(struct osoe_thread_record);
    h->dir.threads_per_pe = per_pe;
    h->dir.block_bytes = (uint32_t)sizeof(struct osoe_counter_block);
    memcpy((char *)p + metrics_offset, metrics, metrics_bytes);
    n->blocks = (struct osoe_counter_block *)((char *)p + blocks_offset);
    h->landing_data_bytes = (uint32_t)largest_piece;
    h->landing_requests = (uint32_t)extents;
    h->landing_requests_offset = (uint32_t)landing_requests_offset;
    h->landing_arena_offset = (uint32_t)landing_arena_offset;
    h->landing_arena_bytes = (uint32_t)landing_arena_bytes;
    n->landing = (unsigned char *)p + landing_offset;
    n->landing_slot_bytes = (uint32_t)landing_slot_bytes;
    n->landing_data_bytes = h->landing_data_bytes;
    n->landing_requests = h->landing_requests;
    n->landing_requests_offset = h->landing_requests_offset;
    n->landing_arena_offset = h->landing_arena_offset;
    n->landing_arena_bytes = h->landing_arena_bytes;
    n->connection_metrics = connection_count
        ? (struct osoe_am_connection_metrics *)((char *)p + connections_offset) : NULL;
    n->connection_count = (uint32_t)connection_count;
    n->team_count = b->max_teams;
    n->epochs = (_Atomic uint32_t *)((char *)p + epochs_offset);
    n->epoch_row_words = (uint32_t)(epoch_row_bytes / sizeof(uint32_t));
    n->contexts = (struct osoe_context_block *)((char *)p + contexts_offset);
    n->contexts_per_pe = b->max_contexts;
    n->threads = (struct osoe_thread_record *)((char *)p + threads_offset);
    n->threads_per_pe = per_pe;
    /*
     * Which machine this logical node is on (§3.14): the node numbers a
     * hung collective names are logical, and a walk down the tree needs the
     * host to run osoe-stat on next. Truncation is not an error - it is a
     * label - so the result is terminated by hand.
     */
    if (gethostname(h->hostname, sizeof h->hostname) != 0)
        snprintf(h->hostname, sizeof h->hostname, "unknown");
    h->hostname[sizeof h->hostname - 1] = '\0';
    /* Every team's counters, whether or not its number is ever used: the rest
     * of each block, its handoff, tree state and record, is zero as it is. */
    for (uint32_t t = 0; t < b->max_teams; t++) {
        osoe_epoch_init(&h->team[t].arrived);
        osoe_epoch_init(&h->team[t].released);
        osoe_epoch_init(&h->team[t].copied);
    }

    /*
     * The node's whole request, checked once, now that the segment exists to
     * say so through: this segment and every local PE's heap. Checked before it
     * did, a refusal had nowhere to be published, and the node's other PEs
     * waited in silence for a segment that never came - the likeliest failure
     * of a first day (the round-three review's R8). Refused through `failed`,
     * which every one of their waits reads; the name stays, a failed job's
     * leftover that osoe-stat --gc removes. The lead knows L and the heap size;
     * every PE has parsed the same size, or check_agreement fails them all. A
     * heap object is the heap and then its internal region (internal-region.h),
     * about 11 MiB at the default 64 teams, fan-out of 4 and two pieces in
     * flight (OSOE_COLLECTIVE_PIPELINE), which is counted here with the heap
     * it follows. Pages cost only once written, so nothing above has taken
     * the room this asks for.
     */
    uint64_t wanted = n->control_bytes +
                      (uint64_t)b->local_count * (n->heap_bytes + n->statics_bytes);
    uint64_t free_bytes;
    if (!osoe_shm_room(wanted, &free_bytes)) {
        char msg[512];
        char statics[96] = "";
        if (n->statics_bytes)
            snprintf(statics, sizeof statics, " and as many copies of the program's %llu MiB "
                     "of statics", (unsigned long long)((n->statics_bytes + (1u << 20) - 1) >> 20));
        snprintf(msg, sizeof msg, "/dev/shm has %llu MiB free, but this node's control "
                 "segment and %u heaps of %llu MiB%s need %llu MiB: lower "
                 "SHMEM_SYMMETRIC_SIZE, run fewer PEs per node, or enlarge /dev/shm - or, "
                 "since pages cost only once written, OSOE_SHM_CAPACITY_CHECK=0 reports "
                 "this without refusing", (unsigned long long)(free_bytes >> 20),
                 b->local_count, (unsigned long long)(n->heap_bytes >> 20), statics,
                 (unsigned long long)((wanted + (1u << 20) - 1) >> 20));
        if (!b->shm_capacity_warning_only)
            fail_rendezvous(n, msg);
        fprintf(stderr, "osoe: %s; continuing, since OSOE_SHM_CAPACITY_CHECK=0\n", msg);
    }
}

/* A non-lead opens the control segment by name, waiting for the lead to
 * create and publish it. */
static void open_control(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session)
{
    /* Until the lead has a segment to speak through, local id 1 speaks for the
     * node: one line a node, not one a PE (base/stall-watch.h). */
    struct osoe_stall_watch watch;
    if (b->local_id == 1)
        osoe_stall_watch_begin_noticed(&watch, 0, b->no_progress_ns);
    else
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
again:;
    struct timespec backoff = { 0, 1000 * 1000 };   /* 1 ms, doubling to 64 ms */
    for (;;) {
        char err[256];
        int fd = osoe_shm_open_ours(n->control_name, err, sizeof err);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= sizeof(struct osoe_node_header)) {
                void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd, 0);
                if (p == MAP_FAILED) {
                    close(fd);
                    osoe_fatal("cannot map control segment %s: %s", n->control_name,
                               strerror(errno));
                }
                n->control_fd = fd;     /* kept: see struct osoe_node */
                n->hdr = p;
                n->control_bytes = (uint64_t)st.st_size;
                break;
            }
            close(fd);          /* created but not yet sized */
        } else if (errno != ENOENT) {
            osoe_fatal("the control segment: %s", err);
        }
        if (osoe_stall_watch_stalled(&watch, 0)) {
            /* Nothing to tell: the lead's segment, which the last word would
             * wait the abort grace for, is what has not come (node.h). */
            osoe_node_refusal_disarm();
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead "
                       "has not created %s",
                       (unsigned long long)osoe_stall_watch_seconds(&watch), n->control_name);
        }
        if (osoe_stall_watch_notice_due(&watch))
            osoe_notice("shmem_init on node %u has waited %llu s for the local lead, rank %u, "
                        "to create the node's control segment %s, which %s; still waiting",
                        b->node, (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                        b->gtids[0], n->control_name,
                        fd >= 0 ? "exists but is not sized yet" : "does not exist yet");
        nanosleep(&backoff, NULL);
        if (backoff.tv_nsec < 64 * 1000 * 1000)
            backoff.tv_nsec *= 2;
    }

    /* Sized is not ready: the lead publishes `ready` after the header. What
     * was opened may also be a leftover the lead is about to reclaim, which
     * will never be ready (shm-object.h): then start over. */
    for (;;) {
        if (atomic_load_explicit(&n->hdr->ready, memory_order_acquire))
            break;
        if (osoe_shm_orphaned(n->control_fd)) {
            munmap(n->hdr, n->control_bytes);
            close(n->control_fd);
            n->hdr = NULL;
            n->control_fd = -1;
            goto again;
        }
        check_failed(n);
        if (osoe_stall_watch_stalled(&watch, 0))
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead "
                       "has not initialized %s",
                       (unsigned long long)osoe_stall_watch_seconds(&watch), n->control_name);
        if (osoe_stall_watch_notice_due(&watch))
            osoe_notice("shmem_init on node %u has waited %llu s for the local lead, rank %u, "
                        "to finish the node's control segment %s, which it has created; "
                        "still waiting", b->node,
                        (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                        b->gtids[0], n->control_name);
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(&n->hdr->ready, 0, &sweep, OSOE_FUTEX_SHARED);
    }

    struct osoe_node_header *h = n->hdr;
    uint64_t header_bytes = offsetof(struct osoe_node_header, team) +
                            (uint64_t)h->dir.team_count * sizeof(struct osoe_team_block);
    if (h->dir.magic != OSOE_NODE_MAGIC || h->dir.layout != OSOE_NODE_LAYOUT ||
        h->dir.header_bytes != header_bytes || h->dir.slot_bytes != sizeof(struct osoe_pe_slot) ||
        h->dir.team_bytes != sizeof(struct osoe_team_block) ||
        h->dir.teams_offset != offsetof(struct osoe_node_header, team))
        osoe_fatal("control segment %s has layout %u; this library expects %u (mixed "
                   "library versions?)", n->control_name, h->dir.layout, OSOE_NODE_LAYOUT);
    /* The lead sized the team blocks, the published epochs and every PE's
     * internal region by its own OSOE_MAX_TEAMS, and this PE would index them
     * by its own: the rendezvous agrees the count between nodes (§6.2), and
     * this between a node's PEs, as check_agreement does the heap's size. */
    if (h->dir.team_count != b->max_teams) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_MAX_TEAMS differs between PEs: the local lead has %u, "
                 "this PE has %u (§5.7)", h->dir.team_count, b->max_teams);
        fail_rendezvous(n, msg);
    }
    /* And by its fan-out and pipeline depth, which a created team's local lead
     * - any PE of the node - also builds its tree and keeps its window by. */
    if (h->collective_fanout != b->collective_fanout) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_COLLECTIVE_FANOUT differs between PEs: the local lead "
                 "has %u, this PE has %u (§5.3)", h->collective_fanout, b->collective_fanout);
        fail_rendezvous(n, msg);
    }
    if (h->collective_pipeline != b->collective_pipeline) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_COLLECTIVE_PIPELINE differs between PEs: the local "
                 "lead has %u, this PE has %u (§5.8)", h->collective_pipeline,
                 b->collective_pipeline);
        fail_rendezvous(n, msg);
    }
    /* And by its OSOE_MAX_CONTEXTS, which sizes every PE's pool of context
     * blocks (layout 21): this PE would claim blocks past its own pool's end. */
    if (h->dir.contexts_per_pe != b->max_contexts) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_MAX_CONTEXTS differs between PEs: the local lead has "
                 "%u, this PE has %u (§4.11)", h->dir.contexts_per_pe, b->max_contexts);
        fail_rendezvous(n, msg);
    }
    /* And by its OSOE_MAX_THREADS, which sizes every PE's counter blocks and
     * thread records: this PE would claim past its own (the round-three
     * review's C3; until then a non-lead never read the variable). */
    if (h->dir.blocks_per_pe != b->max_threads + 1) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_MAX_THREADS differs between PEs: the local lead has "
                 "%u, this PE has %u (§3.14)", h->dir.blocks_per_pe - 1, b->max_threads);
        fail_rendezvous(n, msg);
    }
    /* And by its OSOE_GET_LANDING_BYTES, which sizes every PE's landing zone:
     * this PE's gets would be counted against a zone of another size. */
    struct osoe_am_config am;
    osoe_am_config_read(&am);
    if (h->landing_arena_bytes != am.get_landing_bytes) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_GET_LANDING_BYTES differs between PEs: the local lead "
                 "has %u, this PE has %llu (§3.12)", h->landing_arena_bytes,
                 (unsigned long long)am.get_landing_bytes);
        fail_rendezvous(n, msg);
    }
    if (h->job != b->job || h->step != b->step || h->node != b->node ||
        h->session != session || h->dir.local_count != b->local_count ||
        h->world_size != b->world_size)
        osoe_fatal("control segment %s describes a different job: %u local of %u PEs, "
                   "session %u; this PE expects %u of %u, session %u", n->control_name,
                   h->dir.local_count, h->world_size, h->session, b->local_count,
                   b->world_size, session);
    if (h->dir.slots_offset + (uint64_t)h->dir.local_count * h->dir.slot_bytes > n->control_bytes ||
        h->dir.blocks_offset + h->dir.blocks_bytes > n->control_bytes ||
        h->dir.connections_offset + h->dir.connections_bytes > n->control_bytes ||
        h->dir.metrics_offset + h->dir.metrics_bytes > n->control_bytes ||
        header_bytes > n->control_bytes ||
        h->dir.epoch_row_bytes < (uint64_t)h->dir.team_count * sizeof(uint32_t) ||
        h->dir.epoch_row_count != h->dir.local_count ||
        h->dir.epochs_offset + (uint64_t)h->dir.epoch_row_count * h->dir.epoch_row_bytes >
            n->control_bytes)
        osoe_fatal("control segment %s is too small for its own directory",
                   n->control_name);
    n->slots = (struct osoe_pe_slot *)((char *)n->hdr + h->dir.slots_offset);
    n->blocks = (struct osoe_counter_block *)((char *)n->hdr + h->dir.blocks_offset);
    /* Read from the directory rather than recomputed: a PE that derived the
     * offset itself would disagree with the lead the moment the two ran
     * different builds, which is what the frozen directory exists to prevent. */
    n->landing = (unsigned char *)n->hdr + h->dir.landing_offset;
    n->landing_slot_bytes = h->dir.landing_slot_bytes;
    n->landing_data_bytes = h->landing_data_bytes;
    n->landing_requests = h->landing_requests;
    n->landing_requests_offset = h->landing_requests_offset;
    n->landing_arena_offset = h->landing_arena_offset;
    n->landing_arena_bytes = h->landing_arena_bytes;
    n->connection_metrics = h->dir.connection_count
        ? (struct osoe_am_connection_metrics *)((char *)n->hdr + h->dir.connections_offset)
        : NULL;
    n->connection_count = h->dir.connection_count;
    n->team_count = h->dir.team_count;
    n->epochs = (_Atomic uint32_t *)((char *)n->hdr + h->dir.epochs_offset);
    n->epoch_row_words = h->dir.epoch_row_bytes / (uint32_t)sizeof(uint32_t);
    n->contexts = (struct osoe_context_block *)((char *)n->hdr + h->dir.contexts_offset);
    n->contexts_per_pe = h->dir.contexts_per_pe;
    n->threads = (struct osoe_thread_record *)((char *)n->hdr + h->dir.threads_offset);
    n->threads_per_pe = h->dir.threads_per_pe;

    /* The metric table is fixed for a build, so a mismatch is a mixed-library
     * job that the layout check above did not catch (§3.13's argument for
     * checksumming the handler table, applied here). */
    uint32_t want_count;
    const struct osoe_metric_descriptor *want = osoe_metrics_table(&want_count);
    if (h->dir.metrics_count != want_count ||
        h->dir.metrics_checksum != osoe_metric_checksum(want, want_count))
        osoe_fatal("control segment %s carries a different metric table (%u metrics, "
                   "checksum %#x); this library has %u and %#x", n->control_name,
                   h->dir.metrics_count, h->dir.metrics_checksum, want_count,
                   osoe_metric_checksum(want, want_count));
}

/*
 * The O(L) answer to "which local id is world rank r" (node.h). O(L log L)
 * once at init - an insertion sort would be L^2 = 65,536 steps at 256, which
 * is also fine, but qsort says what it does.
 */
static int compare_rank(const void *a, const void *b)
{
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : x > y;
}

static void build_local_index(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    uint32_t count = b->local_count;
    n->local_count = count;
    n->first_rank = count ? b->gtids[0] : 0;
    n->local_block = true;
    for (uint32_t i = 0; i < count; i++)
        if (b->gtids[i] != n->first_rank + i)
            n->local_block = false;
    n->sorted_rank = NULL;
    n->sorted_local = NULL;
    if (n->local_block)
        return;
    uint32_t (*pairs)[2] = malloc(sizeof *pairs * count);
    n->sorted_rank = malloc(sizeof *n->sorted_rank * count);
    n->sorted_local = malloc(sizeof *n->sorted_local * count);
    if (!pairs || !n->sorted_rank || !n->sorted_local)
        osoe_fatal("out of memory");
    for (uint32_t i = 0; i < count; i++) {
        pairs[i][0] = b->gtids[i];
        pairs[i][1] = i;
    }
    qsort(pairs, count, sizeof *pairs, compare_rank);
    for (uint32_t i = 0; i < count; i++) {
        n->sorted_rank[i] = pairs[i][0];
        n->sorted_local[i] = pairs[i][1];
    }
    free(pairs);
}

int32_t osoe_node_local_search(const struct osoe_node *n, uint32_t rank)
{
    uint32_t lo = 0, hi = n->local_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (n->sorted_rank[mid] < rank)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n->local_count && n->sorted_rank[lo] == rank ? (int32_t)n->sorted_local[lo]
                                                             : -1;
}

void osoe_node_join(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session)
{
    bool lead = b->local_id == 0;
    n->control_fd = -1;
    n->keep_name = b->keep_control_segment;

    control_name(n->control_name, sizeof n->control_name, b, session);
    n->heap_size = b->symmetric_size;
    /* The heap, then OSOE_REGION_INTERNAL, in one object (internal-region.h):
     * the allocator is given heap_size and never reaches the tail. */
    n->internal_offset = page_round(b->symmetric_size ? b->symmetric_size : 1);
    n->internal_bytes = page_round(osoe_internal_region_bytes(b->collective_fanout,
                                                              b->collective_pipeline,
                                                              b->max_teams, b->node_count));
    n->heap_bytes = n->internal_offset + n->internal_bytes;
    n->heap_of = calloc(b->local_count, sizeof *n->heap_of);
    if (!n->heap_of)
        osoe_fatal("out of memory");
    find_statics(n);
    n->statics_of = NULL;
    if (n->statics_bytes) {
        n->statics_of = calloc(b->local_count, sizeof *n->statics_of);
        if (!n->statics_of)
            osoe_fatal("out of memory");
    }
    build_local_index(n, b);

    if (lead)
        create_control(n, b, session);
    else
        open_control(n, b, session);

    struct osoe_pe_slot *me = &n->slots[b->local_id];
    if (atomic_load_explicit(&me->phase, memory_order_acquire) != OSOE_PHASE_EMPTY) {
        char msg[128];
        snprintf(msg, sizeof msg, "another process already joined with this %sLOCALID",
                 b->launcher ? b->launcher : "OSOE_");
        fail_rendezvous(n, msg);
    }
    int len = snprintf(me->heap_name, sizeof me->heap_name, "%s.heap.%u", n->control_name,
                       b->local_id);
    if (len < 0 || (size_t)len >= sizeof me->heap_name)
        fail_rendezvous(n, "heap object name too long");

    uint64_t base = create_own_heap(n, b, me, lead ? 0 : n->hdr->heap_base);
    if (!base) {
        char msg[256];
        if (lead)
            snprintf(msg, sizeof msg, "no candidate heap base is free for a %llu-byte heap; "
                     "set OSOE_HEAP_BASE to a free address", (unsigned long long)n->heap_bytes);
        else
            snprintf(msg, sizeof msg, "cannot map this PE's heap at the node's base %#llx "
                     "plus its shift %#llx (OSOE_HEAP_SHIFT), which the local lead chose; set "
                     "OSOE_HEAP_BASE to an address free in every PE",
                     (unsigned long long)n->hdr->heap_base,
                     (unsigned long long)(b->heap_shift * b->rank));
        shm_unlink(me->heap_name);
        fail_rendezvous(n, msg);
    }
    n->heap_of[b->local_id] = (void *)(uintptr_t)(base + b->heap_shift * b->rank);

    if (lead) {
        n->hdr->heap_base = base;
        /* release: a futex word that is its own predicate (§3.16). */
        atomic_store_explicit(&n->hdr->ready, 1, memory_order_release);
        osoe_futex_wake(&n->hdr->ready, INT_MAX, OSOE_FUTEX_SHARED);
    }
    /* After the lead's `ready`, so that no PE waits on the lead's copy to
     * begin its own. */
    if (n->statics_bytes)
        create_own_statics(n, b, me);

    /* Publish this PE's slot, then count it. */
    me->rank = b->rank;
    me->pid = (int32_t)getpid();
    me->tid = (int32_t)gettid();
    /* Before any thread of the library's exists, so this is the task's own
     * binding: what the node's agents may run on (agent.c). None known - a
     * kernel that will not say - leaves the agents where they are. */
    cpu_set_t task;
    CPU_ZERO(&task);
    if (sched_getaffinity(0, sizeof task, &task) == 0)
        for (int cpu = 0; cpu < CPU_SETSIZE && cpu < OSOE_TASK_CPU_WORDS * 64; cpu++)
            if (CPU_ISSET(cpu, &task))
                me->task_cpus[cpu / 64] |= UINT64_C(1) << (cpu % 64);
    me->symmetric_size = b->symmetric_size;
    me->heap_bytes = n->heap_bytes;
    me->statics_bytes = n->statics_bytes;
    atomic_store_explicit(&me->phase, OSOE_PHASE_JOINED, memory_order_release);
    atomic_fetch_add_explicit(&n->hdr->joined, 1, memory_order_seq_cst);
    osoe_futex_wake(&n->hdr->joined, INT_MAX, OSOE_FUTEX_SHARED);

    if (wait_count(n, b, &n->hdr->joined, b->local_count, OSOE_PHASE_JOINED,
                   "join the node (call shmem_init)") != 0) {
        char missing[256], msg[400];
        describe_missing(n, b, OSOE_PHASE_JOINED, missing, sizeof missing);
        snprintf(msg, sizeof msg, "no local PE has joined for OSOE_NO_PROGRESS_TIMEOUT "
                 "seconds; missing local ids:%s", missing);
        fail_rendezvous(n, msg);
    }
    check_agreement(n, b);
    map_siblings(n, b);
    osoe_metrics_init(n->blocks, n->hdr->dir.blocks_per_pe, b->local_id);

    atomic_store_explicit(&me->phase, OSOE_PHASE_MAPPED, memory_order_release);
    atomic_fetch_add_explicit(&n->hdr->mapped, 1, memory_order_seq_cst);
    osoe_futex_wake(&n->hdr->mapped, INT_MAX, OSOE_FUTEX_SHARED);

    /* The lead removes heap and statics names once every PE holds a mapping,
     * so a later crash leaves neither behind (§6.2) - and the control segment's name with
     * them, since every local PE opened it before it could map a sibling's
     * heap. From here nothing this node created in /dev/shm has a name: no
     * death of any process can strand it, and osoe-stat finds the segment
     * through a live PE's descriptor (§3.14). OSOE_KEEP_CONTROL_SEGMENT keeps
     * the control segment's name until finalize instead.
     *
     * Only the lead waits for `mapped`, yet no local PE returns from init with
     * these names still there: the lead publishes lead_agent_running after
     * removing them, and every other PE waits for that word before it returns
     * (init.c, §6.2). So the removal belongs here, before that store; moved
     * after it, a PE could leave init while the names remain. */
    if (lead) {
        if (wait_count(n, b, &n->hdr->mapped, b->local_count, OSOE_PHASE_MAPPED,
                       "map their node-mates' heaps") != 0) {
            char missing[256], msg[400];
            describe_missing(n, b, OSOE_PHASE_MAPPED, missing, sizeof missing);
            snprintf(msg, sizeof msg, "no local PE has finished mapping its siblings' "
                     "heaps for OSOE_NO_PROGRESS_TIMEOUT seconds; missing local ids:%s",
                     missing);
            fail_rendezvous(n, msg);
        }
        for (uint32_t i = 0; i < b->local_count; i++) {
            if (shm_unlink(n->slots[i].heap_name) != 0)
                osoe_fatal("cannot remove heap name %s: %s", n->slots[i].heap_name,
                           strerror(errno));
            if (n->statics_bytes && shm_unlink(n->slots[i].statics_name) != 0)
                osoe_fatal("cannot remove statics name %s: %s", n->slots[i].statics_name,
                           strerror(errno));
        }
        if (!n->keep_name && shm_unlink(n->control_name) != 0)
            osoe_fatal("cannot remove the control segment's name %s: %s", n->control_name,
                       strerror(errno));
    }
}

/*
 * First writer wins: one CAS from zero installs the disposition, the status and
 * the rank together (node.h), so a global exit's status is never overwritten
 * by a fatal that follows it, and a reader's one load has all three. An
 * earlier draft claimed the word, wrote the rank and status beside it and then
 * published; a declarer killed between the claim and the publish would have
 * left every reader waiting for a publish that could not come.
 */
bool osoe_node_declare_abort(struct osoe_node *n, uint32_t pe, int status,
                             enum osoe_abort_disposition how)
{
    if (!n->hdr)
        return false;
    uint64_t none = 0;
    uint64_t word = (uint64_t)how << OSOE_ABORT_DISPOSITION_SHIFT |
                    (uint64_t)(uint8_t)status << OSOE_ABORT_STATUS_SHIFT | pe;
    if (!atomic_compare_exchange_strong_explicit(&n->hdr->aborting, &none, word,
                                                 memory_order_seq_cst, memory_order_acquire))
        return false;
    osoe_node_wake_agents(n);
    return true;
}

/*
 * The stop protocol's order (agent.c): the word is stored, then each agent's
 * wake word bumped, then woken - so an agent that loaded its wake word before
 * the bump finds the futex compare failing, and one that loaded it after sees
 * the word. In local-id order, so the lead's agent first: it is the one that
 * tells the other nodes, and it wakes them all again when it does (agent.c),
 * which covers an agent a declarer did not reach - its own agent can see the
 * word and end the process before the declarer is done. O(L).
 */
void osoe_node_wake_agents(struct osoe_node *n)
{
    for (uint32_t i = 0; n->slots && i < n->local_count; i++) {
        atomic_fetch_add_explicit(&n->slots[i].agent_wake, 1, memory_order_seq_cst);
        osoe_futex_wake(&n->slots[i].agent_wake, INT_MAX, OSOE_FUTEX_SHARED);
    }
}

enum osoe_abort_disposition osoe_node_abort_read(const struct osoe_node *n, uint32_t *pe,
                                                 int *status)
{
    if (!n->hdr)
        return OSOE_ABORT_NONE;
    uint64_t word = atomic_load_explicit(&n->hdr->aborting, memory_order_acquire);
    if (word != 0) {
        *pe = (uint32_t)word;
        *status = (uint8_t)(word >> OSOE_ABORT_STATUS_SHIFT);
    }
    return (enum osoe_abort_disposition)(uint8_t)(word >> OSOE_ABORT_DISPOSITION_SHIFT);
}

void osoe_node_leave(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    struct osoe_pe_slot *me = &n->slots[b->local_id];
    osoe_metrics_fini();
    for (uint32_t i = 0; i < b->local_count; i++)
        if (n->heap_of[i])
            munmap(n->heap_of[i], n->slots[i].heap_bytes);
    free(n->heap_of);
    n->heap_of = NULL;
    /* The object's mappings over the program's own statics stay: they are
     * where its globals now live, and a later session copies them from there
     * into its own object (§6.4). */
    for (uint32_t i = 0; n->statics_of && i < b->local_count; i++)
        if (n->statics_of[i])
            munmap(n->statics_of[i], n->statics_bytes);
    free(n->statics_of);
    n->statics_of = NULL;
    free(n->sorted_rank);
    free(n->sorted_local);
    n->sorted_rank = n->sorted_local = NULL;
    n->local_block = false;
    n->local_count = 0;

    atomic_store_explicit(&me->phase, OSOE_PHASE_FINALIZED, memory_order_release);
    uint32_t done = atomic_fetch_add_explicit(&n->hdr->finalized, 1, memory_order_seq_cst) + 1;
    /* With OSOE_KEEP_CONTROL_SEGMENT the name lasted until now: exactly one
     * PE's increment reaches L, and that PE removes it. Others may still hold
     * mappings, which stay valid. The session number in the name keeps this
     * from removing a segment a PE has already re-created (§6.4). Otherwise the
     * name went at init, and there is nothing to remove. ENOENT is not an error
     * either way: someone - osoe-stat --gc, an operator - may have beaten us
     * to it, and the object is what matters, not its name. */
    if (n->keep_name && done == b->local_count && shm_unlink(n->control_name) != 0 &&
        errno != ENOENT)
        osoe_fatal("cannot remove control segment %s: %s", n->control_name, strerror(errno));

    munmap(n->hdr, n->control_bytes);
    if (n->control_fd >= 0)
        close(n->control_fd);
    n->control_fd = -1;
    n->hdr = NULL;
    n->slots = NULL;
    n->blocks = NULL;
    n->landing = NULL;
    n->epochs = NULL;
    n->team_count = 0;
    n->control_name[0] = '\0';
}
