/*
 * The point-to-point wait loop (§4.9), shared by shmem_wait_until, shmem_test,
 * their multiple-element forms and the signal routines (spec §9.11, §9.8).
 *
 * `routine` names the caller in diagnostics. `width` is the variable's width in
 * bytes - 2, 4 or 8 - and values are carried as uint64_t, which is what the
 * boundary's load and rmw take.
 *
 * **Signedness is a parameter, not an afterthought.** Table 6 has signed types,
 * and `SHMEM_CMP_LT` on an `int` holding -1 is not the same question as on a
 * `uint32_t` holding 0xffffffff. A value crossing this boundary is extended to
 * 64 bits according to `sgn`, and compared as an int64_t or a uint64_t
 * accordingly.
 */
#ifndef OSOE_WAIT_H
#define OSOE_WAIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Block until the variable satisfies the comparison, and return the value that
 * did. The value matters to shmem_signal_wait_until (spec §9.11.15), which
 * returns it; every other caller discards it. */
uint64_t osoe_wait_until(const char *routine, const void *ivar, unsigned width, bool sgn,
                         int cmp, uint64_t want);

/* One coherent load and a compare; never blocks and never touches the watch
 * slot. */
int osoe_test(const char *routine, const void *ivar, unsigned width, bool sgn, int cmp,
              uint64_t want);

/*
 * The multiple-element waits and tests (spec §9.11.2-§9.11.14).
 *
 * `status` is the optional mask: NULL means every element is in the set, and
 * otherwise a nonzero entry excludes its element. `values` is NULL for the
 * forms that compare every element against one `value`, and an array of
 * `nelems` elements of `width` bytes for the `_vector` forms.
 *
 * The empty set is not an error and each routine has its own answer for it
 * (§9.11 and 1.6 change #19): `all` returns at once, `test_all` returns 1,
 * `any` and `test_any` return SIZE_MAX, `some` and `test_some` return 0.
 */
struct osoe_wait_many {
    const char *routine;
    const void *ivars;
    size_t nelems;
    unsigned width;
    bool sgn;
    const int *status;
    int cmp;
    uint64_t value;             /* used when values == NULL */
    const void *values;         /* the _vector forms' cmp_values */
};

void   osoe_wait_all(const struct osoe_wait_many *w);
size_t osoe_wait_any(const struct osoe_wait_many *w);
size_t osoe_wait_some(const struct osoe_wait_many *w, size_t *indices);
int    osoe_test_all(const struct osoe_wait_many *w);
size_t osoe_test_any(const struct osoe_wait_many *w);
size_t osoe_test_some(const struct osoe_wait_many *w, size_t *indices);

#endif /* OSOE_WAIT_H */
