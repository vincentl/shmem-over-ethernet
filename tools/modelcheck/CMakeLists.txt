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
# close (a million states with the trapped-state check), abort (its largest
# case, goodbye [2:3], 11.8 million states, where a fatal's of 5.7 million and
# 18 minutes was the largest when this was first written), abort_exit (a case
# of ten million; 17), deadlock_owners (16 minutes in the ninth review's run)
# and, since v0.2 step 4's zero-copy send, reservation (21
# minutes in the development VM beside another model in the ninth review's run,
# its largest case of 23 million states under x86-TSO about 13 of them, against
# the 40 its timeout gives) - made the 24 models 55 minutes of the 61 the release workflow took (the
# release-fixes plan's measurement, in the review's VM; one at a time on
# 2026-10-03 they took 29 minutes in the development VM). So every model carries
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
# OSOE_MULTIPLE_MODELS=ON. They were made opt-in when phase 1 stopped at
# SERIALIZED and the first three alone were about 18 minutes of every release
# run (the test review of 2026-09-23); the library provides MULTIPLE now, so
# turn it on when §4.11's code or engine.py changes, and before a release (the
# release checklist). model.selftest holds the engine to its own cases, not
# to these state spaces. get_completion's one case of what a SERIALIZED
# library needs, §4.6's equality wait, runs regardless.
find_package(Python3 COMPONENTS Interpreter)
option(OSOE_MULTIPLE_MODELS "run the models of the SHMEM_THREAD_MULTIPLE designs (§4.11)" OFF)

if(Python3_Interpreter_FOUND AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  set(osoe_models credit arrival ack reservation blocked_record range_watch
                  chained_wake lead_wake rendezvous wait_word credit_wait tree_barrier close
                  abort abort_exit lock pipeline allgather scan scan_pipeline deadlock
                  team_block split_numbers pool_dry posted_bitmap lane lane_scan visit
                  control_ack carrier helper_request deadlock_owners abort_owners)
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
  # The cost of a collective's shape (v0.2 step 2): its fit to the Pi's table,
  # the closed form against its walk and against the simulation, the choices'
  # pins, and its two broken variants; and the grid collective-shape.c's test
  # reads, as the model writes it now.
  add_test(NAME model.shape_timing
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shape_timing.py check)
  add_test(NAME model.shape_timing.grid
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shape_timing.py grid-compare
            ${PROJECT_SOURCE_DIR}/shmem/tests/shape-grid.txt)
  set_tests_properties(model.shape_timing model.shape_timing.grid PROPERTIES
                       LABELS "tier1;model" TIMEOUT 600)
  # Träff's circulant reduce-scatter and allgather (v0.2 step 3): the rounds'
  # arithmetic for every node count from 2 to 70, against the fold the brief
  # described (rabenseifner_model.py, kept as that comparison's evidence); the
  # protocol through rows, credits and grants on two to five leads, and with a
  # small tree collective before and after it; each breakage failing; and the
  # digests collective-circulant.c's test reads.
  add_test(NAME model.circulant
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/circulant_model.py check)
  add_test(NAME model.circulant.digest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/circulant_model.py
            digest-compare ${PROJECT_SOURCE_DIR}/shmem/tests/circulant-digest.txt)
  set_tests_properties(model.circulant PROPERTIES LABELS "model" TIMEOUT 1800)
  set_tests_properties(model.circulant.digest PROPERTIES LABELS "tier1;model" TIMEOUT 300)
elseif(NOT Python3_Interpreter_FOUND)
  message(STATUS "python3 not found: the concurrency models will not run as tests")
endif()
