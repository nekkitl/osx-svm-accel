# Risks and limitations

This is experimental kernel and VMM code, verified on one reference host. Host kernel panic, a hung CPU and data loss remain possible. Use a test host, backups and disposable guest disks. No warranty is provided.

## What the checks establish

Unsupported guest state, memory layouts and VMEXITs are rejected. Reference runs compare the recorded VM_CR, EFER.SVME and VM_HSAVE_PA values on all 16 host logical CPUs and check quarantine counters. Memory preparation validates host MTRR/PAT/SYSCFG controls. These checks do not capture every host register, prove full XSTATE/debug-state restoration, or establish a hardened sandbox. A build, mock or no-RUN check does not prove hardware execution.

## Execution and stop

`confirm-experimental-run=on` explicitly admits experimental guest execution. `confirm-no-kdp-watchpoints=on` is the operator's confirmation that KDP hardware watchpoints are inactive; do not set it when that condition is unknown. Physical interrupt exits, QEMU kick and host rendezvous have passed controlled tests. There is no universal worst-case stop latency guarantee. A previous TLB shootdown timeout caused a host panic. The saved RIP alone did not prove whether guest duration, GIF masking or a host critical section caused it.

Use QMP `stop` and verify `query-status` reports `paused` before controlled teardown. A guest error followed by `shutdown` does not prove a successful kick. Closing a window, QMP quit or killing QEMU cannot guarantee recovery from a stuck kernel RUN. Preserve diagnostics if stop is unconfirmed.

## Compatibility and resources

- Public RUN policy: 1–2 vCPUs, 256 MiB–4 GiB, no CPU hotplug. RAM pinning can still fail because of host resources or unsupported memory types.
- BIOS PC is the reference machine profile. Other firmware, devices and guest OSes may encounter unsupported instructions, MSRs, MMIO or interrupt paths.
- Short two-core tests have passed. Long stability tests and performance characterization have not been completed. See [matrix](hardware.md).
- macOS kernel-export dependencies can change across releases. Local export list edits do not add symbols to the running kernel; verify actual loading.
- Signing and loading depend on the host's existing security configuration. An ad-hoc signature verifies integrity but does not establish trusted kext approval. This package does not alter SIP, AMFI or boot policy.
- Quarantine/rollback retain resources after unsafe cleanup failures; some failures may require a host reboot. NPT isolation is not a mature security boundary for untrusted workloads.
- Migration, nested virtualization, GPU passthrough and transparent HVF or Virtualization.framework compatibility are not claimed.
