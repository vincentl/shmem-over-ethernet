# SPDX-License-Identifier: LGPL-3.0-or-later
#
# ThreadSanitizer's verdict, given inside ctest (the ninth review's C9-1).
#
# The tool replaces a process's exit status with its own, 66, once it has
# reported anything - on a nonzero return from main and in its own handling of a
# deadly signal alike - and ctest ignores the exit status of a test judged by a
# pass expression. Read from ctest's log, then, a test that failed its own
# checks or crashed beside a race looked like one that failed by the race alone,
# and a race in a test that passed was never read. So in a ThreadSanitizer build
# every test runs with these options:
#
# - exitcode=0, which leaves each process its own status, for ctest to judge
#   every test as it judges it in the other presets;
# - abort_on_error=1, which makes a crash the tool catches end by SIGABRT, where
#   exitcode=0 alone would end it with 0;
# - log_path, which writes the tool's reports to files of the test's name in
#   one directory, where they are read whether the test passed or not.
#
# The reports are judged apart, by thread-sanitizer-gate.py, run as the cleanup
# of a fixture every test requires: after every other test, whatever their
# verdicts, and its failure is the run's. The fixture's setup empties the
# directory and runs a canary with one race, whose report the gate must find - a
# gate reading a directory the tool never wrote to would pass anything. ctest
# adds both to any subset of the tests it is asked to run.
#
# The options are appended to whatever TSAN_OPTIONS the shell has, so a
# developer's own come first and these win. CMake 3.28 at least: the properties
# are set from the top directory on tests every directory registers.

function(osoe_thread_sanitizer_tests directory out)
  get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
  get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  set(found "")
  foreach(test IN LISTS tests)
    list(APPEND found "${directory}|${test}")
  endforeach()
  foreach(subdirectory IN LISTS subdirectories)
    osoe_thread_sanitizer_tests("${subdirectory}" more)
    list(APPEND found ${more})
  endforeach()
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

function(osoe_thread_sanitizer_gate)
  if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR
      "The ThreadSanitizer build needs CMake 3.28 or newer, to give every test the "
      "options its gate reads by (cmake/OsoeThreadSanitizer.cmake); this is "
      "CMake ${CMAKE_VERSION}.")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_package(Threads REQUIRED)
  # Collected before the fixture's own two tests are added: neither requires it.
  osoe_thread_sanitizer_tests("${PROJECT_SOURCE_DIR}" tests)

  set(reports "${CMAKE_BINARY_DIR}/thread-sanitizer-reports")
  set(gate "${PROJECT_SOURCE_DIR}/cmake/thread-sanitizer-gate.py")
  add_executable(osoe_thread_sanitizer_canary
    "${PROJECT_SOURCE_DIR}/cmake/thread-sanitizer-canary.c")
  target_link_libraries(osoe_thread_sanitizer_canary PRIVATE Threads::Threads)
  # The internal region's places, by the library's own layout (round eight's Q2-2).
  add_executable(osoe_thread_sanitizer_place
    "${PROJECT_SOURCE_DIR}/cmake/thread-sanitizer-place.c")
  target_include_directories(osoe_thread_sanitizer_place PRIVATE
    "${PROJECT_SOURCE_DIR}/shmem/src" "${PROJECT_SOURCE_DIR}/shmem/include")
  target_link_libraries(osoe_thread_sanitizer_place PRIVATE osoe_base)
  add_test(NAME tsan.reports.setup
    COMMAND ${Python3_EXECUTABLE} -I ${gate} setup ${reports}
            $<TARGET_FILE:osoe_thread_sanitizer_canary>)
  add_test(NAME tsan.reports
    COMMAND ${Python3_EXECUTABLE} -I ${gate} judge ${reports}
            --ctest ${CMAKE_CTEST_COMMAND} --build ${CMAKE_BINARY_DIR}
            --place $<TARGET_FILE:osoe_thread_sanitizer_place>)
  set_tests_properties(tsan.reports.setup PROPERTIES
    FIXTURES_SETUP osoe-thread-sanitizer TIMEOUT 60
    ENVIRONMENT_MODIFICATION
      "TSAN_OPTIONS=string_append::exitcode=0:abort_on_error=1:log_path='${reports}/tsan.reports.setup'")
  set_tests_properties(tsan.reports PROPERTIES
    FIXTURES_CLEANUP osoe-thread-sanitizer TIMEOUT 300)

  foreach(entry IN LISTS tests)
    string(FIND "${entry}" "|" bar)
    string(SUBSTRING "${entry}" 0 ${bar} directory)
    math(EXPR after "${bar} + 1")
    string(SUBSTRING "${entry}" ${after} -1 test)
    set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY
      ENVIRONMENT_MODIFICATION
        "TSAN_OPTIONS=string_append::exitcode=0:abort_on_error=1:log_path='${reports}/${test}'")
    set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY
      FIXTURES_REQUIRED osoe-thread-sanitizer)
  endforeach()
endfunction()
