# Performance and the provider ABI

[Русский](../ru/performance-abi.md)

The current focus is reducing the time the virtual machine monitor (VMM) spends servicing guest execution. The main places to measure are synchronous IOKit calls, VMEXIT dispatch, repeated state copies, NPT map updates and reacquisition of QEMU's Big QEMU Lock (BQL). The [runtime record](../../evidence/reference-2026-11-17/README.md) contains the observed results.

## What the ABI does today

- The kernel provider owns the CPU's SVM state, manages the VMCB and NPT, executes `VMRUN` and restores host state. The world switch keeps host and guest XSTATE in separate kernel buffers. QEMU receives guest XSTATE through the negotiated state ABI.
- QEMU owns the guest CPU model, instruction emulation and devices. An exit that needs device or x86 emulation returns to QEMU.
- The diagnostic CPUID/MSR probe and the VM API are separate. The probe has ABI version 13. VM control uses interface version 2 and can negotiate x86 state version 3. The declarations live in [amdvm_probe_abi.h](../../svm-kext/amdvm_probe_abi.h) and [amdvm_vm_abi_draft.h](../../svm-kext/amdvm_vm_abi_draft.h).
- RUN batches the request, x86 state and pending event. Its result carries the exit and updated state. With a provider that advertises shared RUN, QEMU can map the shared area for request and result data. Each RUN still uses a synchronous IOKit selector. The November E71D archive was built without the optional shared RUN capability.

## Rules for reducing overhead

1. **Batch state and version extensions.** Keep register transfer, event injection and entry in one request. Capability negotiation determines whether the caller uses an optional path. A change to an existing wire layout needs a compatible fallback.
2. **Keep kernel fast paths narrow.** Handle a local exit in the provider only when its semantics are defined and isolation and host restoration are preserved. Retain intercepts for operations that require QEMU device behavior or whose replacement has not been validated. Host MSRs and I/O remain inaccessible to the guest.
3. **Use larger NPT pages when backing permits it.** A 2 MiB or 1 GiB mapping needs aligned, pinned, physically contiguous backing with consistent permissions and memory type. MMIO, holes and mapping boundaries use finer mappings or remain unmapped as appropriate. Treat large pages as a design option.
4. **Tie ASID reuse to TLB validity.** Track CPU and generation. Account for ASID reuse, vCPU migration and NPT changes before reusing a cached translation. Use targeted invalidation only where the architecture and mapping lifetime allow it.
5. **Measure interrupt and device paths separately.** Profile APIC activity, event injection, IRQ delivery and virtio notifications independently. Queue sharing and notification batching are design options that still need correctness and workload measurements.

## Measuring a change

Hold the guest image, firmware, vCPU and RAM configuration, workload phase and measurement window constant. Change one option per comparison. Record guest throughput or latency alongside RUN/s, exit counts by class, provider call time, BQL reacquisition, memory-map refresh and guest progress. Report whether the guest stopped cleanly. In an SMP run the summed vCPU counters can exceed wall time.

Use `perf-stats=on` for aggregate QEMU counters and keep `trace-vmexit=off` for timing comparisons. Repeat guest latency or throughput measurements with instrumentation disabled to check its effect. A lower counter shows where time moved. An end-to-end speedup needs a matching improvement in the guest. Compare against QEMU TCG or VirtualBox SVM where the guest setup and workload can be matched, and report any differences in devices or firmware.
