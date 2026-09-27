/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Symmetric statics (design §4.3; node.c, symmetric.c): the program's global
 * and static variables as symmetric objects, in a program linked as oshcc links
 * one - -fno-pie, -no-pie and osoe's linker script (osoe_statics_program in
 * CMakeLists.txt).
 *
 * What a corpus program does with them, each once in the heap's stead:
 *   - values the program wrote before shmem_init, in .data and far into a
 *     large .bss, are still there after it, and another PE reads them;
 *   - puts, gets, their nonblocking forms and put-with-signal into both;
 *   - ISx's pattern: a global AMO target, and a static reduction target
 *     reduced with a static pSync and pWrk by the deprecated sum_to_all, and
 *     by the team form with the source in the heap and the dest in statics;
 *   - a broadcast from every root into a static, across pieces, and
 *     shmem_broadcast32 with a static pSync;
 *   - a point-to-point wait on a static, woken by a neighbour's put;
 *   - shmem_ptr and shmem_addr_accessible on a static;
 *   - one PE sends the address of a static to another, which finds its own
 *     static there: -no-pie puts every PE's statics at one address;
 *   - a second session: the globals keep their values across shmem_finalize,
 *     and are symmetric again after the next shmem_init, a marker in every
 *     page of the large .bss copied through both;
 *   - a put and a get from one static to an overlapping part of itself, on
 *     this PE, which must move as memmove would;
 *   - a variable in a section of its own, which the linker places between the
 *     two ranges, so that they are not adjacent and the bss range's offsets
 *     start after the data range's end rather than at its address;
 *   - a wait on a static, as a node-mate's "blocked in" record names it.
 * Across nodes on the maps, where every one of them crosses the wire by
 * (region, offset). Built a second way, -fpie and -pie with the script
 * (OSOE_TEST_STATICS_PIE): every PE's statics at a different address, where
 * only the address agreement is not expected to hold.
 *
 * With OSOE_TEST_STATICS_MISUSE=stack a PE puts to its stack, which must end
 * the job naming both regions (shmem.statics.misuse).
 */
#define _POSIX_C_SOURCE 200809L       /* nanosleep, under -std=c11 too */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { BROADCAST = 5000, BIG = 8 << 20 };

/* .data: initialized alike on every PE. */
static int initialized[4] = { 11, 22, 33, 44 };
long long counter_global = 5;       /* external, as ISx's receive_offset is */

/* .bss */
static long pSync[SHMEM_REDUCE_SYNC_SIZE];
static long long pWrk[SHMEM_REDUCE_MIN_WRKDATA_SIZE];
static long long total, mine_count;
static int broadcast_source[BROADCAST], broadcast_dest[BROADCAST];
static long flag;
static uint64_t signal_word;
static int signalled[64];
static uintptr_t address_of_flag;
static int written_before_init;
static char big[BIG];

static int me, npes, left, right;

/* Between the two ranges: a writable section of its own, which the linker
 * places after the data it gathers and before the bss (osoe-statics.ld). */
__attribute__((section("osoe_between"), used)) static int between = 1;
static char overlap[4096];
static long blocked_on;

/* The script's brackets, which the test reads to know the ranges' places. */
extern unsigned char __osoe_statics_data_start[] __attribute__((weak));
extern unsigned char __osoe_statics_data_end[] __attribute__((weak));
extern unsigned char __osoe_statics_bss_start[] __attribute__((weak));

/* A marker in every 4 KiB of `big` - no page size is smaller - and on each
 * side of every boundary between them, written before shmem_init; below the
 * last two MiB, which puts_and_gets overwrites. */
enum { MARKED = BIG - (2 << 20) };
static char marker(size_t at)
{
    return (char)((at / 4096) % 251 + 1);
}

static void mark_big(void)
{
    for (size_t at = 4096; at < MARKED; at += 4096) {
        big[at - 1] = marker(at - 1);
        big[at] = marker(at);
        big[at + (at / 4096) % 4000] = marker(at);
    }
}

static size_t big_unmarked(const char *copy)
{
    size_t wrong = 0;
    for (size_t at = 4096; at < MARKED; at += 4096)
        wrong += copy[at - 1] != marker(at - 1) || copy[at] != marker(at) ||
                 copy[at + (at / 4096) % 4000] != marker(at);
    return wrong;
}

