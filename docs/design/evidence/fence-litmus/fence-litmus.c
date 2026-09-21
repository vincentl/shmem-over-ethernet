/*
 * What carries fence's store ordering on aarch64 (§4.6 rule 2; second review,
 * atomics F5)?
 *
 * On the intranode path shmem_fence is one seq_cst store to a thread-local word
 * nothing reads (base/order.c), and a put is a memcpy: plain stores. The
 * property fence promises is message passing between two puts:
 *
 *     writer:  data = r;  <between>;  flag = r;       (plain stores)
 *     reader:  wait until flag == r (acquire);  read data (plain)
 *
 * and the outcome data != r is forbidden. On x86-64 the seq_cst store is an
 * xchg and orders everything. On aarch64 it is an stlr, whose release orders
 * the stores *before* it but lets a later plain store pass it - so by the
 * architecture the stlr alone does not order data before flag. This measures
 * it, with <between> one of:
 *
 *     plain      nothing: the control. It must show violations, or the harness
 *                cannot see the effect and a zero elsewhere means nothing.
 *     stlr       the serializing store alone (fence's intranode path).
 *     stlr-ldar  the serializing store, then an acquire load of a thread-local
 *                word: what the next library call's osoe_count does first
 *                (base/counters.h loads the counter acquire, then stores it).
 *     dmb        atomic_thread_fence(seq_cst): a full barrier, for reference.
 *
 * Three threads, pinned to different CPUs: the writer, the reader, and a
 * stress thread that keeps writing another word of data's cache line, so the
 * writer's store to data waits for the line while its store to flag (a line it
 * owns) does not - which is what lets a weak machine show the reordering at a
 * useful rate.
 *
 *   gcc -O2 -pthread fence-litmus.c -o fence-litmus
 *   ./fence-litmus plain|stlr|stlr-ldar|dmb [ROUNDS]
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SLOTS = 1024, LINE = 64 };
enum variant { PLAIN, STLR, STLR_LDAR, DMB };

struct slot {
    _Alignas(LINE) volatile uint32_t data;
    volatile uint32_t data_neighbour;   /* the stress thread's word, same line */
    _Alignas(LINE) volatile uint32_t flag;
};

static struct slot slots[SLOTS];
static _Alignas(LINE) _Atomic uint64_t reader_done;
static _Alignas(LINE) _Atomic uint64_t writer_round;
static _Alignas(LINE) _Atomic int stop;
static enum variant variant;
static uint64_t rounds;
static uint64_t violations;

static _Thread_local _Atomic uint32_t serial;       /* base/order.c's word */
static _Thread_local _Atomic uint32_t counter;      /* stands for a counter block */

static void pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

static void *writer(void *arg)
{
    (void)arg;
    pin(0);
    for (uint64_t r = 1; r <= rounds; r++) {
        /* Never lap the reader: slot r % SLOTS is free once round r - SLOTS is read. */
        while (r > SLOTS && atomic_load_explicit(&reader_done, memory_order_acquire) < r - SLOTS + 1)
            ;
        atomic_store_explicit(&writer_round, r, memory_order_release);
        struct slot *s = &slots[r % SLOTS];
        s->data = (uint32_t)r;
        switch (variant) {
        case PLAIN:
            break;
        case STLR:
            atomic_store_explicit(&serial, 1, memory_order_seq_cst);
            break;
        case STLR_LDAR:
            atomic_store_explicit(&serial, 1, memory_order_seq_cst);
            (void)atomic_load_explicit(&counter, memory_order_acquire);
            break;
        case DMB:
            atomic_thread_fence(memory_order_seq_cst);
            break;
        }
        s->flag = (uint32_t)r;
    }
    return NULL;
}

static void *reader(void *arg)
{
    (void)arg;
    pin(1);
    for (uint64_t r = 1; r <= rounds; r++) {
        struct slot *s = &slots[r % SLOTS];
        while (__atomic_load_n(&s->flag, __ATOMIC_ACQUIRE) != (uint32_t)r)
            ;
        if (s->data != (uint32_t)r)
            violations++;
        atomic_store_explicit(&reader_done, r, memory_order_release);
    }
    return NULL;
}

static void *stress(void *arg)
{
    (void)arg;
    pin(2);
    uint32_t v = 0;
    while (!atomic_load_explicit(&stop, memory_order_acquire)) {
        uint64_t r = atomic_load_explicit(&writer_round, memory_order_acquire);
        slots[(r + 1) % SLOTS].data_neighbour = ++v;
        slots[r % SLOTS].data_neighbour = ++v;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    static const char *names[] = { "plain", "stlr", "stlr-ldar", "dmb" };
    if (argc < 2)
        return 2;
    variant = PLAIN;
    for (int i = 0; i < 4; i++)
        if (strcmp(argv[1], names[i]) == 0)
            variant = (enum variant)i;
    rounds = argc > 2 ? strtoull(argv[2], NULL, 10) : 10000000;
    pthread_t w, rd, st;
    pthread_create(&st, NULL, stress, NULL);
    pthread_create(&rd, NULL, reader, NULL);
    pthread_create(&w, NULL, writer, NULL);
    pthread_join(w, NULL);
    pthread_join(rd, NULL);
    atomic_store_explicit(&stop, 1, memory_order_release);
    pthread_join(st, NULL);
    printf("%-10s %llu rounds, %llu saw flag without data (forbidden by fence)\n",
           names[variant], (unsigned long long)rounds, (unsigned long long)violations);
    return 0;
}
