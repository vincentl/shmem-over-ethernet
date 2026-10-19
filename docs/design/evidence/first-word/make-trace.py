# SPDX-License-Identifier: LGPL-3.0-or-later
# make-trace.py TREE OUT: copies of the end paths' files with a trace of each
# step (scratch only, never committed): every event a line in
# $DIW_TRACE_DIR/<pid>, "<monotonic ns> rank R tid T agent|main|other EVENT".
import pathlib
import sys

tree, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])

TRACE = r'''
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
static _Atomic int diw_fd = -1;
void osoe_end_trace(const char *event)
{
    int fd = atomic_load(&diw_fd);
    if (fd < 0) {
        const char *dir = getenv("DIW_TRACE_DIR");
        if (!dir)
            return;
        char path[512];
        snprintf(path, sizeof path, "%s/%d", dir, (int)getpid());
        int mine = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        int none = -1;
        if (mine < 0)
            return;
        if (atomic_compare_exchange_strong(&diw_fd, &none, mine))
            fd = mine;
        else {
            close(mine);
            fd = none;
        }
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    char line[256];
    int n = snprintf(line, sizeof line, "%lld rank %u tid %ld %s %s\n",
                     (long long)t.tv_sec * 1000000000LL + t.tv_nsec, osoe.bootstrap.rank,
                     (long)syscall(SYS_gettid),
                     osoe_agent_is_this_thread() ? "agent" : "other", event);
    if (n > 0)
        (void)!write(fd, line, (size_t)n);
}
'''

edits = {
    'shmem/src/fatal.h': [
        ('bool osoe_fatal_first_word(void);\n',
         'bool osoe_fatal_first_word(void);\nvoid osoe_end_trace(const char *event);\n'),
    ],
    'shmem/src/fatal.c': [
        ('bool osoe_fatal_first_word(void)\n{\n    return !atomic_exchange_explicit(&end_said, true, memory_order_seq_cst);\n}\n',
         TRACE + 'bool osoe_fatal_first_word(void)\n{\n    bool first = !atomic_exchange_explicit(&end_said, true, memory_order_seq_cst);\n'
         '    osoe_end_trace(first ? "first word: won" : "first word: lost");\n    return first;\n}\n'),
        ('    char out[2048];\n    char msg[1024];\n    vsnprintf(msg, sizeof msg, fmt, ap);\n',
         '    char out[2048];\n    char msg[1024];\n    vsnprintf(msg, sizeof msg, fmt, ap);\n    osoe_end_trace("fatal: entered");\n'),
        ('    fflush(NULL);\n    if (n > 0) {\n        size_t len = (size_t)n < sizeof out ? (size_t)n : sizeof out - 1;\n        ssize_t ignored = write(STDERR_FILENO, out, len);\n        (void)ignored;\n    }\n',
         '    fflush(NULL);\n    osoe_end_trace("fatal: flushed");\n    if (n > 0) {\n        size_t len = (size_t)n < sizeof out ? (size_t)n : sizeof out - 1;\n        ssize_t ignored = write(STDERR_FILENO, out, len);\n        (void)ignored;\n        osoe_end_trace("fatal: wrote");\n    }\n'),
        ('     */\n    _exit(EXIT_FAILURE);\n}\n',
         '     */\n    osoe_end_trace("fatal: _exit");\n    _exit(EXIT_FAILURE);\n}\n'),
    ],
    'shmem/src/abort.c': [
        ('    if (osoe_agent_speaks())\n        for (;;)\n            pause();\n    uint32_t pe = 0;\n',
         '    osoe_end_trace("abort_end: entered");\n    if (osoe_agent_speaks()) {\n        osoe_end_trace("abort_end: the agent speaks, pausing");\n        for (;;)\n            pause();\n    }\n    uint32_t pe = 0;\n'),
        ('    this_thread_ends = true;\n',
         '    this_thread_ends = true;\n    osoe_end_trace("abort_end: this thread ends the process");\n'),
        ('        (void)ignored;\n    }\n    _exit(status);\n}\n',
         '        (void)ignored;\n        osoe_end_trace("abort_end: wrote");\n    }\n    osoe_end_trace("abort_end: _exit");\n    _exit(status);\n}\n'),
    ],
    'shmem/src/agent.c': [
        ('        if (osoe_abort_pending()) {\n',
         '        if (osoe_abort_pending()) {\n            osoe_end_trace("agent: sees the abort");\n'),
    ],
    'shmem/src/sweep.c': [
        ('    if (how != OSOE_ABORT_NONE)\n        osoe_fatal(',
         '    if (how != OSOE_ABORT_NONE)\n        osoe_end_trace("sweep: sees the abort"), osoe_fatal('),
    ],
    'shmem/src/liveness.c': [
        ('    char msg[384], where[128];\n',
         '    osoe_end_trace("liveness: a local PE ended");\n    char msg[384], where[128];\n'),
    ],
}

for name, pairs in edits.items():
    text = (tree / name).read_text()
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'{name}: {text.count(old)} matches for {old[:60]!r}')
        text = text.replace(old, new)
    target = out / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)
    print('traced', name)
