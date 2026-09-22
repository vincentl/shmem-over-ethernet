cmake_minimum_required(VERSION 3.25)

# Fails if any file above the backend boundary includes, however indirectly, a
# header below it (design §4.10; second review, roce-seam F6).
#
#   cmake -DSOURCE_DIR=<repository root> -P CheckBoundary.cmake
#
# transport.h and internode.h are the whole of what the library above them may
# call. Before this test existed that was a promise with nothing under it: the
# AM backend's state sat in struct osoe_state, so every §4 file could name the
# wires and the engine, and a future optimisation reading one would compile,
# pass every preset - there is one backend - and be found by the RoCE port a
# year later. The project already guards osoe-stat's header isolation and the
# export list with tests; this is the same guard for the seam the scaling goals
# rest on.
#
# **The lists below are the one place "above" and "below" are defined.** Every
# library source file must be in exactly one of the three, so a new file cannot
# slip past unclassified.

# The library above the boundary: the SHMEM layer (§4), the collectives (§5),
# initialization and the agent (§6, §3.12), and the library's own services.
# Each of these, and everything each includes, is checked.
set(OSOE_ABOVE_BOUNDARY
  accessible.c agent.c allocator.c amo.c barrier.c bootstrap.c deprecated.c
  fatal.c info.c init.c malloc.c metrics.c profile.c quiet.c rma.c session.c
  signal.c symmetric.c team.c wait.c watchdog.c)

# The AM backend (§3): the files a second backend replaces. No file above may
# reach any header here.
set(OSOE_BELOW_BOUNDARY
  am.c am.h am-state.h connection.c connection.h connection-area.c
  connection-area.h internode-am.c progress.c progress.h progress-host.c
  progress-host.h reply.c reply.h transport-am.c transport-local.h wire.c wire.h)

# Neither: the boundary's own implementation that every backend keeps, and the
# shared infrastructure both sides build on. Not checked, and listed so that
# the classification is complete. Two of these still reach AM headers, and
# both are known (roce-seam §3.2): node.c sizes the landing zones from the AM
# chunk (L5), and job.c checks the AM handler table's checksum at rendezvous
# (L6). Both are the rendezvous's business, not the SHMEM layer's.
set(OSOE_ON_BOUNDARY
  transport-local.c watch.c node.c job.c shm-object.c)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repository root> -P CheckBoundary.cmake")
endif()
set(src "${SOURCE_DIR}/shmem/src")

# Every source file is classified exactly once.
file(GLOB sources RELATIVE "${src}" "${src}/*.c")
set(unclassified "")
foreach(f IN LISTS sources)
  set(n 0)
  foreach(list IN ITEMS OSOE_ABOVE_BOUNDARY OSOE_BELOW_BOUNDARY OSOE_ON_BOUNDARY)
    if(f IN_LIST ${list})
      math(EXPR n "${n} + 1")
    endif()
  endforeach()
  if(NOT n EQUAL 1)
    list(APPEND unclassified "${f} (in ${n} lists)")
  endif()
endforeach()
if(unclassified)
  list(JOIN unclassified "\n  " report)
  message(FATAL_ERROR "shmem/src files not in exactly one boundary list "
                      "(cmake/CheckBoundary.cmake):\n  ${report}")
endif()

# A quoted include resolves against shmem/src first and then the repository
# root (base/...), as the build's include path does.
function(resolve name out)
  if(EXISTS "${src}/${name}")
    set(${out} "${src}/${name}" PARENT_SCOPE)
  elseif(EXISTS "${SOURCE_DIR}/${name}")
    set(${out} "${SOURCE_DIR}/${name}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

set(violations "")
set(checked 0)
foreach(top IN LISTS OSOE_ABOVE_BOUNDARY)
  # Breadth-first over quoted includes, remembering how each file was reached
  # so a failure names the whole chain.
  set(queue "${src}/${top}")
  set(seen "${src}/${top}")
  set(via_${top} "${top}")
  while(queue)
    list(POP_FRONT queue file)
    get_filename_component(file_name "${file}" NAME)
    string(MAKE_C_IDENTIFIER "${file}" key)
    if(NOT DEFINED chain_${key})
      set(chain_${key} "${top}")
    endif()
    file(STRINGS "${file}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
    foreach(line IN LISTS lines)
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\".*" "\\1" name "${line}")
      get_filename_component(leaf "${name}" NAME)
      set(chain "${chain_${key}} -> ${name}")
      if(leaf IN_LIST OSOE_BELOW_BOUNDARY AND NOT name MATCHES "/")
        list(APPEND violations "${chain}")
        continue()
      endif()
      resolve("${name}" path)
      if(path AND NOT path IN_LIST seen)
        list(APPEND seen "${path}")
        list(APPEND queue "${path}")
        string(MAKE_C_IDENTIFIER "${path}" pkey)
        set(chain_${pkey} "${chain}")
      endif()
    endforeach()
  endwhile()
  math(EXPR checked "${checked} + 1")
endforeach()

if(violations)
  list(REMOVE_DUPLICATES violations)
  list(JOIN violations "\n  " report)
  message(FATAL_ERROR "files above the backend boundary reach headers below it "
                      "(transport.h and internode.h are the only way down; "
                      "cmake/CheckBoundary.cmake):\n  ${report}")
endif()
message(STATUS "${checked} files above the boundary, none reaching a header below it")
