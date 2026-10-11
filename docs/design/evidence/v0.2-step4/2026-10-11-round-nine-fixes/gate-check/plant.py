#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The gate's check: plants in a copy of the tree, each inert unless its
variable is set, so one ThreadSanitizer build runs the controls too.
  GATE_PLANT_SEGV=after   test-ack-echo.c: PE 1 dereferences NULL after
                          shmem_finalize (a crash the tool catches)
  GATE_PLANT_WRONG_PE=<pe> test-circulant-reduce.c: that PE's first checked
                          sum of 4 KiB or more has one byte wrong, which the
                          test's own check finds
  GATE_PLANT_RACE=global|heap test-circulant-reduce.c, judged by a pass
                          expression: PE 0 starts a thread that writes a word its
                          main thread writes too, unordered - a global, or the
                          first word of a symmetric array - with no helper in it"""
import sys
root = sys.argv[1]


def plant(path, old, new):
    p = f'{root}/{path}'
    t = open(p).read()
    assert t.count(old) == 1, (path, old)
    open(p, 'w').write(t.replace(old, new))


ack = 'shmem/tests/test-ack-echo.c'
plant(ack, '#include <shmem.h>\n', '#include <shmem.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n')
plant(ack, '''    shmem_finalize();
    return CHECK_DONE();''', '''    shmem_finalize();
    if (me == 1 && getenv("GATE_PLANT_SEGV")) {
        fprintf(stderr, "PE 1: planted crash after shmem_finalize\\n");
        *(volatile long *)(uintptr_t)getenv("GATE_PLANT_NOWHERE") = 1;
    }
    return CHECK_DONE();''')

circ = 'shmem/tests/test-circulant-reduce.c'
plant(circ, '''static void check_byte_sum(const uint8_t *dest, size_t n, int round, const char *what)
{
''', '''static void check_byte_sum(const uint8_t *dest, size_t n, int round, const char *what)
{
    static int planted;
    const char *wrong = getenv("GATE_PLANT_WRONG_PE");
    if (wrong && atoi(wrong) == me && !planted && n >= 4096) {
        ((uint8_t *)(uintptr_t)dest)[n / 2] ^= 1;    /* as if landed wrong */
        planted = 1;
    }
''')
plant(circ, 'int main(int argc, char **argv)\n{\n', '''static long planted_global;
static long *planted_target;
static void *planted_writer(void *arg)
{
    (void)arg;
    *planted_target = 1;        /* unordered against main's write below */
    return NULL;
}

int main(int argc, char **argv)
{
''')
plant(circ, '''    int *mine = shmem_malloc(2 * sizeof *mine), *all = shmem_malloc(2 * sizeof *all);
    CHECK_FATAL(mine && all);
''', '''    int *mine = shmem_malloc(2 * sizeof *mine), *all = shmem_malloc(2 * sizeof *all);
    CHECK_FATAL(mine && all);
    long *planted_heap = shmem_malloc(sizeof *planted_heap);
    const char *race = getenv("GATE_PLANT_RACE");
    if (race && me == 0) {
        planted_target = strcmp(race, "heap") == 0 ? planted_heap : &planted_global;
        pthread_t planted;
        pthread_create(&planted, NULL, planted_writer, NULL);
        *planted_target = 2;
        pthread_join(planted, NULL);
    }
''')
print('planted')
