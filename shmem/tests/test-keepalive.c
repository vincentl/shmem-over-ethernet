/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Keepalive on a job of one machine's logical nodes (§3.15; the review of v0.2
 * step 4's merge point, correctness Q2-1; the author, 2026-10-10).
 *
 * Where every node of a job has a loopback address, the lead leaves TCP
 * keepalive off on its node-pair connections unless OSOE_KEEPALIVE sets it:
 * the machine's one kernel closes a dead node's connections and answers for a
 * stopped one, so a probe can only be lost - and an overloaded machine's
 * loopback backlog loses them, which ended healthy jobs of tens of logical
 * nodes as vanished hosts. Set, the setting holds, as it does between machines.
 *
 * PE 0, node 0's lead, the process that holds its node's wires, reads
 * SO_KEEPALIVE on every connected TCP socket it has after shmem_init - its
 * control wire and data wire toward node 1 - and checks it against the mode it
 * is given: `off` or `on`.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"

int main(int argc, char **argv)
{
    CHECK_FATAL(argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0));
    int expected = strcmp(argv[1], "on") == 0;
    shmem_init();
    if (shmem_my_pe() == 0) {
        int sockets = 0, wrong = 0;
        DIR *d = opendir("/proc/self/fd");
        CHECK_FATAL(d != NULL);
        for (struct dirent *e; (e = readdir(d)) != NULL;) {
            int fd = atoi(e->d_name);
            if (e->d_name[0] == '.' || fd == dirfd(d))
                continue;
            int type = 0, keepalive = 0;
            socklen_t length = sizeof type;
            struct sockaddr_storage peer;
            socklen_t peer_length = sizeof peer;
            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_STREAM ||
                getpeername(fd, (struct sockaddr *)&peer, &peer_length) != 0)
                continue;
            length = sizeof keepalive;
            CHECK(getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, &length) == 0);
            sockets++;
            if ((keepalive != 0) != expected)
                wrong++;
        }
        closedir(d);
        printf("PE 0: %d connected sockets, %d with keepalive %s\n", sockets,
               sockets - wrong, expected ? "on" : "off");
        CHECK(sockets >= 2);
        CHECK(wrong == 0);
        if (sockets >= 2 && wrong == 0)
            printf("KEEPALIVE %s OK\n", expected ? "ON" : "OFF");
    }
    shmem_barrier_all();
    shmem_finalize();
    return CHECK_DONE();
}
