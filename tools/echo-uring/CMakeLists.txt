add_executable(echo-uring echo-uring.c)
target_link_libraries(echo-uring PRIVATE PkgConfig::LIBURING)
osoe_warnings(echo-uring)

# §8.4's floor harness, run on loopback. It fails on any io_uring shape §3.3
# relies on that stops working - the small run verifies a stream word by word
# through -ENOBUFS, re-arms and short sends, and a lost re-arm hangs it into
# the timeout - and on the ratio of its io_uring exchanges to the same
# exchanges over blocking send and recv on the same link.
#
# The pins were set after the first runs (phase 1 step 3 slice 1, 4-core
# aarch64 VM): round trip 1.01-1.05x idle and up to 1.16x under the load loop's
# 4x oversubscription, stream 0.89-1.06x. What is pinned is the median, over
# seven pairs of trials, of each pair's io_uring/blocking ratio - a pair is
# measured together and shares whatever the machine was doing. On a loaded
# machine a round trip falls into one of three modes (about 4, 15 or 25 us by
# where the two processes land and whether a CPU must wake), and comparing the
# best of each kind instead failed when one blocking trial caught the fastest
# mode and no io_uring trial did: 3.4x twice in about ninety runs under load
# with three trials, 4.3x once under ctest -j4 with seven. Trials are 1 GiB: a
# 256 MiB stream lasts 13 ms, shorter than a scheduler slice among 16 busy
# loops, and dipped to 0.68x. So a round trip more than 1.5x the blocking one,
# or a stream below 0.5x of it, is a regression of the io_uring path rather
# than noise. A 50 us pause in the pump measured 12x and 0.05x; a missing
# re-arm after -ENOBUFS hung. The pins are checked only in an optimized build
# without a sanitizer (the release preset): under asan and tsan the io_uring
# round trip measured 6.4x and 3.9x, which is the instrumentation of the
# harness's own loop, not the link. Every preset still runs the verified stream.
add_test(NAME env.echo-uring
  COMMAND echo-uring --rounds 3000 --mebibytes 1024 --check 1.5 0.5)
set_tests_properties(env.echo-uring PROPERTIES LABELS "env" TIMEOUT 120)
