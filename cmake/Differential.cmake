# Run one program under several logical node maps and require identical output
# (design §8.1).
#
# The maps are §8.1's four poles at the same world size, so the only thing that
# differs between runs is which pairs of PEs are on the same logical node - that
# is, which operations take the intranode path and which cross the AM layer. Any
# difference in the transcript is a difference between the two paths, which is
# the whole claim §4.10 makes and the one thing analysis cannot settle.
#
# Lines are sorted before comparison because the launcher interleaves the output
# of its ranks in whatever order they happen to run; the program is written so
# that every line is independent of the map (see its header).
#
# Expects: LAUNCH, EXE, MAPS (semicolon-separated), NRANKS, and optionally ENVS.

cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0007 NEW)       # keep empty list elements out of the sort

if(NOT DEFINED LAUNCH OR NOT DEFINED EXE OR NOT DEFINED MAPS OR NOT DEFINED NRANKS)
  message(FATAL_ERROR "Differential.cmake: LAUNCH, EXE, MAPS and NRANKS are required")
endif()

set(reference "")
set(reference_map "")

foreach(map IN LISTS MAPS)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env ${ENVS}
            ${LAUNCH} -n ${NRANKS} -m ${map} -t 60 -- ${EXE}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "map ${map} exited ${status}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
  endif()

  # Sort, and drop blank lines, so the comparison is over the set of results
  # rather than over the order the launcher happened to print them in.
  string(REPLACE "\n" ";" lines "${out}")
  list(REMOVE_ITEM lines "")
  list(SORT lines)
  string(REPLACE ";" "\n" sorted "${lines}")

  if(reference STREQUAL "")
    set(reference "${sorted}")
    set(reference_map "${map}")
  elseif(NOT sorted STREQUAL reference)
    # Report the first differing line rather than two whole transcripts: the
    # first difference is the one that says what broke, and the rest are
    # usually its consequences.
    string(REPLACE "\n" ";" a "${reference}")
    string(REPLACE "\n" ";" b "${sorted}")
    list(LENGTH a alen)
    list(LENGTH b blen)
    set(first "")
    math(EXPR last "${alen} - 1")
    if(blen LESS alen)
      math(EXPR last "${blen} - 1")
    endif()
    foreach(i RANGE ${last})
      list(GET a ${i} al)
      list(GET b ${i} bl)
      if(NOT al STREQUAL bl)
        set(first "line ${i}:\n  ${reference_map}: ${al}\n  ${map}: ${bl}")
        break()
      endif()
    endforeach()
    if(first STREQUAL "" AND NOT alen EQUAL blen)
      set(first "map ${reference_map} produced ${alen} lines, map ${map} produced ${blen}")
    endif()
    message(FATAL_ERROR
      "node map ${map} disagrees with ${reference_map}\n${first}\n"
      "The same program over two logical layouts must produce the same results; "
      "a difference here is a difference between the intranode path and the AM "
      "layer (§8.1).")
  endif()
endforeach()

message(STATUS "differential: ${NRANKS} ranks agree over maps: ${MAPS}")
