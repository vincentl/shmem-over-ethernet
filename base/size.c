/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <stdbool.h>

#include "base/size.h"

enum { MAX_FRACTION_DIGITS = 18 };  /* 10^18 < 2^60: exact in uint64_t */

/* Multiply the exact (whole, frac/scale) pair by 10, keeping 0 <= frac <
 * scale. frac < scale <= 10^18, so frac*10 < 10^19, comfortably below
 * UINT64_MAX (~1.8*10^19): the multiply cannot overflow, only the carry into
 * `whole` can. Used to walk a positive decimal exponent one digit at a time
 * so overflow is caught exactly where it would occur, the same way the
 * leading digits above are. */
static int shift_up(uint64_t *whole, uint64_t *frac, uint64_t scale)
{
    uint64_t f = *frac * 10;
    uint64_t carry = f / scale;
    if (*whole > (UINT64_MAX - carry) / 10)
        return -1;
    *whole = *whole * 10 + carry;
    *frac = f % scale;
    return 0;
}

int osoe_parse_size(const char *s, uint64_t *out)
{
    const char *p = s;
    uint64_t whole = 0;
    bool any_digit = false;

    for (; *p >= '0' && *p <= '9'; p++) {
        unsigned d = (unsigned)(*p - '0');
        if (whole > (UINT64_MAX - d) / 10)
            return -1;
        whole = whole * 10 + d;
        any_digit = true;
    }

    /* The fraction as frac / scale, with scale = 10^kept. Digits past the
     * eighteenth only matter for the ceiling: remember whether any is
     * nonzero. `kept` is also how much room is left for a negative exponent
     * below to move more digits in. */
    uint64_t frac = 0, scale = 1;
    bool sticky = false;
    unsigned kept = 0;
    if (*p == '.') {
        p++;
        for (; *p >= '0' && *p <= '9'; p++) {
            any_digit = true;
            if (kept < MAX_FRACTION_DIGITS) {
                frac = frac * 10 + (unsigned)(*p - '0');
                scale *= 10;
                kept++;
            } else if (*p != '0') {
                sticky = true;
            }
        }
    }
    if (!any_digit)
        return -1;

    /*
     * A decimal exponent (spec §8 says only "floating point value"; "1e3" is
     * the reading of that phrase a C programmer expects, and every
     * strtod-based implementation accepts it - conformance review F9 /
     * omissions 11). It is folded into the exact (whole, frac/scale) pair
     * one digit at a time by the same shift used above, rather than through
     * any binary floating point, so the result stays the integer ceiling
     * base/size.h promises no matter how the value was spelled.
     *
     * A positive exponent multiplies by 10 per digit (shift_up); a negative
     * one divides by moving a digit out of `whole` and into the fraction.
     * Past MAX_FRACTION_DIGITS kept, a further digit moved in that way can no
     * longer be represented exactly - it only ever makes the value inexact,
     * which only ever rounds the final ceiling up, so it is folded into
     * `sticky` instead of lost. Either loop is capped at 25 digits: a 64-bit
     * `whole` has at most 20 decimal digits, so 25 shifts either overflow (a
     * positive exponent) or fully drain it (a negative one) with room to
     * spare, and once that happens every further shift of the same sign is a
     * no-op - `whole` and `frac` are already 0, or shift_up would already
     * have returned -1.
     */
    if (*p == 'e' || *p == 'E') {
        p++;
        bool negative = false;
        if (*p == '+' || *p == '-') {
            negative = *p == '-';
            p++;
        }
        if (*p < '0' || *p > '9')
            return -1;
        uint32_t exponent = 0;
        for (; *p >= '0' && *p <= '9'; p++) {
            if (exponent < 1000000)   /* saturate: far past MAX_SHIFTS already */
                exponent = exponent * 10 + (unsigned)(*p - '0');
        }
        enum { MAX_SHIFTS = 25 };
        uint32_t shifts = exponent < MAX_SHIFTS ? exponent : MAX_SHIFTS;
        if (negative) {
            for (uint32_t i = 0; i < shifts; i++) {
                unsigned digit = (unsigned)(whole % 10);
                whole /= 10;
                if (kept < MAX_FRACTION_DIGITS) {
                    frac += digit * scale;   /* < digit*scale + scale <= new scale */
                    scale *= 10;
                    kept++;
                } else if (digit != 0) {
                    sticky = true;
                }
            }
        } else {
            for (uint32_t i = 0; i < shifts; i++) {
                if (shift_up(&whole, &frac, scale) != 0)
                    return -1;
            }
        }
    }

    unsigned shift = 0;
    switch (*p) {
    case '\0': break;
    case 'k': case 'K': shift = 10; break;
    case 'm': case 'M': shift = 20; break;
    case 'g': case 'G': shift = 30; break;
    case 't': case 'T': shift = 40; break;
    default: return -1;
    }

    if (whole > (UINT64_MAX >> shift))
        return -1;
    uint64_t result = whole << shift;

    /* floor(frac * 2^shift / scale), by long division one bit at a time;
     * rem < scale <= 10^18 throughout, so rem * 2 cannot overflow. */
    uint64_t q = 0, rem = frac;
    for (unsigned i = 0; i < shift; i++) {
        rem <<= 1;
        q <<= 1;
        if (rem >= scale) {
            rem -= scale;
            q |= 1;
        }
    }
    bool inexact = rem != 0 || sticky;
    uint64_t add = q + (inexact ? 1 : 0);
    if (result > UINT64_MAX - add)
        return -1;
    *out = result + add;
    return 0;
}
