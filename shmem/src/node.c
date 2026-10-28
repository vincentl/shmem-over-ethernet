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
#include "base/pid-namespace.h"
#include "base/print-line.h"
#include "base/stall-watch.h"
#include "abort.h"
#include "fatal.h"
#include "shm-object.h"
#include "am-configuration.h"

#include "blocked.h"
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
_Static_assert(sizeof(struct osoe_segment_directory) == 312,
               "frozen by §3.14: append only. 136 at layouts 6 and 7; 160 at layout 12, "
               "with the connections section; 208 at layout 18, with the team blocks and "
               "the published epochs; 232 at layout 21, with the context pools; 256 at "
               "layout 23, with the thread records; 288 at layout 29, with the team "
               "blocks' map and free bitmap; 312 at layout 36, with the owners' records");

/* And a PE of another osoe, whatever its layout, finds `ready` and `failed`
 * where layout 37 put them - 512 and 528 bytes in on a line of 128, 448 and 464
 * on a line of 64 - so that it waits for the one and is refused by the layout
 * it then reads (open_control), rather than waiting on a word moved under it:
 * layout 32 moved them, and a PE of 0.1.0 beside a lead of 0.2 waited until the
 * launcher's timeout (round nine's portability P9-3; P14). The connection area
 * pins its own the same way (connection-area.c). A layout that must move them
 * changes the magic. */
_Static_assert(offsetof(struct osoe_node_header, ready) == (OSOE_CACHELINE == 128 ? 512 : 448) &&
               offsetof(struct osoe_node_header, failed) ==
                       (OSOE_CACHELINE == 128 ? 528 : 464),
               "the control segment's ready and failed stay where layout 37 put them");

/*
 * Candidate heap bases, tried in order by the lead; the first where its own
 * heap maps becomes B for the node (§4.1). No single address is free under
 * every build in the development VM (aarch64, 48-bit): AddressSanitizer and
 * ThreadSanitizer reserve disjoint ranges. The list covers a plain build, ASan,
 * TSan, 39-bit address spaces, and the older ThreadSanitizer of GCC 11 (Rocky
 * Linux 9), whose application memory is the low 8 GiB, a 20 GiB band at
 * 0xaaaa00000000 and the top 4 GiB, in that order. OSOE_HEAP_BASE replaces it.
 */
static const uint64_t heap_base_candidates[] = {
    UINT64_C(0x100000000000),
    UINT64_C(0x300000000000),
    UINT64_C(0x010000000000),
    UINT64_C(0x001000000000),
    UINT64_C(0x000100000000),
};

/*
 * ThreadSanitizer's interception of mmap, asked for an address outside its
 * application memory, replaces the address with none where the call does not
 * say MAP_FIXED, and refuses the call with EINVAL where it does. Asked for a
 * candidate by MAP_FIXED_NOREPLACE alone, then, it hands the kernel a request
 * for address 0 itself: a user is refused (EPERM, below vm.mmap_min_addr) and
 * the next candidate is tried, but root may map page 0, and the tool then ends
 * the process ("mmap at bad address") - as the first two candidates, its shadow
 * and its meta ranges on x86-64, did to every job root ran (P34 of
 * docs/prompts/v0.2-open-items.md). So under it the heap asks MAP_FIXED as
 * well, which the kernel reads as MAP_FIXED_NOREPLACE alone - it sets MAP_FIXED
 * itself for that flag, since Linux 4.17, the library's floor - and a candidate
 * the tool keeps for itself is refused by name and the next one tried, never
 * mapped at 0.
 */
#if defined(__SANITIZE_THREAD__)
#define UNDER_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_THREAD_SANITIZER 1
#endif
#endif
#ifndef UNDER_THREAD_SANITIZER
#define UNDER_THREAD_SANITIZER 0
#endif

/* Sections of the control segment start on a cache line, so none shares one. */
static uint64_t align_up(uint64_t n);

/* Where the pool's posted bitmap is (layout 30; node.h): the line after the
 * free bitmap, which is as long - found from the free bitmap's directory
 * fields, so that the directory did not grow for it. */
