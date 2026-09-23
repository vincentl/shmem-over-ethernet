add_executable(echo-uring echo-uring.c)
target_link_libraries(echo-uring PRIVATE PkgConfig::LIBURING)
osoe_warnings(echo-uring)

# §8.4's floor harness, run on loopback. It fails on any io_uring shape §3.3
# relies on that stops working - the small run verifies a stream word by word
# through -ENOBUFS, re-arms and short sends, and a lost re-arm hangs it into
# the timeout - and on the ratio of its io_uring exchanges to the same
# exchanges over blocking send and recv on the same link.
#
# The pins (phase 1 step 3 slice 1, 4-core aarch64 VM) are two ratios, each the
# median over seven pairs of trials measured side by side: the *fastest* round
# trip, io_uring over blocking, at most 1.5x - measured 1.02-1.25x idle, under
# 16 busy loops and beside a ctest -j4 run - and the stream at least 0.5x -
# measured 0.85-1.06x. The fastest round trip is the length of the path; the
# median is where the woken process gets to run, which beside ctest -j4 was
# 3-5x slower for io_uring than for blocking recv (COOP_TASKRUN and
# DEFER_TASKRUN alike; §8.4), so it is reported and not pinned - pinning it
# failed there, as did "best of each kind" before pairing (3.4x and 4.3x on
# single runs). Trials are 1 GiB: a 256 MiB stream lasts 13 ms, shorter than a
# scheduler slice among 16 busy loops, and dipped to 0.68x. A 50 us pause in
# the pump measured 13x and 0.05x; a missing re-arm after -ENOBUFS hung. The
# pins are checked only in an optimized build without a sanitizer (the release
# preset): under asan and tsan the io_uring round trip measured 6.4x and 3.9x,
# the instrumentation of the harness's own loop. Every preset still runs the
# verified stream.
add_test(NAME env.echo-uring
  COMMAND echo-uring --rounds 3000 --mebibytes 1024 --check 1.5 0.5)
set_tests_properties(env.echo-uring PROPERTIES LABELS "env" TIMEOUT 120)
