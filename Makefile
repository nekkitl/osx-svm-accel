.PHONY: help provider provider-experimental checks qemu qemu-checks
help:
	@echo 'Targets: provider (no RUN), provider-experimental, checks (offline), qemu, qemu-checks'
provider:
	$(MAKE) -C svm-kext all
provider-experimental:
	$(MAKE) -C svm-kext experimental-candidate
checks:
	$(MAKE) -C svm-kext offline-checks
qemu:
	sh tools/build-qemu.sh
qemu-checks:
	sh tools/check-qemu.sh
