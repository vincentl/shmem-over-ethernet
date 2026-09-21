#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "shm-object.h"

/* Whose is this object, and is it ours? */
static int check_owner(int fd, const char *name, char *err, size_t errlen)
{
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, errlen, "cannot stat /dev/shm%s: %s", name, strerror(errno));
        return -1;
    }
    if (st.st_uid != geteuid()) {
        snprintf(err, errlen, "/dev/shm%s belongs to uid %u, not this user (uid %u); "
                 "another user's object has this job's name, and it is not opened",
                 name, (unsigned)st.st_uid, (unsigned)geteuid());
        return -1;
    }
    return 0;
}

int osoe_shm_create(const char *name, long owner_offset, char *err, size_t errlen)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST) {
            snprintf(err, errlen, "cannot create /dev/shm%s: %s", name, strerror(errno));
            return -1;
        }

        int old = shm_open(name, O_RDONLY, 0);
        if (old < 0) {
            if (errno == ENOENT)
                continue;               /* removed meanwhile: try again */
            snprintf(err, errlen, "/dev/shm%s already exists and cannot be examined: %s",
                     name, strerror(errno));
            return -1;
        }
        if (check_owner(old, name, err, errlen) != 0) {
            close(old);
            return -1;
        }
        int32_t pid = 0;
        if (owner_offset != OSOE_SHM_NO_OWNER &&
            pread(old, &pid, sizeof pid, (off_t)owner_offset) != (ssize_t)sizeof pid)
            pid = 0;                    /* shorter than the header: never initialized */
        close(old);
        if (pid > 0 && !(kill((pid_t)pid, 0) != 0 && errno == ESRCH)) {
            snprintf(err, errlen, "/dev/shm%s already exists, and its creator, pid %d, is "
                     "still running: is another job using these job and step ids?", name,
                     (int)pid);
            return -1;
        }
        /* A leftover of a job that died during init: its creator is gone, or
         * never recorded itself. This user's, so this user's to remove. */
        if (shm_unlink(name) != 0 && errno != ENOENT) {
            snprintf(err, errlen, "cannot reclaim /dev/shm%s, left by pid %d: %s", name,
                     (int)pid, strerror(errno));
            return -1;
        }
        fprintf(stderr, "osoe: reclaimed /dev/shm%s, left behind by %s\n", name,
                pid > 0 ? "a process that is gone" : "a job that died while creating it");
    }
    snprintf(err, errlen, "cannot create /dev/shm%s: it keeps reappearing", name);
    return -1;
}

int osoe_shm_open_ours(const char *name, char *err, size_t errlen)
{
    err[0] = '\0';
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        if (errno != ENOENT)
            snprintf(err, errlen, "cannot open /dev/shm%s: %s", name, strerror(errno));
        return -1;
    }
    if (check_owner(fd, name, err, errlen) != 0) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

bool osoe_shm_orphaned(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_nlink == 0;
}

bool osoe_shm_room(uint64_t wanted, uint64_t *free_bytes)
{
    struct statvfs sv;
    *free_bytes = 0;
    if (statvfs("/dev/shm", &sv) != 0 || sv.f_blocks == 0)
        return true;
    *free_bytes = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
    return wanted <= *free_bytes;
}
