# dev: the development environment

`lima/` defines the virtual machines osoe is developed in on a Mac, since
io_uring is Linux-only. `osoe.yaml` is the main one's Lima template: Debian 13
with four CPUs, 8 GiB of memory and a 30 GiB disk, and `build-essential`,
`cmake`, `pkgconf`, `liburing-dev` and `iproute2` installed in it;
`osoe-rocky.yaml`, Rocky Linux 9 with no liburing headers, is where the
single-node build is tested. `create.sh NAME` creates and starts a machine from
`NAME.yaml` where there is one and from `osoe.yaml` otherwise - named `osoe`
unless given another name - with this
repository mounted writable at the path it has on the Mac, so that a command run
from the repository root starts in the same directory inside the VM. Nothing in
the build or the library depends on it: a Linux machine with the packages of the
root `README.md`'s Requirements needs none of this. Start with that README's
"Developing on macOS", which gives the commands; `docs/README.md` explains the
virtual machines the records name.
