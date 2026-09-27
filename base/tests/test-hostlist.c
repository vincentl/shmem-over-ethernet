/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Slurm's compressed host lists (§6.2): the parser §6.2 asked for with tier-1
 * tests, since it would otherwise first be debugged on a cluster. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "base/hostlist.h"
#include "check.h"

/* `list` names exactly the hosts in `expected`, space-separated, in order. */
static void expect(const char *list, const char *expected)
{
    uint32_t count = 0;
    char why[128];
    if (!osoe_hostlist_count(list, &count, why, sizeof why)) {
        fprintf(stderr, "'%s' refused: %s\n", list, why);
        CHECK(0);
        return;
    }
    char want[512];
    snprintf(want, sizeof want, "%s", expected);
    uint32_t i = 0;
    for (char *name = strtok(want, " "); name; name = strtok(NULL, " "), i++) {
        char got[64];
        if (!osoe_hostlist_entry(list, i, got, sizeof got) || strcmp(got, name) != 0) {
            fprintf(stderr, "'%s' entry %u: got '%s', expected '%s'\n", list, i,
                    osoe_hostlist_entry(list, i, got, sizeof got) ? got : "(none)", name);
            CHECK(0);
        }
    }
    CHECK(count == i);
    char past[64];
    CHECK(!osoe_hostlist_entry(list, i, past, sizeof past));     /* one past the end */
}

static void refuse(const char *list)
{
    uint32_t count;
    char why[128] = "";
    CHECK(!osoe_hostlist_count(list, &count, why, sizeof why));
    CHECK(why[0] != '\0');
    char out[64];
    CHECK(!osoe_hostlist_entry(list, 0, out, sizeof out) || strchr(list, ',') != NULL);
}

int main(void)
{
    expect("pi01", "pi01");
    expect("pi[01-04,07]", "pi01 pi02 pi03 pi04 pi07");
    expect("pi[1-3],login,x[08-10]", "pi1 pi2 pi3 login x08 x09 x10");
    expect("pi-[001-004]", "pi-001 pi-002 pi-003 pi-004");     /* the author's cluster */
    expect("rack[1-2]-a", "rack1-a rack2-a");
    expect("n[9-11]", "n9 n10 n11");                  /* width follows the lower bound */
    expect("127.0.0.[1-4]", "127.0.0.1 127.0.0.2 127.0.0.3 127.0.0.4");  /* osoe-launch's */
    expect("a,b[3],c", "a b3 c");

    refuse("");
    refuse("pi[01-04");
    refuse("pi01-04]");
    refuse("pi[4-2]");
    refuse("pi[a-b]");
    refuse("pi[1-2]x[3-4]");                          /* multi-dimensional: refused by name */
    refuse("a,,b");
    refuse("pi[1,]");

    /* A name that does not fit is not returned truncated. */
    char tiny[4];
    CHECK(!osoe_hostlist_entry("longname", 0, tiny, sizeof tiny));
    return CHECK_DONE();
}
