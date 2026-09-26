cmake_minimum_required(VERSION 3.25)

# Fails if a shared library exports any symbol outside the OpenSHMEM API.
#
#   cmake -DLIB=<path to libosoe.so> -P CheckExports.cmake
#
# Internal functions must stay hidden (see shmem/src/export.h) so they cannot
# collide with application symbols.
#
# `pshmem_` is the profiling interface (spec §10): every routine is defined
# under that name and `shmem_` is a weak alias to it, so both spellings are
# exported on purpose. `shmemx_` is the extension namespace (shmemx.h).
# Deprecated spec routines with no prefix at all (start_pes, _my_pe, ...) are
# listed one by one rather than admitted by a pattern, because a pattern loose
# enough to cover them would admit anything.
set(ALLOWED "^p?shmem_|^shmemx_")
set(ALLOWED_EXACT
  start_pes _my_pe _num_pes shmalloc shfree shrealloc shmemalign
  pstart_pes p_my_pe p_num_pes pshmalloc pshfree pshrealloc pshmemalign)

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
  if(sym MATCHES "${ALLOWED}" OR sym IN_LIST ALLOWED_EXACT)
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
