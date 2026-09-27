# SPDX-License-Identifier: LGPL-3.0-or-later
# One planted-defect run of osoe-pacer (tools/pacer/CMakeLists.txt): runs it
# on the 0,0,1,1 map and requires every pattern of PATTERNS to match its
# output, each on its own and in any order - a planted run's failure lines come
# from several PEs, in an order that depends on who reserved first - and FAIL
# to match nowhere.
#
#   cmake -DLAUNCH=<osoe-launch> -DPACER=<osoe-pacer> "-DARGUMENTS=--quick;..."
#         "-DPATTERNS=<regex>;<regex>..." -DFAIL=<regex> -P PacerPlant.cmake
#
# The launcher runs with -k: a run that found a failure exits 1 by design, on
# every PE, and the first PE to exit must not have the others killed in their
# finalize. So FAIL names only a signal, the timeout and leftovers.
cmake_minimum_required(VERSION 3.25)

foreach(var LAUNCH PACER ARGUMENTS PATTERNS FAIL)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

execute_process(
  COMMAND ${LAUNCH} -k -n 4 -m 0,0,1,1 -t 240 -- ${PACER} ${ARGUMENTS}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
set(output "${out}${err}")
message("${output}")
if(output MATCHES "${FAIL}")
  message(FATAL_ERROR "The planted run failed as a run: '${CMAKE_MATCH_0}'")
endif()
foreach(pattern IN LISTS PATTERNS)
  if(NOT output MATCHES "${pattern}")
    message(FATAL_ERROR "The planted run did not print what its check must: '${pattern}'")
  endif()
endforeach()
