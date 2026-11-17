# Publication snapshot — 2026-11-17

- Refresh the stock-XNU provider and the QEMU 11.1.50 overlay from the v5 development tree, using the same pinned upstream QEMU revision.
- Share the VM NPT map across vCPUs. Retain per-vCPU CPU state and use scatter-backed NPT allocation for larger maps. Preserve the ordered host page vector and reuse the current NPT leaf table during sequential mapping.
- Add x86 state ABI v3 negotiation, shared RUN transport, tighter RUN lifetime accounting, reusable buffers, and conservative VMCB clean-bit and TLB handling.
- Update the QEMU string-I/O state synchronization and no-entry KICK result handling. Add optional RUN counters, bounded BQL reacquisition, and capability checks for native CR3, RDTSC, PAUSE and MTRR caching.
- Expand the public experimental profile to 1–8 vCPUs and 256 MiB–16 GiB, with CPU hotplug rejected. Document observed boots, regressions and the TCG controls.
- Package the latest unsigned provider separately. It adds page-fault diagnostics and remains a candidate for hardware evaluation.

Русская сводка и цифры приведены в [документации по замерам](docs/ru/hardware.md).

# Publication snapshot — 2026-10-14

Experimental source package; no production-stability claim.

- Export the current stock-profile XNU provider, client and offline tests. The experimental provider rebuild matches the hardware reference binary.
- Export all 40 QEMU additions/changes against the pinned upstream commit.
- Use guest-independent `confirm-experimental-run`; choose disks, ISO, CPU topology and RAM with ordinary QEMU arguments. Public admission is 1–2 vCPUs and 256 MiB–4 GiB; no CPU hotplug.
- Include the AP INIT/SIPI HLT-latch fix and per-vCPU trace budgets.
- Record separate 4-GiB Alpine boot, finite 2-vCPU/4-GiB ROM, and successful 2-core/256-MiB Alpine workload/stop/cleanup observations.
- Correct the stale diagnostic-result layout test (128 to 160 bytes), matching the existing ABI header; no ABI or production-provider change for this fix.
- Add checksum-verified QEMU preparation, offline build/check targets and a QMP helper that requires confirmed pause before quit.
- Update English/Russian guides, retain the development story, and move the detailed future AMD-HVF discussion into a separate research proposal.
