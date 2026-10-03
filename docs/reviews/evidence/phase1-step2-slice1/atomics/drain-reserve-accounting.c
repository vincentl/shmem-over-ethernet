/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * osoe_connection_drain_reserve (connection.c:149) against the two calls that
 * bracket it in §3.5's state machine, FREE -> DRAINING -> OPEN.
 *
 * Build, from the repository root:
 *   gcc -std=c11 -I. -Ishmem/src <this file> shmem/src/connection.c -o drain
 */
#include <stdio.h>
#include <string.h>
#include "connection.h"

int main(void)
{
    static struct osoe_connection_shared shared;
    static unsigned char data[2 * 4096];
    struct osoe_connection c = { &shared, data, 4096 };
    osoe_connection_init(&c, 4);

    /* One full generation of buffer 0, so it is FREE the way progress.c leaves it. */
    osoe_connection_open(&c, 0, 1);
    osoe_connection_seal(&c, 0);
    osoe_connection_release(&c, 0);

    /* DRAINING: the progress thread writes one 64-byte reply. */
    struct osoe_reservation r;
    bool ok = osoe_connection_drain_reserve(&c, 0, 64, &r);
    uint32_t head = OSOE_BUFFER_USED(atomic_load_explicit(&shared.buffer[0].head, memory_order_acquire));
    uint32_t filled = atomic_load_explicit(&shared.buffer[0].filled, memory_order_acquire);
    uint32_t bytes = 0;
    printf("drain_reserve ok=%d off=%u  head=%u filled=%u  sendable=%d\n", ok, r.off, head,
           filled, osoe_connection_sendable(&c, 0, &bytes));

    /* DRAINING -> OPEN, as test-connection.c does it. */
    osoe_connection_open(&c, 0, 2);
    head = atomic_load_explicit(&shared.buffer[0].head, memory_order_acquire);
    printf("after open: head=%u (the reply was at offset %u..%u)\n", head, r.off, r.off + r.len);

    struct osoe_reservation w;
    osoe_connection_reserve(&c, 64, &w);
    printf("first writer reservation lands at offset %u: %s\n", w.off,
           w.off == r.off ? "ON TOP OF THE DRAINED REPLY" : "clear of it");
    return 0;
}
