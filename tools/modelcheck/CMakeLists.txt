# The models run as tier-1 tests, not only as a recorded argument (§7.1 step 0,
# §8.2). Each exits nonzero if any case disagrees with its expectation, so a
# change to a protocol that breaks it fails the suite rather than waiting to be
# noticed in a design review.
#
# Only the models for protocols this step implements are registered. The older
# ones under this directory are the record of designs already settled, and
# docs/design/evidence/ holds their runs.
find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
  foreach(name wire credit arrival)
    add_test(NAME model.${name}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${name}_model.py)
    set_tests_properties(model.${name} PROPERTIES LABELS "tier1" TIMEOUT 300)
  endforeach()
else()
  message(STATUS "python3 not found: the concurrency models will not run as tests")
endif()
