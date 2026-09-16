# Fails if a shared library exports any symbol outside the OpenSHMEM API.
#
#   cmake -DLIB=<path to libosoe.so> -P CheckExports.cmake
#
# Internal functions must stay hidden (see shmem/src/export.h) so they cannot
# collide with application symbols. Extend ALLOWED as deprecated unprefixed
# spec routines (start_pes, _my_pe, ...) are implemented.
set(ALLOWED "^shmem_")

find_program(NM nm REQUIRED)
execute_process(COMMAND "${NM}" -D --defined-only "${LIB}"
  OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nm failed on ${LIB}")
endif()

string(REGEX MATCHALL "[^\n]+" lines "${out}")
set(unexpected "")
set(count 0)
foreach(line IN LISTS lines)
  string(REGEX REPLACE "^[0-9a-fA-F]* *[A-Za-z] +" "" sym "${line}")
  if(sym MATCHES "${ALLOWED}")
    math(EXPR count "${count} + 1")
  else()
    list(APPEND unexpected "${sym}")
  endif()
endforeach()

if(unexpected)
  list(JOIN unexpected "\n  " report)
  message(FATAL_ERROR "Unexpected exported symbols in ${LIB}:\n  ${report}")
endif()
message(STATUS "${count} exported symbols, all within the public API")
