#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$root/tools/prepare-qemu.py"
mkdir -p "$root/qemu-build"
cd "$root/qemu-build"
../qemu-src/configure --target-list=x86_64-softmmu --enable-amdsvm \
  --enable-slirp --disable-docs --disable-gtk --disable-sdl "$@"
ninja -j"${JOBS:-4}" qemu-system-x86_64 qemu-img
# Generated Mach-O files on some external volumes acquire Finder metadata.
# Remove that metadata from build outputs before strict signature verification.
xattr -c "$root/qemu-build/qemu-system-x86_64" "$root/qemu-build/qemu-img"
codesign --verify --strict "$root/qemu-build/qemu-system-x86_64"
