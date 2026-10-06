# SPDX-License-Identifier: LGPL-3.0-or-later
# The tests a job of more than one node needs, and how the two builds tell
# them apart (v0.2 step 1).
#
# A test that runs a job of more than one logical node - over a node map, with
# osoe-launch -m or OSOE_NODE_MAP, or in tier 3's network namespaces - or that
# builds or links the internode backend's own files or liburing, carries the
# label `internode`. So `ctest -L internode` on the full build runs exactly the
# tests the single-node build lacks, and `ctest -LE internode` the ones both
# run.
#
# In the single-node build (OSOE_SINGLE_NODE) such a test is not registered at
# all, and its program not built: each sits in a block `if(OSOE_INTERNODE)`, a
# configure-time choice, so that ctest sees only what can run there.
if(OSOE_SINGLE_NODE)
  set(OSOE_INTERNODE OFF)
else()
  set(OSOE_INTERNODE ON)
endif()

# What a program built from the library's own sources links beside them: the
# full build's liburing, and nothing in the single-node build, whose sources
# need none (the white-box tests of shmem/tests/CMakeLists.txt, which take
# the library's sources from its target and so the build's own).
if(OSOE_SINGLE_NODE)
  set(OSOE_LIBURING_LINK "")
else()
  set(OSOE_LIBURING_LINK PkgConfig::LIBURING)
endif()

# osoe_internode_label(<test>...): add the label to tests already added,
# after whatever set their LABELS, which replaces the list.
function(osoe_internode_label)
  foreach(test IN LISTS ARGN)
    set_property(TEST ${test} APPEND PROPERTY LABELS internode)
  endforeach()
endfunction()

# osoe_map_nodes(<map> <out>): how many logical nodes a node map such as
# "0,0,1,2" names - its distinct entries.
function(osoe_map_nodes map out)
  string(REPLACE "," ";" ranks "${map}")
  list(REMOVE_DUPLICATES ranks)
  list(LENGTH ranks n)
  set(${out} ${n} PARENT_SCOPE)
endfunction()
