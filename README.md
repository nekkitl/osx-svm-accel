# osx-svm-accel — AMD SVM acceleration for QEMU on macOS

[Русский](docs/ru/README.md) · [Changelog](CHANGELOG.md)

**Experimental snapshot · 2026-11-17**

AMDVM is an experimental AMD SVM provider for x86_64 macOS and a native QEMU accelerator named `amdsvm`. The kernel extension executes guest CPU instructions through SVM; QEMU supplies the virtual machine and device model. Users choose their own disks, installation media, memory and CPU topology through ordinary QEMU options. There is no guest-OS detector or bundled image.

On the Ryzen 7 5700X reference host, earlier AMDVM builds took Alpine to serial login with 8 vCPUs and 4 GiB in 20.221 s and with 8 GiB in 35.629 s, on different build pairs. One older build also reached login at 12 and 16 GiB. Later runs at those sizes and at 1 vCPU / 256 MiB failed before login. See the [performance and compatibility record](evidence/reference-2026-11-17/README.md).

**Windows 10 22H2 completed a full installation and booted from its virtual disk to the desktop under AMDVM.** The recorded desktop boot used 8 vCPUs and 4 GiB on an earlier QEMU/provider pair. See the [Windows run record](evidence/reference-2026-11-17/README.md#windows-10).

**Current focus:** reducing VMM overhead in the QEMU/provider RUN path, including IOKit calls, VMEXIT handling, state transfer and contention on QEMU’s global lock. See [performance and ABI](docs/en/performance-abi.md).

AMDVM requires no additional XNU patches or custom XNU build beyond the existing Hackintosh configuration. It does not use Hypervisor.framework or TCG for guest CPU execution; intercepted operations are emulated in QEMU.

**Research prototype, tested on one host.** Read [risks](docs/en/risks.md). Guest results apply to the recorded builds and configurations.

![Installed Alpine console and package installation](docs/img/alpine-neofetch.png)

The screenshot shows an earlier single-vCPU / 2-GiB run. See the [October 14 evidence](evidence/reference-2026-10-14/README.md) for the original pinned two-core workload.

## Build and use

```sh
make provider-experimental  # build only; no installation or hardware entry
make checks                # offline provider checks
make qemu                  # pinned upstream checkout + source overlay + build
make qemu-checks            # offline QEMU unit tests
```

Requires an Intel/AMD x86_64 macOS build host, Xcode Command Line Tools, Python 3, Git, Ninja, pkg-config, GLib and Pixman. SLIRP is resolved from the system or QEMU's pinned subproject. See [build and run](docs/en/build-and-run.md) for signing, manual installation, verification, rollback and guest commands.

After the matching provider is loaded, select:

```text
-accel amdsvm,confirm-experimental-run=on,confirm-no-kdp-watchpoints=on
```

Use ordinary `-m`, `-smp`, `-drive` and `-cdrom` options. The public frontend accepts **1–8 vCPUs and 256 MiB–16 GiB RAM**, subject to provider capabilities and memory preparation. It rejects CPU hotplug. The paired source and unsigned kext build were checked offline. See the [manifest](MANIFEST.json) for the release build validation and the separate historical runtime results.

Verify the extracted source package with `shasum -a 256 -c SHA256SUMS`.

## Contents

| Path | Contents |
| --- | --- |
| `svm-kext/` | provider, versioned ABI, diagnostic client, mock headers |
| `tools/` | offline tests and QEMU preparation/build scripts |
| `qemu/` | 51 added/modified upstream files and checksum manifest; no full QEMU tree |
| `evidence/` | selected reference results, identities and validation limits |
| `docs/en/`, `docs/ru/` | English and Russian documentation |

## Documentation

- [Architecture](docs/en/architecture.md)
- [QEMU changes](docs/en/qemu-integration.md)
- [Performance and ABI](docs/en/performance-abi.md)
- [Hardware and test matrix](docs/en/hardware.md)
- [Build, run and rollback](docs/en/build-and-run.md)
- [Risks and limitations](docs/en/risks.md)
- [Development method](docs/en/development.md)
- [Project story](docs/en/story.md)
- [AMD-HVF research proposal](docs/en/amd-hvf-proposal.md)

[osx-svm-poc](https://github.com/nekkitl/osx-svm-poc) remains the separate minimal HLT-transition PoC. This repository contains the QEMU integration and current provider source. AMD-HVF is a separate future compatibility study.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for changes, checks and hardware reports. Issue templates cover runtime results, build/loading problems and feature requests.

## License

Original project code: GPL-2.0-or-later. Upstream-derived QEMU files retain their notices and applicable licenses. See [LICENSE](LICENSE), [qemu/COPYING](qemu/COPYING) and [THIRD_PARTY.md](THIRD_PARTY.md).

© 2026 Nick Ognev.
