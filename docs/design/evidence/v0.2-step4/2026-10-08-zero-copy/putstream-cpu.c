/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* v0.2 step 4 part 0: a put stream from PE 0 to the first PE on another node,
 * with the CPU each node's agents spent over the stream's window, read from
 * /proc (no perf on the Pi) - for the zero-copy send on and off.
 * usage: putstream-cpu [BYTES [SECONDS]]
 * OSOE_PUTSTREAM_STAT, if set, is osoe-stat's path: PE 0 dumps its node's
 * counters (osoe-stat -d -j) to OSOE_PUTSTREAM_OUT.0 and .1 at the window's
 * start and end. */
#define _GNU_SOURCE
#include <dirent.h>
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Seconds of CPU the threads of this process named `name` have used. */
static double thread_cpu(const char *name)
{
    double ticks = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[64], line[1024];
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(line, sizeof line, f)) {
            char *open = strchr(line, '('), *close = strrchr(line, ')');
            if (open && close && (size_t)(close - open - 1) == strlen(name) &&
                strncmp(open + 1, name, strlen(name)) == 0) {
                /* After the comm: state, then fields 4 to 13, then utime, stime. */
                unsigned long utime = 0, stime = 0;
                char *p = close + 2;
                int field = 3;
                for (char *tok = strtok(p, " "); tok; tok = strtok(NULL, " "), field++) {
                    if (field == 14)
                        utime = strtoul(tok, NULL, 10);
                    if (field == 15) {
                        stime = strtoul(tok, NULL, 10);
                        break;
                    }
                }
                ticks += (double)(utime + stime);
            }
        }
        fclose(f);
    }
    closedir(d);
    return ticks / (double)sysconf(_SC_CLK_TCK);
}

static void dump(int which)
{
    const char *stat = getenv("OSOE_PUTSTREAM_STAT"), *out = getenv("OSOE_PUTSTREAM_OUT");
    if (!stat || !out)
        return;
    char command[2048];
    snprintf(command, sizeof command,
             "%s -d -j $(%s -l 2>/dev/null | awk '$1 ~ /osoe\\./ {print $1}' | grep -v tombstone) "
             "> %s.%d 2>&1", stat, stat, out, which);
    if (system(command) != 0)
        fprintf(stderr, "putstream-cpu: %s failed\n", command);
}

int main(int argc, char **argv)
{
    size_t bytes = argc > 1 ? (size_t)strtoull(argv[1], NULL, 0) : (size_t)1 << 20;
    double seconds = argc > 2 ? atof(argv[2]) : 20;
    size_t batch = ((size_t)64 << 20) / bytes ? ((size_t)64 << 20) / bytes : 1;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    unsigned char *window = shmem_malloc(bytes);
    unsigned char *source = malloc(bytes);
    memset(source, 1, bytes);
    /* The first PE on another node than PE 0's, as PE 0 sees it, told to all. */
    static int told = -1;
    int target = -1;
    if (me == 0) {
        for (int p = 1; p < npes && target < 0; p++)
            if (shmem_ptr(window, p) == NULL)
                target = p;
        for (int p = 1; p < npes; p++)
            shmem_int_p(&told, target, p);
        shmem_quiet();
    }
    shmem_barrier_all();
    if (me != 0)
        target = told;
    char host[64];
    gethostname(host, sizeof host);
    shmem_barrier_all();
    double agent0 = thread_cpu("osoe-agent"), start = now();
    if (me == 0 && target >= 0) {
        dump(0);
        size_t total = 0;
        double t0 = now();
        while (now() - t0 < seconds) {
            for (size_t i = 0; i < batch; i++)
                shmem_putmem(window, source, bytes, target);
            shmem_quiet();
            total += batch * bytes;
        }
        double t = now() - t0;
        dump(1);
        double agent = thread_cpu("osoe-agent") - agent0;
        printf("putstream-cpu sender host=%s bytes=%zu seconds=%.1f mib_per_s=%.1f "
               "agent_cpu_s=%.2f agent_cpu_s_per_gib=%.3f\n", host, bytes, t,
               (double)total / (1 << 20) / t, agent, agent / ((double)total / (1 << 30)));
        fflush(stdout);
    }
    shmem_barrier_all();
    if (me == target) {
        double agent = thread_cpu("osoe-agent") - agent0, t = now() - start;
        printf("putstream-cpu receiver host=%s seconds=%.1f agent_cpu_s=%.2f agent_busy=%.2f\n",
               host, t, agent, agent / t);
        fflush(stdout);
    }
    shmem_barrier_all();
    shmem_free(window);
    free(source);
    shmem_finalize();
    return 0;
}
