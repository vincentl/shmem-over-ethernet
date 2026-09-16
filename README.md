# eoshmem

An implementation of OpenSHMEM 1.6 for commodity Ethernet, built on io_uring and
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

## Documentation

- [Design notes](docs/design/shmem-ethernet-design.md)
- [Specification reference](docs/reference/README.md)

## License

Not yet chosen. Until a license is added, no rights are granted to use, copy,
modify, or distribute this code.
