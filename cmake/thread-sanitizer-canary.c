/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The ThreadSanitizer gate's canary (OsoeThreadSanitizer.cmake): two threads
 * write one word with nothing ordering them, a race the tool reports on every
 * run, whatever the timing - neither write happens before the other. Its report
 * must reach the directory the gate reads; if it does not, the options the
 * tests are given did not reach them either, and the gate fails rather than
 * pass a directory the tool never wrote to.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static int word;

static void *writer(void *value)
{
    word = (int)(intptr_t)value;
    return NULL;
}

int main(void)
{
    pthread_t first, second;
    if (pthread_create(&first, NULL, writer, (void *)(intptr_t)1) != 0 ||
        pthread_create(&second, NULL, writer, (void *)(intptr_t)2) != 0)
        return 1;
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    printf("the canary's word is %d\n", word);
    return 0;
}
