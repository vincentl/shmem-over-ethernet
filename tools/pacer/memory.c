/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * memory - the symmetric heap's routines (proposal §4.8;
 * docs/prompts/task-stress-phase3.md).
 *
 * A seeded sequence of shmem_malloc, shmem_calloc, shmem_align,
 * shmem_malloc_with_hints (both hints), shmem_realloc and shmem_free, the same
 * on every PE since each is collective: up to three quarters of the budget and
 * back down, over and over. Each block is filled with its own pattern when it
 * is made; a realloc must keep the pattern of what it keeps, a free finds the
 * pattern whole - a block the allocator let overlap another's would not be.
 * A calloc's block is zero; an align's is aligned; every block is accessible
 * from this PE and the next (shmem_addr_accessible).
 *
 * Symmetric means alike on every PE, and the check says so without assuming
 * that a symmetric object sits at one address everywhere, which the
 * specification does not promise: every result - its offset from a block made
 * at setup, or none - goes into a hash, and at the end of each round the
 * largest hash over the team must equal the smallest.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { BLOCKS = 64, STEPS_A_ROUND = 16 };
enum kind { MALLOC, CALLOC, ALIGN, HINTS, KINDS };

static const char *const kind_names[KINDS] = { "malloc", "calloc", "align", "malloc_with_hints" };
static const char *const phase_names[] = { "allocate", "fill", "check", NULL };

struct block {
    unsigned char *address;
    size_t size;
    uint64_t tag;                   /* what its pattern derives from */
};

static struct {
    uint64_t seed;
    uint64_t budget;                /* the most the blocks may hold at once */
    int plant_fill, plant_calloc, plant_align, plant_accessible, plant_agree;
    shmem_team_t team;
    int n, me, me_world, next_world;
    unsigned char *anchor;          /* made at setup: offsets are from here */
    uint64_t *hash, *largest, *smallest;    /* [1] each, symmetric */
    struct block blocks[BLOCKS];
    int count;
    uint64_t used, made;
    int shrinking;
    long round;
    double operations, bytes, phase[3];
} memory;

static uint64_t memory_memory(int npes, const struct pacer_options *options, char *fields,
                              size_t size)
{
    (void)npes;
    memory.seed = options->seed;
    memory.budget = options->memory / 4 * 3;
    if (options->quick && memory.budget > (UINT64_C(8) << 20))
        memory.budget = UINT64_C(8) << 20;      /* the smoke test's minute */
    memory.plant_fill = options->plant == PACER_PLANT_MEMORY_FILL;
    memory.plant_calloc = options->plant == PACER_PLANT_MEMORY_CALLOC;
    memory.plant_align = options->plant == PACER_PLANT_MEMORY_ALIGN;
    memory.plant_accessible = options->plant == PACER_PLANT_MEMORY_ACCESSIBLE;
    memory.plant_agree = options->plant == PACER_PLANT_MEMORY_AGREE;
    char budget[32];
    pacer_size(budget, sizeof budget, memory.budget);
    snprintf(fields, size, "blocks=%d peak=%s kinds=malloc,calloc,align,malloc_with_hints",
             BLOCKS, budget);
    return options->memory;
}

static int memory_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    memory.team = team;
    memory.n = shmem_team_n_pes(team);
    memory.me = shmem_team_my_pe(team);
    memory.me_world = shmem_my_pe();
    memory.next_world = shmem_team_translate_pe(team, (memory.me + 1) % memory.n,
                                                SHMEM_TEAM_WORLD);
    memory.anchor = shmem_malloc(64);
    memory.hash = shmem_calloc(1, sizeof *memory.hash);
    memory.largest = shmem_calloc(1, sizeof *memory.largest);
    memory.smallest = shmem_calloc(1, sizeof *memory.smallest);
    if (!memory.anchor || !memory.hash || !memory.largest || !memory.smallest)
        return -1;
    memory.count = 0;
    memory.used = memory.made = 0;
    memory.shrinking = 0;
    memory.round = 0;
    memory.operations = memory.bytes = 0;
    memset(memory.phase, 0, sizeof memory.phase);
    return 0;
}

/* ---- patterns and the agreement ------------------------------------------------ */

static void fill(struct block *b)
{
    double started = pacer_now();
    for (size_t i = 0; i < b->size; i += 8) {
        uint64_t word = random_mix(memory.seed ^ random_name("memory"), b->tag, i >> 3, 0);
        size_t k = b->size - i < 8 ? b->size - i : 8;
        memcpy(b->address + i, &word, k);
    }
    memory.bytes += (double)b->size;
    memory.phase[1] += pacer_now() - started;
}