static uint64_t team_posted_offset_of(uint64_t free_offset, uint32_t free_words)
{
    return align_up(free_offset + (uint64_t)free_words * sizeof(uint64_t));
}

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
    uint64_t launch;
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
    char err[512];
    int fd = osoe_shm_create(refusal.name, (long)offsetof(struct osoe_node_header, creator_pid),
                             (long)offsetof(struct osoe_node_header, creator_pid_namespace),
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
    h->creator_pid_namespace = osoe_pid_namespace_own();
    h->dir.magic = OSOE_NODE_MAGIC;
    h->dir.layout = OSOE_NODE_LAYOUT;
    h->dir.directory_bytes = sizeof h->dir;
    h->dir.header_bytes = (uint32_t)sizeof *h;
    h->dir.segment_bytes = bytes;
    h->job = refusal.job;
    atomic_store_explicit(&h->launch, refusal.launch, memory_order_release);
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
            /* Not a leftover of another launch by the same names, which this
             * launch's lead is about to reclaim (open_control). */
            if ((atomic_load_explicit(&h->ready, memory_order_acquire) ||
                 atomic_load_explicit(&h->failed, memory_order_acquire)) &&
                atomic_load_explicit(&h->launch, memory_order_acquire) != refusal.launch) {
                munmap(h, sizeof *h);
                fd = -1;
            } else {
                atomic_store_explicit(&h->failed, 1, memory_order_release);
                osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
                osoe_futex_wake(&h->joined, INT_MAX, OSOE_FUTEX_SHARED);
                osoe_futex_wake(&h->mapped, INT_MAX, OSOE_FUTEX_SHARED);
                return;
            }
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
    refusal.launch = b->launch;
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

/* The world rank of the lowest local PE not yet at `phase`, or UINT32_MAX. */
static uint32_t first_missing_rank(const struct osoe_node *n, const struct osoe_bootstrap *b,
                                   enum osoe_phase phase)
{
    for (uint32_t i = 0; i < b->local_count; i++)
        if (atomic_load_explicit(&n->slots[i].phase, memory_order_acquire) < phase)
            return b->gtids[i];
    return UINT32_MAX;
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
/*
 * Whether the node's lead has ended: its process gone, by the pid it recorded
 * in the header. Liveness watches it only from after the join (liveness.h);
 * before, a lead that died with its segment made left the node's other PEs
 * waiting on its count in silence, since the lead is the one that speaks for
 * the node (the adversarial read of 2026-10-03). A segment of another launch
 * is never this far (open_control); before the lead's `ready` it may be, and
 * lead_ended_before_ready is the check there.
 *
 * Only from inside the lead's PID namespace (D26): from another, its pid names
 * another process or none, and a lead at work would be taken for one that had
 * ended - a refusal naming a cause that is not the cause. There the lead is
 * not judged at all, and the node is refused once its PEs have joined
 * (check_agreement), every PE naming the same two. Its namespace is beside its
 * pid in the header, read after the same acquire.
 */
static bool lead_ended(const struct osoe_node *n)
{
    if (osoe_pid_namespace_differ(n->hdr->creator_pid_namespace, n->pid_namespace))
        return false;
    int32_t pid = n->hdr->creator_pid;
    return pid > 0 && kill((pid_t)pid, 0) != 0 && errno == ESRCH;
}

static int wait_count(struct osoe_node *n, const struct osoe_bootstrap *b, _Atomic uint32_t *word,
                      uint32_t target, enum osoe_phase phase, const char *doing)
{
    struct osoe_stall_watch watch;
    uint32_t start = atomic_load_explicit(word, memory_order_acquire);
    if (b->local_id == 0)
        osoe_stall_watch_begin_noticed(&watch, start, b->no_progress_ns);
    else
        osoe_stall_watch_begin(&watch, start, b->no_progress_ns);
    /* The "blocked in" record says shmem_init and the PE waited for - the
     * lowest local PE not there yet, by rank - begun again as it changes, so
     * that osoe-stat shows an init hang where every PE read "-" (D7; round
     * three's readiness report). Nothing else reads it: the deadlock
     * detector counts waits by osoe_wait_sleeps, which start-up never calls.
     * Every enter is left on the way out: the record nests (metrics.c), and
     * one left open would name shmem_init under every later wait. */
    bool recorded = false;
    uint32_t recorded_rank = UINT32_MAX;
    for (;;) {
        uint32_t v = atomic_load_explicit(word, memory_order_acquire);
        check_failed(n);
        if (v == target) {
            if (recorded)
                osoe_blocked_leave();
            return 0;
        }
        if (b->local_id != 0 && lead_ended(n)) {
            char msg[256];
            snprintf(msg, sizeof msg, "the local lead, rank %u (pid %d), has ended while this "
                     "PE waited for local PEs to %s", b->gtids[0], (int)n->hdr->creator_pid,
                     doing);
            fail_rendezvous(n, msg);
        }
        if (osoe_stall_watch_stalled(&watch, v)) {
            if (recorded)
                osoe_blocked_leave();
            return -1;
        }
        if (osoe_stall_watch_notice_due(&watch)) {
            char missing[256];
            describe_missing(n, b, phase, missing, sizeof missing);
            osoe_notice("shmem_init on node %u has waited %llu s for local PEs to %s: %u of %u "
                        "have; still waiting for local id%s", b->node,
                        (unsigned long long)osoe_stall_watch_waited_seconds(&watch), doing, v,
                        target, missing);
        }
        /* No PE at all where every local PE is at `phase` and the count
         * has yet to say so. */
        uint32_t missing_rank = first_missing_rank(n, b, phase);
        if (!recorded || missing_rank != recorded_rank) {
            if (recorded)
                osoe_blocked_leave();
            if (missing_rank == UINT32_MAX)
                osoe_blocked_enter("shmem_init", OSOE_BLOCKED_DETAIL_NONE, 0);
            else
                osoe_blocked_enter("shmem_init", OSOE_BLOCKED_DETAIL_PE, missing_rank);
            recorded = true;
            recorded_rank = missing_rank;
        }
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(word, v, &sweep, OSOE_FUTEX_SHARED);
    }
}

/*
 * Why the kernel would not map a heap where it was asked to, in words: from
 * the errno of mmap's MAP_FIXED_NOREPLACE, or 0 where a kernel without it
 * placed the mapping elsewhere.
 */
static const char *heap_refusal(int refused)
{
    switch (refused) {
    case 0:
        return "the kernel placed it elsewhere";
    case EEXIST:
        return "something is mapped there already";
    case ENOMEM:
        return "it runs past the end of this process's address space, or past its limit of "
               "mappings";
    case EPERM:
        return "the kernel refuses an address that low (vm.mmap_min_addr)";
#if UNDER_THREAD_SANITIZER
    case EINVAL:
        return "ThreadSanitizer keeps that address for itself, outside its application memory";
#endif
    default:
        return strerror(refused);
    }
}

/* Create this PE's heap object and map it. With base == 0, try the
 * candidates and return the one that worked; 0 if none did, with *refused the
 * last refusal's errno (heap_refusal). */
static uint64_t create_own_heap(struct osoe_node *n, const struct osoe_bootstrap *b,
                                struct osoe_pe_slot *slot, uint64_t base, int *refused)
{
    /* A heap has nowhere to record its creator; a leftover with this name
     * predates the control segment this node's lead has just created, and so
     * is a dead job's, reclaimed if it is this user's. */
    char err[256];
    int fd = osoe_shm_create(slot->heap_name, OSOE_SHM_NO_OWNER, OSOE_SHM_NO_NAMESPACE, err,
                             sizeof err);
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
                       MAP_SHARED | MAP_FIXED_NOREPLACE | (UNDER_THREAD_SANITIZER ? MAP_FIXED : 0),
                       fd, 0);
        *refused = p == MAP_FAILED ? errno : 0;
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
    int fd = osoe_shm_create(slot->statics_name, OSOE_SHM_NO_OWNER, OSOE_SHM_NO_NAMESPACE, err,
                             sizeof err);
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
            snprintf(msg, sizeof msg, "local PE %u (rank %u)'s heap: %s", i, b->gtids[i],
                     err[0] ? err : strerror(errno));
            fail_rendezvous(n, msg);
        }
        void *p = mmap(NULL, s->heap_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) {
            char msg[256];
            snprintf(msg, sizeof msg, "cannot map local PE %u (rank %u)'s heap (%llu bytes): %s",
                     i, b->gtids[i], (unsigned long long)s->heap_bytes, strerror(errno));
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
            snprintf(msg, sizeof msg, "local PE %u (rank %u)'s statics: %s", i, b->gtids[i],
                     err[0] ? err : strerror(errno));
            fail_rendezvous(n, msg);
        }
        p = mmap(NULL, n->statics_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) {
            char msg[256];
            snprintf(msg, sizeof msg, "cannot map local PE %u (rank %u)'s statics (%llu bytes): "
                     "%s", i, b->gtids[i], (unsigned long long)n->statics_bytes, strerror(errno));
            fail_rendezvous(n, msg);
        }
        madvise(p, n->statics_bytes, MADV_DONTDUMP);
        n->statics_of[i] = p;
    }
}

