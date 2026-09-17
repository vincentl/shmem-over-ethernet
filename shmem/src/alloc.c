#include <stdlib.h>
#include <string.h>

#include "alloc.h"

int osoe_alloc_init(struct osoe_alloc *a, uint64_t capacity)
{
    a->capacity = capacity / OSOE_ALLOC_ALIGN * OSOE_ALLOC_ALIGN;
    a->room = 16;
    a->blocks = malloc(a->room * sizeof *a->blocks);
    if (!a->blocks)
        return -1;
    a->count = 0;
    if (a->capacity > 0)
        a->blocks[a->count++] = (struct osoe_alloc_block){ 0, a->capacity, true };
    return 0;
}

void osoe_alloc_fini(struct osoe_alloc *a)
{
    free(a->blocks);
    *a = (struct osoe_alloc){ 0 };
}

int osoe_alloc_take(struct osoe_alloc *a, uint64_t size, uint64_t *offset)
{
    if (size > UINT64_MAX - (OSOE_ALLOC_ALIGN - 1))
        return 1;
    size = (size + OSOE_ALLOC_ALIGN - 1) / OSOE_ALLOC_ALIGN * OSOE_ALLOC_ALIGN;

    size_t i = 0;
    while (i < a->count && !(a->blocks[i].free && a->blocks[i].size >= size))
        i++;
    if (i == a->count)
        return 1;

    struct osoe_alloc_block *b = &a->blocks[i];
    if (b->size > size) {
        /* Split: the remainder stays free, just after the new block. */
        if (a->count == a->room) {
            size_t room = a->room * 2;
            struct osoe_alloc_block *grown = realloc(a->blocks, room * sizeof *grown);
            if (!grown)
                return -1;
            a->blocks = grown;
            a->room = room;
            b = &a->blocks[i];
        }
        memmove(b + 2, b + 1, (a->count - i - 1) * sizeof *b);
        b[1] = (struct osoe_alloc_block){ b->offset + size, b->size - size, true };
        b->size = size;
        a->count++;
    }
    b->free = false;
    *offset = b->offset;
    return 0;
}

static void merge_with_next(struct osoe_alloc *a, size_t i)
{
    a->blocks[i].size += a->blocks[i + 1].size;
    memmove(&a->blocks[i + 1], &a->blocks[i + 2],
            (a->count - i - 2) * sizeof a->blocks[0]);
    a->count--;
}

int osoe_alloc_give(struct osoe_alloc *a, uint64_t offset)
{
    size_t lo = 0, hi = a->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a->blocks[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == a->count || a->blocks[lo].offset != offset || a->blocks[lo].free)
        return -1;

    a->blocks[lo].free = true;
    if (lo + 1 < a->count && a->blocks[lo + 1].free)
        merge_with_next(a, lo);
    if (lo > 0 && a->blocks[lo - 1].free)
        merge_with_next(a, lo - 1);
    return 0;
}