/* The first of `bytes` bytes of a block not its pattern, or -1. */
static long damaged(const unsigned char *address, size_t bytes, uint64_t tag)
{
    for (size_t i = 0; i < bytes; i += 8) {
        uint64_t word = random_mix(memory.seed ^ random_name("memory"), tag, i >> 3, 0);
        size_t k = bytes - i < 8 ? bytes - i : 8;
        if (memcmp(address + i, &word, k) != 0)
            for (size_t j = 0; j < k; j++)
                if (address[i + j] != (unsigned char)(word >> (8 * j)))
                    return (long)(i + j);
    }
    return -1;
}

static void agree(uint64_t operation, const void *result, size_t size)
{
    uint64_t offset = result ? (uint64_t)((uintptr_t)result - (uintptr_t)memory.anchor)
                             : UINT64_MAX;
    memory.hash[0] = random_finalize(memory.hash[0] ^ random_mix(operation, offset, size, 0));
}

/* ---- the operations ------------------------------------------------------------ */

static size_t draw_size(uint64_t r)
{
    /* Log-uniform from 8 bytes to an eighth of the budget. */
    uint64_t most = memory.budget / 8 > 8 ? memory.budget / 8 : 8;
    int bits = 3;
    while ((UINT64_C(1) << (bits + 1)) <= most)
        bits++;
    int b = 3 + (int)(r % (uint64_t)(bits - 2));
    uint64_t size = (UINT64_C(1) << b) + (r >> 8) % (UINT64_C(1) << b);
    return (size_t)(size < most ? size : most);
}

static void allocate(uint64_t r)
{
    if (memory.count == BLOCKS)
        return;
    size_t size = draw_size(r);
    if (memory.used + size > memory.budget)
        return;
    enum kind kind = (enum kind)(memory.made % KINDS);
    size_t alignment = (size_t)16 << ((r >> 32) % 11);          /* 16 to 16 KiB */
    long hints = (r >> 40) & 1 ? SHMEM_MALLOC_ATOMICS_REMOTE : SHMEM_MALLOC_SIGNAL_REMOTE;
    double started = pacer_now();
    unsigned char *address;
    switch (kind) {
    case CALLOC: address = shmem_calloc(size / 8 + 1, 8); size = (size / 8 + 1) * 8; break;
    case ALIGN: address = shmem_align(alignment, size); break;
    case HINTS: address = shmem_malloc_with_hints(size, hints); break;
    default: address = shmem_malloc(size); break;
    }
    memory.phase[0] += pacer_now() - started;
    memory.operations++;
    agree((uint64_t)kind + 1, address, size);
    memory.made++;
    if (!address)
        return;                     /* none, alike everywhere or the hash says so */

    started = pacer_now();
    if (kind == CALLOC) {
        if (memory.plant_calloc && memory.me_world == 1) {
            address[size / 2] = 1;  /* the planted defect: a byte in a fresh calloc */
            memory.plant_calloc = 0;
        }
        for (size_t i = 0; i < size; i++)
            if (address[i]) {
                pacer_failure("memory round=%ld calloc: byte %zu of %zu is %02x, not zero",
                              memory.round, i, size, address[i]);
                break;
            }
    }
    if (kind == ALIGN) {
        uintptr_t at = (uintptr_t)address;
        if (memory.plant_align && memory.me_world == 1) {
            at++;                   /* the planted defect: an address misread */
            memory.plant_align = 0;
        }
        if (at % alignment)
            pacer_failure("memory round=%ld align: a block of %zu bytes aligned to %zu is at "
                          "an address %zu past a multiple of it", memory.round, size, alignment,
                          (size_t)(at % alignment));
    }
    int here = shmem_addr_accessible(address, memory.me_world);
    int there = shmem_addr_accessible(address, memory.next_world);
    if (memory.plant_accessible && memory.me_world == 1) {
        there = !there;             /* the planted defect: one answer misread */
        memory.plant_accessible = 0;
    }
    if (here != 1 || there != 1)
        pacer_failure("memory round=%ld %s: a block of %zu bytes is %saccessible from this PE "
                      "and %saccessible from PE %d", memory.round, kind_names[kind], size,
                      here == 1 ? "" : "not ", there == 1 ? "" : "not ", memory.next_world);
    memory.phase[2] += pacer_now() - started;

    struct block *b = &memory.blocks[memory.count++];
    b->address = address;
    b->size = size;
    b->tag = memory.made;
    fill(b);
    if (memory.plant_fill && memory.me_world == 1) {
        b->address[size / 2] ^= 1;  /* the planted defect: a byte of a block overwritten */
        memory.plant_fill = 0;
    }
    memory.used += size;
}