/*
 * Every local PE must be in one PID namespace (D26), and have parsed the same
 * SHMEM_SYMMETRIC_SIZE (§4.1, §4.4). Every PE runs this check on the same
 * slots, so all of them fail together, naming the same PEs.
 *
 * The namespaces first, since the node's PEs watch each other by process id:
 * from another namespace a pid names another process, or none - a death
 * unseen, or a live PE taken for dead, by the liveness pidfds (liveness.h) and
 * by the deadlock detector's /proc/<pid>/task (deadlock.c). Each known one is
 * compared with the first PE's that is known, a PE whose /proc could not say
 * left out (base/pid-namespace.h); until this check every pid-based judgement
 * of the start is made only from inside the lead's namespace (lead_ended).
 */
static void check_agreement(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    uint32_t first = UINT32_MAX;
    for (uint32_t i = 0; i < b->local_count; i++) {
        struct osoe_pid_namespace ns = n->slots[i].pid_namespace;
        if (!osoe_pid_namespace_known(ns))
            continue;
        if (first == UINT32_MAX) {
            first = i;
            continue;
        }
        if (osoe_pid_namespace_differ(n->slots[first].pid_namespace, ns)) {
            char one[48], other[48], msg[512];
            osoe_pid_namespace_text(n->slots[first].pid_namespace, one, sizeof one);
            osoe_pid_namespace_text(ns, other, sizeof other);
            snprintf(msg, sizeof msg, "the node's PEs are in more than one PID namespace: "
                     "local PE %u (rank %u) is in %s and local PE %u (rank %u) in %s; a node's "
                     "PEs watch each other by process id and must share one - one container a "
                     "node, or containers that keep the host's PID namespace (README, "
                     "Building)", first, b->gtids[first], one, i, b->gtids[i], other);
            fail_rendezvous(n, msg);
        }
    }
    for (uint32_t i = 0; i < b->local_count; i++) {
        if (n->slots[i].symmetric_size != b->symmetric_size) {
            char msg[256];
            snprintf(msg, sizeof msg, "SHMEM_SYMMETRIC_SIZE differs between PEs: local PE "
                     "%u (rank %u) has %llu bytes, this PE has %llu", i, b->gtids[i],
                     (unsigned long long)n->slots[i].symmetric_size,
                     (unsigned long long)b->symmetric_size);
            fail_rendezvous(n, msg);
        }
        /* A backstop. Every setting that sizes the heap object - the symmetric
         * size above, the fan-out, the pipeline's depth, the small
         * collectives' bytes, the team count and the thread count in
         * open_control - is agreed by name; this fires only for one that a
         * later change forgets to agree, before the progress thread reads a
         * node-mate's internal region by this PE's size of it. */
        if (n->slots[i].heap_bytes != n->heap_bytes) {
            char msg[256];
            snprintf(msg, sizeof msg, "local PE %u (rank %u)'s heap object is %llu bytes and this "
                     "PE's %llu: the PEs size the library's internal region differently", i,
                     b->gtids[i],
                     (unsigned long long)n->slots[i].heap_bytes,
                     (unsigned long long)n->heap_bytes);
            fail_rendezvous(n, msg);
        }
        if (n->slots[i].statics_bytes != n->statics_bytes) {
            char msg[256];
            snprintf(msg, sizeof msg, "the PEs run different programs: local PE %u (rank %u)'s "
                     "symmetric statics are %llu bytes, this PE's %llu (§4.3)", i, b->gtids[i],
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
     * The header ends with the pool of team blocks (layout 29, §5.7; the team
     * redesign's step D): OSOE_TEAM_BLOCKS of them, about a kilobyte each with
     * 128-byte lines, so a megabyte of the segment at the default of 1,024.
     * Until layout 29 a block per team number, OSOE_MAX_TEAMS of them.
     */
    uint64_t teams_offset = offsetof(struct osoe_node_header, team);
    uint64_t teams_bytes = (uint64_t)b->team_blocks * sizeof(struct osoe_team_block);
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
    struct osoe_am_configuration am;
    osoe_am_configuration_read(&am, b->local_count);
    /* Since layout 23 a zone is extents (node.h): its header and bitmap, a
     * record an extent, then the extents. A piece of a get is at most a run of
     * 64 extents in one word of the bitmap, and at most a chunk, which the
     * wire's largest message holds. Whole extents, which osoe_am_configuration_read
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
     * shares no line with another. Since layout 30 a 64-bit word, the team's
     * block beside its epoch (node.h): 2,048 bytes a PE at the default 256 numbers.
     */
    uint64_t epoch_row_bytes = align_up((uint64_t)b->max_teams * sizeof(uint64_t));
    uint64_t epochs_offset = align_up(connections_offset + connections_bytes);
    uint64_t epochs_bytes = (uint64_t)b->local_count * epoch_row_bytes;

    /*
     * Each local PE's pool of context blocks (layout 21; §4.6, §4.11): a
     * cache line per context, OSOE_MAX_CONTEXTS of them a PE, 8 KiB a PE at
     * the default. Last, so that nothing before it moved when it was added.
     * Then one for each thread place (layout 37), the block a thread's
     * blocking gets count in, 2.1 KiB a PE at the default 17 places, so that a
     * blocking get's pieces go out together as a nonblocking get's do
     * (transport-internode.c, remote_get; docs/decisions/2026-10-10-blocking-gets.md).
     */
    uint64_t contexts_offset = align_up(epochs_offset + epochs_bytes);
    uint64_t contexts_bytes = (uint64_t)b->local_count * (b->max_contexts + per_pe) *
                              sizeof(struct osoe_context_block);
    /* Each local PE's thread records (layout 23; §4.11 item 3): a cache line a
     * thread, OSOE_MAX_THREADS of them a PE, one per counter block. */
    uint64_t threads_offset = align_up(contexts_offset + contexts_bytes);
    uint64_t threads_bytes = (uint64_t)b->local_count * per_pe *
                             sizeof(struct osoe_thread_record);
    /* The pool's map, a word per name - OSOE_MAX_TEAMS names for each local PE,
     * 1,024 bytes a PE at the default - and its bitmap of free blocks (layout 29;
     * step D); and its bitmap of posted blocks, as long, on a line of its own
     * (layout 30; team_posted_offset). Last, so that nothing before them moved. */
    uint64_t team_map_offset = align_up(threads_offset + threads_bytes);
    uint64_t team_map_bytes = (uint64_t)b->local_count * b->max_teams * sizeof(uint32_t);
    uint32_t team_free_words = (b->team_blocks + 63) / 64;
    uint64_t team_free_offset = align_up(team_map_offset + team_map_bytes);
    uint64_t team_free_bytes = (uint64_t)team_free_words * sizeof(uint64_t);
    uint64_t team_posted_offset = team_posted_offset_of(team_free_offset, team_free_words);
    /* The AM layer's counters, a record an owner of the node's wires - the
     * lead's agent and its OSOE_PROGRESS_HELPERS helpers (owner-map.h) - a
     * cache line or two each, none on a job of one node (layout 36). Last, so
     * that nothing before them moved. */
    uint64_t owner_count = connection_count ? (uint64_t)b->progress_helpers + 1 : 0;
    uint64_t owners_offset = align_up(team_posted_offset + team_free_bytes);
    uint64_t owners_bytes = owner_count * sizeof(struct osoe_am_owner_metrics);
    n->control_bytes = page_round(owners_offset + owners_bytes);

    /* Room for a leftover's whole story, another namespace's included. */
    char err[512];
    int fd = osoe_shm_create(n->control_name,
                             (long)offsetof(struct osoe_node_header, creator_pid),
                             (long)offsetof(struct osoe_node_header, creator_pid_namespace),
                             err, sizeof err);
    if (fd < 0)
        osoe_fatal("the control segment: %s", err);
    if (ftruncate(fd, (off_t)n->control_bytes) != 0) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot size control segment: %s", strerror(errno));
    }
    /* The pages the lead writes now - the header with the pool of team blocks,
     * the slots, the metric table, and the map of team names and the free
     * bitmap at the end - allocated rather than only sized: a /dev/shm too full
     * for even these refuses here, by name, where a store into a page it
     * cannot have would end the lead by SIGBUS and say nothing. The map's
     * other entries are stored at splits, by whichever PE leads a team on the
     * node, so it is allocated whole. The rest is checked below, with the
     * segment there to say so through. */
    int short_of = posix_fallocate(fd, 0, (off_t)blocks_offset);
    if (short_of == 0)
        short_of = posix_fallocate(fd, (off_t)team_map_offset,
                                   (off_t)(n->control_bytes - team_map_offset));
    if (short_of != 0) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot allocate the control segment's first %llu KiB and its last %llu "
                   "KiB in /dev/shm: %s", (unsigned long long)(blocks_offset >> 10),
                   (unsigned long long)((n->control_bytes - team_map_offset) >> 10),
                   strerror(short_of));
    }
    void *p = mmap(NULL, n->control_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot map control segment: %s", strerror(errno));
    }
    n->control_fd = fd;             /* kept: see struct osoe_node */

    /* ftruncate zeroed it: every atomic word starts at 0. The creator's pid and
     * namespace before `launch`'s release store below (node.h). */
    n->hdr = p;
    n->hdr->creator_pid = (int32_t)getpid();
    n->hdr->creator_pid_namespace = n->pid_namespace;
    n->slots = (struct osoe_pe_slot *)((char *)p + slots_offset);
    struct osoe_node_header *h = n->hdr;
    h->dir.magic = OSOE_NODE_MAGIC;
    h->dir.layout = OSOE_NODE_LAYOUT;
    h->dir.header_bytes = (uint32_t)header_bytes;
    h->dir.segment_bytes = n->control_bytes;
    h->job = b->job;
    atomic_store_explicit(&h->launch, b->launch, memory_order_release);
    h->step = b->step;
    h->node = b->node;
    h->session = session;
    h->dir.local_count = b->local_count;
    h->world_size = b->world_size;
    h->collective_fanout = b->collective_fanout;
    h->collective_pipeline = b->collective_pipeline;
    h->small_collective_bytes = b->small_collective_bytes;
    h->collective_shape = b->collective_shape;
    h->collective_places = b->collective_places;
    h->link_hop_ns = b->link_hop_ns;
    h->link_message_ns = b->link_message_ns;
    h->link_bytes_per_second = b->link_bytes_per_second;
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
    h->dir.team_count = b->team_blocks;
    h->dir.team_map_offset = team_map_offset;
    h->dir.team_map_bytes = team_map_bytes;
    h->dir.team_map_row = b->max_teams;
    h->dir.team_free_offset = team_free_offset;
    h->dir.team_free_words = team_free_words;
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
    h->dir.owners_offset = owners_offset;
    h->dir.owners_bytes = owners_bytes;
    h->dir.owner_bytes = (uint32_t)sizeof(struct osoe_am_owner_metrics);
    h->dir.owner_count = (uint32_t)owner_count;
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
    n->owner_metrics = owner_count
        ? (struct osoe_am_owner_metrics *)((char *)p + owners_offset) : NULL;
    n->owner_count = (uint32_t)owner_count;
    n->team_count = b->team_blocks;
    n->team_map = (_Atomic uint32_t *)((char *)p + team_map_offset);
    n->team_map_row = b->max_teams;
    n->team_free = (_Atomic uint64_t *)((char *)p + team_free_offset);
    n->team_free_words = team_free_words;
    n->team_posted = (_Atomic uint64_t *)((char *)p + team_posted_offset);
    n->epochs = (_Atomic uint64_t *)((char *)p + epochs_offset);
    n->epoch_row_words = (uint32_t)(epoch_row_bytes / sizeof(uint64_t));
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
    /*
     * The pool (layout 29; step D): the predefined teams' blocks, the first
     * three, each named (local id 0, its number) and mapped under that name,
     * every local PE holding it, at generation 1, never given back; and every
     * other block free. A free block is zero until it is first claimed, and
     * named then (osoe_node_team_name), which zeroes what a team counts on in
     * it whatever another team left there. The rest of each predefined block,
     * its handoff, tree state and record, is zero as it is; its record is
     * written by the lead's osoe_team_start (team.c).
     */
    for (uint32_t t = 0; t < OSOE_PREDEFINED_TEAMS; t++) {
        osoe_epoch_init(&h->team[t].arrived);
        osoe_epoch_init(&h->team[t].released);
        osoe_epoch_init(&h->team[t].copied);
        atomic_store_explicit(&h->team[t].name.lead, 0, memory_order_release);
        atomic_store_explicit(&h->team[t].name.number, t, memory_order_release);
        atomic_store_explicit(&h->team[t].name.holders, b->local_count, memory_order_release);
        atomic_store_explicit(&h->team[t].name.generation, 1, memory_order_release);
        atomic_store_explicit(osoe_node_team_map_entry(n, 0, t), t + 1, memory_order_release);
    }
    for (uint32_t i = OSOE_PREDEFINED_TEAMS; i < b->team_blocks; i++) {
        atomic_fetch_or_explicit(&n->team_free[i / 64], UINT64_C(1) << (i % 64),
                                 memory_order_seq_cst);
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
     * about 15.6 MiB at the defaults - 256 team numbers of about 24 KiB each, a
     * fan-out of 8, four pieces in flight (OSOE_COLLECTIVE_PIPELINE) and a lane
     * of 577 KiB for each of 17 thread records - which is counted here with the
     * heap it follows. Pages cost only once written, so the room above has taken
     * is only the segment's first and last pages, allocated so that they could
     * be written (about 1.1 MiB at the defaults, the pool of team blocks most
     * of it, counted again in `wanted`: a check that errs by that much toward
     * refusing).
     */
    uint64_t wanted = n->control_bytes +
                      (uint64_t)b->local_count * (n->heap_bytes + n->statics_bytes);
    uint64_t free_bytes;
    if (!osoe_shm_room(wanted, &free_bytes)) {
        char msg[768];
        char statics[96] = "";
        if (n->statics_bytes)
            snprintf(statics, sizeof statics, " and as many copies of the program's %llu MiB "
                     "of statics", (unsigned long long)((n->statics_bytes + (1u << 20) - 1) >> 20));
        snprintf(msg, sizeof msg, "/dev/shm has %llu MiB free, but this node's control "
                 "segment and %u heap%s of %llu MiB - each the symmetric heap and %llu MiB "
                 "of the library's own region, which OSOE_MAX_TEAMS, "
                 "OSOE_SMALL_COLLECTIVE_BYTES and OSOE_MAX_THREADS size -%s need %llu MiB: lower "
                 "SHMEM_SYMMETRIC_SIZE or those, run fewer PEs per node, or enlarge /dev/shm "
                 "(a container's --shm-size) - or, since pages cost only once written, "
                 "OSOE_SHM_CAPACITY_CHECK=0 reports this without refusing",
                 (unsigned long long)(free_bytes >> 20), b->local_count,
                 b->local_count == 1 ? "" : "s", (unsigned long long)(n->heap_bytes >> 20),
                 (unsigned long long)((n->internal_bytes + (1u << 20) - 1) >> 20), statics,
                 (unsigned long long)((wanted + (1u << 20) - 1) >> 20));
        if (!b->shm_capacity_warning_only)
            fail_rendezvous(n, msg);
        osoe_print_line("osoe: %s; continuing, since OSOE_SHM_CAPACITY_CHECK=0\n", msg);
    }
}

/*
 * Whether the node's lead has ended after it made the control segment and
 * before it finished it: killed while it made its heap, with the segment sized
 * and `ready` unstored, it left its node-mates waiting for the launcher to end
 * them, their notice saying that it was still at work (the release fixes'
 * review, correctness F2). Its pid is in the header, a plain field the lead
 * stores before its release store of `launch` (create_control), so it is read
 * only after an acquire load that returns this start's launch. That launch also
 * says that the segment is this start's own and not a failed one's leftover by
 * the same names, which this start's lead reclaims and a node-mate waits past
 * (open_control). A launch of 0 - a first Slurm run, a bootstrap made by hand
 * without OSOE_LAUNCH - is none: two starts with it cannot be told apart, so
 * the wait stays as it was, ended by the launcher, which under srun -K1 ends
 * the step when the lead dies (README). And a segment that has lost its name is
 * a leftover its lead reclaimed, where the loop starts over and does not end;
 * so the name is looked at last, after the death.
 */
static bool lead_ended_before_ready(const struct osoe_node *n, const struct osoe_bootstrap *b)
{
    if (b->launch == 0 ||
        atomic_load_explicit(&n->hdr->launch, memory_order_acquire) != b->launch)
        return false;
    return lead_ended(n) && !osoe_shm_orphaned(n->control_fd);
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
     * will never be ready (shm-object.h): then start over. And a leftover of
     * another launch by the same names - a requeued job, a reused pid - may
     * be ready, or failed, already: its `launch` says so, read after the
     * acquire load that published it (the adversarial read of 2026-10-03),
     * and it is waited past as one not yet reclaimed is. A lead that has
     * ended without storing `ready` ends the start (lead_ended_before_ready),
     * where its launch says that the pid in the header is this start's. */
    for (;;) {
        bool ready = atomic_load_explicit(&n->hdr->ready, memory_order_acquire) != 0;
        bool failed = atomic_load_explicit(&n->hdr->failed, memory_order_acquire) != 0;
        bool another_launch =
            (ready || failed) &&
            atomic_load_explicit(&n->hdr->launch, memory_order_acquire) != b->launch;
        if (ready && !another_launch)
            break;
        if (another_launch || osoe_shm_orphaned(n->control_fd)) {
            munmap(n->hdr, n->control_bytes);
            close(n->control_fd);
            n->hdr = NULL;
            n->control_fd = -1;
            if (another_launch) {
                /* Until this launch's lead reclaims it - and said, as every
                 * start-up wait is: the first loop, which maps a sized object
                 * at once, never reaches its own notice here. */
                if (osoe_stall_watch_stalled(&watch, 0))
                    osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): %s is "
                               "another launch's, and the local lead has not reclaimed it",
                               (unsigned long long)osoe_stall_watch_seconds(&watch),
                               n->control_name);
                if (osoe_stall_watch_notice_due(&watch))
                    osoe_notice("shmem_init on node %u has waited %llu s for the local lead, "
                                "rank %u, to reclaim %s, which another launch of this job's "
                                "ids left; still waiting", b->node,
                                (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                                b->gtids[0], n->control_name);
                struct timespec reclaim = { 0, 10 * 1000 * 1000 };
                nanosleep(&reclaim, NULL);
            }
            goto again;
        }
        /* The lead's end is seen before `failed` is read: a lead that refused
         * published that before it ended (publish_failure), and its node-mates
         * say so, not that it ended. */
        bool lead_gone = lead_ended_before_ready(n, b);
        check_failed(n);
        if (lead_gone) {
            char msg[256];
            snprintf(msg, sizeof msg, "the local lead, rank %u (pid %d), has ended before "
                     "finishing the node's control segment %s", b->gtids[0],
                     (int)n->hdr->creator_pid, n->control_name);
            fail_rendezvous(n, msg);
        }
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
    /* The lead sized the map, the published epochs and every PE's internal
     * region by its own OSOE_MAX_TEAMS, and this PE would index them by its
     * own: the rendezvous agrees the count between nodes (§6.2), and this
     * between a node's PEs, as check_agreement does the heap's size. */
    if (h->dir.team_map_row != b->max_teams) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_MAX_TEAMS differs between PEs: the local lead has %u, "
                 "this PE has %u (§5.7)", h->dir.team_map_row, b->max_teams);
        fail_rendezvous(n, msg);
    }
    /* And the pool by its OSOE_TEAM_BLOCKS, whose free bitmap this PE would
     * claim from as the local lead of a team (layout 29; step D). */
    if (h->dir.team_count != b->team_blocks) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_TEAM_BLOCKS differs between PEs: the local lead has "
                 "%u, this PE has %u (§5.7)", h->dir.team_count, b->team_blocks);
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
    /* And by its OSOE_SMALL_COLLECTIVE_BYTES, which sizes every team number's
     * small rows and decides which way each data collective moves (layout 31). */
    if (h->small_collective_bytes != b->small_collective_bytes) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_SMALL_COLLECTIVE_BYTES differs between PEs: the local "
                 "lead has %u, this PE has %u (§5.8)", h->small_collective_bytes,
                 b->small_collective_bytes);
        fail_rendezvous(n, msg);
    }
    /* And by its collective shape and link (layout 32), which any PE of the
     * node may choose a team's shape from as the team's local lead. */
    if (h->collective_shape != b->collective_shape) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_COLLECTIVE_SHAPE differs between PEs: the local lead "
                 "has %s, this PE has %s", osoe_collective_shape_name(h->collective_shape),
                 osoe_collective_shape_name(b->collective_shape));
        fail_rendezvous(n, msg);
    }
    /* And by how collect finds its places (layout 39), which any PE of the node
     * may decide for a team as its local lead. */
    if (h->collective_places != b->collective_places) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_COLLECTIVE_PLACES differs between PEs: the local lead "
                 "has %s, this PE has %s", osoe_collective_places_name(h->collective_places),
                 osoe_collective_places_name(b->collective_places));
        fail_rendezvous(n, msg);
    }
    if (h->link_hop_ns != b->link_hop_ns || h->link_message_ns != b->link_message_ns ||
        h->link_bytes_per_second != b->link_bytes_per_second) {
        char msg[320];
        snprintf(msg, sizeof msg, "OSOE_LINK_* differ between PEs: the local lead has a hop of "
                 "%llu ns, a message cost of %llu ns and %llu bytes a second, this PE %llu, "
                 "%llu and %llu (0: not set)", (unsigned long long)h->link_hop_ns,
                 (unsigned long long)h->link_message_ns,
                 (unsigned long long)h->link_bytes_per_second,
                 (unsigned long long)b->link_hop_ns, (unsigned long long)b->link_message_ns,
                 (unsigned long long)b->link_bytes_per_second);
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
    /* And by its OSOE_PROGRESS_HELPERS, which on a job of more than one node
     * sizes the owner records here and the node's connection area, whose
     * owners' words this PE would mark and wake by its own count (owner-map.h;
     * the adversarial read of Part 1, N3, which found the area refusing such a
     * PE in the name of OSOE_AM_*). */
    if (b->node_count > 1 && h->dir.owner_count != b->progress_helpers + 1) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_PROGRESS_HELPERS differs between PEs: the local lead "
                 "has %u, this PE has %u (§3.12)",
                 h->dir.owner_count ? h->dir.owner_count - 1 : 0, b->progress_helpers);
        fail_rendezvous(n, msg);
    }
    if (h->job != b->job || h->step != b->step || h->node != b->node ||
        h->session != session || h->dir.local_count != b->local_count ||
        h->world_size != b->world_size)
        osoe_fatal("control segment %s describes a different job: %u local of %u PEs, "
                   "session %u; this PE expects %u of %u, session %u", n->control_name,
                   h->dir.local_count, h->world_size, h->session, b->local_count,
                   b->world_size, session);
    /* And by its OSOE_GET_LANDING_BYTES, which sizes every PE's landing zone:
     * this PE's gets would be counted against a zone of another size. After
     * the node's PEs are known to be counted alike, which the default is split
     * among (P32). */
    struct osoe_am_configuration am;
    osoe_am_configuration_read(&am, b->local_count);
    if (h->landing_arena_bytes != am.get_landing_bytes) {
        char msg[256];
        snprintf(msg, sizeof msg, "OSOE_GET_LANDING_BYTES differs between PEs: the local lead "
                 "has %u, this PE has %llu (§3.12)", h->landing_arena_bytes,
                 (unsigned long long)am.get_landing_bytes);
        fail_rendezvous(n, msg);
    }
    if (h->dir.slots_offset + (uint64_t)h->dir.local_count * h->dir.slot_bytes > n->control_bytes ||
        h->dir.blocks_offset + h->dir.blocks_bytes > n->control_bytes ||
        h->dir.connections_offset + h->dir.connections_bytes > n->control_bytes ||
        h->dir.metrics_offset + h->dir.metrics_bytes > n->control_bytes ||
        header_bytes > n->control_bytes ||
        h->dir.epoch_row_bytes < (uint64_t)h->dir.team_map_row * sizeof(uint64_t) ||
        h->dir.epoch_row_count != h->dir.local_count ||
        h->dir.epochs_offset + (uint64_t)h->dir.epoch_row_count * h->dir.epoch_row_bytes >
            n->control_bytes ||
        h->dir.team_map_bytes < (uint64_t)h->dir.local_count * h->dir.team_map_row *
                                    sizeof(uint32_t) ||
        h->dir.team_map_offset + h->dir.team_map_bytes > n->control_bytes ||
        h->dir.team_free_words < (h->dir.team_count + 63) / 64 ||
        team_posted_offset_of(h->dir.team_free_offset, h->dir.team_free_words) +
                (uint64_t)h->dir.team_free_words * sizeof(uint64_t) >
            n->control_bytes ||
        h->dir.owner_bytes != sizeof(struct osoe_am_owner_metrics) ||
        h->dir.owners_bytes < (uint64_t)h->dir.owner_count * h->dir.owner_bytes ||
        h->dir.owners_offset + h->dir.owners_bytes > n->control_bytes ||
        /* The context pool, at its stride since layout 37 - the pool and a
         * block for each thread place - and the thread records, which every
         * PE indexes from the directory's counts (the adversarial read of
         * v0.2's blocking gets, note 9): a mis-sized section is refused here,
         * not stored past. */
        h->dir.context_bytes != sizeof(struct osoe_context_block) ||
        h->dir.contexts_bytes < (uint64_t)h->dir.local_count *
                                    ((uint64_t)h->dir.contexts_per_pe + h->dir.threads_per_pe) *
                                    h->dir.context_bytes ||
        h->dir.contexts_offset + h->dir.contexts_bytes > n->control_bytes ||
        h->dir.thread_record_bytes != sizeof(struct osoe_thread_record) ||
        h->dir.threads_bytes < (uint64_t)h->dir.local_count * h->dir.threads_per_pe *
                                   h->dir.thread_record_bytes ||
        h->dir.threads_offset + h->dir.threads_bytes > n->control_bytes)
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
    n->owner_metrics = h->dir.owner_count
        ? (struct osoe_am_owner_metrics *)((char *)n->hdr + h->dir.owners_offset)
        : NULL;
    n->owner_count = h->dir.owner_count;
    n->team_count = h->dir.team_count;
    n->team_map = (_Atomic uint32_t *)((char *)n->hdr + h->dir.team_map_offset);
    n->team_map_row = h->dir.team_map_row;
    n->team_free = (_Atomic uint64_t *)((char *)n->hdr + h->dir.team_free_offset);
    n->team_free_words = h->dir.team_free_words;
    n->team_posted = (_Atomic uint64_t *)((char *)n->hdr +
                                          team_posted_offset_of(h->dir.team_free_offset,
                                                                h->dir.team_free_words));
    n->epochs = (_Atomic uint64_t *)((char *)n->hdr + h->dir.epochs_offset);
    n->epoch_row_words = h->dir.epoch_row_bytes / (uint32_t)sizeof(uint64_t);
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

