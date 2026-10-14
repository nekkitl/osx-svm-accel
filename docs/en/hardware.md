# Hardware and recorded results

Reference host: Ryzen 7 5700X (8 cores / 16 logical CPUs), macOS Tahoe 26.0.1, build 25A362, x86_64. AMDVM uses the existing Hackintosh kernel configuration without an additional AMDVM-specific XNU patch or custom kernel build. Only this host has been verified. SVM, NPT and firmware enablement are required.

Recorded host memory controls: SYSCFG `0x740000`, TOP_MEM `0xd0000000`, TOP_MEM2 `0x830000000`; 8 variable MTRRs (4 active), 48 physical address bits. These describe this host, not universal settings to write on another machine.

## Matrix

| Scenario | Result and scope |
| --- | --- |
| CPUID/SVM MSR query across 16 host CPUs | PASS |
| No-RUN RAM/NPT preparation, state roundtrip and destroy | PASS |
| Fixed HLT and CPUID → OUT → HLT ROMs | PASS |
| Alpine 3.24 / 1 vCPU / 512 MiB, 1 GiB, 2 GiB | interactive boot recorded |
| Alpine / 4 GiB with 2 configured vCPUs, before AP fix | booted; CPU1 failed alive handshake, only CPU0 online |
| Finite SMP ROM / 2 vCPU / 4 GiB | both vCPUs executed; stop and host rendezvous PASS |
| Alpine / 2 cores / 256 MiB, after AP fix | online=0-1, nproc=2, both pinned finite workloads PASS |
| Same 2-core / 256-MiB disk boot under TCG | both cores online; control experiment |
| Fixed SVM SMP run stop + poweroff + quit | paused in 1.626 ms; recorded SVM MSRs restored across 16 CPUs, quarantine zero |
| 4 guest cores / 8 GiB, other guest OSes, migration/hotplug | not established |

The latest SMP test used `pc`, one socket, two cores, one thread per core, VirtIO block/network/GPU, 1280×800 display and SLIRP NAT. Guest kernel: `6.18.55-0-virt`; an earlier session used `6.18.52-0-virt`. No additional guest kernel arguments were injected. See [reference evidence](../../evidence/reference-2026-10-14/README.md).

The publication frontend accepts 1–2 vCPUs and 256 MiB–4 GiB. This is an admission policy, not verification of every configuration. The provider's internal 64-slot ABI capacity is not a demonstrated 64-vCPU guest capability. The generic frontend differs from the recorded hardware binary in its public RUN profile and resource admission. Its build/offline checks are recorded separately in [MANIFEST.json](../../MANIFEST.json).

## Hardware reports

Include CPU model, motherboard/AGESA, host RAM, macOS version/build, kext UUID and SHA-256, QEMU base commit and binary hash, command line, guest version, requested resources, actual guest online CPUs, first failure and stop/cleanup result. Remove credentials and private host paths from shared logs.