static void release(int k)
{
    struct block *b = &memory.blocks[k];
    double started = pacer_now();
    long at = damaged(b->address, b->size, b->tag);
    if (at >= 0)
        pacer_failure("memory round=%ld free: byte %ld of a block of %zu bytes is not what was "
                      "written there", memory.round, at, b->size);
    memory.phase[2] += pacer_now() - started;
    started = pacer_now();
    shmem_free(b->address);
    memory.phase[0] += pacer_now() - started;
    memory.operations++;
    agree(10, NULL, b->size);
    memory.used -= b->size;
    memory.blocks[k] = memory.blocks[--memory.count];
}

static void resize(int k, uint64_t r)
{
    struct block *b = &memory.blocks[k];
    size_t size = r & 1 ? b->size / 2 + 8 : b->size * 2;
    if (size > b->size && memory.used + (size - b->size) > memory.budget)
        size = b->size / 2 + 8;
    double started = pacer_now();
    unsigned char *address = shmem_realloc(b->address, size);
    memory.phase[0] += pacer_now() - started;
    memory.operations++;
    agree(11, address, size);
    started = pacer_now();
    if (!address) {
        /* The block stands as it was (§9.3.3). */
        long at = damaged(b->address, b->size, b->tag);
        if (at >= 0)
            pacer_failure("memory round=%ld realloc: a refused realloc left byte %ld of %zu "
                          "not what was written", memory.round, at, b->size);
        memory.phase[2] += pacer_now() - started;
        return;
    }
    size_t kept = size < b->size ? size : b->size;
    long at = damaged(address, kept, b->tag);
    if (at >= 0)
        pacer_failure("memory round=%ld realloc: of %zu bytes kept from %zu, byte %ld is not "
                      "what was written", memory.round, kept, b->size, at);
    memory.phase[2] += pacer_now() - started;
    memory.used = memory.used - b->size + size;
    b->address = address;
    b->size = size;
    b->tag = ++memory.made;
    fill(b);
}

static void memory_step(long units, int last)
{
    for (long u = 0; u < units; u++, memory.round++) {
        /* One stream for every PE: the sequence is collective. */
        uint64_t state = random_mix(memory.seed ^ random_name("memory"), (uint64_t)memory.round,
                                    0, 0);
        for (int s = 0; s < STEPS_A_ROUND; s++) {
            uint64_t r = random_next(&state);
            if (!memory.shrinking && memory.used > memory.budget / 4 * 3)
                memory.shrinking = 1;
            else if (memory.shrinking && (memory.used < memory.budget / 10 || !memory.count))
                memory.shrinking = 0;
            int choice = (int)(r % 20);
            if (memory.count && (memory.shrinking ? choice < 12 : choice < 1))
                release((int)((r >> 8) % (uint64_t)memory.count));
            else if (memory.count && (memory.shrinking ? choice < 18 : choice < 4))
                resize((int)((r >> 8) % (uint64_t)memory.count), r >> 16);
            else
                allocate(r >> 4);
        }
        if (last && u == units - 1)
            while (memory.count)
                release(memory.count - 1);
        /* Alike on every PE, or not: the round's hash, largest against smallest. */
        if (memory.plant_agree && memory.me_world == 1) {
            memory.hash[0] ^= 1;    /* the planted defect: one result misrecorded */
            memory.plant_agree = 0;
        }
        shmem_uint64_max_reduce(memory.team, memory.largest, memory.hash, 1);
        shmem_uint64_min_reduce(memory.team, memory.smallest, memory.hash, 1);
        if (memory.largest[0] != memory.smallest[0])
            pacer_failure("memory round=%ld: the PEs' allocations differ, hashes from %016llx "
                          "to %016llx", memory.round, (unsigned long long)memory.smallest[0],
                          (unsigned long long)memory.largest[0]);
        memory.hash[0] = memory.largest[0];     /* start the next round alike */
    }
}

static void memory_checkpoint(struct pacer_counters *counters)
{
    counters->work = memory.operations;
    counters->bytes = memory.bytes;
    for (int k = 0; k < 3; k++)
        counters->phase_seconds[k] = memory.phase[k];
    memory.operations = memory.bytes = 0;
    memset(memory.phase, 0, sizeof memory.phase);
}

static void memory_teardown(void)
{
    while (memory.count)
        shmem_free(memory.blocks[--memory.count].address);
    shmem_free(memory.smallest);
    shmem_free(memory.largest);
    shmem_free(memory.hash);
    shmem_free(memory.anchor);
}

const struct pacer_exercise pacer_memory = {
    .name = "memory",
    .work = "operations",
    .phases = phase_names,
    .quick_rounds = 24,
    .memory = memory_memory,
    .setup = memory_setup,
    .step = memory_step,
    .checkpoint = memory_checkpoint,
    .teardown = memory_teardown,
};
