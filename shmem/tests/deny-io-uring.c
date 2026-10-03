/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * deny-io-uring WHERE PROGRAM [ARGS]... - run PROGRAM as it would run where
 * io_uring is refused in one place: `setup`, io_uring_setup itself, with EPERM,
 * as the kernel.io_uring_disabled sysctl refuses it; `enable`, io_uring_register's
 * IORING_REGISTER_ENABLE_RINGS; or `enter`, io_uring_enter. A seccomp filter,
 * inherited across fork and exec, so that it can wrap osoe-launch, as
 * deny-futex2 does. The last two are contrived - every kernel osoe accepts has
 * both calls - and are here because shmem_init's check once missed them, and a
 * job then ended after its PEs had left shmem_init (Part 6's adversarial
 * reads, 2026-10-03).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/io_uring.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "the filter reads an argument's low half at the lower address");

static int usage(void)
{
    fprintf(stderr, "usage: deny-io-uring setup|enable|enter PROGRAM [ARGS]...\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    const char *where = argv[1];
    unsigned call;
    if (strcmp(where, "setup") == 0)
        call = __NR_io_uring_setup;
    else if (strcmp(where, "enable") == 0)
        call = __NR_io_uring_register;
    else if (strcmp(where, "enter") == 0)
        call = __NR_io_uring_enter;
    else
        return usage();
    /* The call refused outright, or - for enable - only when its second
     * argument, the register opcode, is ENABLE_RINGS. */
    struct sock_filter refuse_call[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, call, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter refuse_enable[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, call, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, args) + sizeof(__u64)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IORING_REGISTER_ENABLE_RINGS, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = { sizeof refuse_call / sizeof refuse_call[0], refuse_call };
    if (strcmp(where, "enable") == 0)
        program = (struct sock_fprog){ sizeof refuse_enable / sizeof refuse_enable[0],
                                       refuse_enable };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
        perror("deny-io-uring: seccomp");
        return 2;
    }
    execvp(argv[2], argv + 2);
    perror(argv[2]);
    return 2;
}
