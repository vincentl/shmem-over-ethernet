# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Two checks over the library's includes:
#
#   1. No file above the backend boundary includes, however indirectly, a
#      header below it (design §4.10; second review, roce-seam F6) - nor the
#      generic internode implementation's, which only a backend calls; and no
#      file on the boundary does either, but the three that are known to.
#   2. Nothing but the API includes an API header or a public one (phase 1
#      step 5 slice 2): the runtime, the files on the boundary and the backend
#      include nothing of the OpenSHMEM layer's, so the includes run one way,
#      from the API down.
#   3. Nothing outside the internode backend includes, however indirectly, a
#      header of the internode backend's (v0.2 step 1): the part of the
#      transport every job uses reaches it through internode-backend.h alone,
#      so a second internode backend replaces those files and nothing else.
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
# the same runtime, one of the stances the successor-API notes weigh
# (docs/design/successor-api-stage-setting.md §6). It is about includes: the
# backend still calls the upcalls the API defines - osoe_internode_released,
# and the two a data half asks of a team, osoe_internode_team_lead and
# osoe_internode_team_local_ranks - which internode.h declares. (A third, the
# team's world ranks, went when the team's record carried them: decision E.)
#
# **An include is judged by the file it resolves to**, quoted or angled, the
# way the build's include path finds it - shmem/src, the repository root, the
# public include directory - so neither a "./name.h" nor a <name.h> spelling of
# a forbidden header gets past. A name that resolves nowhere in the repository
# is a system header.
#
# **The lists below are the one place the layers are defined.** Every library
# source file and header must be in exactly one of them, so a new file cannot
# slip past unclassified.

# The API: the OpenSHMEM routines and what serves only them. Nothing else may
# include its headers.
set(OSOE_API
  accessible.c active-set.c alltoall.c amo.c barrier.c broadcast.c collect.c collectives.c context.c
  deprecated.c info.c init.c link-measure.c lock.c malloc.c profile.c quiet.c reductions.c
  rma-typed.c rma.c scans.c session.c signal.c symmetric.c team.c wait.c
  barrier.h collectives.h context.h export.h link-measure.h lock.h rma.h symmetric.h team.h
  wait.h)

# The runtime: initialization's machinery and the agent (§6, §3.12), and the
# library's own services - fatal, abort, liveness, the sweep, the deadlock
# detector, metrics, the blocked record, waiting on a word of one's own region,
# the test hook that perturbs a protocol's steps - which the API and the
# backends both build on. Above the boundary, like the API.
set(OSOE_RUNTIME
  abort.c agent.c allocator.c blocked.c bootstrap.c deadlock.c fatal.c liveness.c metrics.c
  perturb.c region-wait.c sweep.c
  abort.h agent.h allocator.h blocked.h bootstrap.h deadlock.h fatal.h liveness.h
  metrics.h perturb.h region-wait.h state.h)

# The AM backend (§3): the files a second backend replaces. No file above may
# reach any of them.
set(OSOE_BELOW_BOUNDARY
  am.c am.h am-state.h connection.c connection.h connection-area.c
  connection-area.h internode-am.c progress.c progress.h progress-host.c
  progress-host.h reply.c reply.h ring.c ring.h transport-am.c transport-context.h
  transport-internode.c transport-local.h transport-single-node.c wire.c wire.h)

# The generic implementation of internode.h's data half is for backends to
# call or to replace (internode-generic.h); above the boundary it would be a
# way around the seam, so it is refused there as a backend's header is. And
# the internode backend's entry points (internode-backend.h), which the part
# of the transport every job uses calls, and job.c for the rendezvous of the
# node leads: a backend's, refused above the boundary in the same way.
set(OSOE_BACKEND_FACING internode-backend.h internode-generic.h)

# On the boundary: its own implementation that every backend keeps, and the
# shared infrastructure both sides build on - the seam's two headers, the
# internode tree's topology, which an RDMA backend computes the same way, the
# reductions' combine functions, the generic internode implementation a
# backend may call, and the rendezvous. The rank map (rank-map.c) is every
# build's; the rendezvous (rendezvous.c) and its sockets (listener.c), which
# bootstrap a job of more than one node out of band (§6.2), sit on the boundary
# as an RDMA backend would keep them, and are the internode backend's files
# below (the third check).
#
# Two more every build keeps: the AM layer's sizes (am-configuration.c), which
# lay out every PE's landing zone in the node segment whatever the job's shape,
# and the wire's settings (wire-settings.c), which every PE checks as values
# before its node joins - each read on one node as on many, so that a bad
# value is refused the same way.
set(OSOE_ON_BOUNDARY
  am-configuration.c collective-shape.c collective-tree.c combine.c internode-generic.c job.c
  listener.c node.c rank-map.c rendezvous.c shm-object.c transport-local.c watch.c
  wire-settings.c
  am-configuration.h collective-shape.h collective-tree.h combine.h internal-region.h
  internode.h job.h
  listener.h node.h rank-map.h rendezvous.h rmw.h shm-object.h transport.h watch-range.h
  wire-settings.h)

