# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Every line the library prints to stderr is written whole (base/print-line.h;
# D30): no fprintf(stderr, ...) or vfprintf(stderr, ...) in its sources.
#
#   cmake -DSOURCE_DIR=<repository root> -P CheckWholeLines.cmake
#
# glibc 2.37 and later write an unbuffered stream's formatted output 128 bytes
# at a time, so a longer line printed that way reaches stderr in pieces that
# another PE's output can split (round nine's portability read, P9-7): 22 calls
# did until D30. The library prints through osoe_print_line or osoe_notice,
# which format the line first and write it once. Over shmem/src and base, every
# .c and .h but their tests, which print their own diagnostics; matched in whole
# files, so that a call broken over lines is found too.
set(found "")
set(checked 0)
file(GLOB_RECURSE sources LIST_DIRECTORIES false
     "${SOURCE_DIR}/shmem/src/*.c" "${SOURCE_DIR}/shmem/src/*.h"
     "${SOURCE_DIR}/base/*.c" "${SOURCE_DIR}/base/*.h")
foreach(path IN LISTS sources)
  file(RELATIVE_PATH where "${SOURCE_DIR}" "${path}")
  if(where MATCHES "/tests/")
    continue()
  endif()
  math(EXPR checked "${checked} + 1")
  file(READ "${path}" text)
  if(text MATCHES "fprintf[ \t\n]*\\([ \t\n]*stderr")
    list(APPEND found "${where}")
  endif()
endforeach()

if(checked EQUAL 0)
  message(FATAL_ERROR "no source read under ${SOURCE_DIR}: the check reads nothing")
endif()
if(found)
  list(JOIN found "\n  " lines)
  message(FATAL_ERROR "a line printed by fprintf(stderr, ...), which glibc 2.37 and later "
                      "write 128 bytes at a time - print it through osoe_print_line "
                      "(base/print-line.h):\n  ${lines}")
endif()
message("WHOLE LINES OK: ${checked} sources, none printing by fprintf(stderr, ...)")