/* The bitmap's word and bit for block `block_index`. */
static _Atomic uint64_t *free_word(const struct osoe_node *n, uint32_t block_index)
{
    return &n->team_free[block_index / 64];
}

bool osoe_node_team_claim_rival;

/*
 * The members count goes to 0 as the block is claimed (node.h, osoe_team_name):
 * until then it is its last team's, which osoe_node_team_returning reads as
 * "on its way back" while the block is neither held nor in the bitmap.
 *
 * A word is left for the one below once it is seen empty, and the claim fails
 * once every word has been. A swap lost to a word changed since its load - a
 * lead claiming or giving back in it at the same time - starts that count
 * again at the word `local_id` names, once a claim (P9; docs/decisions/
 * 2026-10-18.md): 257 leads claiming at once would otherwise all try the top
 * word again. A swap failing spuriously, the word as loaded, is tried again
 * where it was, so that a claim nobody contends takes the highest free block.
 */
uint32_t osoe_node_team_claim(struct osoe_node *n, uint32_t local_id)
{
    uint32_t words = n->team_free_words, w = words - 1, left = words;
    bool moved = false;
    while (left > 0) {
        _Atomic uint64_t *word = &n->team_free[w];
        uint64_t bits = atomic_load_explicit(word, memory_order_acquire);
        bool lost = false;
        while (bits && !lost) {
            uint32_t bit = 63 - (uint32_t)__builtin_clzll(bits);   /* the highest free */
            uint32_t block_index = w * 64 + bit;
            if (block_index >= n->team_count)
                osoe_fatal("block %u of a pool of %u is marked free", block_index,
                           n->team_count);
            uint64_t seen = bits;
            if (osoe_node_team_claim_rival) {
                osoe_node_team_claim_rival = false;
                atomic_fetch_and_explicit(word, ~(UINT64_C(1) << bit), memory_order_seq_cst);
            }
            if (atomic_compare_exchange_weak_explicit(word, &bits,
                                                      bits & ~(UINT64_C(1) << bit),
                                                      memory_order_seq_cst,
                                                      memory_order_acquire)) {
                atomic_store_explicit(&osoe_node_team(n, block_index)->name.members, 0,
                                      memory_order_release);
                return block_index;
            }
            lost = bits != seen && !moved;
        }
        if (lost) {
            moved = true;
            w = local_id % words;
            left = words;
        } else {
            w = (w == 0 ? words : w) - 1;
            left--;
        }
    }
    return UINT32_MAX;
}

