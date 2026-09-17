#include <stdlib.h>

#include "base/env.h"

/* Parse digits from *s up to a delimiter (NUL or ','); advance *s past them. */
static int parse_digits(const char **s, uint64_t max, uint64_t *out)
{
    const char *p = *s;
    uint64_t v = 0;
    if (*p < '0' || *p > '9')
        return -1;
    for (; *p >= '0' && *p <= '9'; p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (max - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    *s = p;
    *out = v;
    return 0;
}

int osoe_parse_u64(const char *s, uint64_t *out)
{
    uint64_t v;
    if (parse_digits(&s, UINT64_MAX, &v) != 0 || *s != '\0')
        return -1;
    *out = v;
    return 0;
}

int osoe_parse_u32(const char *s, uint32_t *out)
{
    uint64_t v;
    if (parse_digits(&s, UINT32_MAX, &v) != 0 || *s != '\0')
        return -1;
    *out = (uint32_t)v;
    return 0;
}

int osoe_parse_u32_list(const char *s, uint32_t *out, size_t cap, size_t *n)
{
    size_t count = 0;
    for (;;) {
        uint64_t v;
        if (count == cap || parse_digits(&s, UINT32_MAX, &v) != 0)
            return -1;
        out[count++] = (uint32_t)v;
        if (*s == '\0')
            break;
        if (*s != ',')
            return -1;
        s++;
    }
    *n = count;
    return 0;
}

enum osoe_env_status osoe_env_u64(const char *name, uint64_t *out)
{
    const char *s = getenv(name);
    if (!s)
        return OSOE_ENV_UNSET;
    return osoe_parse_u64(s, out) == 0 ? OSOE_ENV_OK : OSOE_ENV_INVALID;
}

enum osoe_env_status osoe_env_u32(const char *name, uint32_t *out)
{
    const char *s = getenv(name);
    if (!s)
        return OSOE_ENV_UNSET;
    return osoe_parse_u32(s, out) == 0 ? OSOE_ENV_OK : OSOE_ENV_INVALID;
}
