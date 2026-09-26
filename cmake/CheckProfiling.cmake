cmake_minimum_required(VERSION 3.25)

# Fails unless the profiling interface has the shape spec section 10 needs.
#
#   cmake -DLIB=<path to libosoe.so> -P CheckProfiling.cmake
#
# Every public routine must be the STRONG definition under its pshmem_ name and
# a WEAK alias under its shmem_ name, so that an application can define its own
# shmem_X and still reach the implementation through pshmem_X. Interposition
# happens anyway in a shared-library build, where the executable's definition
# preempts the library's whatever its binding - which is exactly why a
# behavioural test cannot see this and this one is needed. The binding is what
# makes it work when the library is linked statically, where a strong shmem_X
# in the archive would be a duplicate symbol instead.
#
# The check runs over the whole symbol table rather than a sample: a routine
# added without its alias is the failure mode, and that is a routine nobody
# would think to list here.

find_program(NM nm REQUIRED)
execute_process(COMMAND "${NM}" -D --defined-only "${LIB}"
  OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nm failed on ${LIB}")
endif()

set(strong "")
set(weak "")
string(REGEX MATCHALL "[^\n]+" lines "${out}")
foreach(line IN LISTS lines)
  if(line MATCHES "^[0-9a-fA-F]+ +([A-Za-z]) +([A-Za-z_][A-Za-z0-9_]*)$")
    set(kind "${CMAKE_MATCH_1}")
    set(sym "${CMAKE_MATCH_2}")
    if(kind STREQUAL "T")
      list(APPEND strong "${sym}")
    elseif(kind STREQUAL "W")
      list(APPEND weak "${sym}")
    endif()
  endif()
endforeach()

set(problems "")
set(checked 0)
foreach(sym IN LISTS strong)
  if(sym MATCHES "^shmem_(.+)$")
    list(APPEND problems "${sym} is a strong definition; it must be a weak alias for pshmem_${CMAKE_MATCH_1}")
  endif()
endforeach()

foreach(sym IN LISTS weak)
  if(sym MATCHES "^shmem_(.+)$")
    set(p "pshmem_${CMAKE_MATCH_1}")
    if(NOT p IN_LIST strong)
      list(APPEND problems "${sym} is weak but ${p} is not a strong definition")
    else()
      math(EXPR checked "${checked} + 1")
    endif()
  endif()
endforeach()

# The deprecated routines without a shmem_ prefix: the old name is weak, and
# both profiling spellings, p<name> and pshmem_<name>, are strong.
set(deprecated start_pes _my_pe _num_pes shmalloc shfree shrealloc shmemalign)
set(deprecated_checked 0)
foreach(name IN LISTS deprecated)
  if(NOT name IN_LIST weak)
    list(APPEND problems "${name} is not a weak alias")
  endif()
  foreach(p IN ITEMS "p${name}" "pshmem_${name}")
    if(NOT p IN_LIST strong)
      list(APPEND problems "${p} is not a strong definition")
    endif()
  endforeach()
  math(EXPR deprecated_checked "${deprecated_checked} + 1")
endforeach()

foreach(sym IN LISTS strong)
  if(sym MATCHES "^pshmem_(.+)$")
    set(s "shmem_${CMAKE_MATCH_1}")
    if(NOT s IN_LIST weak AND NOT CMAKE_MATCH_1 IN_LIST deprecated)
      list(APPEND problems "${sym} has no weak shmem_ alias")
    endif()
  endif()
endforeach()

if(problems)
  list(JOIN problems "\n  " report)
  message(FATAL_ERROR "Profiling interface (spec section 10) is wrong:\n  ${report}")
endif()
if(checked EQUAL 0)
  message(FATAL_ERROR "No shmem_/pshmem_ pairs found in ${LIB}; the check proved nothing")
endif()
message(STATUS "${checked} routines, each a strong pshmem_ definition with a weak shmem_ alias, and ${deprecated_checked} deprecated ones with both profiling names")
