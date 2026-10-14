#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
for name in firmware-dispatch memory-map memory-types msr-exit provider-lifecycle vm-provider; do
  ninja -C "$root/qemu-build" "tests/unit/test-amdsvm-$name"
  "$root/qemu-build/tests/unit/test-amdsvm-$name"
done
"$root/qemu-build/qemu-system-x86_64" -accel help
