/*
 * deny-futex2 PROGRAM [ARGS]... - run PROGRAM as it would run on a kernel older
 * than Linux 6.7: futex_wake (454), futex_wait (455) and futex_requeue (456)
 * return ENOSYS. A seccomp filter, inherited across fork and exec, so it can
 * wrap osoe-launch. Used by the portability review to see what libosoe does on
 * a kernel that lacks futex2: refusal at init, or something later?
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 454, 0, 2),
        BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 456, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { sizeof filter / sizeof filter[0], filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
        perror("seccomp");
        return 2;
    }
    execvp(argv[1], argv + 1);
    perror(argv[1]);
    return 2;
}
