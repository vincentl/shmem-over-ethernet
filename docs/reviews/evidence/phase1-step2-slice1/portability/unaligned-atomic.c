/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Does an unaligned 64-bit atomic fault on this machine? (design §3.4 says it
 * does on aarch64.) Prints the outcome for offsets 0..7 within a buffer. */
#include <signal.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static sigjmp_buf back;
static void on_fault(int sig) { siglongjmp(back, sig); }

int main(void)
{
    static _Alignas(16) unsigned char buffer[32];
    signal(SIGBUS, on_fault);
    signal(SIGSEGV, on_fault);
    for (int k = 0; k < 8; k++) {
        int sig = sigsetjmp(back, 1);
        if (sig) { printf("offset %d: signal %d\n", k, sig); continue; }
        _Atomic uint64_t *w = (_Atomic uint64_t *)(void *)(buffer + k);
        uint64_t old = atomic_fetch_add_explicit(w, 1, memory_order_seq_cst);
        printf("offset %d: ok (old %llu)\n", k, (unsigned long long)old);
    }
    return 0;
}
