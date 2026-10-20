/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Prints the byte at which a control segment's header records its creator's
 * pid, for leftovers.sh: to plant a control segment whose creator is running,
 * the script stores its own pid there. The number comes from the library's own
 * node.h, so that the script holds no copy of it to drift. The job tables'
 * offset, which the script does hold, is pinned by a static assertion where
 * the header is declared (job.c), osoe-stat reading it too; the control
 * segment's has no such pin, being behind the whole segment directory.
 */
#include <stddef.h>
#include <stdio.h>

#include "node.h"

int main(void)
{
    printf("%zu\n", offsetof(struct osoe_node_header, creator_pid));
    return 0;
}