void osoe_node_team_return(struct osoe_node *n, uint32_t block_index)
{
    if (block_index < OSOE_PREDEFINED_TEAMS || block_index >= n->team_count)
        osoe_fatal("block %u given back to a pool of %u, whose first %u are never claimed",
                   block_index, n->team_count, OSOE_PREDEFINED_TEAMS);
    atomic_fetch_or_explicit(free_word(n, block_index), UINT64_C(1) << (block_index % 64),
                             memory_order_seq_cst);
}

/* The tree state of a block, for the team it is named for (node.h): zero, as
 * the progress thread leaves it between collectives, and no parent and no
 * child missing - OSOE_COLLECTIVE_TREE_NONE, which osoe-stat reads as "none" -
 * until the engine learns the team's tree and publishes it. */
static void collective_state_reset(struct osoe_collective_state *c)
{
    atomic_store_explicit(&c->epoch_open, 0, memory_order_release);
    atomic_store_explicit(&c->epoch_closed, 0, memory_order_release);
    atomic_store_explicit(&c->own_arrived, 0, memory_order_release);
    atomic_store_explicit(&c->children, 0, memory_order_release);
    atomic_store_explicit(&c->children_arrived, 0, memory_order_release);
    atomic_store_explicit(&c->lowest_missing, OSOE_COLLECTIVE_TREE_NONE, memory_order_release);
    atomic_store_explicit(&c->parent, OSOE_COLLECTIVE_TREE_NONE, memory_order_release);
    atomic_store_explicit(&c->fanout, 0, memory_order_release);
    atomic_store_explicit(&c->most_pieces_in_flight, 0, memory_order_release);
    atomic_store_explicit(&c->gather_steps, 0, memory_order_release);
    atomic_store_explicit(&c->gather_shares, 0, memory_order_release);
    atomic_store_explicit(&c->collective_class, 0, memory_order_release);
    atomic_store_explicit(&c->lane, 0, memory_order_release);
    atomic_store_explicit(&c->shape_fanout, 0, memory_order_release);
    atomic_store_explicit(&c->shape_window, 0, memory_order_release);
    atomic_store_explicit(&c->shape_children, 0, memory_order_release);
    atomic_store_explicit(&c->algorithm, 0, memory_order_release);
    atomic_store_explicit(&c->circulant_rounds, 0, memory_order_release);
    atomic_store_explicit(&c->collect_lengths, 0, memory_order_release);
    atomic_store_explicit(&c->collect_way, 0, memory_order_release);
    atomic_store_explicit(&c->data_epoch_open, 0, memory_order_release);
    atomic_store_explicit(&c->data_epoch_closed, 0, memory_order_release);
    osoe_collective_data_wait(c, OSOE_DATA_WAIT_NONE, OSOE_COLLECTIVE_TREE_NONE, 0);
}

