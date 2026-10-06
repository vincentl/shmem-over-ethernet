/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * deny-futex PROGRAM [ARGS]... - run PROGRAM in a sandbox that denies the
 * classic futex(2) call, which fails with EPERM, as a container's seccomp
 * profile that leaves futex out would make it. A seccomp filter, inherited
 * across fork and exec, so it can wrap osoe-launch, as deny-futex2.c does for
 * the futex2 calls.
 *
 * The single-node build's counterpart of deny-futex2 (v0.2 step 1): that
 * build waits with the classic futex (base/futex-classic.c), which every
 * kernel since 2.6.25 has, so the one way to be refused it is a sandbox, and
 * shmem_init's start-up check must say so by name, before any object exists.
 * A process that never contends a lock makes no futex call of its own before
 * that check, which is what lets a program start under this filter at all.
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: deny-futex PROGRAM [ARGS]...\n");
        return 2;
    }
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_futex, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = { sizeof filter / sizeof filter[0], filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
        perror("deny-futex: seccomp");
        return 2;
    }
    execvp(argv[1], argv + 1);
    perror(argv[1]);
    return 2;
}