# The two files on the boundary that reach an AM header, each known (roce-seam
# §3.2): rendezvous.c checks the AM handler table's checksum (L6);
# transport-local.c, the intranode backend every build keeps, includes the
# header the AM backend shares with it. A third, node.c, sized the landing
# zones from the AM chunk (L5) until v0.2's step 1 gave the AM layer's sizes a
# file of their own on the boundary. Every other file on the boundary is held
# to the first check.
set(OSOE_ON_BOUNDARY_READING_AM rendezvous.c transport-local.c)

# The internode backend (internode-backend.h): every file only a job of more
# than one node uses, on AM - the far half, the engine, the wires and the ring,
# the connection area, the replies and the messages, the rendezvous of the node
# leads, and internode.h over AM. A second internode backend replaces them
# whole, so no other file may reach a header of theirs (the third check); the
# lists above say where in the layers each sits. The single-node build's
# internode backend, transport-single-node.c, is not among them: it stands in
# for them where none is compiled, so it is held to the third check too.
set(OSOE_INTERNODE_BACKEND
  am.c am.h am-state.h connection.c connection.h connection-area.c connection-area.h
  internode-am.c listener.c listener.h progress.c progress.h progress-host.c
  progress-host.h rendezvous.c rendezvous.h reply.c reply.h ring.c ring.h
  transport-internode.c wire.c wire.h)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repository root> -P CheckBoundary.cmake")
endif()
set(src "${SOURCE_DIR}/shmem/src")
set(public_directory "${SOURCE_DIR}/shmem/include")

# Every source file and header is classified exactly once.
file(GLOB files RELATIVE "${src}" "${src}/*.c" "${src}/*.h")
set(unclassified "")
foreach(f IN LISTS files)
  set(n 0)
  foreach(list IN ITEMS OSOE_API OSOE_RUNTIME OSOE_BELOW_BOUNDARY OSOE_BACKEND_FACING
                        OSOE_ON_BOUNDARY)
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

# A list of shmem/src names as the resolved paths an include is compared with.
function(paths_of list out)
  set(result "")
  foreach(name IN LISTS ${list})
    file(REAL_PATH "${src}/${name}" p)
    list(APPEND result "${p}")
  endforeach()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

