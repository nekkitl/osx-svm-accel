# Build, run and rollback

Read [risks](risks.md) first. Commands are run from the repository root on x86_64 macOS. Hardware execution requires a test host with SVM/NPT enabled. Install Xcode Command Line Tools, Python 3, Git, Ninja, pkg-config, GLib and Pixman. Keep their executables/pkg-config files in PATH. QEMU's configure creates its Python environment and resolves pinned subprojects (network access may be required). No firmware, OS images, SDK/KDK or third-party binaries are included in this repository.

## 1. Compile and check

```sh
make provider-experimental
make checks
make qemu
make qemu-checks
```
`provider-experimental` enables both legacy entry and VM-scoped RUN/KICK in `svm-kext/build/experimental/`. It also compiles the optional provider PAUSE and native CR3 capabilities. Both stay off unless requested in QEMU. Plain `make provider` leaves hardware RUN gated. Neither target installs/loads a kext or executes VMRUN. QEMU is checked out at `f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c` into ignored `qemu-src/`, with only the checksummed source overlay applied. Outputs are in `qemu-build/`. When upgrading the October snapshot, move its ignored `qemu-src/` and `qemu-build/` directories aside before building. The overlay script checks the current file hashes and refuses to overwrite a previous overlay.

Optional kernel import audit: set `XNU_KPI_EXPORTS` to a matching export list, then run `python3 svm-kext/audit_kpi_imports.py svm-kext/build/experimental/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe "$XNU_KPI_EXPORTS"` as one command. No custom XNU compilation is required by AMDVM. An export-list audit does not prove binding in the running kernel.

## 2. Sign, record and install manually

Close existing guests and verify their stop/cleanup before replacing a loaded provider.

The following uses an ad-hoc signature for integrity checking. It does not grant trusted kext approval; use an appropriate signing identity and loading policy for your host. This package does not change SIP/AMFI or boot policy.

```sh
codesign --force --sign - svm-kext/build/experimental/AMDVMProbe.kext
codesign --verify --strict --verbose=4 svm-kext/build/experimental/AMDVMProbe.kext
xcrun dwarfdump --uuid svm-kext/build/experimental/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe
shasum -a 256 svm-kext/build/experimental/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe
```
Signing can change SHA-256; record the final artifact after signing. The reference manifest identifies the original hardware-tested artifact separately. Save the rollback directory path printed/assigned below across a reboot.

```sh
backup_dir=$(mktemp -d /var/tmp/amdvm-rollback.XXXXXX)
echo "$backup_dir"
if [ -d /Library/Extensions/AMDVMProbe.kext ]; then
  sudo ditto /Library/Extensions/AMDVMProbe.kext "$backup_dir/AMDVMProbe.kext"
fi
sudo ditto svm-kext/build/experimental/AMDVMProbe.kext /Library/Extensions/AMDVMProbe.kext
sudo chown -R root:wheel /Library/Extensions/AMDVMProbe.kext
sudo chmod -R a+rX /Library/Extensions/AMDVMProbe.kext
sudo kmutil print-diagnostics --bundle-path /Library/Extensions/AMDVMProbe.kext
sudo kmutil load -p /Library/Extensions/AMDVMProbe.kext
```
If macOS requests approval, approve this exact extension in System Settings and reboot when requested. Copying a bundle alone does not update the Auxiliary Kernel Collection. After reboot, compare the loaded UUID with your candidate:

```sh
kmutil showloaded -b com.nekkit.amdvm.probe --show loaded
xcrun dwarfdump --uuid /Library/Extensions/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe
sudo svm-kext/build/experimental/amdvm-probe
sudo svm-kext/build/experimental/amdvm-probe --vm-negotiate
```
These probe commands do not execute VMRUN. Stop if UUID/ABI negotiation does not match or loading reports unresolved symbols. Rebuild/resolve the reported load error before guest entry. SDK/KDK versions and actual kernel exports can vary across hosts.

## 3. Choose your guest and resources

Use your own install ISO and QCOW2 disk. This example selects 2 cores and 256 MiB; you can choose 1–8 vCPUs and 256 MiB–16 GiB within the public policy. The provider may reject memory preparation if resources/types are unsuitable. Set `confirm-no-kdp-watchpoints` only when that condition is known to hold.

```sh
./qemu-build/qemu-img create -f qcow2 guest.qcow2 8G
# Set VM_ISO to the ISO you selected before running this command.
: "${VM_ISO:?Set VM_ISO to your installation ISO}"
QMP_SOCKET=/tmp/amdvm-qmp-$$.sock
sudo ./qemu-build/qemu-system-x86_64 \
  -accel amdsvm,confirm-experimental-run=on,confirm-no-kdp-watchpoints=on \
  -machine pc -m 256M -smp cpus=2,sockets=1,cores=2,threads=1 \
  -L ./qemu-src/pc-bios -display cocoa \
  -drive file=guest.qcow2,format=qcow2,if=virtio \
  -cdrom "$VM_ISO" -boot once=d \
  -vga none -device virtio-gpu-pci,xres=1280,yres=800 \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -qmp unix:"$QMP_SOCKET",server=on,wait=off
```
For subsequent disk boots, omit `-cdrom` and `-boot once=d`. Start with a disposable image or overlay and consult the [recorded guest profiles](hardware.md). The example assumes VirtIO drivers. Guest boot uses the normal QEMU firmware and device configuration.

## 4. Observe stop and cleanup

In another terminal, use the same recorded QMP socket path. Prefer guest shutdown for normal use. For a controlled pause/teardown:

```sh
sudo python3 tools/qmp.py --socket "$QMP_SOCKET" status
sudo python3 tools/qmp.py --socket "$QMP_SOCKET" stop
sudo python3 tools/qmp.py --socket "$QMP_SOCKET" quit
sudo svm-kext/build/experimental/amdvm-probe
```
The helper rejects quit on a running VM and verifies paused after stop. If stop fails or times out, preserve diagnostics; a forced process kill is not a recovery guarantee. Compare the post-run SVM MSRs with your pre-run snapshot and check quarantine counters are zero. This checks named properties, not complete host register restoration.

## 5. Roll back the provider

Close guests and verify their stop/cleanup first. If a previous bundle was saved, restore it using the recorded `backup_dir`:

```sh
sudo ditto "$backup_dir/AMDVMProbe.kext" /Library/Extensions/AMDVMProbe.kext
sudo chown -R root:wheel /Library/Extensions/AMDVMProbe.kext
sudo chmod -R a+rX /Library/Extensions/AMDVMProbe.kext
sudo kmutil load -p /Library/Extensions/AMDVMProbe.kext
```
Follow any approval/reboot request and verify the restored loaded UUID. If there was no previous provider, use the removal procedure appropriate to your macOS release. Do not keep duplicate bundle identifiers in Extensions.
