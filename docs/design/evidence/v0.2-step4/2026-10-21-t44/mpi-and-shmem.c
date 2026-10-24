/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * T44 (docs/prompts/v0.2-open-items.md): MPI and osoe in one program.
 *
 *   mpi-and-shmem MODE [INIT [FINALIZE]]
 *
 * INIT names the libraries to start and the order to start them in, "mpi,shmem"
 * (the default), "shmem,mpi", or one alone; FINALIZE the order they end in, by
 * default INIT's reverse. The environment: T44_ROUNDS, rounds of `check` (20);
 * T44_SECONDS, how long `hold` and `benign-wait` last (10 and 12);
 * T44_THREADS=multiple, MPI_Init_thread and shmem_init_thread at their
 * highest level; T44_EARLY=mpi or shmem, a library ended before the modes
 * below block, as `deadlock-osoe` does; T44_WAIT=mpi, what the other PEs wait
 * in while PE 1 ends the job (`crash` and the three after it): shmem_barrier_all
 * by default.
 *
 * MODE:
 *   check          both libraries' collectives and transfers, alternated, on
 *                  private memory, the symmetric heap and a static
 *   hold           print this process's threads and the signals that are
 *                  handled, hold for T44_SECONDS (so that `ss` can list the
 *                  sockets), finalize
 *   benign-wait    PE 0 waits in shmem, PE 1 computes and then releases it,
 *                  every other PE is blocked in MPI_Barrier meanwhile: no
 *                  deadlock, for the detector to leave alone
 *   deadlock-osoe  PE 0 waits in shmem for a flag nobody sets, every other
 *                  PE in shmem_barrier_all: osoe's own deadlock
 *   deadlock-mixed PE 0 blocks in MPI_Barrier, every other PE in
 *                  shmem_barrier_all: a deadlock across the two libraries
 *   sessions       shmem finalized and initialized again, three times, with MPI
 *                  working between and inside the sessions
 *   window         MPI one-sided put and get on a window over the symmetric heap,
 *                  beside shmem puts into the same memory
 *   fork           system(), and children that leave by exit and _exit
 *   crash          PE 1 raises SIGSEGV; the others wait
 *   mpi-abort      PE 1 calls MPI_Abort(3); the others wait
 *   global-exit    PE 1 calls shmem_global_exit(3); the others wait
 *   plain-exit     PE 1 calls exit(0) before either library is finalized; the
 *                  others wait
 *
 * Built with `OSOE_CC=mpicc oshcc` so that osoe's header comes first and its
 * linker script gathers the statics. The statics below are the program's own.
 * Built with plain oshcc and -DT44_WITHOUT_MPI it is the baseline, osoe alone
 * with no Open MPI in the process (without-mpi.h).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef T44_WITHOUT_MPI
#include "without-mpi.h"        /* the baseline: the same program, no Open MPI linked */
#else
#include <mpi.h>
#endif
#include <shmem.h>

enum { BIG = 1 << 17 };                 /* doubles: 1 MiB */

static double static_big[BIG];          /* the program's statics, symmetric under oshcc */
static int static_small[4];
static int release_flag;
static long long shared_counter;

static int *main_argc_pointer;
static char ***main_argv_pointer;
static bool mpi_up, shmem_up;
static int mpi_rank = -1, mpi_size = -1, pe = -1, npes = -1;
static int failures;
static const char *who = "-";

static void say(const char *format, ...)
{
    char line[512];
    int len = snprintf(line, sizeof line, "PE %s: ", who);
    va_list arguments;
    va_start(arguments, format);
    len += vsnprintf(line + len, sizeof line - (size_t)len - 1, format, arguments);
    va_end(arguments);
    if (len > (int)sizeof line - 2)
        len = (int)sizeof line - 2;
    line[len++] = '\n';
    ssize_t ignored = write(STDOUT_FILENO, line, (size_t)len);
    (void)ignored;
}

#define CHECK(condition) check((condition), #condition, __LINE__)
static void check(bool condition, const char *text, int line)
{
    if (!condition) {
        say("WRONG at line %d: %s", line, text);
        failures++;
    }
}

