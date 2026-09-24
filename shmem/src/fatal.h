/*
 * Loud failure (§3.15). The library's error path is to print what went wrong,
 * naming the PE and, once it exists, the control segment an investigator
 * points osoe-stat at (§3.14), and to terminate the process.
 */
#ifndef OSOE_FATAL_H
#define OSOE_FATAL_H

_Noreturn void osoe_fatal(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/*
 * osoe_fatal with a last act before the abort: the message is written, then
 * `publish(context)` runs - a rendezvous's `failed` word - and then the
 * process leaves as osoe_fatal does.
 *
 * The message first, always. Anything published lets another process end the
 * job: its PEs stop, the launcher sees a failure and sends the rest SIGTERM. A
 * PE that published before writing could be killed with its reason still
 * unwritten, and the job then said only that it "failed on another node" -
 * seen once in 30 runs of shmem.job.duplicate-rank under load.
 */
_Noreturn void osoe_fatal_then(void (*publish)(void *), void *context, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* OSOE_FATAL_H */