/*
 * A block's life (node.h, osoe_team_name; tools/modelcheck/team_block_model.py,
 * write_block): by the team's local lead, on a block it has claimed and so the
 * only writer of. Nobody of another team holds the block - a block is in the
 * bitmap only once its last holder has left - and nobody of this one has found
 * it yet: the map entry comes after. The handoff word is left as the last team
 * posted it, consumed already, and its first post carries the new generation
 * (osoe_arrival_handoff).
 */
uint32_t osoe_node_team_name(struct osoe_node *n, uint32_t block_index, uint32_t lead,
                             uint32_t number, uint32_t holders)
{
    struct osoe_team_block *b = osoe_node_team(n, block_index);
    if (!b || block_index < OSOE_PREDEFINED_TEAMS)
        osoe_fatal("block %u of a pool of %u named for team %u", block_index, n->team_count,
                   number);
    uint32_t generation = atomic_load_explicit(&b->name.generation, memory_order_acquire) + 1;
    if (generation % 2 == 0)
        osoe_fatal("block %u, claimed for team %u, is at generation %u: a team holds it",
                   block_index, number, generation - 1);
    atomic_store_explicit(&b->name.generation, generation, memory_order_release);
    osoe_epoch_init(&b->arrived);
    osoe_epoch_init(&b->released);
    osoe_epoch_init(&b->copied);
    collective_state_reset(&b->tree);
    atomic_store_explicit(&b->name.lead, lead, memory_order_release);
    atomic_store_explicit(&b->name.number, number, memory_order_release);
    /* The holders before the members, with release: osoe_node_team_returning
     * reads the members first, with acquire, and so never sees this team's
     * members beside the last team's holders. */
    atomic_store_explicit(&b->name.holders, holders, memory_order_release);
    atomic_store_explicit(&b->name.members, holders, memory_order_release);
    return generation;
}

