/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Every C11 generic of shmem.h, on every type of spec Tables 6, 7, 8 and 12,
 * with and without a context. Compile-only: -std=c11 -pedantic -Wall -Wextra -Werror. */
#include <stddef.h>
#include <stdint.h>
#include <shmem.h>

#define STANDARD(X) X(int) X(long) X(long long) X(unsigned int) X(unsigned long) \
    X(unsigned long long) X(int32_t) X(int64_t) X(uint32_t) X(uint64_t) X(size_t) X(ptrdiff_t)
#define FLOATS(X) X(float) X(double)
#define BITWISE(X) X(unsigned int) X(unsigned long) X(unsigned long long) \
    X(int32_t) X(int64_t) X(uint32_t) X(uint64_t)

#define USE_STANDARD(T) { T *p = shmem_malloc(sizeof(T)); const T *cp = p; T v = 1; size_t ix[1]; int st[1] = {0}; \
    v = shmem_atomic_compare_swap(p, v, v, 0); v = shmem_atomic_compare_swap(c, p, v, v, 0); \
    shmem_atomic_inc(p, 0); shmem_atomic_inc(c, p, 0); \
    v = shmem_atomic_fetch_inc(p, 0); v = shmem_atomic_fetch_inc(c, p, 0); \
    shmem_atomic_add(p, v, 0); shmem_atomic_add(c, p, v, 0); \
    v = shmem_atomic_fetch_add(p, v, 0); v = shmem_atomic_fetch_add(c, p, v, 0); \
    shmem_wait_until(p, SHMEM_CMP_NE, (T)99); (void)shmem_test(p, SHMEM_CMP_NE, (T)99); \
    shmem_wait_until_all(p, 1, st, SHMEM_CMP_NE, (T)99); \
    (void)shmem_wait_until_any(p, 1, st, SHMEM_CMP_NE, (T)99); \
    (void)shmem_wait_until_some(p, 1, ix, st, SHMEM_CMP_NE, (T)99); \
    shmem_wait_until_all_vector(p, 1, st, SHMEM_CMP_NE, &v); \
    (void)shmem_wait_until_any_vector(p, 1, st, SHMEM_CMP_NE, &v); \
    (void)shmem_wait_until_some_vector(p, 1, ix, st, SHMEM_CMP_NE, &v); \
    (void)shmem_test_all(p, 1, st, SHMEM_CMP_NE, (T)99); (void)shmem_test_any(p, 1, st, SHMEM_CMP_NE, (T)99); \
    (void)shmem_test_some(p, 1, ix, st, SHMEM_CMP_NE, (T)99); \
    (void)shmem_test_all_vector(p, 1, st, SHMEM_CMP_NE, &v); (void)shmem_test_any_vector(p, 1, st, SHMEM_CMP_NE, &v); \
    (void)shmem_test_some_vector(p, 1, ix, st, SHMEM_CMP_NE, &v); (void)cp; }
#define USE_EXTENDED(T) { T *p = shmem_malloc(sizeof(T)); const T *cp = p; T v = 1; \
    v = shmem_atomic_fetch(p, 0); v = shmem_atomic_fetch(cp, 0); v = shmem_atomic_fetch(c, p, 0); \
    v = shmem_atomic_fetch(c, cp, 0); shmem_atomic_set(p, v, 0); shmem_atomic_set(c, p, v, 0); \
    v = shmem_atomic_swap(p, v, 0); v = shmem_atomic_swap(c, p, v, 0); }
#define USE_BITWISE(T) { T *p = shmem_malloc(sizeof(T)); T v = 1; \
    shmem_atomic_and(p, v, 0); shmem_atomic_and(c, p, v, 0); v = shmem_atomic_fetch_and(p, v, 0); \
    v = shmem_atomic_fetch_and(c, p, v, 0); shmem_atomic_or(p, v, 0); shmem_atomic_or(c, p, v, 0); \
    v = shmem_atomic_fetch_or(p, v, 0); v = shmem_atomic_fetch_or(c, p, v, 0); \
    shmem_atomic_xor(p, v, 0); shmem_atomic_xor(c, p, v, 0); v = shmem_atomic_fetch_xor(p, v, 0); \
    v = shmem_atomic_fetch_xor(c, p, v, 0); }

int main(void)
{
    shmem_ctx_t c = SHMEM_CTX_DEFAULT;
    STANDARD(USE_STANDARD)
    STANDARD(USE_EXTENDED)
    FLOATS(USE_EXTENDED)
    BITWISE(USE_BITWISE)
    { short *p = shmem_malloc(2); shmem_wait_until(p, SHMEM_CMP_NE, (short)9); (void)shmem_test(p, SHMEM_CMP_NE, (short)9); shmem_wait(p, (short)9); }
    { unsigned short *p = shmem_malloc(2); shmem_wait_until(p, SHMEM_CMP_NE, (unsigned short)9); (void)shmem_test(p, SHMEM_CMP_NE, (unsigned short)9); }
    (void)shmem_sync(SHMEM_TEAM_WORLD);
    return 0;
}
