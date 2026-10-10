/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* v0.2 step 4 part 2: what a control wire costs the lead, per peer node. */
#define _GNU_SOURCE
#include <stdio.h>
#include "progress.h"
#include "wire.h"
#include "am.h"
int main(void)
{
    printf("progress_control=%zu wire=%zu parser=%zu progress_connection=%zu\n",
           sizeof(struct osoe_progress_control), sizeof(struct osoe_wire),
           sizeof(struct osoe_am_parser), sizeof(struct osoe_progress_connection));
    return 0;
}
