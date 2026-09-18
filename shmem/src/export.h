/*
 * Symbol export control and the profiling interface (§7.4; spec §10).
 *
 * The library is compiled with hidden visibility, so a definition is exported
 * only when marked OSOE_API. Mark public API definitions only; internal
 * functions stay hidden so they cannot collide with application symbols. The
 * shmem.exports test enforces this.
 *
 * **Every public routine is defined under its `pshmem_` name, and `shmem_` is a
 * weak alias to it.** That is the MPI convention and the reason for it is the
 * same: an application that defines its own `shmem_putmem` - a profiling
 * wrapper, a trace, a fault injector - replaces the weak one at link time and
 * calls `pshmem_putmem` to reach the real implementation. With the strong
 * definition on the `shmem_` name instead, static linking would be a duplicate
 * symbol and the interposition the spec requires would be impossible.
 *
 * The library's own internal calls go to the `pshmem_` name, so interposing
 * `shmem_putmem` does not also intercept the put that `shmem_putmem_signal`
 * makes on the program's behalf. That is the behaviour a profiler wants: it
 * counts what the application called, not what the library did about it.
 */
#ifndef OSOE_EXPORT_H
#define OSOE_EXPORT_H

#define OSOE_API __attribute__((visibility("default")))

/*
 * Give `shmem_<name>` to the definition of `pshmem_<name>`, which must already
 * be defined or declared. `__typeof__` keeps the signature in one place: a
 * repeated prototype here would be a second thing to get wrong, on 500
 * routines.
 */
#define OSOE_ALIAS(name)                                                      \
    extern __typeof__(pshmem_##name) shmem_##name                             \
        __attribute__((weak, alias("pshmem_" #name), visibility("default")));

#endif /* OSOE_EXPORT_H */
