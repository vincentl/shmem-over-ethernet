# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Every OSOE_ setting the library and its tools read is in the README
# (docs/decisions/2026-10-09.md, item 5).
#
#   cmake -DSOURCE_DIR=<repository root> -P CheckSettingsDocumented.cmake
#
# OSOE_TEST_ABORT_DELAY_MS, read since v0.2 step 4's Part 3, had no row in the
# README's settings until that part's own documents added one, and neither of
# the part's two adversarial reads had asked for it. A setting the README does
# not name is one a reader cannot find; a setting it names that nothing reads
# is a README sentence that is false, which the reviews read for.
#
# Over the library (shmem/src, base) and the tools (tools), every .c, .h, .sh,
# .in, .py and .cmake file but the test suite's own - files under a tests/
# directory or named test-*, and tools/launch/tier3.sh, the tier-3 tests'
# wrapper, whose OSOE_TEST_READY names a file it gives the test programs, and
# shmem/src/unknown-settings.c, whose list of every name the tree reads holds
# the test suite's too (CheckSettingsKnown.cmake checks it) - it
# collects every OSOE_ name read: a quoted literal ("OSOE_...", 'OSOE_...'), a
# shell variable ($OSOE_..., ${OSOE_...}), and the launcher's names, which
# bootstrap.c makes from launcher_name("...") with the OSOE_ prefix (or
# SLURM_'s). Each must appear as `NAME` in README.md's "Environment variables"
# section - its tables, or the paragraph of the tools' settings - or its
# "Starting PEs without a launcher", the table of the launcher's names, as the
# README says.

set(names "")
set(checked 0)
file(GLOB_RECURSE sources LIST_DIRECTORIES false
     "${SOURCE_DIR}/shmem/src/*" "${SOURCE_DIR}/base/*" "${SOURCE_DIR}/tools/*")
foreach(path IN LISTS sources)
  file(RELATIVE_PATH where "${SOURCE_DIR}" "${path}")
  get_filename_component(base "${path}" NAME)
  if(where MATCHES "(^|/)tests/" OR base MATCHES "^test-" OR where STREQUAL "tools/launch/tier3.sh"
     OR where STREQUAL "shmem/src/unknown-settings.c"
     OR NOT base MATCHES "\\.(c|h|sh|in|py|cmake)$")
    continue()
  endif()
  math(EXPR checked "${checked} + 1")
  file(READ "${path}" text)
  string(REGEX MATCHALL "[\"']OSOE_[A-Z0-9_]+[\"']" quoted "${text}")
  string(REGEX MATCHALL "\\$\\{?OSOE_[A-Z0-9_]+" shell "${text}")
  string(REGEX MATCHALL "launcher_name\\(\"[A-Z0-9_]+\"\\)" launcher "${text}")
  foreach(found IN LISTS quoted shell)
    string(REGEX REPLACE "^[\"'$\\{]*(OSOE_[A-Z0-9_]+).*$" "\\1" name "${found}")
    list(APPEND names "${name}|${where}")
  endforeach()
  foreach(found IN LISTS launcher)
    string(REGEX REPLACE "^launcher_name\\(\"([A-Z0-9_]+)\"\\)$" "OSOE_\\1" name "${found}")
    list(APPEND names "${name}|${where}")
  endforeach()
endforeach()

if(checked EQUAL 0)
  message(FATAL_ERROR "no library or tool source found under ${SOURCE_DIR}: nothing was checked")
endif()
file(READ "${SOURCE_DIR}/README.md" whole)
# The two sections, each from its heading to the next heading of its level.
set(readme "")
foreach(heading IN ITEMS "### Environment variables" "### Starting PEs without a launcher")
  string(FIND "${whole}" "${heading}\n" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "README.md has no section \"${heading}\": the check would see nothing")
  endif()
  string(SUBSTRING "${whole}" ${at} -1 rest)
  string(LENGTH "${heading}" skip)
  string(SUBSTRING "${rest}" ${skip} -1 after)
  # The nearer of the next heading of its level and the next above it, or the end.
  string(LENGTH "${after}" next)
  foreach(marker IN ITEMS "\n### " "\n## ")
    string(FIND "${after}" "${marker}" found)
    if(NOT found EQUAL -1 AND found LESS next)
      set(next ${found})
    endif()
  endforeach()
  math(EXPR length "${skip} + ${next}")
  string(SUBSTRING "${rest}" 0 ${length} section)
  string(APPEND readme "${section}")
endforeach()
set(seen "")
set(missing "")
foreach(entry IN LISTS names)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 name)
  list(GET parts 1 where)
  if(name IN_LIST seen)
    continue()
  endif()
  list(APPEND seen "${name}")
  string(FIND "${readme}" "`${name}`" at)
  if(at EQUAL -1)
    list(APPEND missing "${name} (read in ${where})")
  endif()
endforeach()
list(LENGTH seen count)
if(count EQUAL 0)
  message(FATAL_ERROR "no OSOE_ setting found in ${checked} files: the scan itself is broken")
endif()
if(missing)
  list(JOIN missing "\n  " report)
  message(FATAL_ERROR "OSOE_ settings the code reads that README.md's \"Environment variables\" "
                      "and \"Starting PEs without a launcher\" do not name as `NAME` "
                      "(cmake/CheckSettingsDocumented.cmake):\n  ${report}")
endif()
message(STATUS "${checked} library and tool files, ${count} OSOE_ settings read, every one in the README's settings or launcher sections")
