#include "base/siphash.h"

static uint64_t rotate(uint64_t x, int b)
{
    return (x << b) | (x >> (64 - b));
}

/* Little-endian, whatever the host is: the algorithm's byte order. */
static uint64_t load(const unsigned char *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

#define ROUND                                                                   \
    do {                                                                        \
        v0 += v1; v1 = rotate(v1, 13); v1 ^= v0; v0 = rotate(v0, 32);          \
        v2 += v3; v3 = rotate(v3, 16); v3 ^= v2;                               \
        v0 += v3; v3 = rotate(v3, 21); v3 ^= v0;                               \
        v2 += v1; v1 = rotate(v1, 17); v1 ^= v2; v2 = rotate(v2, 32);          \
    } while (0)

uint64_t osoe_siphash(const unsigned char key[16], const void *data, size_t len)
{
    const unsigned char *m = data;
    uint64_t k0 = load(key, 8), k1 = load(key + 8, 8);
    uint64_t v0 = k0 ^ UINT64_C(0x736f6d6570736575);
    uint64_t v1 = k1 ^ UINT64_C(0x646f72616e646f6d);
    uint64_t v2 = k0 ^ UINT64_C(0x6c7967656e657261);
    uint64_t v3 = k1 ^ UINT64_C(0x7465646279746573);
    size_t full = len - len % 8;
    for (size_t i = 0; i < full; i += 8) {
        uint64_t b = load(m + i, 8);
        v3 ^= b;
        ROUND;
        ROUND;
        v0 ^= b;
    }
    uint64_t last = ((uint64_t)len << 56) | load(m + full, len % 8);
    v3 ^= last;
    ROUND;
    ROUND;
    v0 ^= last;
    v2 ^= 0xff;
    ROUND;
    ROUND;
    ROUND;
    ROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