/*
 * tools/modelcheck/pool_dry_model.py. A block keeps its last team's members
 * after that team has left it, and a claim zeroes them (osoe_node_team_claim):
 * a nonzero count with fewer holders is a team leaving the block, or a block
 * its last holder has given back and the bitmap does not show yet, or one it
 * shows again - a lead asks only once its claim found the pool empty, so a free
 * block is one back since; a zero count is a block claimed, or never named,
 * which is back only if its bit is set. The members first, with acquire: the
 * namer writes the holders before them, with release, so a block being named
 * reads as claimed or as held. One read between a claim and its zeroing reads
 * as on its way back, which costs the caller a round and nothing more.
 */
bool osoe_node_team_returning(const struct osoe_node *n)
{
    for (uint32_t i = OSOE_PREDEFINED_TEAMS; i < n->team_count; i++) {
        const struct osoe_team_name *name = &osoe_node_team(n, i)->name;
        uint32_t members = atomic_load_explicit(&name->members, memory_order_acquire);
        if (members != 0 ? atomic_load_explicit(&name->holders, memory_order_seq_cst) < members
                         : (atomic_load_explicit(free_word(n, i), memory_order_seq_cst) &
                            UINT64_C(1) << (i % 64)) != 0)
            return true;
    }
    return false;
}