static void start_one(const char *name)
{
    bool multiple = getenv("T44_THREADS") && !strcmp(getenv("T44_THREADS"), "multiple");
    if (!strcmp(name, "mpi")) {
        int provided = -1;
        if (multiple)
            MPI_Init_thread(main_argc_pointer, main_argv_pointer, MPI_THREAD_MULTIPLE, &provided);
        else
            MPI_Init(main_argc_pointer, main_argv_pointer);
        MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
        MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
        mpi_up = true;
        say("MPI started: rank %d of %d%s", mpi_rank, mpi_size,
            multiple ? (provided == MPI_THREAD_MULTIPLE ? ", THREAD_MULTIPLE" : ", not MULTIPLE")
                     : "");
    } else if (!strcmp(name, "shmem")) {
        int provided = -1;
        if (multiple)
            shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided);
        else
            shmem_init();
        pe = shmem_my_pe();
        npes = shmem_n_pes();
        shmem_up = true;
        say("shmem started: PE %d of %d%s", pe, npes,
            multiple ? (provided == SHMEM_THREAD_MULTIPLE ? ", THREAD_MULTIPLE" : ", not MULTIPLE")
                     : "");
    } else {
        fprintf(stderr, "mpi-and-shmem: no library '%s'\n", name);
        exit(2);
    }
}

static void end_one(const char *name)
{
    if (!strcmp(name, "mpi") && mpi_up) {
        MPI_Finalize();
        mpi_up = false;
        say("MPI finalized");
    } else if (!strcmp(name, "shmem") && shmem_up) {
        shmem_finalize();
        shmem_up = false;
        say("shmem finalized");
    }
}

static void each(const char *list, void (*act)(const char *))
{
    char copy[64], *save;
    snprintf(copy, sizeof copy, "%s", list);
    for (char *name = strtok_r(copy, ",", &save); name; name = strtok_r(NULL, ",", &save))
        act(name);
}

/* The reverse of a comma list of two. */
static const char *reverse_of(const char *list)
{
    return !strcmp(list, "mpi,shmem") ? "shmem,mpi" : !strcmp(list, "shmem,mpi") ? "mpi,shmem" : list;
}

static void *buffer(size_t bytes)
{
    return shmem_up ? shmem_malloc(bytes) : malloc(bytes);
}

static void release(void *pointer)
{
    if (shmem_up)
        shmem_free(pointer);
    else
        free(pointer);
}

/* ---- check ----------------------------------------------------------------- */

