/*
 * The SHMEM_SYMMETRIC_SIZE grammar (spec §8, design §4.4).
 *
 *   size       := number [ multiplier [ anything ] ]
 *   number     := mantissa [ exponent ]
 *   mantissa   := digits [ "." [ digits ] ] | "." digits
 *   exponent   := ( "e" | "E" ) [ "+" | "-" ] digits
 *   multiplier := k K m M g G t T      (2^10, 2^20, 2^30, 2^40)
 *
 * The result is the integer ceiling of number times multiplier, computed
 * exactly in integer arithmetic ("3.1M" is 3250586, as the spec's example
 * says, and so is "3.1e6" for the same reason) - no binary floating point is
 * used anywhere, including for the exponent (conformance review F9 /
 * omissions 11). Characters after the multiplier are ignored, as the spec
 * requires ("20kk" is 20480). Rejected: an empty string, a sign on the
 * mantissa, leading or trailing space, anything other than an exponent or a
 * multiplier after the mantissa, and a value that does not fit in 64 bits.
 * Zero parses fine here (it is a size like any other); SHMEM_SYMMETRIC_SIZE
 * rejects it specifically, in bootstrap.c, because a heap of no bytes is
 * never useful (conformance review F9 / omissions 11).
 */
#ifndef OSOE_BASE_SIZE_H
#define OSOE_BASE_SIZE_H

#include <stdint.h>

/* 0 on success, -1 if s is invalid. */
int osoe_parse_size(const char *s, uint64_t *out);

#endif /* OSOE_BASE_SIZE_H */
