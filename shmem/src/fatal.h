/*
 * Loud failure (§3.15). The library's error path is to print what went wrong,
 * naming the PE and, once it exists, the control segment an investigator
 * points osoe-stat at (§3.14), and to terminate the process.
 */
#ifndef OSOE_FATAL_H
#define OSOE_FATAL_H

_Noreturn void osoe_fatal(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* OSOE_FATAL_H */
