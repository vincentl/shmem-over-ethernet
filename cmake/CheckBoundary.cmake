cmake_minimum_required(VERSION 3.25)

# Two checks over the library's includes:
#
#   1. No file above the backend boundary includes, however indirectly, a
#      header below it (design §4.10; second review, roce-seam F6) - nor the
#      generic internode implementation's, which only a backend calls.
#   2. Nothing but the API includes an API header (phase 1 step 5 slice 2):
#      the runtime, the files on the boundary and the backend never depend on
#      the OpenSHMEM layer, so dependencies run one way, from the API down.
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
# The second check was true before it was written - the author asked whether
# the layers were separate, the answer came from reading every include, and
# nothing kept it so. It is what lets a second API sit beside OpenSHMEM's on
# the same runtime, which is one of the stances the successor-API notes weigh
# (docs/design/successor-api-stage-setting.md §6), and it costs one more list.
#
# **The lists below are the one place the layers are defined.** Every library
# source file must be in exactly one of the four, so a new file cannot slip past
# unclassified.

# The API: the OpenSHMEM routines and what serves only them. Its headers are
# the ones nothing else may include, beside the public headers themselves.
set(OSOE_API
  accessible.c amo.c barrier.c broadcast.c collectives.c deprecated.c info.c init.c
  malloc.c profile.c quiet.c reductions.c rma-typed.c rma.c session.c signal.c
  symmetric.c team.c wait.c)
set(OSOE_API_HEADERS
  barrier.h collectives.h export.h rma.h symmetric.h team.h wait.h)

# The runtime: initialization's machinery and the agent (§6, §3.12), and the
# library's own services - fatal, abort, liveness, the sweep, metrics, the
# blocked record, waiting on a word of one's own region - which the API and the
# backends both build on. Above the boundary, like the API.
set(OSOE_RUNTIME
  abort.c agent.c allocator.c bootstrap.c fatal.c liveness.c metrics.c region-wait.c
  sweep.c)

# The AM backend (§3): the files a second backend replaces. No file above may
# reach any header here.
set(OSOE_BELOW_BOUNDARY
  am.c am.h am-state.h connection.c connection.h connection-area.c
  connection-area.h internode-am.c progress.c progress.h progress-host.c
  progress-host.h reply.c reply.h ring.c ring.h transport-am.c transport-local.h wire.c
  wire.h)

# The generic implementation of internode.h's data half is for backends to
# call or to replace (internode-generic.h); above the boundary it would be a
# way around the seam, so it is refused there as a backend's header is.
set(OSOE_BACKEND_HEADERS internode-generic.h)

# On the boundary: its own implementation that every backend keeps, and the
# shared infrastructure both sides build on - the internode tree's topology
# among it, which an RDMA backend computes the same way, the reductions'
# combine functions, and the generic internode implementation a backend may
# call. Not checked by the first walk, and listed so that the classification is
# complete. Two of these still reach AM headers, and both are known (roce-seam
# §3.2): node.c sizes the landing zones from the AM chunk (L5), and the
# rendezvous (job.c, and since phase 1 step 3 rendezvous.c) checks the AM
# handler table's checksum (L6). Both are the rendezvous's business, not the
# SHMEM layer's. The rendezvous's sockets (listener.c) and the rank map
# (rank-map.c) are every build's: bootstrap is out of band (§6.2).
set(OSOE_ON_BOUNDARY
  collective-tree.c combine.c internode-generic.c transport-local.c watch.c node.c job.c
  shm-object.c rank-map.c rendezvous.c listener.c)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repository root> -P CheckBoundary.cmake")
endif()
set(src "${SOURCE_DIR}/shmem/src")

# Every source file is classified exactly once.
file(GLOB sources RELATIVE "${src}" "${src}/*.c")
set(unclassified "")
foreach(f IN LISTS sources)
  set(n 0)
  foreach(list IN ITEMS OSOE_API OSOE_RUNTIME OSOE_BELOW_BOUNDARY OSOE_ON_BOUNDARY)
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
  message(FATAL_ERROR "shmem/src files not in exactly one layer list "
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

# Breadth-first over `top`'s quoted includes, remembering how each file was
# reached so that a failure names the whole chain. Every include of a header
# named in the list `forbidden` is a violation, and so, when `public` is true,
# is every include of a public header (<shmem.h>, <pshmem.h>, <shmemx.h>, in
# either spelling). The chains go into `out`.
function(walk top forbidden public out)
  set(found "")
  set(queue "${src}/${top}")
  set(seen "${src}/${top}")
  string(MAKE_C_IDENTIFIER "${src}/${top}" key)
  set(chain_${key} "${top}")
  while(queue)
    list(POP_FRONT queue file)
    string(MAKE_C_IDENTIFIER "${file}" key)
    file(STRINGS "${file}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]")
    foreach(line IN LISTS lines)
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[\"<]([^\">]+)[\">].*" "\\1" name
             "${line}")
      get_filename_component(leaf "${name}" NAME)
      set(chain "${chain_${key}} -> ${name}")
      if(public AND name MATCHES "^(mpp/)?(shmem|pshmem|shmemx)\\.h$")
        list(APPEND found "${chain}")
        continue()
      endif()
      if(line MATCHES "^[ \t]*#[ \t]*include[ \t]*<")
        continue()                      # a system header
      endif()
      if(leaf IN_LIST ${forbidden} AND NOT name MATCHES "/")
        list(APPEND found "${chain}")
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
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

# 1. Above the boundary, nothing below it.
set(below ${OSOE_BELOW_BOUNDARY} ${OSOE_BACKEND_HEADERS})
set(violations "")
set(above 0)
foreach(top IN LISTS OSOE_API OSOE_RUNTIME)
  walk("${top}" below FALSE found)
  list(APPEND violations ${found})
  math(EXPR above "${above} + 1")
endforeach()
if(violations)
  list(REMOVE_DUPLICATES violations)
  list(JOIN violations "\n  " report)
  message(FATAL_ERROR "files above the backend boundary reach headers below it "
                      "(transport.h and internode.h are the only way down; "
                      "cmake/CheckBoundary.cmake):\n  ${report}")
endif()

# 2. Outside the API, nothing of the API's.
set(violations "")
set(outside 0)
foreach(top IN LISTS OSOE_RUNTIME OSOE_ON_BOUNDARY OSOE_BELOW_BOUNDARY)
  if(NOT top MATCHES "\\.c$")
    continue()                          # headers are walked from the files that include them
  endif()
  walk("${top}" OSOE_API_HEADERS TRUE found)
  list(APPEND violations ${found})
  math(EXPR outside "${outside} + 1")
endforeach()
if(violations)
  list(REMOVE_DUPLICATES violations)
  list(JOIN violations "\n  " report)
  message(FATAL_ERROR "files outside the API include its headers (dependencies run from "
                      "the API down, never back; cmake/CheckBoundary.cmake):\n  ${report}")
endif()

message(STATUS "${above} files above the boundary, none reaching a header below it; "
               "${outside} outside the API, none including one of its headers")
