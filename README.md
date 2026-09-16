# osoe

OpenSHMEM Over Ethernet: an implementation of OpenSHMEM 1.6 for commodity Ethernet, built on io_uring and
designed to build entirely from a Linux distribution's own packages — no fabric
libraries.

**Status:** design and environment work. There is no usable library yet.

## Requirements

- Linux 6.7 or newer
- liburing 2.6 or newer
- CMake 3.25 or newer, pkg-config, and a C11 compiler

Debian 13 (Trixie) and Raspberry Pi OS Trixie provide all of these from their own
repositories. Older releases are not supported, even where the kernel is new
enough: see the design notes, §1.

## Building

Builds use CMake presets and run on Linux only. Each preset builds under
`build/<preset>`, which git ignores:

```sh
cmake --workflow --preset debug      # configure, build, and test
```

Presets: `debug`, `release` (optimized, with debug info), `asan`
(AddressSanitizer and UndefinedBehaviorSanitizer), and `tsan` (ThreadSanitizer).

The `env.probe` test checks that the running kernel provides every io_uring and
futex facility osoe relies on. If it fails, nothing else will work on that
machine.

### Developing on macOS

io_uring is Linux-only, so development on a Mac happens in a Lima VM defined in
this repository. The presets are disabled on macOS and refuse to run there.

```sh
brew install lima
dev/lima/create.sh                                 # Debian 13 VM named osoe
limactl shell osoe cmake --workflow --preset debug # run from the repository root
```

The VM mounts the repository at the same path it has on the Mac, so commands run
from the repository root start in the same directory inside the VM, and
`build/debug/compile_commands.json` works with an editor on the Mac.

## Documentation

- [Design notes](docs/design/shmem-ethernet-design.md)
- [Specification reference](docs/reference/README.md)

## License

Not yet chosen. Until a license is added, no rights are granted to use, copy,
modify, or distribute this code.