static void check_mpi_alone(double *heap_big)
{
    int want = mpi_size * (mpi_size + 1) / 2;
    int mine[4], sum[4] = { 0 };
    for (int i = 0; i < 4; i++)
        mine[i] = mpi_rank + 1;
    MPI_Allreduce(mine, sum, 4, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    CHECK(sum[0] == want && sum[3] == want);

    int *heap = buffer(4 * sizeof *heap);
    for (int i = 0; i < 4; i++)
        heap[i] = mpi_rank + 1;
    MPI_Allreduce(MPI_IN_PLACE, heap, 4, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    CHECK(heap[0] == want && heap[3] == want);
    release(heap);

    for (int i = 0; i < 4; i++)
        static_small[i] = mpi_rank + 1;
    MPI_Allreduce(MPI_IN_PLACE, static_small, 4, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    CHECK(static_small[0] == want && static_small[3] == want);

    /* A megabyte each, from the heap and from a static: past any transport's
     * eager limit. */
    for (int i = 0; i < BIG; i++)
        heap_big[i] = mpi_rank + 1;
    MPI_Allreduce(MPI_IN_PLACE, heap_big, BIG, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    CHECK(heap_big[0] == want && heap_big[BIG - 1] == want);
    for (int i = 0; i < BIG; i++)
        static_big[i] = mpi_rank + 1;
    MPI_Allreduce(MPI_IN_PLACE, static_big, BIG, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    CHECK(static_big[0] == want && static_big[BIG - 1] == want);

    /* A ring of sends, heap to heap and static to heap. */
    double *incoming = buffer(sizeof *incoming * BIG);
    int next = (mpi_rank + 1) % mpi_size, previous = (mpi_rank + mpi_size - 1) % mpi_size;
    for (int i = 0; i < BIG; i++)
        heap_big[i] = mpi_rank;
    MPI_Sendrecv(heap_big, BIG, MPI_DOUBLE, next, 7, incoming, BIG, MPI_DOUBLE, previous, 7,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    CHECK(incoming[0] == previous && incoming[BIG - 1] == previous);
    for (int i = 0; i < BIG; i++)
        static_big[i] = mpi_rank + 100;
    MPI_Sendrecv(static_big, BIG, MPI_DOUBLE, next, 8, incoming, BIG, MPI_DOUBLE, previous, 8,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    CHECK(incoming[0] == previous + 100 && incoming[BIG - 1] == previous + 100);
    release(incoming);
}

static void check_shmem_alone(int round)
{
    int want = npes * (npes + 1) / 2;
    int *source = shmem_malloc(4 * sizeof *source), *target = shmem_malloc(4 * sizeof *target);
    int *slot = shmem_malloc(sizeof *slot);
    for (int i = 0; i < 4; i++)
        source[i] = pe + 1;
    *slot = -1;
    shmem_barrier_all();
    shmem_int_sum_reduce(SHMEM_TEAM_WORLD, target, source, 4);
    CHECK(target[0] == want && target[3] == want);
    shmem_int_put(slot, &pe, 1, (pe + 1) % npes);
    shmem_barrier_all();
    CHECK(*slot == (pe + npes - 1) % npes);
    shmem_longlong_atomic_add(&shared_counter, pe + 1, 0);
    shmem_barrier_all();
    CHECK(shmem_longlong_g(&shared_counter, 0) == (long long)want * (round + 1));
    shmem_barrier_all();
    shmem_free(slot);
    shmem_free(target);
    shmem_free(source);
}

static void mode_check(void)
{
    bool both = mpi_up && shmem_up;
    if (both && mpi_size == npes)
        CHECK(mpi_rank == pe);
    else if (both)
        say("MPI is a world of %d and shmem a job of %d: not one job, ranks not compared",
            mpi_size, npes);
    int rounds = getenv("T44_ROUNDS") ? atoi(getenv("T44_ROUNDS")) : 20;
    double *heap_big = buffer(sizeof *heap_big * BIG);
    for (int round = 0; round < rounds; round++) {
        if (mpi_up) {
            check_mpi_alone(heap_big);
            MPI_Barrier(MPI_COMM_WORLD);
        }
        if (shmem_up) {
            check_shmem_alone(round);
            if (both) {
                /* The libraries' waits alternate: an MPI barrier between a
                 * shmem barrier and a put, another between the put and the
                 * barrier that completes it. The shmem barriers carry the
                 * order - the MPI world of a singleton is one rank. */
                int *slot = shmem_malloc(sizeof *slot);
                *slot = -1;
                shmem_barrier_all();
                MPI_Barrier(MPI_COMM_WORLD);
                shmem_int_p(slot, pe, (pe + 1) % npes);
                shmem_quiet();
                MPI_Barrier(MPI_COMM_WORLD);
                shmem_barrier_all();
                CHECK(*slot == (pe + npes - 1) % npes);
                shmem_barrier_all();
                shmem_free(slot);
            }
        }
    }
    release(heap_big);
    say("check: %d rounds, %s", rounds, failures ? "WRONG" : "ok");
}

/* ---- hold ------------------------------------------------------------------ */

static void print_threads(void)
{
    DIR *tasks = opendir("/proc/self/task");
    int count = 0;
    for (struct dirent *entry; tasks && (entry = readdir(tasks));) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        char path[320], name[64] = "?", state = '?', text[512];
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", entry->d_name);
        FILE *file = fopen(path, "r");
        if (file) {
            if (fgets(name, sizeof name, file))
                name[strcspn(name, "\n")] = '\0';
            fclose(file);
        }
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", entry->d_name);
        file = fopen(path, "r");
        if (file) {
            size_t length = fread(text, 1, sizeof text - 1, file);
            fclose(file);
            text[length] = '\0';
            const char *paren = strrchr(text, ')');
            if (paren && paren[1] == ' ')
                state = paren[2];
        }
        say("thread %s %s %c", entry->d_name, name, state);
        count++;
    }
    if (tasks)
        closedir(tasks);
    say("%d threads", count);
}

static void print_signals(void)
{
    static const struct { int number; const char *name; } names[] = {
        { SIGHUP, "SIGHUP" }, { SIGINT, "SIGINT" }, { SIGQUIT, "SIGQUIT" }, { SIGILL, "SIGILL" },
        { SIGTRAP, "SIGTRAP" }, { SIGABRT, "SIGABRT" }, { SIGBUS, "SIGBUS" }, { SIGFPE, "SIGFPE" },
        { SIGUSR1, "SIGUSR1" }, { SIGSEGV, "SIGSEGV" }, { SIGUSR2, "SIGUSR2" },
        { SIGPIPE, "SIGPIPE" }, { SIGALRM, "SIGALRM" }, { SIGTERM, "SIGTERM" },
        { SIGCHLD, "SIGCHLD" }, { SIGCONT, "SIGCONT" }, { SIGTSTP, "SIGTSTP" },
        { SIGTTIN, "SIGTTIN" }, { SIGTTOU, "SIGTTOU" }, { SIGURG, "SIGURG" },
        { SIGXCPU, "SIGXCPU" }, { SIGXFSZ, "SIGXFSZ" }, { SIGVTALRM, "SIGVTALRM" },
        { SIGPROF, "SIGPROF" }, { SIGWINCH, "SIGWINCH" }, { SIGIO, "SIGIO" }, { SIGSYS, "SIGSYS" },
    };
    int changed = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        struct sigaction action;
        if (sigaction(names[i].number, NULL, &action) != 0)
            continue;
        if (!(action.sa_flags & SA_SIGINFO) && action.sa_handler == SIG_DFL)
            continue;
        changed++;
        if (!(action.sa_flags & SA_SIGINFO) && action.sa_handler == SIG_IGN) {
            say("signal %s is ignored", names[i].name);
            continue;
        }
        void *handler = (action.sa_flags & SA_SIGINFO) ? (void *)action.sa_sigaction
                                                        : (void *)action.sa_handler;
        Dl_info where;
        const char *library = "?", *symbol = "?";
        if (dladdr(handler, &where)) {
            library = where.dli_fname ? strrchr(where.dli_fname, '/') ? strrchr(where.dli_fname, '/') + 1
                                                                       : where.dli_fname : "?";
            symbol = where.dli_sname ? where.dli_sname : "?";
        }
        say("signal %s is handled in %s by %s%s", names[i].name, library, symbol,
            (action.sa_flags & SA_RESETHAND) ? " (once)" : "");
    }
    say("%d signals not at their default", changed);
}

static void mode_hold(void)
{
    print_threads();
    print_signals();
    int seconds = getenv("T44_SECONDS") ? atoi(getenv("T44_SECONDS")) : 10;
    sleep((unsigned)seconds);
    if (shmem_up)
        shmem_barrier_all();
    if (mpi_up)
        MPI_Barrier(MPI_COMM_WORLD);
    print_threads();
}

/* ---- the waits ------------------------------------------------------------- */

static void mode_benign_wait(void)
{
    int seconds = getenv("T44_SECONDS") ? atoi(getenv("T44_SECONDS")) : 12;
    if (pe == 0) {
        say("waiting in shmem for PE 1");
        shmem_int_wait_until(&release_flag, SHMEM_CMP_EQ, 1);
        say("released");
    } else if (pe == 1) {
        say("computing for %d seconds", seconds);
        sleep((unsigned)seconds);
        shmem_int_p(&release_flag, 1, 0);
        shmem_quiet();
    } else {
        say("blocked in MPI_Barrier");
    }
    MPI_Barrier(MPI_COMM_WORLD);
    say("passed the MPI barrier");
}

static void wait_the_others_way(void)
{
    if (getenv("T44_WAIT") && !strcmp(getenv("T44_WAIT"), "mpi"))
        MPI_Barrier(MPI_COMM_WORLD);
    else
        shmem_barrier_all();
}

static void mode_deadlock_osoe(void)
{
    if (pe == 0) {
        say("waiting in shmem for a flag nobody sets");
        shmem_int_wait_until(&release_flag, SHMEM_CMP_EQ, 1);
    } else {
        say("in shmem_barrier_all");
        shmem_barrier_all();
    }
    say("not reached");
}

static void mode_deadlock_mixed(void)
{
    if (pe == 0) {
        say("blocking in MPI_Barrier");
        MPI_Barrier(MPI_COMM_WORLD);
    } else {
        say("in shmem_barrier_all");
        shmem_barrier_all();
    }
    say("not reached");
}

static void mode_sessions(void)
{
    if (!(mpi_up && shmem_up)) {
        say("sessions needs both libraries");
        failures++;
        return;
    }
    double *heap_big = NULL;
    for (int session = 0; session < 3; session++) {
        /* Each session ends with the program's statics given back to it, as
         * its own memory, and the next maps them again. */
        shmem_finalize();
        MPI_Barrier(MPI_COMM_WORLD);
        shmem_init();
        pe = shmem_my_pe();
        npes = shmem_n_pes();
        heap_big = buffer(sizeof *heap_big * BIG);
        check_mpi_alone(heap_big);
        release(heap_big);
        shared_counter = 0;
        check_shmem_alone(0);
        MPI_Barrier(MPI_COMM_WORLD);
        shmem_barrier_all();
    }
    say("check: 3 rounds, %s", failures ? "WRONG" : "ok");
}

static void mode_window(void)
{
    if (!(mpi_up && shmem_up) || mpi_size != npes) {
        say("window needs both libraries in one job");
        failures++;
        return;
    }
    enum { SLOTS = 8 };
    int *memory = shmem_malloc(SLOTS * sizeof *memory);
    for (int i = 0; i < SLOTS; i++)
        memory[i] = -1;
    shmem_barrier_all();
    MPI_Win window;
    MPI_Win_create(memory, SLOTS * sizeof *memory, sizeof *memory, MPI_INFO_NULL, MPI_COMM_WORLD,
                   &window);
    int next = (mpi_rank + 1) % mpi_size, previous = (mpi_rank + mpi_size - 1) % mpi_size;
    for (int round = 0; round < 10; round++) {
        MPI_Win_fence(0, window);
        MPI_Put(&mpi_rank, 1, MPI_INT, next, 0, 1, MPI_INT, window);     /* slot 0 by MPI */
        shmem_int_p(&memory[1], pe, next);                               /* slot 1 by shmem */
        MPI_Win_fence(0, window);
        shmem_barrier_all();
        CHECK(memory[0] == previous);
        CHECK(memory[1] == previous);
        int got = -2;
        MPI_Get(&got, 1, MPI_INT, next, 1, 1, MPI_INT, window);          /* what shmem put there */
        MPI_Win_fence(0, window);
        CHECK(got == mpi_rank);
        memory[0] = memory[1] = -1;
        shmem_barrier_all();
    }
    MPI_Win_free(&window);
    shmem_free(memory);
    say("window: %s", failures ? "WRONG" : "ok");
}

static void mode_fork(void)
{
    shared_counter = 1;
    int status = system("exit 0");
    CHECK(status == 0);
    say("system(): status %d", status);
    pid_t child = fork();
    if (child == 0) {
        shared_counter = 42;
        exit(0);                        /* the handlers of both libraries run in the child */
    }
    waitpid(child, &status, 0);
    say("child that left by exit(0): %s %d; it wrote shared_counter = 42, the parent reads %lld",
        WIFEXITED(status) ? "exited" : "killed by signal", WIFEXITED(status) ? WEXITSTATUS(status)
                                                                              : WTERMSIG(status),
        shared_counter);
    child = fork();
    if (child == 0)
        _exit(0);
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (shmem_up && mpi_up) {
        MPI_Barrier(MPI_COMM_WORLD);
        shmem_barrier_all();
    }
    say("fork: %s", failures ? "WRONG" : "ok");
}

static void mode_ends_the_job(const char *mode)
{
    if (pe != 1) {
        say("waiting");
        wait_the_others_way();
        say("not reached");
        return;
    }
    sleep(2);                           /* the others are waiting by now */
    if (!strcmp(mode, "crash")) {
        say("raising SIGSEGV");
        raise(SIGSEGV);
    } else if (!strcmp(mode, "mpi-abort")) {
        say("MPI_Abort(3)");
        MPI_Abort(MPI_COMM_WORLD, 3);
    } else if (!strcmp(mode, "global-exit")) {
        say("shmem_global_exit(3)");
        shmem_global_exit(3);
    } else {
        say("exit(0) with neither library finalized");
        exit(0);
    }
}

int main(int argc, char **argv)
{
    main_argc_pointer = &argc;
    main_argv_pointer = &argv;
    const char *mode = argc > 1 ? argv[1] : "check";
    const char *init = argc > 2 ? argv[2] : "mpi,shmem";
    const char *finalize = argc > 3 ? argv[3] : reverse_of(init);
    if (getenv("OSOE_PROCID"))
        who = getenv("OSOE_PROCID");
    else if (getenv("SLURM_PROCID"))
        who = getenv("SLURM_PROCID");
    else if (getenv("OMPI_COMM_WORLD_RANK"))
        who = getenv("OMPI_COMM_WORLD_RANK");

    each(init, start_one);
    if (getenv("T44_EARLY"))
        each(getenv("T44_EARLY"), end_one);

    if (!strcmp(mode, "check"))
        mode_check();
    else if (!strcmp(mode, "hold"))
        mode_hold();
    else if (!strcmp(mode, "benign-wait"))
        mode_benign_wait();
    else if (!strcmp(mode, "deadlock-osoe"))
        mode_deadlock_osoe();
    else if (!strcmp(mode, "deadlock-mixed"))
        mode_deadlock_mixed();
    else if (!strcmp(mode, "sessions"))
        mode_sessions();
    else if (!strcmp(mode, "window"))
        mode_window();
    else if (!strcmp(mode, "fork"))
        mode_fork();
    else if (!strcmp(mode, "crash") || !strcmp(mode, "mpi-abort") ||
             !strcmp(mode, "global-exit") || !strcmp(mode, "plain-exit"))
        mode_ends_the_job(mode);
    else {
        fprintf(stderr, "mpi-and-shmem: no mode '%s'\n", mode);
        return 2;
    }

    each(finalize, end_one);
    return failures ? 1 : 0;
}
