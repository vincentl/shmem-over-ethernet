/* SHMEM_SYMMETRIC_SIZE grammar (spec §8, design §4.4). */
#include <stdint.h>

#include "base/size.h"
#include "check.h"

static void ok(const char *s, uint64_t want)
{
    uint64_t v = 0;
    int r = osoe_parse_size(s, &v);
    if (r != 0 || v != want)
        fprintf(stderr, "parse_size(\"%s\"): r=%d v=%llu, want %llu\n", s, r,
                (unsigned long long)v, (unsigned long long)want);
    CHECK(r == 0 && v == want);
}

static void bad(const char *s)
{
    uint64_t v = 12345;
    int r = osoe_parse_size(s, &v);
    if (r != -1)
        fprintf(stderr, "parse_size(\"%s\") accepted: %llu\n", s, (unsigned long long)v);
    CHECK(r == -1 && v == 12345);
}

int main(void)
{
    /* The spec's own examples. */
    ok("20m", 20971520);
    ok("3.1M", 3250586);
    ok("20kk", 20480);
    ok(".5m", 524288);
    ok("0.5m", 524288);

    ok("0", 0);
    ok("1", 1);
    ok("4096", 4096);
    ok("1k", 1024);
    ok("1K", 1024);
    ok("1g", UINT64_C(1) << 30);
    ok("1G", UINT64_C(1) << 30);
    ok("2t", UINT64_C(2) << 40);
    ok("2T", UINT64_C(2) << 40);
    ok("5.", 5);
    ok("0.1", 1);                   /* ceiling */
    ok("1.5", 2);
    ok("2.0", 2);
    ok("1.000000000000000000001", 2);   /* a nonzero digit past the 18th */
    ok("1.0000000000000000000000k", 1024);
    ok("0.001k", 2);                /* 1.024 -> 2 */
    ok("1mjunk", 1048576);          /* anything after the multiplier is ignored */
    ok("18446744073709551615", UINT64_MAX);
    ok("16777215t", (UINT64_C(16777215) << 40));
    ok("16777215.99999999t", UINT64_C(18446744073709540621));   /* just under 2^64 */

    bad("");
    bad(".");
    bad("k");
    bad("-1");
    bad("+1");
    bad(" 1");
    bad("1 ");
    bad("1x");
    bad("1e3");
    bad("0x10");
    bad("1.2.3");
    bad("18446744073709551616");
    bad("16777216t");               /* 2^64 */
    bad("16777215.9999999999999999999t");   /* ceiling reaches 2^64 */
    return CHECK_DONE();
}
