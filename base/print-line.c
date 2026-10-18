/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "base/print-line.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void osoe_print_line(const char *fmt, ...)
{
    int saved_errno = errno;
    char small[1024];
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    char *text = small;
    if (n >= (int)sizeof small) {
        char *large = malloc((size_t)n + 1);
        if (large && vsnprintf(large, (size_t)n + 1, fmt, again) == n) {
            text = large;
        } else {
            free(large);
            n = (int)sizeof small - 1;          /* the line cut, rather than lost */
        }
    }
    va_end(again);
    for (size_t done = 0; n > 0 && done < (size_t)n;) {
        ssize_t written = write(STDERR_FILENO, text + done, (size_t)n - done);
        if (written > 0)
            done += (size_t)written;
        else if (written < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    if (text != small)
        free(text);
    errno = saved_errno;
}
