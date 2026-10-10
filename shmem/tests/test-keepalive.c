/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Keepalive, as a job's nodes' addresses and OSOE_KEEPALIVE make it (§3.15;
 * the review of v0.2 step 4's merge point, correctness Q2-1; the author,
 * 2026-10-10).
 *
 * Where every node of a job has a loopback address, the lead leaves TCP
 * keepalive off on its node-pair connections unless OSOE_KEEPALIVE sets it:
 * such a job is in one network namespace, with no link between its nodes to
 * cut, so a probe can find nothing and can only be lost - and an overloaded
 * machine's receive backlog loses them, which ended healthy jobs of tens of
 * logical nodes as vanished hosts. Set, the setting holds; and nodes with
 * addresses of their own - a cluster's, tier 3's namespaces - keep it at its
 * defaults.
 *
 * Run on two nodes of one PE each, so that both PEs are leads: each reads
 * SO_KEEPALIVE on every connected TCP socket it has after shmem_init - node 0's
 * control and data wires toward node 1, which it connected, and node 1's, which
 * it accepted - and checks it against the mode it is given, `off` or `on`. PE 0
 * says the job's verdict. The silent-peer check, which keepalive's setting
 * turns off and on with it, says its own choice under SHMEM_DEBUG, which the
 * registrations read.
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
    CHECK_FATAL(shmem_n_pes() == 2);
    /* The PEs whose sockets were not as expected, counted at PE 0 - on the
     * heap, since this program's statics are not symmetric (not oshcc's). */
    int *failed_pes = shmem_malloc(sizeof *failed_pes);
    CHECK_FATAL(failed_pes != NULL);
    *failed_pes = 0;
    shmem_barrier_all();
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
            getpeername(fd, (struct sockaddr *)&peer, &peer_length) != 0 ||
            peer.ss_family != AF_INET)
            continue;
        length = sizeof keepalive;
        CHECK(getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, &length) == 0);
        sockets++;
        if ((keepalive != 0) != expected)
            wrong++;
    }
    closedir(d);
    printf("PE %d: %d connected TCP sockets, %d with keepalive %s\n", shmem_my_pe(), sockets,
           sockets - wrong, expected ? "on" : "off");
    CHECK(sockets >= 2);
    CHECK(wrong == 0);
    if (sockets < 2 || wrong != 0)
        shmem_int_atomic_inc(failed_pes, 0);
    shmem_barrier_all();
    if (shmem_my_pe() == 0 && shmem_int_atomic_fetch(failed_pes, 0) == 0)
        printf("KEEPALIVE %s OK\n", expected ? "ON" : "OFF");
    shmem_barrier_all();
    shmem_free(failed_pes);
    shmem_finalize();
    return CHECK_DONE();
}
