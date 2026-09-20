/*
 * Store-buffer (SB / Dekker) litmus test for §3.16's two-sided rule.
 *
 * Two threads, pinned to different cores. Each round:
 *     thread 0:  x = 1;  r0 = y;
 *     thread 1:  y = 1;  r1 = x;
 * The outcome r0 == 0 && r1 == 0 is forbidden under sequential consistency and
 * is exactly a lost wake in a two-sided protocol (§4.9's watch insert against
 * an AMO store).
 *
 * Three variants, chosen by argv[1]:
 *     plain    volatile plain stores and loads: the control. It must show
 *              both-zero outcomes, or the harness cannot see the effect at all
 *              and a zero from the other variants means nothing.
 *     release  atomic_store_explicit(release) + atomic_load_explicit(acquire)
 *     seqcst   atomic_store_explicit(seq_cst) + atomic_load_explicit(acquire),
 *              which is the pair the tree uses (watch.c, rmw.h).
 *     amo      side 0 as seqcst (the waiter publishing watch_rng); side 1 a
 *              seq_cst fetch_add then an acquire load (an AMO add, then
 *              osoe_transport_notify's load of watch_rng).
 *     put      side 0 as seqcst; side 1 a PLAIN store then an acquire load,
 *              which is a put's memcpy followed by notify's load - the case
 *              §4.10 calls best effort.
 *
 * Build: gcc -O2 -pthread store-buffer-litmus.c -o store-buffer-litmus
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum variant { PLAIN, RELEASE, SEQCST, AMO, PUT };
static enum variant variant;
static long rounds;

static _Alignas(128) _Atomic uint32_t ax;
static _Alignas(128) _Atomic uint32_t ay;
static _Alignas(128) volatile uint32_t px;
static _Alignas(128) volatile uint32_t py;

static pthread_barrier_t start, finish;
static _Alignas(128) uint32_t result[2];

static void pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

static void *side(void *arg)
{
    int me = (int)(intptr_t)arg;
    pin(me);
    for (long i = 0; i < rounds; i++) {
        pthread_barrier_wait(&start);
        /* A short, unequal spin so the two sides collide at varying phases. */
        for (volatile int k = 0; k < (int)((i * 7 + me * 3) % 23); k++)
            ;
        uint32_t r;
        switch (variant) {
        case PLAIN:
            if (me == 0) { px = 1; r = py; } else { py = 1; r = px; }
            break;
        case RELEASE:
            if (me == 0) {
                atomic_store_explicit(&ax, 1, memory_order_release);
                r = atomic_load_explicit(&ay, memory_order_acquire);
            } else {
                atomic_store_explicit(&ay, 1, memory_order_release);
                r = atomic_load_explicit(&ax, memory_order_acquire);
            }
            break;
        case AMO:
            if (me == 0) {
                atomic_store_explicit(&ax, 1, memory_order_seq_cst);
                r = atomic_load_explicit(&ay, memory_order_acquire);
            } else {
                atomic_fetch_add_explicit(&ay, 1, memory_order_seq_cst);
                r = atomic_load_explicit(&ax, memory_order_acquire);
            }
            break;
        case PUT:
            if (me == 0) {
                atomic_store_explicit(&ax, 1, memory_order_seq_cst);
                r = py;
            } else {
                py = 1;
                r = atomic_load_explicit(&ax, memory_order_acquire);
            }
            break;
        default:
            if (me == 0) {
                atomic_store_explicit(&ax, 1, memory_order_seq_cst);
                r = atomic_load_explicit(&ay, memory_order_acquire);
            } else {
                atomic_store_explicit(&ay, 1, memory_order_seq_cst);
                r = atomic_load_explicit(&ax, memory_order_acquire);
            }
            break;
        }
        result[me] = r;
        pthread_barrier_wait(&finish);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s plain|release|seqcst|amo|put rounds\n", argv[0]);
        return 2;
    }
    variant = !strcmp(argv[1], "plain") ? PLAIN : !strcmp(argv[1], "release") ? RELEASE :
              !strcmp(argv[1], "amo") ? AMO : !strcmp(argv[1], "put") ? PUT : SEQCST;
    rounds = atol(argv[2]);
    pthread_barrier_init(&start, NULL, 3);
    pthread_barrier_init(&finish, NULL, 3);
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&t[i], NULL, side, (void *)(intptr_t)i);
    long both_zero = 0;
    for (long i = 0; i < rounds; i++) {
        atomic_store_explicit(&ax, 0, memory_order_seq_cst);
        atomic_store_explicit(&ay, 0, memory_order_seq_cst);
        px = py = 0;
        pthread_barrier_wait(&start);
        pthread_barrier_wait(&finish);
        if (result[0] == 0 && result[1] == 0)
            both_zero++;
    }
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    printf("%s: %ld rounds, %ld both-zero (forbidden under seq_cst)\n", argv[1], rounds,
           both_zero);
    return 0;
}
