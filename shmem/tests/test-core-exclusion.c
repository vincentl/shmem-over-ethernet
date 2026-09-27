/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * What a core file holds (review round two, omissions 4): this PE's own heap,
 * and not its siblings' heaps or the node's connection area.
 *
 * Every PE of a node maps every local heap and the whole connection area, so
 * without MADV_DONTDUMP a core from each of L PEs held L heaps and the node's
 * traffic: L times the node's memory, written once per crashing PE. The kernel
 * reports the setting per mapping as the `dd` flag in /proc/self/smaps, which
 * is the property itself rather than a core file's size, and needs no
 * core_pattern.
 *
 * Run on two nodes of two PEs, so that there is a sibling heap and a
 * connection area to look at.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"

/* Does this smaps VmFlags line carry the two-letter flag `flag`? */
static bool has_flag(const char *line, const char *flag)
{
    for (const char *p = line + strlen("VmFlags:"); (p = strstr(p, flag)) != NULL; p += 2)
        if (p[-1] == ' ' && (p[2] == ' ' || p[2] == '\n' || p[2] == '\0'))
            return true;
    return false;
}

int main(void)
{
    shmem_init();
    const char *local = getenv("OSOE_LOCALID");
    CHECK_FATAL(local != NULL);
    unsigned me = (unsigned)atoi(local);

    FILE *f = fopen("/proc/self/smaps", "r");
    CHECK_FATAL(f != NULL);
    char line[1024];
    char path[512] = "";
    unsigned own = 0, siblings = 0, areas = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmFlags:", 8) != 0) {
            /* A mapping's first line: "start-end perms offset dev inode path". */
            char *slash = strstr(line, "/dev/shm/osoe.");
            if (strchr(line, '-') && strchr(line, '-') < strchr(line, ' '))
                snprintf(path, sizeof path, "%s", slash ? slash : "");
            continue;
        }
        if (!path[0])
            continue;
        bool dd = has_flag(line, "dd");
        const char *heap = strstr(path, ".heap.");
        if (heap) {
            if ((unsigned)atoi(heap + strlen(".heap.")) == me) {
                own++;
                if (dd)
                    fprintf(stderr, "own heap excluded from the core: %s", path);
                CHECK(!dd);
            } else {
                siblings++;
                if (!dd)
                    fprintf(stderr, "sibling heap in the core: %s", path);
                CHECK(dd);
            }
        } else if (strstr(path, ".conn.")) {
            areas++;
            if (!dd)
                fprintf(stderr, "connection area in the core: %s", path);
            CHECK(dd);
        }
        path[0] = '\0';
    }
    fclose(f);
    /* Something to have looked at: an empty result would pass everything. */
    CHECK(own == 1);
    CHECK(siblings == 1);
    CHECK(areas == 1);
    if (own != 1 || siblings != 1 || areas != 1)
        fprintf(stderr, "mappings seen: own heap %u, sibling heaps %u, connection areas %u\n",
                own, siblings, areas);

    shmem_finalize();
    return CHECK_DONE();
}
