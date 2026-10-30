# SPDX-License-Identifier: LGPL-3.0-or-later
# What names a version is made from the one value project(osoe VERSION ...)
# holds, in the top-level CMakeLists.txt.
#
# Every target is compiled with that value as the macro OSOE_VERSION_STRING, a
# string literal such as "0.2.0": the library's start-up line and SHMEM_INFO's
# text print it, and so does each tool's --version. It is set once, here, for
# every target, rather than target by target: the library's own sources are
# also compiled into white-box tests, each of which would have to remember it.
# SHMEM_VENDOR_STRING (shmem.h) stays "osoe": it is a name, as other
# implementations' are, and the version is a separate fact. oshcc, a script,
# takes the value from its template instead (tools/wrapper/oshcc.in), and the
# shared library's VERSION and SOVERSION come from PROJECT_VERSION
# (shmem/CMakeLists.txt).
#
# The single-node build says so after the version, wherever the version is
# printed - "0.2.0 (single-node build)" - so that a user can tell from any
# tool's --version, SHMEM_INFO or the start-up line which build is installed
# (OSOE_SINGLE_NODE, the top-level CMakeLists.txt). OSOE_VERSION_TEXT is that
# text; the library's file name and soname keep the bare version.
set(OSOE_VERSION_TEXT "${PROJECT_VERSION}")
# And what SHMEM_INFO adds in that build: which of the settings it lists do
# nothing there (the v0.2 step 1 review, C3). A string, as the version is, so
# that the two builds differ by a value and not by a branch.
set(OSOE_BUILD_NOTE "")
if(OSOE_SINGLE_NODE)
  string(APPEND OSOE_VERSION_TEXT " (single-node build)")
  set(OSOE_BUILD_NOTE "  This is the single-node build: it runs a job of one node, and checks\\n  OSOE_PORT_RANGE, OSOE_JOB_SECRET, OSOE_KEEPALIVE, OSOE_PROGRESS_HELPERS,\\n  OSOE_COLLECTIVE_SHAPE and the OSOE_LINK_ settings as values only - each sets\\n  the connections between nodes, the threads that serve them or a large\\n  collective's steps between nodes, none of which a job of one node has.\\n\\n")
endif()
add_compile_definitions(OSOE_VERSION_STRING="${OSOE_VERSION_TEXT}"
                        OSOE_BUILD_NOTE="${OSOE_BUILD_NOTE}")

# The same text as a regular expression, for a test that compares what a
# program prints with what CMake knows: a dot or a parenthesis in it matches
# only itself.
string(REGEX REPLACE "([.()])" "[\\1]" OSOE_VERSION_PATTERN "${OSOE_VERSION_TEXT}")

# osoe_version_test(<test> <program> <tool> [<arguments>...]): a test that the
# program, run with the arguments (--version without them), prints exactly
# "<tool> (osoe) <version>" - the form every tool's --version takes, the
# version as OSOE_VERSION_TEXT has it - and exits 0.
function(osoe_version_test test program tool)
  set(arguments --version)
  if(ARGN)
    set(arguments ${ARGN})
  endif()
  add_test(NAME ${test}
    COMMAND ${CMAKE_COMMAND} "-DPROGRAM=${program}" "-DARGUMENTS=${arguments}"
            "-DEXPECTED=${tool} (osoe) ${OSOE_VERSION_TEXT}"
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckVersion.cmake)
  set_tests_properties(${test} PROPERTIES LABELS "tier1" TIMEOUT 30)
endfunction()
