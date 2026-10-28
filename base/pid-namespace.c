/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <stdio.h>
#include <sys/stat.h>

#include "base/pid-namespace.h"

struct osoe_pid_namespace osoe_pid_namespace_own(void)
{
    struct stat st;
    if (stat("/proc/self/ns/pid", &st) != 0 || st.st_ino == 0)
        return (struct osoe_pid_namespace){ 0, 0 };
    return (struct osoe_pid_namespace){ (uint64_t)st.st_dev, (uint64_t)st.st_ino };
}

bool osoe_pid_namespace_known(struct osoe_pid_namespace n)
{
    return n.inode != 0;
}

bool osoe_pid_namespace_differ(struct osoe_pid_namespace a, struct osoe_pid_namespace b)
{
    return osoe_pid_namespace_known(a) && osoe_pid_namespace_known(b) &&
           (a.device != b.device || a.inode != b.inode);
}

void osoe_pid_namespace_text(struct osoe_pid_namespace n, char *buffer, size_t size)
{
    if (osoe_pid_namespace_known(n))
        snprintf(buffer, size, "pid:[%llu]", (unsigned long long)n.inode);
    else
        snprintf(buffer, size, "unknown");
}