static void before_and_after_init(void)
{
    CHECK(written_before_init == 1234);
    CHECK(big[BIG - 1] == 7);
    CHECK(big_unmarked(big) == 0);
    char *copy = malloc(BIG);
    CHECK_FATAL(copy);
    shmem_getmem(copy, big, BIG, right);
    CHECK(big_unmarked(copy) == 0);
    free(copy);
    /* The section between keeps the ranges apart, so the bss range's offsets
     * are computed, not the data range's run on. */
    CHECK(between == 1);
    CHECK((const void *)__osoe_statics_data_end != (const void *)__osoe_statics_bss_start);
    CHECK(initialized[2] == 33);
    shmem_barrier_all();
    CHECK(shmem_int_g(&written_before_init, right) == 1234);
    CHECK(shmem_char_g(&big[BIG - 1], right) == 7);
    CHECK(shmem_int_g(&initialized[2], right) == 33);
    CHECK(shmem_longlong_g(&counter_global, right) == 5);
    shmem_barrier_all();
}

static void puts_and_gets(void)
{
    /* Into .data and far into .bss, blocking and not. */
    shmem_int_p(&initialized[3], me, right);
    enum { CHUNK = 1 << 20 };
    char *pattern = malloc(CHUNK), *back = malloc(CHUNK);
    CHECK_FATAL(pattern && back);
    for (size_t i = 0; i < CHUNK; i++)
        pattern[i] = (char)(me * 7 + (int)i);
    shmem_putmem_nbi(&big[BIG - CHUNK - 1], pattern, CHUNK, right);
    shmem_quiet();
    shmem_barrier_all();
    CHECK(initialized[3] == left);
    size_t wrong = 0;
    for (size_t i = 0; i < CHUNK; i++)
        wrong += big[BIG - CHUNK - 1 + i] != (char)(left * 7 + (int)i);
    CHECK(wrong == 0);
    shmem_getmem_nbi(back, &big[BIG - CHUNK - 1], CHUNK, right);
    shmem_quiet();
    for (size_t i = 0; i < CHUNK; i++)
        wrong += back[i] != (char)(me * 7 + (int)i);
    CHECK(wrong == 0);
    free(pattern);
    free(back);

    /* Put-with-signal: the data and the signal both statics. */
    int data[64];
    for (int i = 0; i < 64; i++)
        data[i] = me * 100 + i;
    shmem_barrier_all();
    shmem_int_put_signal(signalled, data, 64, &signal_word, 1, SHMEM_SIGNAL_ADD, right);
    CHECK(shmem_signal_wait_until(&signal_word, SHMEM_CMP_GE, 1) >= 1);
    for (int i = 0; i < 64; i++)
        wrong += signalled[i] != left * 100 + i;
    CHECK(wrong == 0);
    CHECK(shmem_signal_fetch(&signal_word) == 1);
    shmem_barrier_all();
}

/* To this PE's own statics, from overlapping bytes of the same static: the
 * transport must see one address range, as memmove needs, not the program's
 * and the library's alias of it. */
static void overlapping(void)
{
    for (size_t i = 0; i < sizeof overlap; i++)
        overlap[i] = (char)(i % 199);
    shmem_putmem(overlap + 1, overlap, 2048, me);
    shmem_quiet();
    size_t wrong = 0;
    for (size_t i = 0; i < 2048; i++)
        wrong += overlap[i + 1] != (char)(i % 199);
    for (size_t i = 0; i < sizeof overlap; i++)
        overlap[i] = (char)(i % 199);
    shmem_getmem(overlap + 7, overlap, 3000, me);
    for (size_t i = 0; i < 3000; i++)
        wrong += overlap[i + 7] != (char)(i % 199);
    if (wrong)
        fprintf(stderr, "PE %d: %zu bytes wrong after an overlapping transfer\n", me, wrong);
    CHECK(wrong == 0);
    shmem_barrier_all();
}

/* PE 1 waits on a static; PE 0, its node-mate, reads the wait's record, which
 * names the static's offset among the statics, then releases it. */
