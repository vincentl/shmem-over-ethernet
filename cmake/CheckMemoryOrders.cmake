# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Design §3.16's rule, checked: every atomic access in the library names its
# order, and the order is one of the three the project uses.
#
#   cmake -DSOURCE_DIR=<repository root> -P CheckMemoryOrders.cmake
#
# The rule had nothing under it. Step D of the team redesign added 37
# `memory_order_relaxed` accesses and one `atomic_load` with no order, each on a
# protocol word, and all four presets passed them; the review of its merge
# point found them (docs/reviews/archive/phase1-step7-slice3-correctness.md,
# G2), each ordered by another word, so that what was false was the rule. Now
# a source that breaks it fails the suite, as an include across the backend
# boundary does (CheckBoundary.cmake).
#
# Over shmem/src and base, every .c and .h, it fails on:
#   - `memory_order_relaxed`, `memory_order_consume` or `memory_order_acq_rel`
#     anywhere, comments included - the library names `acquire`, `release` and
#     `seq_cst` and nothing else (§3.16, "Three of the six orders");
#   - a C11 atomic call without `_explicit` - `atomic_load`, `atomic_store`,
#     `atomic_exchange`, `atomic_compare_exchange_strong` and `_weak`, the
#     `atomic_fetch_` family, `atomic_flag_test_and_set` and `atomic_flag_clear`
#     - matched as a whole identifier, so that OpenSHMEM's own
#     `shmem_long_atomic_fetch_add` and its kin are not.
# What it cannot see is an operator on an `_Atomic` object - `x++`, `x = y` -
# which is sequentially consistent and as forbidden on a protocol word; the
# reviews and the adversarial reads are what catch those.

set(violations "")
set(checked 0)
file(GLOB_RECURSE sources LIST_DIRECTORIES false
     "${SOURCE_DIR}/shmem/src/*.c" "${SOURCE_DIR}/shmem/src/*.h"
     "${SOURCE_DIR}/base/*.c" "${SOURCE_DIR}/base/*.h")
# Whole files, matched as one string: file(STRINGS) would split them at a
# non-ASCII byte and a list at a ';' outside brackets, so a line's number is
# not reported - the file and what was found are.
foreach(path IN LISTS sources)
  file(RELATIVE_PATH where "${SOURCE_DIR}" "${path}")
  if(where MATCHES "/tests/")
    continue()
  endif()
  math(EXPR checked "${checked} + 1")
  file(READ "${path}" text)
  string(REGEX MATCHALL "memory_order_(relaxed|consume|acq_rel)" orders "${text}")
  string(REGEX MATCHALL "[^A-Za-z0-9_]atomic_(load|store|exchange|compare_exchange_strong|compare_exchange_weak|fetch_[a-z]+|flag_test_and_set|flag_clear)[ \t]*\\(" calls "${text}")
  if(orders OR calls)
    list(APPEND violations "${where}: ${orders} ${calls}")
  endif()
endforeach()

if(checked EQUAL 0)
  message(FATAL_ERROR "no library source found under ${SOURCE_DIR}: nothing was checked")
endif()
if(violations)
  list(JOIN violations "\n  " report)
  message(FATAL_ERROR "atomic accesses that do not name acquire, release or seq_cst "
                      "(design §3.16; cmake/CheckMemoryOrders.cmake):\n  ${report}")
endif()
message(STATUS "${checked} library files, every atomic access naming acquire, release or seq_cst")
