/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The deprecated collectives on an active set that is not the whole job
 * (Annex F; phase 1 step 5 slice 7, docs/prompts/done/phase1-step5-slice7-prompt.md).
 *
 * The whole job keeps the world team's collectives (collectives.c). Any other
 * set cannot use a team - creating one needs every PE of its parent, and only
 * the set's members call these - so it gets algorithms among its members,
 * over the program's own pSync and the library's public RMA, which is what the
 * spec hands these routines. Nothing here touches the node segment.
 *
 * **Every signal is a count.** A sender adds 1 to a pSync word at the
 * receiver; the receiver waits for the word to reach its start value,
 * SHMEM_SYNC_VALUE, plus the count it expects, and takes the count back with
 * an atomic add of the negative. So a word is back at SHMEM_SYNC_VALUE once
 * every member is done with it, as the spec requires pSync to be found, and a
 * fast member's add for its next call, arriving early, is simply counted in
 * that call: a member reaches the next call only after its part of this one,
 * so an early add never stands in for an add this call still waits for. The
 * words (pSync holds SHMEM_*_SYNC_SIZE, 256 longs, here): 0-63 a barrier's
 * rounds, and the rest below.
 *
 * **A binomial tree over the set's ranks**, rooted at the collective's root:
 * a member's relative rank v = (rank - root) mod size; its parent is v less
 * its lowest set bit, its children v + 2^k for each 2^k below that bit, and
 * its subtree the contiguous ranks [v, v + lowbit(v)) - log2(size) deep. Up
 * the tree a member waits for its children and signals its parent; down it,
 * a parent puts to each child with shmem_putmem_signal on the child's release
 * word. Every down phase follows an up phase, so a buffer is written only
 * after its owner has entered (the rule fcollect's lead keeps, collectives.c).
 *
 * pWrk is not used: private memory holds a reduction's incoming chunk.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pshmem.h>
#include <shmem.h>

#include "collectives.h"
#include "combine.h"
#include "fatal.h"
#include "state.h"
#include "wait.h"

enum {
    SLOT_BARRIER = 0,           /* 0-63: a barrier's rounds, ceil(log2 size) of them */
    SLOT_RELEASED = 65,         /* down: the parent has written this member's part */
    SLOT_TOTAL = 66,            /* collect: this member's subtree's bytes, read by its parent */
    SLOT_OFFSET = 67,           /* collect: where this member's subtree starts, from its parent */
    SLOT_GRAND = 68,            /* collect: the whole result's bytes, from its parent */
    SLOT_RELEASED_DATA = 70,    /* collect's second down pass */
    /* Up: child j's subtree is done - a word a child, 72 + j, so that a
     * parent takes each child's part as soon as that child is ready, the
     * small subtrees first since they finish first (review of slice 7, S1). */
    SLOT_ARRIVED = 72,          /* 72-103 */
    SLOT_ARRIVED_DATA = 104,    /* 104-135: collect's second up pass */
};

/* Chunk of a child's partial a reduction holds at once. */
enum { REDUCE_CHUNK_BYTES = 64 << 10 };

static int set_pe(const struct osoe_active_set *s, int rank)
{
    return s->start + (rank << s->stride_log);
}

/* The span a relative rank's subtree may cover: its lowest set bit, or for
 * the root the smallest power of two at least the set's size. */
static int span_of(int v, int size)
{
    if (v != 0)
        return v & -v;
    int p = 1;
    while (p < size)
        p <<= 1;
    return p;
}

static int subtree_size(int v, int size)
{
    int span = span_of(v, size);
    return v + span <= size ? span : size - v;
}

/* Which of its parent's children v is: child j is parent + 2^j. */
static int child_index(int v)
{
    return __builtin_ctz((unsigned)v);
}

/* The absolute rank of relative rank v under `root`. */
static int absolute(const struct osoe_active_set *s, int root, int v)
{
    return (v + root) % s->size;
}

static void signal_member(long *pSync, int slot, int pe)
{
    pshmem_long_atomic_add(&pSync[slot], 1, pe);
}

/* Wait for `count` adds to this member's word, then take them back. */
static void await(long *pSync, int slot, long count, const char *routine)
{
    if (count == 0)
        return;
    osoe_wait_until(routine, &pSync[slot], sizeof(long), true, SHMEM_CMP_GE,
                    (uint64_t)(SHMEM_SYNC_VALUE + count));
    pshmem_long_atomic_add(&pSync[slot], -count, (int)osoe.bootstrap.rank);
}

/* Put `bytes` of `from` to the same place of `pe`'s `dest`, then add 1 to its
 * release word, ordered after the bytes (shmem_putmem_signal). */
static void put_released(void *dest, const void *from, size_t bytes, long *pSync, int slot,
                         int pe)
{
    pshmem_putmem_signal(dest, from, bytes, (uint64_t *)(void *)&pSync[slot], 1,
                         SHMEM_SIGNAL_ADD, pe);
}

bool osoe_active_set_is_whole_job(const char *routine, int PE_start, int logPE_stride,
                                  int PE_size, struct osoe_active_set *s)
{
    /* Before or after the library's life the job's size is 0, and "not a
     * set of this job" would be a wrong reason for the right refusal. */
    osoe_require_initialized(routine);
    int npes = (int)osoe.bootstrap.world_size, me = (int)osoe.bootstrap.rank;
    if (PE_start == 0 && PE_size == npes && logPE_stride >= 0 &&
        (logPE_stride == 0 || PE_size == 1))
        return true;
    if (PE_size == 1)
        logPE_stride = 0;               /* one member: the stride says nothing */
    if (PE_start < 0 || PE_size < 1 || logPE_stride < 0 || logPE_stride > 30 ||
        (int64_t)PE_start + ((int64_t)(PE_size - 1) << logPE_stride) >= npes)
        osoe_fatal("%s: the active set (PE_start %d, logPE_stride %d, PE_size %d) is not a set "
                   "of this job's %d PEs", routine, PE_start, logPE_stride, PE_size, npes);
    int offset = me - PE_start;
    if (offset < 0 || (offset & ((1 << logPE_stride) - 1)) != 0 ||
        (offset >> logPE_stride) >= PE_size)
        osoe_fatal("%s: PE %d called it, and is not in its active set (PE_start %d, "
                   "logPE_stride %d, PE_size %d)", routine, me, PE_start, logPE_stride, PE_size);
    *s = (struct osoe_active_set){ .start = PE_start, .stride_log = logPE_stride,
                                   .size = PE_size, .rank = offset >> logPE_stride };
    return false;
}

/*
 * A dissemination barrier: in round k a member signals the member 2^k ranks
 * on and waits for the member 2^k ranks back. ceil(log2 size) rounds, one
 * signal each. A barrier quiets first (spec §9.10.3's shmem_barrier); a sync
 * does not.
 */
void osoe_active_set_barrier(const char *routine, const struct osoe_active_set *s,
                             long *pSync, bool quiet)
{
    if (quiet)
        pshmem_quiet();
    int round = 0;
    for (int k = 1; k < s->size; k <<= 1, round++) {
        signal_member(pSync, SLOT_BARRIER + round, set_pe(s, (s->rank + k) % s->size));
        await(pSync, SLOT_BARRIER + round, 1, routine);
    }
}

/* The last child of v: the largest 2^j with v + 2^j a rank, or 0. */
static int last_child_step(int v, int size)
{
    int last = 0;
    for (int k = 1; k < span_of(v, size) && v + k < size; k <<= 1)
        last = k;
    return last;
}

/* Signal v's parent, on the word for v among its children, stores first
 * (quiet): what the parent reads next is what this member wrote. */
static void arrive(const struct osoe_active_set *s, int root, int v, long *pSync, int slot)
{
    pshmem_quiet();
    signal_member(pSync, slot + child_index(v), set_pe(s, absolute(s, root, v - (v & -v))));
}

/* Up the tree rooted at `root`: wait for each child, then signal the parent. */
static void up(const char *routine, const struct osoe_active_set *s, int root, long *pSync,
               int slot)
{
    int v = (s->rank - root + s->size) % s->size;
    for (int k = 1, j = 0; k < span_of(v, s->size) && v + k < s->size; k <<= 1, j++)
        await(pSync, slot + j, 1, routine);
    if (v != 0)
        arrive(s, root, v, pSync, slot);
}

/* Down the tree: wait for the parent's bytes, then put `bytes` of `from` to
 * every child's `dest`, each followed by its release - the largest subtree
 * first, since it has the most levels still to go (review of slice 7, S1). */
static void down(const char *routine, const struct osoe_active_set *s, int root, void *dest,
                 const void *from, size_t bytes, long *pSync, int slot)
{
    int v = (s->rank - root + s->size) % s->size;
    if (v != 0)
        await(pSync, slot, 1, routine);
    for (int k = last_child_step(v, s->size); k >= 1; k >>= 1)
        put_released(dest, from, bytes, pSync, slot, set_pe(s, absolute(s, root, v + k)));
}

void osoe_active_set_broadcast(const char *routine, const struct osoe_active_set *s,
                               void *dest, const void *source, size_t bytes, int root,
                               long *pSync)
{
    if (root < 0 || root >= s->size)
        osoe_fatal("%s: PE_root %d is not a rank of its active set of %d", routine, root,
                   s->size);
    up(routine, s, root, pSync, SLOT_ARRIVED);
    /* The root sends its source and leaves its own dest alone, as the spec
     * says of these routines; every other member forwards what it received. */
    down(routine, s, root, dest, s->rank == root ? source : dest, bytes, pSync, SLOT_RELEASED);
}

/* Gather into rank 0 by each parent getting its children's subtree ranges out
 * of their dests - `offset_of` and `bytes_of` give a subtree's range - then
 * the whole of rank 0's dest down to everyone. */
static void gather_then_spread(const char *routine, const struct osoe_active_set *s,
                               unsigned char *dest, size_t whole,
                               const size_t *child_offset, const size_t *child_bytes,
                               long *pSync, int arrived, int released)
{
    int v = s->rank, n = 0;
    for (int k = 1; k < span_of(v, s->size) && v + k < s->size; k <<= 1, n++) {
        await(pSync, arrived + n, 1, routine);
        if (child_bytes[n])
            pshmem_getmem(dest + child_offset[n], dest + child_offset[n], child_bytes[n],
                          set_pe(s, v + k));
    }
    if (v != 0)
        arrive(s, 0, v, pSync, arrived);
    down(routine, s, 0, dest, dest, whole, pSync, released);
}

void osoe_active_set_fcollect(const char *routine, const struct osoe_active_set *s,
                              void *dest, const void *source, size_t bytes, long *pSync)
{
    unsigned char *to = dest;
    if (bytes && (size_t)s->size > SIZE_MAX / bytes)
        osoe_fatal("%s: %d blocks of %zu bytes reach past what a size can count", routine,
                   s->size, bytes);
    memmove(to + (size_t)s->rank * bytes, source, bytes);
    size_t offsets[32], lengths[32];
    int n = 0;
    for (int k = 1; k < span_of(s->rank, s->size) && s->rank + k < s->size; k <<= 1, n++) {
        offsets[n] = (size_t)(s->rank + k) * bytes;
        lengths[n] = (size_t)subtree_size(s->rank + k, s->size) * bytes;
    }
    gather_then_spread(routine, s, to, (size_t)s->size * bytes, offsets, lengths, pSync,
                       SLOT_ARRIVED, SLOT_RELEASED);
}

/*
 * collect: the members' sizes differ, so first an up pass of subtree byte
 * totals - a member writes its subtree's total into its own pSync word and its
 * parent reads it - then a down pass of where each subtree starts and how long
 * the whole is, then fcollect's up and down over those places.
 */
void osoe_active_set_collect(const char *routine, const struct osoe_active_set *s,
                             void *dest, const void *source, size_t bytes, long *pSync)
{
    int v = s->rank, me = (int)osoe.bootstrap.rank;
    int n = 0;
    size_t lengths[32], offsets[32];
    size_t total = bytes;
    for (int k = 1; k < span_of(v, s->size) && v + k < s->size; k <<= 1, n++) {
        await(pSync, SLOT_ARRIVED + n, 1, routine);
        lengths[n] = (size_t)pshmem_long_g(&pSync[SLOT_TOTAL], set_pe(s, v + k));
        total += lengths[n];
    }
    size_t offset = 0, whole = total;
    if (v != 0) {
        /* The total before the signal its reader waits for (arrive quiets). */
        pshmem_long_atomic_set(&pSync[SLOT_TOTAL], (long)total, me);
        arrive(s, 0, v, pSync, SLOT_ARRIVED);
        await(pSync, SLOT_RELEASED, 1, routine);
        /* The parent read this member's total before it released it. */
        offset = (size_t)pshmem_long_atomic_fetch(&pSync[SLOT_OFFSET], me);
        whole = (size_t)pshmem_long_atomic_fetch(&pSync[SLOT_GRAND], me);
        pshmem_long_atomic_set(&pSync[SLOT_TOTAL], SHMEM_SYNC_VALUE, me);
        pshmem_long_atomic_set(&pSync[SLOT_OFFSET], SHMEM_SYNC_VALUE, me);
        pshmem_long_atomic_set(&pSync[SLOT_GRAND], SHMEM_SYNC_VALUE, me);
    }
    size_t at = offset + bytes;
    n = 0;
    for (int k = 1; k < span_of(v, s->size) && v + k < s->size; k <<= 1, n++) {
        int pe = set_pe(s, v + k);
        offsets[n] = at;
        pshmem_long_p(&pSync[SLOT_OFFSET], (long)at, pe);
        pshmem_long_p(&pSync[SLOT_GRAND], (long)whole, pe);
        pshmem_fence();                 /* the two words before the release */
        signal_member(pSync, SLOT_RELEASED, pe);
        at += lengths[n];
    }
    memmove((unsigned char *)dest + offset, source, bytes);
    gather_then_spread(routine, s, dest, whole, offsets, lengths, pSync, SLOT_ARRIVED_DATA,
                       SLOT_RELEASED_DATA);
}

/*
 * A reduction: source into dest, then up the tree each parent gets its
 * children's partials out of their dests, a chunk at a time into private
 * memory, and combines them into its own (combine.h); down it, rank 0's
 * result to every member. Floating results are one order of combination,
 * rank 0's, the same on every member.
 */
void osoe_active_set_reduce(const char *routine, const struct osoe_active_set *s, void *dest,
                            const void *source, size_t nreduce,
                            enum osoe_reduction_operation operation,
                            struct osoe_element element, long *pSync)
{
    size_t width = element.width, bytes = nreduce * width;
    osoe_combine_function combine = osoe_combine_lookup(operation, element);
    if (!combine)
        osoe_fatal("%s: no such reduction for this type", routine);
    if (dest != source)
        memmove(dest, source, bytes);
    int v = s->rank;
    size_t chunk = bytes < REDUCE_CHUNK_BYTES ? bytes : REDUCE_CHUNK_BYTES;
    chunk -= chunk % width;
    unsigned char *incoming = chunk ? malloc(chunk) : NULL;
    if (chunk && !incoming)
        osoe_fatal("%s: out of memory for a reduction's chunk of %zu bytes", routine, chunk);
    for (int k = 1, j = 0; k < span_of(v, s->size) && v + k < s->size; k <<= 1, j++) {
        await(pSync, SLOT_ARRIVED + j, 1, routine);
        for (size_t at = 0; at < bytes; at += chunk) {
            size_t len = bytes - at < chunk ? bytes - at : chunk;
            pshmem_getmem(incoming, (unsigned char *)dest + at, len, set_pe(s, v + k));
            combine((unsigned char *)dest + at, incoming, len / width);
        }
    }
    free(incoming);
    if (v != 0)
        arrive(s, 0, v, pSync, SLOT_ARRIVED);
    down(routine, s, 0, dest, dest, bytes, pSync, SLOT_RELEASED);
}

/*
 * alltoall and alltoalls: each member puts its blocks straight to each member,
 * starting past itself so the members do not all write to one at once, then a
 * quiet and a barrier. Like the team form (collectives.c), no barrier first:
 * the spec has every dest ready before any member calls.
 */
void osoe_active_set_alltoall(const char *routine, const struct osoe_active_set *s,
                              void *dest, const void *source, ptrdiff_t dest_stride,
                              ptrdiff_t source_stride, size_t nelems, size_t width,
                              long *pSync)
{
    if (dest_stride < 1 || source_stride < 1)
        osoe_fatal("%s: strides %td and %td; both must be at least 1", routine, dest_stride,
                   source_stride);
    size_t stride = (size_t)(dest_stride > source_stride ? dest_stride : source_stride);
    if (nelems && ((size_t)s->size > SIZE_MAX / nelems ||
                   (size_t)s->size * nelems > SIZE_MAX / width / stride))
        osoe_fatal("%s: the blocks reach past what an offset can count", routine);
    for (int step = 0; step < s->size; step++) {
        int j = (s->rank + 1 + step) % s->size;
        unsigned char *to = (unsigned char *)dest +
                            (size_t)s->rank * nelems * (size_t)dest_stride * width;
        const unsigned char *from = (const unsigned char *)source +
                                    (size_t)j * nelems * (size_t)source_stride * width;
        int pe = set_pe(s, j);
        if (dest_stride == 1 && source_stride == 1)
            pshmem_putmem(to, from, nelems * width, pe);
        else if (width == 4)
            pshmem_iput32(to, from, dest_stride, source_stride, nelems, pe);
        else
            pshmem_iput64(to, from, dest_stride, source_stride, nelems, pe);
    }
    osoe_active_set_barrier(routine, s, pSync, true);
}
