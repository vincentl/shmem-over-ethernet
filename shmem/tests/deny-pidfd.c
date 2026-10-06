/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * deny-pidfd PROGRAM [ARGS]... - run PROGRAM in a sandbox where pidfd_open
 * fails with ENOSYS, as on a kernel before Linux 5.3, which has none: the
 * single-node build's floor is 4.17, and below 5.3 every job watches its PEs
 * by pid instead (liveness.c, open_watch). A seccomp filter, inherited across
 * fork and exec, so it can wrap osoe-launch, as deny-futex.c does for the
 * futex calls. The fallback had no test; the v0.2 step 1 review's correctness
 * K5 found it working this way, and this keeps it so.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/* pidfd_open's number for headers older than 5.3: 434 on every architecture
 * of the generic table and on x86-64, as liveness.c has it. */
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: deny-pidfd PROGRAM [ARGS]...\n");
        return 2;
    }
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pidfd_open, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = { sizeof filter / sizeof filter[0], filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
        perror("deny-pidfd: seccomp");
        return 2;
    }
    execvp(argv[1], argv + 1);
    perror(argv[1]);
    return 2;
}