static void blocked_record(void)
{
    blocked_on = 0;
    shmem_barrier_all();
    if (npes < 2 || !shmem_ptr(&blocked_on, 1) || !shmem_ptr(&blocked_on, 0)) {
        shmem_barrier_all();
        return;                         /* PEs 0 and 1 are not node-mates */
    }
    if (me == 1)
        shmem_long_wait_until(&blocked_on, SHMEM_CMP_EQ, 1);
    if (me == 0) {
        unsigned char *at = (unsigned char *)&blocked_on;
        uint64_t data_bytes = (uint64_t)(__osoe_statics_data_end - __osoe_statics_data_start);
        uint64_t offset = at >= __osoe_statics_bss_start
                              ? data_bytes + (uint64_t)(at - __osoe_statics_bss_start)
                              : (uint64_t)(at - __osoe_statics_data_start);
        char segment[OSOE_SEGMENT_NAME_MAX], err[256];
        osoe_test_segment_name(segment, sizeof segment, 0);
        struct osoe_segment s;
        if (osoe_segment_open(&s, segment, err, sizeof err) == 0) {
            struct osoe_segment_blocked b = { 0 };
            int waited = 0;
            /* Its wait's record, not the barrier's before it, which a PE
             * slow to leave may still be publishing. */
            for (; waited < 5000; waited++) {
                if (osoe_segment_blocked(&s, 1, &b) && b.valid && b.blocked &&
                    strcmp(b.routine, "shmem_long_wait_until") == 0)
                    break;
                struct timespec ts = { 0, 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
            CHECK(waited < 5000);
            CHECK(b.detail_kind == OSOE_SEGMENT_DETAIL_STATICS_OFFSET);
            CHECK(b.detail == offset);
            osoe_segment_close(&s);
        } else {
            fprintf(stderr, "%s\n", err);
            CHECK(0);
        }
        shmem_long_atomic_set(&blocked_on, 1, 1);
    }
    shmem_barrier_all();
}

static void isx(void)
{
    /* Every PE counts into PE 0's global, as ISx's key exchange does. */
    shmem_longlong_atomic_fetch_add(&counter_global, 1, 0);
    shmem_barrier_all();
    if (me == 0)
        CHECK(counter_global == 5 + npes);
    shmem_barrier_all();

    for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++)
        pSync[i] = SHMEM_SYNC_VALUE;
    shmem_barrier_all();
    mine_count = me + 1;
    long long want = (long long)npes * (npes + 1) / 2;
    shmem_longlong_sum_to_all(&total, &mine_count, 1, 0, 0, npes, pWrk, pSync);
    CHECK(total == want);
    for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++)
        CHECK(pSync[i] == SHMEM_SYNC_VALUE);

    /* The team form, the source in the heap and the dest in statics, and in
     * place in statics. */
    long long *heap_source = shmem_malloc(sizeof *heap_source);
    CHECK_FATAL(heap_source);
    *heap_source = me + 1;
    total = 0;
    shmem_barrier_all();
    CHECK(shmem_longlong_sum_reduce(SHMEM_TEAM_WORLD, &total, heap_source, 1) == 0);
    CHECK(total == want);
    mine_count = me;
    CHECK(shmem_longlong_max_reduce(SHMEM_TEAM_WORLD, &mine_count, &mine_count, 1) == 0);
    CHECK(mine_count == npes - 1);
    shmem_free(heap_source);
}

static void broadcasts(void)
{
    for (int root = 0; root < npes; root++) {
        for (int i = 0; i < BROADCAST; i++) {
            broadcast_source[i] = me == root ? root * 100000 + i : -1;
            broadcast_dest[i] = -2;
        }
        shmem_barrier_all();
        CHECK(shmem_int_broadcast(SHMEM_TEAM_WORLD, broadcast_dest, broadcast_source,
                                  BROADCAST, root) == 0);
        size_t wrong = 0;
        for (int i = 0; i < BROADCAST; i++)
            wrong += broadcast_dest[i] != root * 100000 + i;
        CHECK(wrong == 0);
    }
    /* The deprecated form, whose root's dest stays as it was. */
    for (int i = 0; i < SHMEM_BCAST_SYNC_SIZE; i++)
        pSync[i] = SHMEM_SYNC_VALUE;
    for (int i = 0; i < BROADCAST; i++) {
        broadcast_source[i] = me == 0 ? 7 * i : -1;
        broadcast_dest[i] = -2;
    }
    shmem_barrier_all();
    shmem_broadcast32(broadcast_dest, broadcast_source, BROADCAST, 0, 0, 0, npes, pSync);
    size_t wrong = 0;
    for (int i = 0; i < BROADCAST; i++)
        wrong += broadcast_dest[i] != (me == 0 ? -2 : 7 * i);
    CHECK(wrong == 0);
    shmem_barrier_all();
}

