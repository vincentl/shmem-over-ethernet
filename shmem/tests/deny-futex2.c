/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * deny-futex2 [--shared] PROGRAM [ARGS]... - run PROGRAM as it would run on a
 * kernel older than Linux 6.7, where futex_wake (454), futex_wait (455) and
 * futex_requeue (456) fail with ENOSYS; with --shared, in a sandbox that lets
 * futex_wake and futex_wait through with FUTEX2_PRIVATE in their flags (their
 * fourth argument) and refuses the shared scope with EPERM. A seccomp filter,
 * inherited across fork and exec, so it can wrap osoe-launch (§1's start-up
 * check; review round two, portability F3, whose deny-futex2.c this follows).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

enum { FUTEX2_PRIVATE_FLAG = 128 };     /* FUTEX2_PRIVATE, which older headers lack */

int main(int argc, char **argv)
{
    int shared_only = argc > 1 && strcmp(argv[1], "--shared") == 0;
    if (argc < 2 + shared_only) {
        fprintf(stderr, "usage: deny-futex2 [--shared] PROGRAM [ARGS]...\n");
        return 2;
    }
    struct sock_filter every[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 454, 0, 2),
        BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 456, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    /* futex_wake and futex_wait take their flags fourth, args[3], whose low 32
     * bits - the first word, on a little-endian machine - the kernel reads. */
    struct sock_filter shared[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 454, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 455, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[3])),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, FUTEX2_PRIVATE_FLAG, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = { sizeof every / sizeof every[0], every };
    if (shared_only)
        program = (struct sock_fprog){ sizeof shared / sizeof shared[0], shared };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
        perror("deny-futex2: seccomp");
        return 2;
    }
    execvp(argv[1 + shared_only], argv + 1 + shared_only);
    perror(argv[1 + shared_only]);
    return 2;
}
