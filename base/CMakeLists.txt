# base: internal building blocks linked into libosoe. Never installed.
#
# An OBJECT library, not STATIC: target_link_libraries() then folds its object
# files into whatever links osoe_base, archive included, so a static libosoe.a
# carries osoe_base's own objects rather than merely recording a dependency on
# a second archive nobody installs (portability review, F10).
add_library(osoe_base OBJECT
  counters.h
  deadline.c
  environment.c
  node-map.c
  epoch.c
  futex.c
  order.c
  size.c
  stall-watch.c
  sweep.c)
target_include_directories(osoe_base PUBLIC ${PROJECT_SOURCE_DIR})
set_target_properties(osoe_base PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)
osoe_warnings(osoe_base)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
