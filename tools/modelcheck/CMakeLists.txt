# SPDX-License-Identifier: LGPL-3.0-or-later
# The models run as tier-1 tests, not only as a recorded argument (§7.1 step 0,
# §8.2). Each exits nonzero if any case disagrees with its expectation, so a
# change to a protocol that breaks it fails the suite rather than waiting to be
# noticed in a design review. Every model is registered, and selftest.py, which
# holds the engine's own checks to cases they must catch (second review, F6).
#
# A model is Python and gives the same verdict whichever preset built the
# library, so running it in four presets buys nothing: every model runs in the
# release preset only, which keeps it from being skipped. The large ones -
# tree_barrier (five logical nodes, every node's thread real; 11 minutes),
# close (a million states with the trapped-state check), abort (a fatal's case
# of 5.7 million states; 18 minutes) and abort_exit (a case of ten million; 17)
# - make the 24 models 55 minutes of the 61 the release workflow took (the
# release-fixes plan's measurement, in the review's VM). So every model carries
# the label "model" beside "tier1": the release test preset leaves that label
# out, and the "models" test preset runs it alone (CMakePresets.json;
# `ctest --preset models`, or the workflow of that name, which configures and
# builds the release tree first). A model added here takes the label in the
# loop below, never by its own add_test.
#
# The six that check §4.11's SHMEM_THREAD_MULTIPLE designs - dirty_set,
# watch_list, get_completion's generation-word cases (about 15 minutes, a case
# of 19 million states), landing_extent and copy_out_claim (under a minute
# together), and pe_quiet (two minutes) - run only with
# OSOE_MULTIPLE_MODELS=ON: phase 1 ships up to SERIALIZED, and together they
# were three quarters of every release run (test review, 2026-09-23). Turn it on when §4.11 is scheduled or
# engine.py changes; model.selftest holds the engine to its own cases, not to
# these state spaces. get_completion's one case of what phase 1 ships, §4.6's
# equality wait, runs regardless.
find_package(Python3 COMPONENTS Interpreter)
option(OSOE_MULTIPLE_MODELS "run the models of the SHMEM_THREAD_MULTIPLE designs (§4.11)" OFF)

if(Python3_Interpreter_FOUND AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  set(osoe_models wire credit arrival ack reservation blocked_record range_watch
                  chained_wake lead_wake rendezvous wait_word credit_wait tree_barrier close
                  abort abort_exit lock pipeline allgather scan scan_pipeline deadlock)
  if(OSOE_MULTIPLE_MODELS)
    list(APPEND osoe_models dirty_set get_completion watch_list landing_extent copy_out_claim
                            pe_quiet)
  else()
    add_test(NAME model.get_completion.equality
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/get_completion_model.py
              equality)
    set_tests_properties(model.get_completion.equality PROPERTIES LABELS "tier1;model"
                         TIMEOUT 600)
  endif()
  foreach(name IN LISTS osoe_models)
    add_test(NAME model.${name}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${name}_model.py)
    set_tests_properties(model.${name} PROPERTIES LABELS "tier1;model" TIMEOUT 2400)
  endforeach()
  add_test(NAME model.selftest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/selftest.py)
  set_tests_properties(model.selftest PROPERTIES LABELS "tier1;model" TIMEOUT 600)
elseif(NOT Python3_Interpreter_FOUND)
  message(STATUS "python3 not found: the concurrency models will not run as tests")
endif()
