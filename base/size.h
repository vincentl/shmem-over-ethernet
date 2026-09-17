/*
 * The SHMEM_SYMMETRIC_SIZE grammar (spec §8, design §4.4).
 *
 *   size   := number [ multiplier [ anything ] ]
 *   number := digits [ "." [ digits ] ] | "." digits
 *   multiplier := k K m M g G t T      (2^10, 2^20, 2^30, 2^40)
 *
 * The result is the integer ceiling of number times multiplier, computed
 * exactly in integer arithmetic ("3.1M" is 3250586, as the spec's example
 * says). Characters after the multiplier are ignored, as the spec requires
 * ("20kk" is 20480). Rejected: an empty string, a sign, leading or trailing
 * space, an exponent ("1e3"), and anything other than a multiplier after the
 * number. The spec says only "floating point value"; decimal notation is the
 * reading that cannot surprise, and an exponent is an error rather than a
 * silent misparse. A value that does not fit in 64 bits is rejected.
 */
#ifndef OSOE_BASE_SIZE_H
#define OSOE_BASE_SIZE_H

#include <stdint.h>

/* 0 on success, -1 if s is invalid. */
int osoe_parse_size(const char *s, uint64_t *out);

#endif /* OSOE_BASE_SIZE_H */
