/* What a real code does: system(), popen(), and a fork() child that leaves with
 * exit() rather than _exit() - so atexit handlers and stdio run in the child. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <shmem.h>
int main(int argc, char **argv)
{
    if (argc > 1) start_pes(0); else shmem_init();   /* argv[1]: use start_pes, which registers atexit */
    int me = shmem_my_pe();
    long *x = shmem_malloc(sizeof(long));
    *x = 7;
    int rc = system("true");
    FILE *p = popen("echo hi", "r"); char buf[8] = {0}; if (p) { fgets(buf, sizeof buf, p); pclose(p); }
    pid_t c = fork();
    if (c == 0) { *x = 8; exit(0); }                  /* child touches the heap, then exit() */
    int st; waitpid(c, &st, 0);
    shmem_barrier_all();
    long v = 0; shmem_getmem(&v, x, sizeof v, me);
    printf("PE %d: system=%d popen=%s child=%d heap after child store=%ld\n", me, rc, buf[0] ? "ok" : "fail", WEXITSTATUS(st), v);
    shmem_free(x);
    if (argc <= 1) shmem_finalize();
    return 0;
}