/*
 * The model's `leave`, each step in the model's order, each breakage of it
 * failing there (node.h). The lead clears its name's entry before its own
 * decrement, and so before the last holder's - the holders' count is a
 * read-modify-write, whose order every decrement reads - and so before the
 * bit. Exactly one member brings the count to zero, after every other
 * member's decrement: every holder's last use of the block comes before its
 * own decrement, and so before the give-back.
 */
bool osoe_node_team_leave(struct osoe_node *n, uint32_t block_index, bool lead)
{
    struct osoe_team_block *b = osoe_node_team(n, block_index);
    if (!b || block_index < OSOE_PREDEFINED_TEAMS)
        osoe_fatal("a member left block %u of a pool of %u, which no created team can hold",
                   block_index, n->team_count);
    if (lead)
        atomic_store_explicit(osoe_node_team_map_entry(
                                  n, atomic_load_explicit(&b->name.lead, memory_order_acquire),
                                  atomic_load_explicit(&b->name.number, memory_order_acquire)),
                              0, memory_order_release);
    uint32_t before = atomic_fetch_sub_explicit(&b->name.holders, 1, memory_order_seq_cst);
    if (before == 0)
        osoe_fatal("a member left block %u, which no member held", block_index);
    if (before > 1)
        return false;
    /* release, as everything the block's last team's members did in it comes
     * before (§3.16); then the generation, then the bit. */
    atomic_store_explicit(&b->record.published, 0, memory_order_release);
    atomic_fetch_add_explicit(&b->name.generation, 1, memory_order_seq_cst);
    atomic_fetch_or_explicit(free_word(n, block_index), UINT64_C(1) << (block_index % 64),
                             memory_order_seq_cst);
    return true;
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
    /* Before the segment: the lead records it beside its pid, and a node-mate
     * judges the lead's pid only where they agree (D26). */
    n->pid_namespace = osoe_pid_namespace_own();

    control_name(n->control_name, sizeof n->control_name, b, session);
    /* SHMEM_SYMMETRIC_SIZE rounded up to the allocator's 16 bytes: §8 asks for
     * a heap at least as large as the value - its own example, 3.1M, is
     * 3,250,586 bytes - where rounding down held 10 fewer (the v0.2 step 1
     * review, C1). Inside the page the internal region begins at, whatever the
     * value. */
    n->heap_size = (b->symmetric_size + 15) / 16 * 16;
    /* The heap, then OSOE_REGION_INTERNAL, in one object (internal-region.h):
     * the allocator is given heap_size and never reaches the tail. */
    n->internal_offset = page_round(b->symmetric_size ? b->symmetric_size : 1);
    /* A lane per thread record, the agent's included (bootstrap.h): the index
     * a thread's lane follows (internode-generic.c). */
    n->internal_bytes = page_round(osoe_internal_region_bytes(b->collective_fanout,
                                                              b->collective_pipeline,
                                                              b->max_teams,
                                                              b->small_collective_bytes,
                                                              b->node_count,
                                                              b->max_threads + 1));
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

    int refused = 0;
    uint64_t base = create_own_heap(n, b, me, lead ? 0 : n->hdr->heap_base, &refused);
    if (!base) {
        char msg[384];
        uint64_t shift = b->heap_shift * b->rank;
        if (lead && !b->heap_base)
            snprintf(msg, sizeof msg, "no candidate heap base is free for a %llu-byte heap; "
                     "set OSOE_HEAP_BASE to a free address", (unsigned long long)n->heap_bytes);
        else if (lead) {
            /* The base was set, and so is the one thing tried: say what it was
             * and why it failed, not to set it (D12). */
            char shifted[64] = "";
            if (shift)
                snprintf(shifted, sizeof shifted, " plus this PE's shift %#llx (OSOE_HEAP_SHIFT)",
                         (unsigned long long)shift);
            snprintf(msg, sizeof msg, "OSOE_HEAP_BASE=%#llx: cannot map this node's %llu-byte "
                     "heap there%s: %s; set OSOE_HEAP_BASE to an address free in every PE, or "
                     "leave it unset for the library's own candidates",
                     (unsigned long long)b->heap_base, (unsigned long long)n->heap_bytes, shifted,
                     heap_refusal(refused));
        } else
            snprintf(msg, sizeof msg, "cannot map this PE's heap at the node's base %#llx "
                     "plus its shift %#llx (OSOE_HEAP_SHIFT), which the local lead chose: %s; "
                     "set OSOE_HEAP_BASE to an address free in every PE",
                     (unsigned long long)n->hdr->heap_base, (unsigned long long)shift,
                     heap_refusal(refused));
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
    me->pid_namespace = n->pid_namespace;
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
