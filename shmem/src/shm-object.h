/*
 * Creating and opening the job's named shared-memory objects (§6.2).
 *
 * Every name is removed during init (§3.14), so the one window in which a job
 * can leave a name behind is init itself: a job killed there strands whatever
 * it had created, and the next job with the same ids used to die on EEXIST,
 * with a message blaming a crash whether or not there had been one (second
 * review, D11 and D15).
 *
 * So a creator that meets an existing object looks at it before refusing. If it
 * belongs to this user and its recorded creator is gone - or never got as far
 * as recording itself - it is a leftover, and is reclaimed. Otherwise the fatal
 * says whose it is. And no process ever opens an object another user owns: the
 * size was the only thing checked before, and a file another user pre-created
 * under the job's name would have been mapped and trusted.
 */
#ifndef OSOE_SHM_OBJECT_H
#define OSOE_SHM_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An object with nowhere to record its creator (a heap: all application
 * memory). Such a leftover is reclaimed whenever it is this user's. */
#define OSOE_SHM_NO_OWNER (-1L)

/*
 * shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600), reclaiming a leftover as
 * above. `owner_offset` is where in the object the creator stores its pid, as
 * an int32_t, or OSOE_SHM_NO_OWNER. Returns the descriptor, or -1 with a reason
 * in `err`. The caller records its own pid at `owner_offset` once it has
 * sized and mapped the object.
 */
int osoe_shm_create(const char *name, long owner_offset, char *err, size_t errlen);

/*
 * shm_open(name, O_RDWR), refusing an object another user owns. Returns the
 * descriptor; -1 with errno ENOENT and an empty `err` when there is no such
 * object yet, which the rendezvous waits on; -1 with a reason in `err`
 * otherwise.
 */
int osoe_shm_open_ours(const char *name, char *err, size_t errlen);

/*
 * Has the object this descriptor refers to lost its name? An opener that found
 * a leftover before its creator reclaimed it holds an object nobody will ever
 * initialize, and waits on its `ready` forever. The creator reclaims by
 * removing the name, so the leftover's link count drops to 0 - and an object
 * that is really being created cannot lose its name while an opener is still
 * waiting for `ready`, since its name goes only once every opener has counted
 * itself (§3.14). So a waiter checks this at each sweep, and starts over when
 * it is true.
 */
bool osoe_shm_orphaned(int fd);

/*
 * Is there room in /dev/shm for `wanted` more bytes (§6.2, /dev/shm capacity)?
 * Sets `*free_bytes` to what statvfs says is free. True as well when that
 * cannot be known: statvfs failed, or the filesystem reports no size at all.
 *
 * Advisory, and only that: another job can take the space a moment later. What
 * it prevents is the common case, a job asking for more than the machine has,
 * which otherwise starts, runs, and dies of SIGBUS in whichever process first
 * touches a page tmpfs cannot supply - often a node lead applying someone
 * else's put, far from the cause.
 */
bool osoe_shm_room(uint64_t wanted, uint64_t *free_bytes);

#endif /* OSOE_SHM_OBJECT_H */
