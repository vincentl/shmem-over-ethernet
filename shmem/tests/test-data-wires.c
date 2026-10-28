/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Data wires made on demand (H23; OSOE_MESH_DATA_WIRES=on-demand; the author's
 * decisions of 2026-10-25): a job whose PEs each talk to one other PE, so that
 * its nodes need few of their data wires, and data-wires.sh counts the ones
 * made from the leads' SHMEM_DEBUG lines.
 *
 *   test-data-wires ring      PE i puts its rank to PE i + 1 and gets PE
 *                             i + 1's back, round the circle: with a PE a node,
 *                             node i needs its wire to node i + 1, whom the
 *                             mesh's schedule has it call, so it dials for its
 *                             own need
 *   test-data-wires skip      PE i to PE i + 3 instead: on five nodes the
 *                             schedule has node i + 3 call node i, so node i
 *                             asks it to dial over their control wire
 *   test-data-wires exit      PE 0 calls shmem_global_exit(3) 300 ms after
 *                             shmem_init, while every other PE is in its first
 *                             shmem_malloc - whose check of its arguments is a
 *                             small reduction over the job, carried on the
 *                             collective tree's data wires (malloc.c, P23) -
 *                             waiting for a wire to node 0, whose lead holds
 *                             every one back (OSOE_TEST_HOLD_DATA_WIRES): the
 *                             job ends with the caller's status, every other
 *                             PE leaving with 0, nobody waiting for the wire
 *
 * Every program makes the collective tree's data wires at its first collective
 * that carries data - shmem_malloc's check among them - and ring and skip then
 * the wires of their own pairs. Both check every value, put and got, and end
 * with a barrier, which the control wires carry; a value that never comes is a
 * hang, which the launcher's timeout ends.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "ring";
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    if (strcmp(mode, "exit") == 0) {
        if (me == 0) {
            struct timespec wait = { 0, 300000000 };
            nanosleep(&wait, NULL);
            shmem_global_exit(3);
        }
        (void)shmem_malloc(sizeof(int));
        fprintf(stderr, "test-data-wires: PE %d's shmem_malloc returned though PE 0 ends the job\n",
                me);
        return 1;
    }
    /* In the heap: a program not linked with oshcc has no symmetric statics. */
    int *slot = shmem_malloc(sizeof *slot);
    *slot = -1;
    shmem_barrier_all();
    int step = strcmp(mode, "skip") == 0 ? 3 : 1;
    int to = (me + step) % n, from = (me - step % n + n) % n;
    shmem_int_p(slot, me, to);
    int got = shmem_int_g(slot, to);
    shmem_quiet();
    shmem_barrier_all();
    int failed = 0;
    if (*slot != from) {
        fprintf(stderr, "test-data-wires: PE %d has %d from PE %d, which put %d\n", me, *slot,
                from, from);
        failed = 1;
    }
    /* What PE `to` held when this PE's get reached it: its own put's value, or
     * -1 if PE (to - step) had not put yet. */
    if (got != -1 && got != (to - step % n + n) % n) {
        fprintf(stderr, "test-data-wires: PE %d got %d from PE %d\n", me, got, to);
        failed = 1;
    }
    shmem_barrier_all();
    shmem_free(slot);
    shmem_finalize();
    if (!failed && me == 0)
        printf("DATA WIRES TEST OK\n");
    return failed;
}
