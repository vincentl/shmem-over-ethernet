# The models run as tier-1 tests, not only as a recorded argument (§7.1 step 0,
# §8.2). Each exits nonzero if any case disagrees with its expectation, so a
# change to a protocol that breaks it fails the suite rather than waiting to be
# noticed in a design review. Every model is registered, and selftest.py, which
# holds the engine's own checks to cases they must catch (second review, F6).
#
# The large ones - get_completion (about 15 minutes, a case of 19 million
# states), dirty_set and tree_barrier (five logical nodes, every node's thread
# real), close (a million states with the trapped-state check) - run in the
# release preset only. A model is Python and
# gives the same verdict whichever preset built the library, so running it four
# times buys nothing; one preset keeps it from being skipped.
find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
  set(osoe_models wire credit arrival ack reservation blocked_record range_watch watch_list
                  chained_wake lead_wake rendezvous wait_word)
  if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    list(APPEND osoe_models dirty_set get_completion tree_barrier close)
  endif()
  foreach(name IN LISTS osoe_models)
    add_test(NAME model.${name}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${name}_model.py)
    set_tests_properties(model.${name} PROPERTIES LABELS "tier1" TIMEOUT 2400)
  endforeach()
  add_test(NAME model.selftest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/selftest.py)
  set_tests_properties(model.selftest PROPERTIES LABELS "tier1" TIMEOUT 600)
else()
  message(STATUS "python3 not found: the concurrency models will not run as tests")
endif()
