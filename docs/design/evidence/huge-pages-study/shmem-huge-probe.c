/* Does madvise(MADV_HUGEPAGE) give a /dev/shm object huge pages under the
 * current settings? Creates a 256 MiB object, maps it (a) at an address the
 * kernel chooses and (b) at a 2 MiB-aligned address, advises, touches every
 * page, and reports ShmemHugePages from /proc/meminfo and the mapping's
 * ShmemPmdMapped and the process's VmPTE from /proc/self. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static long meminfo(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, key, strlen(key)) == 0) { v = atol(line + strlen(key) + 1); break; }
    if (f) fclose(f);
    return v;
}

static long status(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, key, strlen(key)) == 0) { v = atol(line + strlen(key) + 1); break; }
    if (f) fclose(f);
    return v;
}

/* ShmemPmdMapped of the smaps entry that holds address p. */
static long pmd_mapped(void *p)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    char line[512];
    int inside = 0;
    long v = -1;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2)
            inside = (uintptr_t)p >= lo && (uintptr_t)p < hi;
        else if (inside && strncmp(line, "ShmemPmdMapped:", 15) == 0) { v = atol(line + 15); break; }
    }
    if (f) fclose(f);
    return v;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    size_t bytes = (size_t)256 << 20;
    int advise = argc > 1 && strcmp(argv[1], "advise") == 0;
    int memfd = argc > 2 && strcmp(argv[2], "memfd") == 0;
    const char *name = "/huge-probe";
    shm_unlink(name);
    int fd = memfd ? memfd_create("huge-probe", 0) : shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) != 0) { perror("shm"); return 1; }
    printf("%s object: ", memfd ? "memfd" : "/dev/shm");
    long shmem_before = meminfo("ShmemHugePages:");
    long pte_before = status("VmPTE:");

    /* (a) wherever the kernel puts it. */
    void *a = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    /* (b) at a 2 MiB boundary inside an over-sized reservation. */
    size_t huge = (size_t)2 << 20;
    char *reserve = mmap(NULL, bytes + huge, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *aligned = (char *)(((uintptr_t)reserve + huge - 1) & ~(uintptr_t)(huge - 1));
    void *b = mmap(aligned, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) { perror("mmap"); return 1; }
    if (advise) {
        if (madvise(a, bytes, MADV_HUGEPAGE) != 0) perror("madvise a");
        if (madvise(b, bytes, MADV_HUGEPAGE) != 0) perror("madvise b");
    }
    printf("mapping a at %p (%s 2 MiB), b at %p (aligned)\n", a,
           ((uintptr_t)a & (huge - 1)) ? "not on" : "on", b);

    double t0 = now();
    for (size_t i = 0; i < bytes; i += 4096)
        ((volatile char *)b)[i] = 1;            /* first touch through b, the aligned one */
    double t1 = now();
    for (size_t i = 0; i < bytes; i += 4096)
        ((volatile char *)a)[i] = 2;            /* then through a: the same pages, mapped again */
    double t2 = now();

    printf("advise=%d ShmemHugePages %ld -> %ld kB; ShmemPmdMapped a=%ld kB b=%ld kB; "
           "VmPTE %ld -> %ld kB; touch b %.3f s, a %.3f s\n",
           advise, shmem_before, meminfo("ShmemHugePages:"), pmd_mapped(a), pmd_mapped(b),
           pte_before, status("VmPTE:"), t1 - t0, t2 - t1);
    shm_unlink(name);
    return 0;
}