# Where an include resolves - shmem/src, then the repository root (base/...),
# then the public include directory, as the build's include path has it - or
# nothing, for a system header.
function(resolve name out)
  foreach(dir IN ITEMS "${src}" "${SOURCE_DIR}" "${public_directory}")
    if(EXISTS "${dir}/${name}" AND NOT IS_DIRECTORY "${dir}/${name}")
      file(REAL_PATH "${dir}/${name}" p)
      set(${out} "${p}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} "" PARENT_SCOPE)
endfunction()

# Breadth-first over `top`'s includes, remembering how each file was reached
# so that a failure names the whole chain. Every include that resolves to a
# path in the list `forbidden` is a violation; with `forbid_public` true, so is
# every include of a public header. The chains go into `out`. (No variable
# here may share a name with one of the file's own: a function sees its
# caller's, so a parameter would shadow them for resolve() as well.)
function(walk top forbidden forbid_public out)
  file(REAL_PATH "${src}/${top}" start)
  set(found "")
  set(queue "${start}")
  set(seen "${start}")
  string(MAKE_C_IDENTIFIER "${start}" key)
  set(chain_${key} "${top}")
  file(GLOB public_headers "${public_directory}/*.h" "${public_directory}/mpp/*.h")
  set(public_paths "")
  foreach(h IN LISTS public_headers)
    file(REAL_PATH "${h}" p)
    list(APPEND public_paths "${p}")
  endforeach()
  while(queue)
    list(POP_FRONT queue file)
    string(MAKE_C_IDENTIFIER "${file}" key)
    file(STRINGS "${file}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]")
    foreach(line IN LISTS lines)
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[\"<]([^\">]+)[\">].*" "\\1" name
             "${line}")
      set(chain "${chain_${key}} -> ${name}")
      resolve("${name}" path)
      if(NOT path)
        continue()                      # a system header
      endif()
      if(path IN_LIST ${forbidden} OR (forbid_public AND path IN_LIST public_paths))
        list(APPEND found "${chain}")
        continue()
      endif()
      if(NOT path IN_LIST seen)
        list(APPEND seen "${path}")
        list(APPEND queue "${path}")
        string(MAKE_C_IDENTIFIER "${path}" pkey)
        set(chain_${pkey} "${chain}")
      endif()
    endforeach()
  endwhile()
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

# 1. Above the boundary, nothing below it or backend-facing; on it, nothing
#    below but for the three files known to.
set(below_names ${OSOE_BELOW_BOUNDARY} ${OSOE_BACKEND_FACING})
paths_of(below_names below)
paths_of(OSOE_BELOW_BOUNDARY below_only)
set(violations "")
set(checked 0)
foreach(top IN LISTS OSOE_API OSOE_RUNTIME)
  walk("${top}" below FALSE found)
  list(APPEND violations ${found})
  math(EXPR checked "${checked} + 1")
endforeach()
foreach(top IN LISTS OSOE_ON_BOUNDARY)
  if(top IN_LIST OSOE_ON_BOUNDARY_READING_AM)
    continue()
  endif()
  walk("${top}" below_only FALSE found)
  list(APPEND violations ${found})
  math(EXPR checked "${checked} + 1")
endforeach()
if(violations)
  list(REMOVE_DUPLICATES violations)
  list(JOIN violations "\n  " report)
  message(FATAL_ERROR "files above or on the backend boundary reach headers below it "
                      "(transport.h and internode.h are the only way down; "
                      "cmake/CheckBoundary.cmake):\n  ${report}")
endif()

# 2. Outside the API, nothing of the API's.
set(api_headers "")
foreach(name IN LISTS OSOE_API)
  if(name MATCHES "\\.h$")
    list(APPEND api_headers "${name}")
  endif()
endforeach()
paths_of(api_headers api)
set(violations "")
set(outside 0)
foreach(top IN LISTS OSOE_RUNTIME OSOE_ON_BOUNDARY OSOE_BELOW_BOUNDARY OSOE_BACKEND_FACING)
  walk("${top}" api TRUE found)
  list(APPEND violations ${found})
  math(EXPR outside "${outside} + 1")
endforeach()
if(violations)
  list(REMOVE_DUPLICATES violations)
  list(JOIN violations "\n  " report)
  message(FATAL_ERROR "files outside the API include its headers or a public one (the "
                      "includes run from the API down, never back; "
                      "cmake/CheckBoundary.cmake):\n  ${report}")
endif()

# 3. Outside the internode backend, nothing of the internode backend's.
set(internode_headers "")
foreach(name IN LISTS OSOE_INTERNODE_BACKEND)
  if(name MATCHES "\\.h$")
    list(APPEND internode_headers "${name}")
  endif()
endforeach()
paths_of(internode_headers internode)
# And base's io_uring check, which only the internode backend may call: a
# file every build compiles that reached it would not compile without liburing.
file(REAL_PATH "${SOURCE_DIR}/base/ring-check.h" ring_check)
list(APPEND internode "${ring_check}")
set(violations "")
set(common 0)
foreach(list IN ITEMS OSOE_API OSOE_RUNTIME OSOE_BELOW_BOUNDARY OSOE_BACKEND_FACING
                      OSOE_ON_BOUNDARY)
  foreach(top IN LISTS ${list})
    if(top IN_LIST OSOE_INTERNODE_BACKEND)
      continue()
    endif()
    walk("${top}" internode FALSE found)
    list(APPEND violations ${found})
    math(EXPR common "${common} + 1")
  endforeach()
endforeach()
if(violations)
  list(REMOVE_DUPLICATES violations)
  list(JOIN violations "\n  " report)
  message(FATAL_ERROR "files outside the internode backend include its headers (the part "
                      "of the transport every job uses reaches it through "
                      "internode-backend.h alone; cmake/CheckBoundary.cmake):\n  ${report}")
endif()

message(STATUS "${checked} files above or on the boundary, none reaching a header below it; "
               "${outside} outside the API, none including one of its headers; "
               "${common} outside the internode backend, none including one of its headers")
