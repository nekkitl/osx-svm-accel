# AMDVMProbe provider and diagnostic client

`make` builds a stock-profile no-RUN provider and `build/amdvm-probe`. `make experimental-candidate` builds entry-enabled artifacts under `build/experimental/`. These targets only compile; they never sign, install, load or execute the extension. `make offline-checks` uses the adjacent `tools/` and exported QEMU helper headers; retain the full repository layout.

Bundle identifier: `com.nekkit.amdvm.probe`. IOKit service: `AMDVMProbeService`. Versioned legacy and VM-scoped interfaces are in `amdvm_probe_abi.h` and `amdvm_vm_abi_draft.h`. The historical `draft` names are part of the existing source/API vocabulary; selectors implemented in this snapshot are used by the QEMU VM provider.

The provider has no guest-OS selection. See the root [build guide](../docs/en/build-and-run.md), [architecture](../docs/en/architecture.md) and [risks](../docs/en/risks.md) before considering hardware use.
