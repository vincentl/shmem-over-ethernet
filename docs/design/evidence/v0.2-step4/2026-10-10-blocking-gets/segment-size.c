/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* The size of this node's control segment: PE 0 prints the length of its
 * mapping of the object named osoe.<job>.<step>.<node>.<session>, as
 * /proc/self/maps shows it - the name itself is removed from /dev/shm during
 * shmem_init, so a listing of /dev/shm finds nothing (the first version of
 * this probe, 2026-10-10). The other objects' names say heap, job or conn. */
#define _GNU_SOURCE
#include <regex.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

int main(void)
{
    shmem_init();
    shmem_barrier_all();
    if (shmem_my_pe() == 0) {
        regex_t segment;
        regcomp(&segment, "/dev/shm/osoe\\.[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+( \\(deleted\\))?$",
                REG_EXTENDED | REG_NOSUB);
        FILE *maps = fopen("/proc/self/maps", "r");
        char line[1024];
        unsigned long long total = 0;
        while (maps && fgets(line, sizeof line, maps)) {
            line[strcspn(line, "\n")] = 0;
            unsigned long long start, end;
            if (regexec(&segment, line, 0, NULL, 0) == 0 &&
                sscanf(line, "%llx-%llx", &start, &end) == 2)
                total += end - start;
        }
        if (maps)
            fclose(maps);
        printf("segment bytes %llu\n", total);
    }
    shmem_barrier_all();
    shmem_finalize();
    return 0;
}
