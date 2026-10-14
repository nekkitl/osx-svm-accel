# QEMU source overlay

Base: `f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c` from <https://github.com/qemu/qemu>. [manifest.json](../../qemu/manifest.json) records every exported path, upstream SHA-256 (null for additions), and publication SHA-256. The 40-file overlay includes all tracked modifications and AMDVM additions from the reference checkout. The full tree is not vendored.

`tools/prepare-qemu.py` checks the exact Git HEAD, rejects unrelated tracked changes and validates all original/overlay file hashes before copying. It supports idempotent reapplication. `tools/build-qemu.sh` configures Darwin x86_64, amdsvm, Cocoa and SLIRP. Downloaded source/build directories are ignored.

| Files / area | Purpose |
| --- | --- |
| `accel/amdsvm/amdsvm.c` | accelerator registration, generic experimental profile, workers, RUN/KICK, dispatch and VM memory-map lifecycle |
| `amdsvm-vm-provider.c/.h` | VM-scoped provider transport, capabilities, per-vCPU handles/generations, map and destroy |
| `amdsvm-provider-lifecycle.c/.h` | legacy single-vCPU lifecycle used by old diagnostic paths |
| `amdsvm-firmware-dispatch.c/.h` | validated exit classification and emulation callbacks |
| `target/i386/amdsvm/amdsvm-state.c/.h` | architectural CPU/XSTATE mapping, INIT/SIPI, emulation of intercepted operations and MMIO |
| `amdsvm-event.h`, `amdsvm-msr.h` | retained guest events and guest MSR models |
| `amdsvm-memory*` | FlatView collection, ROM aliases, ABI maps and MTRR-derived memory types |
| `target/i386/emulate/*`, `target/i386/hvf/*` | share/reconcile x86 emulation helpers; preserve upstream notices |
| `hw/display/vga.c`, `system/ioport.c` | bounded device-I/O diagnostics retained from reference development |
| root/target/accel Meson, Kconfig, option files, `svm.h` | registration, IOKit dependency and build integration; pinned SLIRP fallback |
| `tests/unit/test-amdsvm-*`, unit Meson | six offline test programs |

Public frontend differences from the hardware reference binary: replace `confirm-alpine-smp` with `confirm-experimental-run`, select the VM API with that flag, and admit 1–2 vCPUs / 256 MiB–4 GiB rather than two exact RAM sizes. World-switch, exit handling and the AP HLT-latch fix are unchanged. The broader admission range and other guest OSes are not established by the earlier tests.

No boot payload, ISO path or guest kernel command line is selected by this frontend. The explicit finite-ROM byte matchers remain diagnostic test modes. See [build and run](build-and-run.md) and [manifest](../../MANIFEST.json).
