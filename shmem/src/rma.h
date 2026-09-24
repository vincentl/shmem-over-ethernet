/*
 * The two transfers every RMA routine is built on (rma.c): shmem_putmem and
 * shmem_getmem's bodies, with the routine a diagnostic names and the element
 * width as parameters, so that the typed, sized, strided and nonblocking forms
 * (rma-typed.c) reuse the checks rather than repeat them, and a wrong PE or a
 * non-symmetric address is reported under the name the program called -
 * shmem_double_put, not the byte form beneath it.
 */
#ifndef OSOE_RMA_H
#define OSOE_RMA_H

#include <stddef.h>

#include <shmem.h>

/*
 * Put nelems elements of `width` bytes from source to dest on pe: returns at
 * local completion, as shmem_putmem (spec §9.6.1.1). nelems 0 is a no-op, and
 * a byte count past SIZE_MAX is fatal, naming `routine`. `flags` are the
 * transport's (transport.h): OSOE_TRANSPORT_MORE from a loop with more to
 * follow, otherwise 0.
 */
void osoe_rma_put(const char *routine, shmem_ctx_t ctx, void *dest, const void *source,
                  size_t nelems, size_t width, int pe, unsigned flags);

/* Get nelems elements of `width` bytes from source on pe into dest: dest holds
 * them on return, as shmem_getmem (spec §9.6.1.5). `flags` as for a put. */
void osoe_rma_get(const char *routine, shmem_ctx_t ctx, void *dest, const void *source,
                  size_t nelems, size_t width, int pe, unsigned flags);

/* Complete this PE's nonblocking gets and nothing else (§4.6): what an iget
 * ends with, naming `routine` wherever it waits. */
void osoe_rma_complete_gets(const char *routine, shmem_ctx_t ctx);

/* nelems elements of `width` bytes as a byte count, fatal naming `routine` if
 * a size_t cannot hold it. */
size_t osoe_rma_bytes(const char *routine, size_t nelems, size_t width);

#endif /* OSOE_RMA_H */
