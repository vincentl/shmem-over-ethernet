/*
 * Atomic memory operations (spec §9.7) over the boundary's rmw (§4.10).
 *
 * The spec's twenty-odd routine names per type are one parameterized
 * read-modify-write: set and swap are a write, fetch is a read, inc and add are
 * an add, and the bitwise routines pass their operator. Phase 1 step 1 provides
 * the 64-bit and 32-bit unsigned types; the rest of Table 6 is a wider macro
 * expansion over the same calls (§7.5). The 32-bit type is here from the start
 * so the 32-bit wait path is exercised (§8.2).
 */
#include <stdint.h>

#include <shmem.h>

#include "export.h"
#include "sym.h"
#include "transport.h"

/* An AMO's target must be a word in the heap, aligned for its width. */
static uint64_t amo_offset(const char *routine, const void *addr, unsigned width, int pe)
{
    osoe_check_pe(routine, pe);
    return osoe_sym_word_offset(routine, addr, width);
}

#define OSOE_AMO(T, NAME, WIDTH)                                                      \
    static uint64_t NAME##_rmw(const char *routine, const void *dest,                 \
                               enum osoe_rmw_op op, uint64_t value, uint64_t compare, \
                               unsigned flags, int pe)                                \
    {                                                                                 \
        uint64_t offset = amo_offset(routine, dest, WIDTH, pe);                       \
        return osoe_tp_rmw(osoe_tp_ctx_default(), pe, OSOE_REGION_HEAP, offset, op,    \
                           WIDTH, value, compare, flags);                             \
    }                                                                                 \
                                                                                      \
    OSOE_API void shmem_##NAME##_atomic_set(T *dest, T value, int pe)                  \
    {                                                                                 \
        NAME##_rmw("shmem_" #NAME "_atomic_set", dest, OSOE_RMW_WRITE, value, 0, 0, pe); \
    }                                                                                 \
    OSOE_API T shmem_##NAME##_atomic_swap(T *dest, T value, int pe)                    \
    {                                                                                 \
        return (T)NAME##_rmw("shmem_" #NAME "_atomic_swap", dest, OSOE_RMW_WRITE,      \
                             value, 0, OSOE_TP_FETCH, pe);                             \
    }                                                                                 \
    OSOE_API T shmem_##NAME##_atomic_fetch(const T *source, int pe)                    \
    {                                                                                 \
        return (T)NAME##_rmw("shmem_" #NAME "_atomic_fetch", source, OSOE_RMW_READ,    \
                             0, 0, OSOE_TP_FETCH, pe);                                 \
    }                                                                                 \
    OSOE_API T shmem_##NAME##_atomic_compare_swap(T *dest, T cond, T value, int pe)    \
    {                                                                                 \
        return (T)NAME##_rmw("shmem_" #NAME "_atomic_compare_swap", dest,              \
                             OSOE_RMW_CSWAP, value, cond, OSOE_TP_FETCH, pe);          \
    }                                                                                 \
    OSOE_API void shmem_##NAME##_atomic_add(T *dest, T value, int pe)                  \
    {                                                                                 \
        NAME##_rmw("shmem_" #NAME "_atomic_add", dest, OSOE_RMW_ADD, value, 0, 0, pe); \
    }                                                                                 \
    OSOE_API T shmem_##NAME##_atomic_fetch_add(T *dest, T value, int pe)               \
    {                                                                                 \
        return (T)NAME##_rmw("shmem_" #NAME "_atomic_fetch_add", dest, OSOE_RMW_ADD,   \
                             value, 0, OSOE_TP_FETCH, pe);                             \
    }                                                                                 \
    OSOE_API void shmem_##NAME##_atomic_inc(T *dest, int pe)                           \
    {                                                                                 \
        NAME##_rmw("shmem_" #NAME "_atomic_inc", dest, OSOE_RMW_ADD, 1, 0, 0, pe);     \
    }                                                                                 \
    OSOE_API T shmem_##NAME##_atomic_fetch_inc(T *dest, int pe)                        \
    {                                                                                 \
        return (T)NAME##_rmw("shmem_" #NAME "_atomic_fetch_inc", dest, OSOE_RMW_ADD,   \
                             1, 0, OSOE_TP_FETCH, pe);                                 \
    }                                                                                 \
    OSOE_API void shmem_##NAME##_atomic_and(T *dest, T value, int pe)                  \
    {                                                                                 \
        NAME##_rmw("shmem_" #NAME "_atomic_and", dest, OSOE_RMW_AND, value, 0, 0, pe); \
    }                                                                                 \
    OSOE_API T shmem_##NAME##_atomic_fetch_and(T *dest, T value, int pe)               \
    {                                                                                 \
        return (T)NAME##_rmw("shmem_" #NAME "_atomic_fetch_and", dest, OSOE_RMW_AND,   \
                             value, 0, OSOE_TP_FETCH, pe);                             \
    }                                                                                 \
    OSOE_API void shmem_##NAME##_atomic_or(T *dest, T value, int pe)                   \
    {                                                                                 \
        NAME##_rmw("shmem_" #NAME "_atomic_or", dest, OSOE_RMW_OR, value, 0, 0, pe);   \
    }                                                                                 \
    OSOE_API T shmem_##NAME##_atomic_fetch_or(T *dest, T value, int pe)                \
    {                                                                                 \
        return (T)NAME##_rmw("shmem_" #NAME "_atomic_fetch_or", dest, OSOE_RMW_OR,     \
                             value, 0, OSOE_TP_FETCH, pe);                             \
    }                                                                                 \
    OSOE_API void shmem_##NAME##_atomic_xor(T *dest, T value, int pe)                  \
    {                                                                                 \
        NAME##_rmw("shmem_" #NAME "_atomic_xor", dest, OSOE_RMW_XOR, value, 0, 0, pe); \
    }                                                                                 \
    OSOE_API T shmem_##NAME##_atomic_fetch_xor(T *dest, T value, int pe)               \
    {                                                                                 \
        return (T)NAME##_rmw("shmem_" #NAME "_atomic_fetch_xor", dest, OSOE_RMW_XOR,   \
                             value, 0, OSOE_TP_FETCH, pe);                             \
    }

OSOE_AMO(uint64_t, uint64, 8)
OSOE_AMO(uint32_t, uint32, 4)
