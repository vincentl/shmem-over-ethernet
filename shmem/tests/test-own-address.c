/*
 * The rule by which a node listens somewhere other than its host list entry
 * (§6.2, listener.h): a name this host resolves to a loopback address - as
 * Debian's /etc/hosts maps a host's own name to 127.0.1.1 - in a job of several
 * nodes, which its peers could never reach there. An IP literal is taken as
 * given, since osoe-launch's logical nodes are 127.0.0.[1-N] on purpose.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>

#include "listener.h"

static int failures;

static void expect(const char *entry, const char *resolved, unsigned nodes, bool want)
{
    struct sockaddr_in a = { .sin_family = AF_INET };
    inet_pton(AF_INET, resolved, &a.sin_addr);
    bool got = osoe_listener_self_is_loopback(entry, &a, nodes);
    if (got != want) {
        printf("WRONG: %s resolving to %s in a job of %u nodes: %s, expected %s\n", entry,
               resolved, nodes, got ? "loopback" : "as given", want ? "loopback" : "as given");
        failures++;
    }
}

int main(void)
{
    expect("pi-001", "127.0.1.1", 4, true);     /* Debian's own-name mapping */
    expect("localhost", "127.0.0.1", 2, true);
    expect("pi-001", "10.0.0.11", 4, false);    /* a name that resolves as it should */
    expect("127.0.0.2", "127.0.0.2", 4, false); /* osoe-launch's logical nodes */
    expect("10.99.0.1", "10.99.0.1", 4, false); /* tier 3's namespaces */
    expect("pi-001", "127.0.1.1", 1, false);    /* one node: nobody to reach it */
    expect("127.1", "127.0.0.1", 2, true);      /* not a dotted quad: a name here */
    expect("10.0.1.300", "127.0.0.1", 2, true);  /* nor an address at all */
    expect("010.0.0.1", "127.0.0.1", 2, true);   /* inet_pton refuses a leading zero */
    expect("10.0.0.0", "127.0.0.1", 2, false);   /* but not a part that is zero */
    if (failures)
        return 1;
    printf("OWN ADDRESS OK\n");
    return 0;
}
