---
name: Hardware result or runtime failure
about: Report a hardware test, guest failure, hang or host panic
title: ''
---

<!-- English or Russian is welcome. Use unknown / not checked for missing information. Remove secrets and private paths before posting. -->

## Result and reproduction

- Expected behavior:
- Observed behavior and first failure:
- Steps to reproduce:
- Evidence stage: source / build / no-RUN prelaunch / hardware VMRUN

## Host

- CPU model:
- Motherboard and BIOS/AGESA version:
- Host RAM:
- macOS version and build:
- Existing kernel configuration or patches relevant to virtualization:

## Artifact identities

- Repository revision:
- Built candidate kext UUID and SHA-256:
- Installed kext UUID and SHA-256:
- Actually loaded kext UUID:
- QEMU upstream base commit:
- QEMU binary SHA-256:

## Guest and command

- Guest OS/version and kernel:
- Firmware and image source/version:
- Requested RAM:
- Requested CPU topology (sockets / cores / threads):
- Actual online guest CPUs (attach guest output if available):

```sh
# Exact QEMU command, with private paths and secrets redacted.
```

## Stop, cleanup and host state

- Stop requested and observed QMP state / latency:
- Guest shutdown and QEMU exit result:
- Cleanup / quarantine result:
- Before/after host SVM MSR checks and CPU count covered:
- Host responsiveness, hang or panic:
- TCG control with the same configuration, if performed:

## Logs

Attach the first relevant error, QEMU stderr, QMP results and panic report if applicable. State which checks were not performed. A VM in shutdown after an error does not prove that stop/kick worked. Do not upload guest disks or installation media.
