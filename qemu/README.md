# QEMU overlay

This directory is not a standalone QEMU checkout. It holds the added/modified files listed in [manifest.json](manifest.json), plus upstream license texts.

From the repository root, run `make qemu`. The preparation script checks out the exact upstream revision into ignored `qemu-src/` and validates original and overlay hashes before copying files. It refuses unrelated modifications. Keep `svm-kext/` beside that checkout: the accelerator includes its ABI headers.

See [integration](../docs/en/qemu-integration.md), [build guide](../docs/en/build-and-run.md) and [public frontend delta](public-interface.diff). The delta explains changes from the hardware reference frontend; it is not the patch to upstream.
