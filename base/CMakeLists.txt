# SPDX-License-Identifier: LGPL-3.0-or-later
# base: internal building blocks linked into libosoe. Never installed.
#
# An OBJECT library, not STATIC: target_link_libraries() then folds its object
# files into whatever links osoe_base, archive included, so a static libosoe.a
# carries osoe_base's own objects rather than merely recording a dependency on
# a second archive nobody installs (portability review, F10).
#
# Which futex calls futex.h is made of is the build's: the full build's futex2
# (futex-futex2.c), and with it io_uring's start-up check (ring-check.c); the
# single-node build's classic futex(2) (futex-classic.c), and no io_uring at
# all (OSOE_SINGLE_NODE, the top-level CMakeLists.txt).
if(OSOE_SINGLE_NODE)
  set(osoe_base_kernel futex-classic.c)
else()
  set(osoe_base_kernel futex-futex2.c ring-check.c)
endif()
add_library(osoe_base OBJECT
  counters.h
  deadline.c
  environment.c
  node-map.c
  epoch.c
  hostlist.c
  order.c
  pid-namespace.c
  print-line.c
  siphash.c
  size.c
  stall-watch.c
  sweep.c
  ${osoe_base_kernel})
target_include_directories(osoe_base PUBLIC ${PROJECT_SOURCE_DIR})
# ring-check.c asks the running kernel for io_uring (§1): liburing, the one
# library beside libc that §1 allows, comes with it to whatever links base.
if(NOT OSOE_SINGLE_NODE)
  target_link_libraries(osoe_base PUBLIC PkgConfig::LIBURING)
endif()
set_target_properties(osoe_base PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)
osoe_warnings(osoe_base)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
