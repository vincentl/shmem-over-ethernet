# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Every source file carries the project's license line, as the README's
# "License" says, but the specification's four examples kept as evidence.
#
#   cmake -DSOURCE_DIR=<repository root> -P CheckLicenseLines.cmake
#
# The line was given to every program of the two evidence trees on 2026-10-02
# (docs/decisions/2026-10-02.md, item 7), and 82 programs and scripts added to
# them since went without it until the review of v0.2 step 4's merge point found
# them (docs/reviews/archive/v0.2-step4-readiness.md, Q5-1): nothing checked
# the rule, so it held only while someone remembered it.
#
# Over the directories that hold the project's files - the top-level
# CMakeLists.txt, base, cmake, dev, docs, shmem and tools, and not build, nor
# what a checkout may keep untracked beside them (the reading corpus) - every
# file of the kinds that carried the line when it was given - .c, .h, .cpp, .py,
# .sh, .cmake, .in, .ld, .yaml and CMakeLists.txt - must hold
# "SPDX-License-Identifier: LGPL-3.0-or-later" somewhere: first, second under a
# shebang, or last where a record cites the file's lines by number
# (docs/README.md, "What was edited in the evidence").

set(exceptions
    "docs/reviews/evidence/phase1-step2-slice1/conformance/spec-example-5.c"
    "docs/reviews/evidence/phase1-step2-slice1/conformance/spec-example-38.c"
    "docs/reviews/evidence/phase1-step2-slice1/conformance/spec-example-42.c"
    "docs/reviews/evidence/phase1-step2-slice1/conformance/spec-example-43.c")
set(line "SPDX-License-Identifier: LGPL-3.0-or-later")

set(sources "${SOURCE_DIR}/CMakeLists.txt")
foreach(directory IN ITEMS base cmake dev docs shmem tools)
  file(GLOB_RECURSE found LIST_DIRECTORIES false "${SOURCE_DIR}/${directory}/*")
  list(APPEND sources ${found})
endforeach()

set(checked 0)
set(excepted 0)
set(missing "")
foreach(path IN LISTS sources)
  file(RELATIVE_PATH where "${SOURCE_DIR}" "${path}")
  get_filename_component(base "${path}" NAME)
  if(NOT base MATCHES "\\.(c|h|cpp|py|sh|cmake|in|ld|yaml)$" AND NOT base STREQUAL "CMakeLists.txt")
    continue()
  endif()
  if(where IN_LIST exceptions)
    math(EXPR excepted "${excepted} + 1")
    continue()
  endif()
  math(EXPR checked "${checked} + 1")
  file(READ "${path}" text)
  string(FIND "${text}" "${line}" at)
  if(at EQUAL -1)
    list(APPEND missing "${where}")
  endif()
endforeach()

if(checked EQUAL 0)
  message(FATAL_ERROR "no source file found under ${SOURCE_DIR}: nothing was checked")
endif()
list(LENGTH exceptions expected)
if(NOT excepted EQUAL expected)
  message(FATAL_ERROR "found ${excepted} of the ${expected} specification examples the README "
                      "names as exceptions: the list here and the README's are out of step")
endif()
if(missing)
  list(LENGTH missing count)
  list(JOIN missing "\n  " shown)
  message(FATAL_ERROR "${count} source files without \"${line}\" (README.md, \"License\"; "
                      "first in the file, or second under a shebang):\n  ${shown}")
endif()
message(STATUS "${checked} source files, each with the license line; "
               "the specification's ${excepted} examples excepted")
