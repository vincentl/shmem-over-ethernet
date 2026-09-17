/* OSOE_ environment parsing (§6.2). */
#include <stdint.h>
#include <stdlib.h>

#include "base/env.h"
#include "check.h"

int main(void)
{
    uint32_t v32;
    uint64_t v64;

    CHECK(osoe_parse_u32("0", &v32) == 0 && v32 == 0);
    CHECK(osoe_parse_u32("4294967295", &v32) == 0 && v32 == UINT32_MAX);
    CHECK(osoe_parse_u32("4294967296", &v32) == -1);
    CHECK(osoe_parse_u32("", &v32) == -1);
    CHECK(osoe_parse_u32("-1", &v32) == -1);
    CHECK(osoe_parse_u32("1 ", &v32) == -1);
    CHECK(osoe_parse_u32("0x1", &v32) == -1);
    CHECK(osoe_parse_u64("18446744073709551615", &v64) == 0 && v64 == UINT64_MAX);
    CHECK(osoe_parse_u64("18446744073709551616", &v64) == -1);

    uint32_t list[4];
    size_t n = 0;
    CHECK(osoe_parse_u32_list("7", list, 4, &n) == 0 && n == 1 && list[0] == 7);
    CHECK(osoe_parse_u32_list("0,1,5,3", list, 4, &n) == 0 && n == 4 &&
          list[0] == 0 && list[1] == 1 && list[2] == 5 && list[3] == 3);
    CHECK(osoe_parse_u32_list("0,1,2,3,4", list, 4, &n) == -1);    /* over cap */
    CHECK(osoe_parse_u32_list("", list, 4, &n) == -1);
    CHECK(osoe_parse_u32_list("1,", list, 4, &n) == -1);
    CHECK(osoe_parse_u32_list(",1", list, 4, &n) == -1);
    CHECK(osoe_parse_u32_list("1,,2", list, 4, &n) == -1);
    CHECK(osoe_parse_u32_list("1;2", list, 4, &n) == -1);

    unsetenv("OSOE_TEST_VAR");
    CHECK(osoe_env_u32("OSOE_TEST_VAR", &v32) == OSOE_ENV_UNSET);
    setenv("OSOE_TEST_VAR", "42", 1);
    CHECK(osoe_env_u32("OSOE_TEST_VAR", &v32) == OSOE_ENV_OK && v32 == 42);
    CHECK(osoe_env_u64("OSOE_TEST_VAR", &v64) == OSOE_ENV_OK && v64 == 42);
    setenv("OSOE_TEST_VAR", "4x", 1);
    CHECK(osoe_env_u32("OSOE_TEST_VAR", &v32) == OSOE_ENV_INVALID);
    setenv("OSOE_TEST_VAR", "", 1);
    CHECK(osoe_env_u32("OSOE_TEST_VAR", &v32) == OSOE_ENV_INVALID);
    return CHECK_DONE();
}
