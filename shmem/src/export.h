/*
 * Symbol export control. The library is compiled with hidden visibility, so a
 * definition is exported only when marked OSOE_API. Mark public API
 * definitions only; internal functions stay hidden so they cannot collide with
 * application symbols. The shmem.exports test enforces this.
 */
#ifndef OSOE_EXPORT_H
#define OSOE_EXPORT_H

#define OSOE_API __attribute__((visibility("default")))

#endif /* OSOE_EXPORT_H */