static void waits(void)
{
    flag = 0;
    shmem_barrier_all();
    for (long round = 1; round <= 5; round++) {
        shmem_long_atomic_set(&flag, round, right);
        shmem_long_wait_until(&flag, SHMEM_CMP_EQ, round);
        CHECK(shmem_long_test(&flag, SHMEM_CMP_EQ, round) == 1);
        shmem_barrier_all();
    }
}

static void pointers(void)
{
    int stack_variable = 0;
    CHECK(shmem_addr_accessible(&flag, right) == 1);
    CHECK(shmem_addr_accessible(&big[BIG - 1], right) == 1);
    CHECK(shmem_addr_accessible(&stack_variable, right) == 0);
    CHECK(shmem_ptr(&initialized[0], me) == (void *)&initialized[0]);

    /* A neighbour on this node is a pointer away; one on another, NULL. */
    int *there = shmem_ptr(&initialized[1], right);
    shmem_barrier_all();
    if (there) {
        CHECK(*there == 22);
        *there = 1000 + me;
    }
    shmem_barrier_all();
    if (shmem_ptr(&initialized[1], left))
        CHECK(initialized[1] == 1000 + left);
    shmem_barrier_all();

    /* The same address in every PE, -no-pie's promise: the one sent is this
     * PE's own. */
    uintptr_t mine = (uintptr_t)&flag;
    shmem_putmem(&address_of_flag, &mine, sizeof mine, right);
    shmem_barrier_all();
#ifndef OSOE_TEST_STATICS_PIE
    CHECK(address_of_flag == (uintptr_t)&flag);
#endif
    shmem_barrier_all();
}

/* osoe-stat's view (layout 17): the node's first PE's statics object, its
 * length and its name - every local PE's are alike, or the rendezvous would
 * have refused them. */
static void segment_says(void)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256], name[128] = "";
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        CHECK_FATAL(0);
    }
    uint64_t bytes = 0;
    CHECK(osoe_segment_read_name(&s, "pe.statics_bytes", 0, &bytes));
    CHECK(bytes >= BIG + sizeof initialized);
    CHECK(osoe_segment_read_str(&s, "pe.statics_name", 0, name, sizeof name));
    CHECK(strstr(name, ".statics.0") != NULL);
    osoe_segment_close(&s);
}

static void second_session(void)
{
    flag = 99 + me;
    shmem_barrier_all();
    shmem_finalize();
    CHECK(flag == 99 + me);             /* the program's own, after the library left */
    flag += 1;

    shmem_init();
    CHECK(flag == 100 + me);
    CHECK(big_unmarked(big) == 0);      /* copied from the last session's object */
    shmem_barrier_all();
    CHECK(shmem_long_g(&flag, right) == 100 + right);
    shmem_barrier_all();
    shmem_long_p(&flag, -me, right);
    shmem_barrier_all();
    CHECK(flag == -left);
    CHECK_INVARIANTS_SESSION(1);
}

int main(void)
{
    written_before_init = 1234;
    big[BIG - 1] = 7;
    mark_big();

    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    left = (me + npes - 1) % npes;
    right = (me + 1) % npes;

    const char *misuse = getenv("OSOE_TEST_STATICS_MISUSE");
    if (misuse && strcmp(misuse, "stack") == 0) {
        int stack_variable = 0;
        shmem_int_p(&stack_variable, 1, right);
        return 1;
    }

    before_and_after_init();
    puts_and_gets();
    isx();
    broadcasts();
    waits();
    overlapping();
    blocked_record();
    pointers();
    segment_says();
    CHECK_INVARIANTS();
    second_session();
    shmem_finalize();
    return CHECK_DONE();
}
