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
# aarch64 VM): round trip 1.01-1.05x idle and 1.08-1.19x under the load loop's
# 4x oversubscription, stream 0.89-1.06x idle. Under load, three trials were
# not enough: the stream dipped to 0.68x once in 40 runs while a trial was
# 256 MiB (13 ms, shorter than a scheduler slice among 16 busy loops), and the
# round trip reached 3.4x twice in about 90, when all three io_uring trials
# landed in the slow one of the two placements a loaded machine gives both
# kinds (about 15 us against 5). So it is seven interleaved trials of 1 GiB,
# best of each, and a round trip more than 1.5x the blocking one, or a stream
# below 0.5x of it, is a regression of the io_uring path rather than noise. A
# 50 us pause in the pump measured 12x and 0.05x; a missing re-arm after
# -ENOBUFS hung. The pins are checked only in an optimized build without a
# sanitizer (the release preset): under asan and tsan the io_uring round trip
# measured 6.4x and 3.9x, which is the instrumentation of the harness's own
# loop, not the link. Every preset still runs the verified stream.
add_test(NAME env.echo-uring
  COMMAND echo-uring --rounds 3000 --mebibytes 1024 --check 1.5 0.5)
set_tests_properties(env.echo-uring PROPERTIES LABELS "env" TIMEOUT 120)
