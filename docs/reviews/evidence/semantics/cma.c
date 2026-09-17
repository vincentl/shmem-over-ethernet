#define _GNU_SOURCE
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
/* Parent forks sibling B (holds private buf) and sibling A (tries to write into B). */
int main(void){
  int p[2]; pipe(p);
  static char buf[16] = "untouched";         /* same VA in both children (fork) */
  pid_t b = fork();
  if (b == 0){ close(p[1]); char c; read(p[0], &c, 1); printf("B sees: %s\n", buf); fflush(stdout); _exit(0); }
  pid_t a = fork();
  if (a == 0){
    char msg[16] = "written-by-A";
    struct iovec l = {msg, 13}, r = {buf, 13};
    ssize_t n = process_vm_writev(b, &l, 1, &r, 1, 0);
    printf("A process_vm_writev -> %zd (%s)\n", n, n < 0 ? strerror(errno) : "ok"); fflush(stdout);
    _exit(0);
  }
  waitpid(a, 0, 0); write(p[1], "x", 1); waitpid(b, 0, 0); return 0;
}
