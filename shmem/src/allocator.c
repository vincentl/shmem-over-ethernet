#include <stdlib.h>
#include <string.h>

#include "allocator.h"

static uint64_t round_up(uint64_t n, uint64_t to)
{
    return (n + to - 1) / to * to;
}

int osoe_allocator_init(struct osoe_allocator *a, uint64_t capacity)
{
    a->capacity = capacity / OSOE_ALLOCATOR_ALIGN * OSOE_ALLOCATOR_ALIGN;
    a->room = 16;
    a->blocks = malloc(a->room * sizeof *a->blocks);
    if (!a->blocks)
        return -1;
    a->count = 0;
    if (a->capacity > 0)
        a->blocks[a->count++] = (struct osoe_allocator_block){ 0, a->capacity, 0, true };
    return 0;
}

void osoe_allocator_fini(struct osoe_allocator *a)
{
    free(a->blocks);
    *a = (struct osoe_allocator){ 0 };
}

/* Make room for `extra` more entries. The array's size depends only on the
 * call sequence, so growing it does not touch determinism. */
static bool reserve(struct osoe_allocator *a, size_t extra)
{
    if (a->count + extra <= a->room)
        return true;
    size_t room = a->room;
    while (room < a->count + extra)
        room *= 2;
    struct osoe_allocator_block *grown = realloc(a->blocks, room * sizeof *grown);
    if (!grown)
        return false;
    a->blocks = grown;
    a->room = room;
    return true;
}

