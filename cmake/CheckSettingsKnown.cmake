# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Every OSOE_ name the tree reads or sets in a PE's environment is in the list of
# shmem/src/unknown-settings.c, which the library's notice of an unknown name
# goes by (D16), and every name the list holds is read or set somewhere.
#
#   cmake -DSOURCE_DIR=<repository root> -P CheckSettingsKnown.cmake
#
# A name read and not listed would draw the notice "is not a variable osoe
# reads" from every job that sets it - a test's own, a tool's, a new setting -
# and a name listed and read nowhere is one the notice would let a typo hide
# behind. It collects, over shmem/, base/, tools/ and cmake/, the test suite's
# files with the rest (shmem.settings-documented leaves them out, its question
# being the README's), and leaving the list's own file out:
#
#   - a quoted literal, "OSOE_..." or 'OSOE_...', in .c, .h, .sh, .in and .py
#     files - getenv's and setenv's argument, a script's string;
#   - a shell variable, $OSOE_... or ${OSOE_...}, in .sh and .in files (in a
#     CMake file ${OSOE_...} is a CMake variable, not the environment's);
#   - an assignment, OSOE_...= after a space, a colon or a quote, in .sh files
#     and CMakeLists.txt: what a test or a tool sets for the PEs it starts
#     (`cmake -E env`, osoe-launch's -e);
#   - the launcher's names, launcher_name("...") with the OSOE_ prefix, as
#     bootstrap.c makes them;
#
# but not the build's own options - option(OSOE_...) and cache variables, such
# as OSOE_TIER3 - which a configure line sets, and messages and comments name
# as NAME=ON; nor the two names shmem.settings.unknown.* set because osoe does
# not read them, to see the notice, which the list must never hold.
set(unknown_on_purpose OSOE_DEADLOCK_SECOND OSOE_FROBNICATE)

set(list_file "${SOURCE_DIR}/shmem/src/unknown-settings.c")
if(NOT EXISTS "${list_file}")
  message(FATAL_ERROR "no ${list_file}: nothing to check against")
endif()
file(READ "${list_file}" text)
string(REGEX MATCHALL "\"OSOE_[A-Z0-9_]+\"" listed_quoted "${text}")
set(listed "")
foreach(found IN LISTS listed_quoted)
  string(REGEX REPLACE "\"" "" name "${found}")
  list(APPEND listed "${name}")
endforeach()
list(LENGTH listed listed_count)
if(listed_count LESS 10)
  message(FATAL_ERROR "${listed_count} names found in ${list_file}: the list's scan is broken")
endif()

set(names "")
set(checked 0)
file(GLOB_RECURSE sources LIST_DIRECTORIES false
     "${SOURCE_DIR}/shmem/*" "${SOURCE_DIR}/base/*" "${SOURCE_DIR}/tools/*"
     "${SOURCE_DIR}/cmake/*")
set(options "")
foreach(path IN LISTS sources ITEMS "${SOURCE_DIR}/CMakeLists.txt")
  get_filename_component(base "${path}" NAME)
  if(base STREQUAL "CMakeLists.txt" OR base MATCHES "\\.cmake$")
    file(READ "${path}" text)
    string(REGEX MATCHALL "option\\(OSOE_[A-Z0-9_]+|set\\(OSOE_[A-Z0-9_]+ [^)]* CACHE " declared
           "${text}")
    foreach(found IN LISTS declared)
      string(REGEX REPLACE "^[^O]*(OSOE_[A-Z0-9_]+).*$" "\\1" name "${found}")
      list(APPEND options "${name}")
    endforeach()
  endif()
endforeach()
if(NOT "OSOE_TIER3" IN_LIST options)
  message(FATAL_ERROR "the build's options were not found (no OSOE_TIER3): the scan is broken")
endif()
foreach(path IN LISTS sources)
  file(RELATIVE_PATH where "${SOURCE_DIR}" "${path}")
  get_filename_component(base "${path}" NAME)
  if(where STREQUAL "shmem/src/unknown-settings.c")
    continue()
  endif()
  set(found_here "")
  if(base MATCHES "\\.(c|h|sh|in|py)$")
    file(READ "${path}" text)
    string(REGEX MATCHALL "[\"']OSOE_[A-Z0-9_]+[\"']" quoted "${text}")
    list(APPEND found_here ${quoted})
    string(REGEX MATCHALL "launcher_name\\(\"[A-Z0-9_]+\"\\)" launcher "${text}")
    foreach(found IN LISTS launcher)
      string(REGEX REPLACE "^launcher_name\\(\"([A-Z0-9_]+)\"\\)$" "OSOE_\\1" name "${found}")
      list(APPEND found_here "${name}")
    endforeach()
  endif()
  if(base MATCHES "\\.(sh|in)$")
    string(REGEX MATCHALL "\\$\\{?OSOE_[A-Z0-9_]+" shell "${text}")
    list(APPEND found_here ${shell})
  endif()
  if(base MATCHES "\\.sh$" OR base STREQUAL "CMakeLists.txt")
    if(base STREQUAL "CMakeLists.txt")
      file(READ "${path}" text)
    endif()
    string(REGEX MATCHALL "[ \t\n:\"']OSOE_[A-Z0-9_]+=" assigned "${text}")
    list(APPEND found_here ${assigned})
  endif()
  if(NOT base MATCHES "\\.(c|h|sh|in|py)$" AND NOT base STREQUAL "CMakeLists.txt")
    continue()
  endif()
  math(EXPR checked "${checked} + 1")
  foreach(found IN LISTS found_here)
    string(REGEX REPLACE "^[^O]*(OSOE_[A-Z0-9_]+).*$" "\\1" name "${found}")
    list(APPEND names "${name}|${where}")
  endforeach()
endforeach()
if(checked EQUAL 0)
  message(FATAL_ERROR "no source found under ${SOURCE_DIR}: nothing was checked")
endif()

set(seen "")
set(missing "")
foreach(entry IN LISTS names)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 name)
  list(GET parts 1 where)
  if(name IN_LIST seen OR name IN_LIST options OR name IN_LIST unknown_on_purpose)
    continue()
  endif()
  list(APPEND seen "${name}")
  if(NOT name IN_LIST listed)
    list(APPEND missing "${name} (in ${where})")
  endif()
endforeach()
set(stale "")
foreach(name IN LISTS listed)
  if(NOT name IN_LIST seen OR name IN_LIST unknown_on_purpose)
    list(APPEND stale "${name}")
  endif()
endforeach()
list(LENGTH seen count)
if(count EQUAL 0)
  message(FATAL_ERROR "no OSOE_ name found in ${checked} files: the scan itself is broken")
endif()
set(report "")
if(missing)
  list(JOIN missing "\n  " lines)
  string(APPEND report "OSOE_ names the tree reads or sets that shmem/src/unknown-settings.c "
                       "does not list, so that a job setting one is told it is unknown:\n  ${lines}\n")
endif()
if(stale)
  list(JOIN stale "\n  " lines)
  string(APPEND report "OSOE_ names shmem/src/unknown-settings.c lists that nothing reads or "
                       "sets, so that a typo of another name could hide behind one:\n  ${lines}\n")
endif()
if(report)
  message(FATAL_ERROR "${report}(cmake/CheckSettingsKnown.cmake)")
endif()
message(STATUS "${checked} files, ${count} OSOE_ names read or set, every one of the "
               "${listed_count} unknown-settings.c lists and no other")
