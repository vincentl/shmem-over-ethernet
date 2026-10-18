/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A line the library prints to stderr, written whole (D30; round nine's
 * portability read, P9-7).
 *
 * glibc 2.37 and later write an unbuffered stream's formatted output through a
 * staging buffer of 128 bytes, one write(2) for each: a line longer than that,
 * printed by fprintf to stderr, reaches the terminal or the pipe in pieces,
 * and another PE's line can land between them. Four processes printing lines of
 * 150 bytes to one pipe broke 5,873 to 7,672 of 8,000 lines on glibc 2.41, and
 * a test that counts the lines beginning "running" never saw its four when the
 * lead's pidfd notice was split by another PE's.
 *
 * osoe_print_line formats the whole text first and writes it with one write(2)
 * - as osoe_notice and the fatal path do - so that a line up to PIPE_BUF bytes
 * (4,096 on Linux) reaches a pipe in one piece, and a longer one, which no
 * write keeps whole on a pipe, at least does not break at 128. The text is the
 * caller's, its "osoe: " and its newline included: the call sites keep the
 * words they printed. Every library line to stderr goes through it or
 * osoe_notice; shmem.whole-lines (CheckWholeLines.cmake) fails on a fprintf
 * to stderr in the library's sources.
 *
 * It keeps errno, so that a caller printing strerror(errno) may go on to look
 * at errno. A text over 1 KiB is formatted into memory it allocates; if that
 * allocation fails, the first 1 KiB is written rather than nothing.
 */
#ifndef OSOE_BASE_PRINT_LINE_H
#define OSOE_BASE_PRINT_LINE_H

void osoe_print_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* OSOE_BASE_PRINT_LINE_H */
