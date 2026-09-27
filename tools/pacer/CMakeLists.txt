# SPDX-License-Identifier: LGPL-3.0-or-later
# osoe-pacer: puts the library through its paces - whole-job patterns from real
# OpenSHMEM codes, every result checked against an oracle
# (docs/design/osoe-pacer-proposal.md). Not installed. base/size.c is compiled
# in so that --memory and the heap's size parse exactly as the library parses
# SHMEM_SYMMETRIC_SIZE; libosoe hides its own copy.
add_executable(osoe-pacer osoe-pacer.c parallel-print.c sort.c alltoall.c print.c
  ${PROJECT_SOURCE_DIR}/base/size.c)
target_include_directories(osoe-pacer PRIVATE ${PROJECT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(osoe-pacer PRIVATE osoe Threads::Threads)
osoe_warnings(osoe-pacer)
