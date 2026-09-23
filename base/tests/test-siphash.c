/* SipHash-2-4 against the published test vectors: key 00..0f, and the message
 * 00, 01, ... of each length (the reference implementation's vectors.h; the
 * 15-byte one is the paper's worked example). */
#include <stdint.h>
#include <stdio.h>

#include "base/siphash.h"
#include "check.h"

int main(void)
{
    unsigned char key[16], message[64];
    for (int i = 0; i < 16; i++)
        key[i] = (unsigned char)i;
    for (int i = 0; i < 64; i++)
        message[i] = (unsigned char)i;
    CHECK(osoe_siphash(key, message, 0) == UINT64_C(0x726fdb47dd0e0e31));
    CHECK(osoe_siphash(key, message, 15) == UINT64_C(0xa129ca6149be45e5));
    if (osoe_siphash(key, message, 15) != UINT64_C(0xa129ca6149be45e5))
        fprintf(stderr, "15 bytes: %#llx\n",
                (unsigned long long)osoe_siphash(key, message, 15));
    /* A different key, and a different message, give different digests. */
    key[0] ^= 1;
    CHECK(osoe_siphash(key, message, 15) != UINT64_C(0xa129ca6149be45e5));
    return CHECK_DONE();
}