/* The index of the block starting exactly at `offset`, or a->count. */
static size_t find(const struct osoe_allocator *a, uint64_t offset)
{
    size_t lo = 0, hi = a->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a->blocks[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < a->count && a->blocks[lo].offset == offset ? lo : a->count;
}

/*
 * Where `size` bytes aligned to `align` would sit in free block i, or false.
 * Unhinted allocations take the lowest aligned position and hinted ones the
 * highest, so the two grow from opposite ends of the heap (§4.4). Both are
 * functions of the block list alone.
 */
static bool placement(const struct osoe_allocator *a, size_t i, uint64_t size, uint64_t align,
                      bool high, uint64_t *at)
{
    const struct osoe_allocator_block *b = &a->blocks[i];
    if (!b->free || b->size < size)
        return false;
    uint64_t end = b->offset + b->size;
    uint64_t p = high ? (end - size) / align * align : round_up(b->offset, align);
    if (high ? p < b->offset : p > end - size)
        return false;
    *at = p;
    return true;
}

int osoe_allocator_take(struct osoe_allocator *a, uint64_t size, uint64_t align, long hints,
                    uint64_t *offset)
{
    if (align < OSOE_ALLOCATOR_ALIGN)
        align = OSOE_ALLOCATOR_ALIGN;
    if (size > UINT64_MAX - (OSOE_ALLOCATOR_ALIGN - 1))
        return 1;
    size = round_up(size, OSOE_ALLOCATOR_ALIGN);

    /* A hinted allocation searches from the top of the heap down; both
     * directions are first fit, so the result depends only on the list. */
    bool high = hints != 0;
    size_t found = a->count;
    uint64_t at = 0;
    for (size_t k = 0; k < a->count; k++) {
        size_t i = high ? a->count - 1 - k : k;
        if (placement(a, i, size, align, high, &at)) {
            found = i;
            break;
        }
    }
    if (found == a->count)
        return 1;

    /* The chosen free block becomes up to three: the padding below the
     * placement, the allocation, and the remainder above. Both padding pieces
     * are multiples of OSOE_ALLOCATOR_ALIGN, since every term is. */
    struct osoe_allocator_block *b = &a->blocks[found];
    uint64_t head = at - b->offset;
    uint64_t tail = b->offset + b->size - at - size;
    size_t extra = (head ? 1u : 0u) + (tail ? 1u : 0u);
    if (!reserve(a, extra))
        return -1;
    b = &a->blocks[found];

    if (extra) {
        memmove(b + 1 + extra, b + 1, (a->count - found - 1) * sizeof *b);
        a->count += extra;
    }
    size_t k = found;
    if (head)
        a->blocks[k++] = (struct osoe_allocator_block){ b->offset, head, 0, true };
    a->blocks[k] = (struct osoe_allocator_block){ at, size, hints, false };
    if (tail)
        a->blocks[k + 1] = (struct osoe_allocator_block){ at + size, tail, 0, true };

    *offset = at;
    return 0;
}

static void merge_with_next(struct osoe_allocator *a, size_t i)
{
    a->blocks[i].size += a->blocks[i + 1].size;
    memmove(&a->blocks[i + 1], &a->blocks[i + 2],
            (a->count - i - 2) * sizeof a->blocks[0]);
    a->count--;
}

int osoe_allocator_give(struct osoe_allocator *a, uint64_t offset)
{
    size_t lo = find(a, offset);
    if (lo == a->count || a->blocks[lo].free)
        return -1;

    a->blocks[lo].free = true;
    a->blocks[lo].hints = 0;
    if (lo + 1 < a->count && a->blocks[lo + 1].free)
        merge_with_next(a, lo);
    if (lo > 0 && a->blocks[lo - 1].free)
        merge_with_next(a, lo - 1);
    return 0;
}

int osoe_allocator_query(const struct osoe_allocator *a, uint64_t offset, uint64_t *size,
                     long *hints)
{
    size_t i = find(a, offset);
    if (i == a->count || a->blocks[i].free)
        return -1;
    *size = a->blocks[i].size;
    *hints = a->blocks[i].hints;
    return 0;
}

int osoe_allocator_resize(struct osoe_allocator *a, uint64_t offset, uint64_t size)
{
    size_t i = find(a, offset);
    if (i == a->count || a->blocks[i].free)
        return -1;
    if (size > UINT64_MAX - (OSOE_ALLOCATOR_ALIGN - 1))
        return 1;
    size = round_up(size, OSOE_ALLOCATOR_ALIGN);

    uint64_t have = a->blocks[i].size;
    if (size == have)
        return 0;

    bool next_free = i + 1 < a->count && a->blocks[i + 1].free;
    if (size < have) {
        uint64_t freed = have - size;
        if (next_free) {
            /* Push the neighbour's start down: no new entry, so this cannot
             * fail. */
            a->blocks[i].size = size;
            a->blocks[i + 1].offset -= freed;
            a->blocks[i + 1].size += freed;
        } else {
            if (!reserve(a, 1))
                return -1;
            struct osoe_allocator_block *b = &a->blocks[i];
            memmove(b + 2, b + 1, (a->count - i - 1) * sizeof *b);
            a->count++;
            b->size = size;
            b[1] = (struct osoe_allocator_block){ offset + size, freed, 0, true };
        }
        return 0;
    }

    /* Growing in place needs the space immediately above. */
    if (!next_free || have + a->blocks[i + 1].size < size)
        return 1;
    uint64_t wanted = size - have;
    a->blocks[i].size = size;
    if (a->blocks[i + 1].size == wanted) {
        memmove(&a->blocks[i + 1], &a->blocks[i + 2],
                (a->count - i - 2) * sizeof a->blocks[0]);
        a->count--;
    } else {
        a->blocks[i + 1].offset += wanted;
        a->blocks[i + 1].size -= wanted;
    }
    return 0;
}

void osoe_allocator_levels(const struct osoe_allocator *a, uint64_t *in_use, uint64_t *largest_free)
{
    uint64_t used = 0, largest = 0;
    for (size_t i = 0; i < a->count; i++) {
        if (a->blocks[i].free) {
            if (a->blocks[i].size > largest)
                largest = a->blocks[i].size;
        } else {
            used += a->blocks[i].size;
        }
    }
    *in_use = used;
    *largest_free = largest;
}
