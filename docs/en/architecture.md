# Architecture

```text
QEMU device model + one worker per guest vCPU
             |
      VM-scoped IOUserClient ABI
             |
 AMDVMProbe.kext: pinned RAM owner / vCPU contexts
             |
    NPT + VMCB + GPR/XSTATE world switch
             |
         AMD SVM / VMRUN
```

The provider accepts x86 architectural state and a guest physical memory map. It does not identify the guest operating system. QEMU retains device emulation, CPUID policy, guest MSR models and VMEXIT dispatch. Unsupported operations produce explicit failures; selecting another ISO does not add missing features.

A connection negotiates legacy single-vCPU mode or the VM-scoped API. The VM owns the pinned memory map; each vCPU has independent CPU and event state while sharing the VM’s NPT map. RUN/KICK generations, admission and ownership prevent simultaneous execution of one vCPU and coordinate teardown with in-flight operations. Memory-map replacement drains siblings before committing the new mapping. These mechanisms have tests and short hardware evidence; broad race/fault safety is not proven.

VMCB carries only part of guest state. Software also transfers other GPRs and separate guest XSTATE. The kernel wrapper preserves the tested host boundary, uses physical interrupt exits, and restores SVM ownership before returning. The host FPU context, CPU migration, IF/GIF and kernel-export constraints are part of the execution contract, not guest-OS special cases.

QEMU handles LAPIC INIT/SIPI through its normal x86 model. An accepted startup clears the accelerator's previous HLT latch; INIT also clears retained guest events. Otherwise a SeaBIOS HLT could block the later Linux AP trampoline.

The public `confirm-experimental-run` profile enables the VM API and admits ordinary `-m`/`-smp` settings within 1–8 vCPUs and 256 MiB–16 GiB. Separate finite-ROM and no-RUN diagnostic modes retain their own checks. Provider capabilities and memory preparation are checked independently of frontend policy.

See [QEMU integration](qemu-integration.md), [test matrix](hardware.md), [risks](risks.md) and the separate [AMD-HVF proposal](amd-hvf-proposal.md).
