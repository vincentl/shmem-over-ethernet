/*
 * The profiling interface (§7.4; spec §10).
 *
 * The claim is that an application can replace any routine and still reach the
 * implementation. Nothing about the library's own symbol table demonstrates
 * that; only interposing does. So this program *is* a profiling library: it
 * defines its own `shmem_malloc`, `shmem_putmem` and `shmem_uint64_atomic_add`,
 * counts the calls, and forwards to `pshmem_`.
 *
 * Three things follow, and all three are checked:
 *
 *   - the application's definition is the one that runs, because the library's
 *     is a weak alias;
 *   - forwarding to `pshmem_` reaches the real implementation, so the program
 *     still works;
 *   - the library's *internal* calls do not pass through the interposer.
 *     `shmem_realloc(NULL, n)` is `shmem_malloc` by definition (spec §9.3.3)
 *     and the library makes that call to `pshmem_malloc`, so a profiler counts
 *     what the application called and not what the library did about it. That
 *     is a decision, not an accident, and it is the one this test pins down.
 *
 *   - and the C11 generic macros expand to the `shmem_` spelling, so they are
 *     interposed too - which they would not be if they named `pshmem_`.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>

#include <pshmem.h>
#include <shmem.h>

#include "check.h"
#include "invariants.h"

static int mallocs, puts_made, adds, old_mallocs, old_my_pes;

/* The deprecated routines are interposable too, forwarding through either
 * profiling spelling (spec §10 item 1): Sandia's `p` form and the literal
 * `pshmem_` one. */
void *shmalloc(size_t size)
{
    old_mallocs++;
    return pshmalloc(size);
}

int _my_pe(void)
{
    old_my_pes++;
    return pshmem__my_pe();
}

void *shmem_malloc(size_t size)
{
    mallocs++;
    return pshmem_malloc(size);
}

void shmem_putmem(void *dest, const void *source, size_t nelems, int pe)
{
    puts_made++;
    pshmem_putmem(dest, source, nelems, pe);
}

/*
 * The interposed AMO is the `ulong` spelling, not `uint64`, and that is worth
 * saying: on LP64 uint64_t *is* unsigned long, so the C11 generic selection -
 * which must name one association per distinct type - resolves
 * shmem_atomic_add on a uint64_t* to shmem_ulong_atomic_add. A profiler that
 * interposed only shmem_uint64_atomic_add would see the direct calls and miss
 * every generic one.
 */
void shmem_ulong_atomic_add(unsigned long *dest, unsigned long value, int pe)
{
    adds++;
    pshmem_ulong_atomic_add(dest, value, pe);
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    int right = (me + 1) % n;

    /* The application's definition runs, and forwarding works. */
    uint64_t *v = shmem_malloc(sizeof *v);
    CHECK_FATAL(v != NULL);  /* NULL would be dereferenced just below */
    CHECK(mallocs == 1);
    *v = 0;
    shmem_barrier_all();

    unsigned char *buf = shmem_malloc(8);
    CHECK_FATAL(buf != NULL);  /* NULL would be dereferenced just below */
    CHECK(mallocs == 2);
    memset(buf, 0, 8);
    shmem_barrier_all();

    unsigned char payload[8];
    for (int i = 0; i < 8; i++)
        payload[i] = (unsigned char)(i + 1);
    shmem_putmem(buf, payload, 8, right);
    CHECK(puts_made == 1);
    shmem_barrier_all();
    for (int i = 0; i < 8; i++)
        CHECK(buf[i] == (unsigned char)(i + 1));

    /* The C11 generic spelling expands to shmem_, so it is interposed too -
     * which it would not be if the macros named pshmem_. */
    unsigned long *w = shmem_malloc(sizeof *w);
    CHECK_FATAL(w != NULL);  /* NULL would be dereferenced just below */
    *w = 0;
    shmem_barrier_all();
    shmem_atomic_add(w, 1UL, me);
    CHECK(adds == 1);
    shmem_ulong_atomic_add(w, 1UL, me);
    CHECK(adds == 2);
    shmem_atomic_add(SHMEM_CTX_DEFAULT, w, 1UL, me);    /* the context arity */
    CHECK(adds == 2);                   /* ctx_ is a different routine */
    CHECK(*w == 3);
    shmem_free(w);

    /* The scalar variable is still usable through the ordinary path. */
    shmem_uint64_atomic_add(v, 2, me);
    CHECK(*v == 2);

    /*
     * The library's own call does not pass through. shmem_realloc with a null
     * pointer *is* shmem_malloc (spec §9.3.3), so the allocation happens - but
     * through pshmem_malloc, so the profiler's count does not move.
     */
    /* The old names: the application's definitions run and forward. */
    long *old = shmalloc(sizeof *old);
    CHECK_FATAL(old != NULL);
    CHECK(old_mallocs == 1);
    CHECK(_my_pe() == me && old_my_pes == 1);
    CHECK(p_my_pe() == me && pshmem__my_pe() == me && p_num_pes() == n &&
          pshmem__num_pes() == n);
    CHECK(old_my_pes == 1);                /* the p forms are not interposed */
    pshmem_shfree(old);

    int before = mallocs;
    uint64_t *grown = shmem_realloc(NULL, 4096);
    CHECK(grown != NULL);
    CHECK(mallocs == before);
    shmem_free(grown);

    /* And an operation that internally puts does not count as a put either:
     * put-with-signal issues its transfer at the boundary, not through the
     * application-facing routine. */
    int puts_before = puts_made;
    uint64_t *sig = shmem_malloc(sizeof *sig);
    CHECK_FATAL(sig != NULL);  /* NULL would be dereferenced just below */
    *sig = 0;
    shmem_barrier_all();
    shmem_putmem_signal(buf, payload, 8, sig, 1, SHMEM_SIGNAL_SET, right);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 1) == 1);
    CHECK(puts_made == puts_before);
    shmem_barrier_all();

    /* §10.1.1: the library makes no use of this and returns immediately. */
    shmem_pcontrol(0);
    shmem_pcontrol(1);
    shmem_pcontrol(3, 17, "ignored");
    pshmem_pcontrol(1);

    shmem_free(sig);
    shmem_free(buf);
    shmem_free(v);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
