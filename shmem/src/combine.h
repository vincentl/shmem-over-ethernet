/*
 * The reductions' combine functions (§5.5), one per operation and element
 * (internode.h), for the two places a reduction combines: the lead's intranode
 * phase (collectives.c) and the generic internode phase (internode-generic.c).
 *
 * **Per element, not per C type.** The spec's Table 10 names some two dozen
 * types, and they are thirteen elements - int and int32_t are one, and so are
 * long, long long, ptrdiff_t and int64_t on LP64 - so each arithmetic is
 * written once, and two types that are the same element cannot be reduced
 * differently.
 *
 * **Integer sum and product wrap**, computed through uintmax_t and converted
 * back: a signed overflow is undefined behaviour in C, where the unsigned
 * arithmetic is defined and the conversion back is modular on every target osoe
 * builds for, so a program that overflows a reduction gets the two's-complement
 * result on every PE rather than a sanitizer's report or an optimiser's
 * surprise. Floating and complex arithmetic is the type's own. Maximum and
 * minimum are the plain comparisons.
 *
 * Complexity: O(count) per call.
 */
#ifndef OSOE_COMBINE_H
#define OSOE_COMBINE_H

#include <stddef.h>

#include "internode.h"

/* accumulator[i] = accumulator[i] (operation) in[i], for every i < count. */
typedef void (*osoe_combine_function)(void *accumulator, const void *in, size_t count);

/*
 * The function for `operation` over `element`, or NULL where the pair is not
 * one of Table 10's - a bitwise operation on a floating element, an ordering
 * on a complex one - or the element is not one this platform has.
 */
osoe_combine_function osoe_combine_lookup(enum osoe_reduction_operation operation,
                                          struct osoe_element element);

#endif /* OSOE_COMBINE_H */
