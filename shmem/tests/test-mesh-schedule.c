/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The mesh's start-up schedule on its own (tier 1; H23, mesh-schedule.h): every
 * pair of nodes has one caller, the callers each node counts are the ones that
 * call it, each slot calls every listener at most once, and a node's slots make
 * every call the rule gives it - at every node count from 1 to 300, and at the
 * large ones to 8,193 by the slots alone; and each slots' order against what
 * tools/modelcheck/mesh_model.py wrote into mesh-schedule-digest.txt
 * (model.mesh.digest checks that the model still writes it).
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "mesh-schedule.h"
#include "wire-settings.h"

static uint64_t jobs[] = { 0, 1, 1678987, UINT64_C(0x123456789abcdef0) };

/* Node count n's schedule: the rule whole for every pair where `pairs`, and by
 * the slots alone otherwise. */
static void structure(uint32_t n, bool pairs)
{
    uint32_t slots = osoe_mesh_schedule_slots(n);
    CHECK(slots == n / 2);
    uint32_t *offsets = calloc(slots + 1, sizeof *offsets);
    uint32_t *called = calloc(n, sizeof *called), *made = calloc(n, sizeof *made);
    uint32_t *received = calloc(n, sizeof *received);
    unsigned char *seen = calloc(n + 1, 1);
    CHECK_FATAL(offsets && called && made && received && seen);
    for (size_t j = 0; j < sizeof jobs / sizeof jobs[0]; j++) {
        osoe_mesh_schedule_order(jobs[j], (uint32_t)j * 7, n, offsets);
        /* A permutation of the offsets 1 .. (n - 1) / 2, and an even n's n / 2. */
        memset(seen, 0, n + 1);
        for (uint32_t s = 0; s < slots; s++) {
            uint32_t d = offsets[s];
            CHECK(d >= 1 && d <= n / 2 && !seen[d]);
            if (d >= 1 && d <= n / 2)
                seen[d] = 1;
        }
        memset(made, 0, n * sizeof *made);
        memset(received, 0, n * sizeof *received);
        for (uint32_t s = 0; s < slots; s++) {
            memset(called, 0, n * sizeof *called);
            uint32_t calls = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t peer = osoe_mesh_schedule_callee(i, offsets[s], n);
                if (peer == n)
                    continue;
                CHECK(peer < n && peer != i && osoe_mesh_schedule_calls(i, peer, n));
                if (peer >= n)
                    continue;
                CHECK(called[peer]++ == 0);         /* the slot a permutation */
                made[i]++;
                received[peer]++;
                calls++;
            }
            /* Every node calls in a slot but the opposite pairs', where half do. */
            CHECK(calls == (n % 2 == 0 && offsets[s] == n / 2 ? n / 2 : n));
        }
        for (uint32_t i = 0; i < n; i++)
            CHECK(received[i] == osoe_mesh_schedule_callers(i, n) &&
                  made[i] + received[i] == n - 1);
    }
    if (pairs)
        for (uint32_t i = 0; i < n; i++) {
            uint32_t callers = 0, callees = 0;
            for (uint32_t k = 0; k < n; k++) {
                if (k == i) {
                    CHECK(!osoe_mesh_schedule_calls(i, k, n));
                    continue;
                }
                /* One caller a pair: one of the two calls, never both. */
                CHECK(osoe_mesh_schedule_calls(i, k, n) != osoe_mesh_schedule_calls(k, i, n));
                callers += osoe_mesh_schedule_calls(k, i, n);
                callees += osoe_mesh_schedule_calls(i, k, n);
            }
            CHECK(callers == osoe_mesh_schedule_callers(i, n) && callees == made[i]);
        }
    free(offsets);
    free(called);
    free(made);
    free(received);
    free(seen);
}

/* mesh_model.py's digest: the number of slots, the order hashed offset by
 * offset with osoe_wire_hash, and its first twelve offsets. */
static unsigned compare(FILE *f, unsigned *differ)
{
    char line[512];
    unsigned orders = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        char *at = line, *end;
        uint64_t job = strtoull(at, &end, 10);
        uint32_t step = (uint32_t)strtoul(end, &end, 10);
        uint32_t n = (uint32_t)strtoul(end, &end, 10);
        uint32_t slots = (uint32_t)strtoul(end, &end, 10);
        uint64_t digest = strtoull(end, &end, 16);
        uint32_t *offsets = calloc(osoe_mesh_schedule_slots(n) + 1, sizeof *offsets);
        CHECK_FATAL(offsets != NULL);
        osoe_mesh_schedule_order(job, step, n, offsets);
        uint64_t h = 0;
        for (uint32_t s = 0; s < osoe_mesh_schedule_slots(n); s++)
            h = osoe_wire_hash(h ^ offsets[s]);
        bool same = slots == osoe_mesh_schedule_slots(n) && h == digest;
        for (uint32_t s = 0; same && s < slots && s < 12; s++)
            same = strtoul(end, &end, 10) == offsets[s];
        if (!same && (*differ)++ < 5)
            fprintf(stderr, "job %" PRIu64 " step %u, %u nodes: the C's order differs from "
                    "the model's\n", job, step, n);
        free(offsets);
        orders++;
    }
    return orders;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s mesh-schedule-digest.txt\n", argv[0]);
        return 2;
    }
    for (uint32_t n = 1; n <= 300; n++)
        structure(n, true);
    static const uint32_t large[] = { 511, 512, 1023, 1024, 2047, 2048, 4095, 4096, 4097,
                                      8192, 8193 };
    for (size_t k = 0; k < sizeof large / sizeof large[0]; k++)
        structure(large[k], large[k] <= 1024);
    FILE *f = fopen(argv[1], "r");
    CHECK_FATAL(f != NULL);
    unsigned differ = 0, orders = compare(f, &differ);
    fclose(f);
    printf("mesh-schedule: %u orders from the model, %u differ\n", orders, differ);
    CHECK(orders >= 280 && differ == 0);
    return CHECK_DONE();
}
