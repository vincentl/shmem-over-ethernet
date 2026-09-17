#include <stdbool.h>

#include "base/size.h"

enum { MAX_FRACTION_DIGITS = 18 };  /* 10^18 < 2^60: exact in uint64_t */

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

    /* The fraction as frac / scale, with scale = 10^digits kept. Digits past
     * the eighteenth only matter for the ceiling: remember whether any is
     * nonzero. */
    uint64_t frac = 0, scale = 1;
    bool sticky = false;
    if (*p == '.') {
        p++;
        unsigned kept = 0;
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
