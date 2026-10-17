# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Every test's PASS_REGULAR_EXPRESSION and SKIP_REGULAR_EXPRESSION is one
# expression, as ctest holds it:
#
#   cmake -DCTEST=<ctest> -DBUILD_DIR=<a configured build> -P CheckPassExpressions.cmake
#
# A test property is a CMake list, so a `;` in a quoted expression splits it in
# two, and ctest passes the test on either: its output need only hold one half.
# Three tests passed on the wrong half until cloud session 4's batch 2 found
# them, and three more until the merge after it (docs/decisions/2026-10-17.md)
# - one of whose halves was " spec", which nearly any output holds. Write `.`
# where the `;` was, or alternatives with `|`. A FAIL_REGULAR_EXPRESSION may be
# a list, since any one of its expressions fails the test - which is how the
# suite names a launcher's several ways of failing - so it is not checked.
#
# It reads the tests as ctest has them, from `ctest --show-only=json-v1`, so a
# `;` that arrives in a variable is found as well as one written out.

foreach(variable CTEST BUILD_DIR)
  if(NOT ${variable})
    message(FATAL_ERROR "CheckPassExpressions.cmake needs -D${variable}=")
  endif()
endforeach()

execute_process(COMMAND ${CTEST} --test-dir ${BUILD_DIR} --show-only=json-v1
                OUTPUT_VARIABLE json ERROR_VARIABLE error RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ctest --show-only=json-v1 failed (${status}): ${error}")
endif()
string(JSON tests GET "${json}" tests)
string(JSON test_count LENGTH "${tests}")

set(checked 0)
set(split 0)
math(EXPR last "${test_count} - 1")
foreach(i RANGE ${last})
  # One test's object at a time: string(JSON) parses its whole input on every
  # call, so the properties are read from the test's text, not the build's.
  string(JSON test GET "${tests}" ${i})
  string(JSON properties ERROR_VARIABLE none GET "${test}" properties)
  if(none)
    continue()
  endif()
  string(JSON name GET "${test}" name)
  string(JSON property_count LENGTH "${properties}")
  math(EXPR last_property "${property_count} - 1")
  foreach(j RANGE ${last_property})
    string(JSON property GET "${properties}" ${j} name)
    if(NOT property MATCHES "^(PASS|SKIP)_REGULAR_EXPRESSION$")
      continue()
    endif()
    math(EXPR checked "${checked} + 1")
    string(JSON type TYPE "${properties}" ${j} value)
    if(NOT type STREQUAL "ARRAY")
      continue()
    endif()
    string(JSON length LENGTH "${properties}" ${j} value)
    if(length GREATER 1)
      string(JSON value GET "${properties}" ${j} value)
      message("${name}: its ${property} is ${length} expressions, any one of which "
              "passes it: ${value}")
      math(EXPR split "${split} + 1")
    endif()
  endforeach()
endforeach()

if(checked EQUAL 0)
  # A check that found nothing to read is not a clean tree (the renames' lesson,
  # CLAUDE.md): the suite has had a PASS expression since its first test.
  message(FATAL_ERROR "no PASS_REGULAR_EXPRESSION or SKIP_REGULAR_EXPRESSION read among "
                      "${test_count} tests: the check reads nothing")
endif()
if(split GREATER 0)
  message(FATAL_ERROR "${split} of ${checked} expressions split by a ';' (write '.' or '|')")
endif()
message("PASS EXPRESSIONS OK: ${checked} expressions among ${test_count} tests, each one")
