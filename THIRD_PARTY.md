# Source origins and third-party notices

Original AMDVM provider, client, tools and accelerator additions are offered under GPL-2.0-or-later unless a file carries another notice. Root [LICENSE](LICENSE) contains GPLv2; the "or later" grant applies to original project code. It does not replace upstream notices or change the licenses of inherited files.

## QEMU and x86 emulation

The overlay is based on QEMU commit `f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c`. It includes modified upstream files as well as new accelerator files. Existing copyright and license headers are preserved. See [qemu/COPYING](qemu/COPYING) and [qemu/COPYING.LIB](qemu/COPYING.LIB).

The changed HVF/x86 emulation files contain notices for Veertu, Google and the Bochs Project, including LGPL-2.1-or-later terms. `x86_task.c` carries GPLv2 terms. VGA and I/O-port files carry Fabrice Bellard's permissive notice. Refer to each file for its precise terms; the QEMU overlay is not uniformly relicensed as original AMDVM code. The [manifest](qemu/manifest.json) identifies added paths and the exact upstream originals of changed files.

## Build-time dependencies

GLib, Pixman and libslirp retain their own licenses. They are resolved by the QEMU build and are not vendored by this source package. The SLIRP fallback uses QEMU's `subprojects/slirp.wrap` at revision `26be815b86e8d49add8c9a8b320239b9594ff03d`. Firmware and other subprojects come from the pinned QEMU checkout; none of their binaries are shipped here.

## XNU / SDK / KDK

Apple headers and kernel export information are used on the build/test host. No Apple SDK/KDK, XNU source tree or patched kernel is included. AMDVM's stock profile does not require adding kernel exports to the running XNU.

## VirtualBox architectural reference

VirtualBox 6.1.50 SVM world-switch and memory-type handling were studied as architectural references. Source comments retain those references. No VirtualBox source tree or binary is included. This records the research origin; it is not an assertion that VirtualBox tests establish AMDVM correctness or a formal independent provenance audit of every generated line.

## Guest software and trademarks

Alpine/Linux appear in reference results and a screenshot; no guest ISO, disk, kernel or rootfs is included. AMD, Ryzen, macOS, QEMU, VirtualBox and Alpine names belong to their respective owners. This is an independent project.
