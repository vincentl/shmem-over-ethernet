# 2026-10-10: the single-node build configures again, and the full build checks it

Found while making `shmem.helper-wake` wait for an owner's sleeping flag: from
the grant fields' merge (`fcb9cf0`) the single-node build did not configure.

## What it was

- **The line:** the grant fields made `shmem.helper-wake` run alone, with
  `set_tests_properties(shmem.helper-wake.0-1-2-3 PROPERTIES RUN_SERIAL TRUE)`
  in a block guarded against ThreadSanitizer only
  (`shmem/tests/CMakeLists.txt:1621` at `7a42c92`).
- **Why it refused the single-node build:** the test is a job of four nodes,
  which `osoe_tier2_map_test` does not register where `OSOE_INTERNODE` is off,
  and CMake refuses a property on a test that does not exist: "CMake Error at
  shmem/tests/CMakeLists.txt:1621 (set_tests_properties): set_tests_properties
  Can not find test to add properties to" - the one error of a configure of
  `main` at `7a42c92` with `-DOSOE_SINGLE_NODE=ON`.
- **Why nothing saw it:** the four presets are the full build's, and the full
  build's suite never configures the other one. The merge point's final check
  built the single-node build at `8e36752`, before the line; the grant fields'
  verification and the step 4 verification script build the four presets
  only.

## What was built

- **The guard:** the block registers `shmem.helper-wake` and sets its property
  only `if(OSOE_INTERNODE AND NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")`.
- **A check in the full build:** `shmem.single-node.configure` (tier 1,
  `cmake/CheckSingleNodeConfigure.cmake`) configures and generates the
  single-node build of the same tree in a scratch directory, of the caller's
  own kind - its build type, its C and linker flags, its shared or static
  library and its timing pins, so each preset configures its own single-node
  counterpart - and fails on any CMake error. Configure only: compiling and
  running the single-node build stays its own suite's, about two minutes and
  404 tests, on the machine that runs it. Not `OSOE_TIER3`, which the
  single-node build refuses by design.
- **The README:** the `cmake/` row names the check; the full build has 858
  tests, and the single-node build runs 399 of them - all but the 458 labelled
  `internode` and this check - and five of its own.

## The verification

*To be filled in from `build/scratch/ladder/merge-point/single-node-configure-check/`.*

*Undo:* the guard back to ThreadSanitizer's alone, the check and its script
removed, the README's counts and row back.
