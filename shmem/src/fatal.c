#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "fatal.h"
#include "state.h"

_Noreturn void osoe_fatal(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (osoe.boot.have_rank)
        fprintf(stderr, "osoe: PE %u: %s\n", osoe.boot.rank, msg);
    else
        fprintf(stderr, "osoe: %s\n", msg);
    if (osoe.node.control_name[0])
        fprintf(stderr, "osoe: PE %u: control segment /dev/shm%s\n", osoe.boot.rank,
                osoe.node.control_name);
    exit(EXIT_FAILURE);
}
