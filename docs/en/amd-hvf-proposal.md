# AMD-HVF — research proposal

## Why a working VMM still doesn't fully solve the AMD-OSX problem

At this point one could stop and focus on optimizing AMDVM, improving stability and expanding guest compatibility. Moreover, these tasks never go away regardless. But the original AMD-OSX problem was somewhat broader than just the lack of hardware acceleration in my QEMU.

The thing is that AMDVM provides its own backend, and using it requires a corresponding integration. In our case that is a separate QEMU accelerator — `amdsvm`. However, most macOS applications that use the system's hardware virtualization facilities know nothing about this interface.

They work through Hypervisor.framework or higher-level Apple mechanisms and check the availability of a system-supported virtualization option. For them, an AMD Hackintosh remains an unsupported platform even though hardware AMD SVM now genuinely works inside XNU.

The situation is quite ironic. I wrote my own VMM, booted Linux and proved that Ryzen can execute virtual machines in hardware under macOS. But applications designed for the standard Apple interface still cannot automatically take advantage of this possibility.

That is where the idea for the next project appeared.

**AMD-HVF Compatibility Layer.**

## AMD-HVF: can AMD SVM be made compatible with Hypervisor.framework?

The idea is not to teach an AMD processor to execute Intel VMX instructions. These are different hardware architectures, and simply swapping CPUID will not turn a VMCB into a VMCS. Nor do I consider directly patching the system Hypervisor.framework an obvious solution: on modern macOS such an approach quickly runs into system protection mechanisms, signing, library loading and closed components.

Much more interesting is trying to implement a compatible software layer that provides applications with the familiar x86 Hypervisor.framework interfaces while actually routing operations into the existing AMDVM.

In this architecture AMDVM remains an independent hardware backend. It continues to be responsible for VMRUN, VMEXIT, memory, processor state and interaction with XNU. Above it appears a separate component that implements the compatible API, translating application calls into operations of the existing provider.

Conceptually it looks like this:

```text
x86 Application / QEMU HVF
             |
             v
AMD-HVF Compatibility Layer
             |
             v
       AMDVM IOUserClient
             |
             v
       AMDVMProbe.kext
             |
             v
          AMD SVM
```

Part of the functionality already has fairly direct correspondences. Creating a virtual machine can be mapped to creating an AMDVM context. Mapping guest memory — to pinning userspace pages and building the NPT. Reading and writing architectural registers — to the existing CPU state transfer mechanism. And the vCPU execution call — to the already working RUN, which ultimately performs a hardware VMRUN.

But these are only the basic operations. The real difficulty begins where the Apple interface directly reflects Intel VMX specifics.

In particular, the x86 Hypervisor.framework provides work with the VMCS and Intel processor control capabilities. Existing programs, including QEMU's HVF backend, may request capability masks, read and write VMCS fields, handle Intel VMEXIT reasons and rely on a certain virtual interrupt model.

AMD SVM is structured differently. Some capabilities have close hardware analogues, others differ in semantics, and still others will require software emulation or cannot be provided at all without significant limitations.

Therefore the task of AMD-HVF is not simply to redirect a `hv_vcpu_run()` call into `VMRUN`. It will need a virtual VMCS model capable of representing the Intel VMX state an application expects and reconciling it with the hardware VMCB.

For example, when an application requests the reason for exiting virtualization, it must receive a result in the format and with the semantics matching its VMX expectations. Meanwhile the real AMD processor may report a completely different event code. Similarly, control fields, exceptions, virtual interrupts and memory capabilities will have to be reconciled.

And here it is not enough to simply translate numeric values. A correct compatibility layer must reproduce the behavior of the interface, including errors, limitations and valid state transitions. Otherwise you get a library that passes simple tests but starts misbehaving on a real virtual machine.

Managing the lifecycle of virtual processors will become a separate direction. Hypervisor.framework imposes its own requirements on vCPU creation, execution threads and interrupting work. The fact that AMDVM can already perform a controlled RUN and react to host IPIs provides the necessary foundation, but does not mean automatically fulfilling the whole Apple contract.

The same applies to guest multiprocessing. Having 16 logical host CPUs and a successful `mp_rendezvous()` does not yet prove full support for 16 guest vCPUs. Separate research will be needed into the virtual APIC, guest inter-processor interrupts, time synchronization and vCPU-to-thread distribution.

That is why I consider AMD-HVF not as another small patch, but as a separate project of compatibility between two virtualization architectures.

## How realistic is this idea?

In my opinion, after a working AMDVM appeared, it stopped being an abstract fantasy. We already have the hardware foundation: a custom kernel provider, memory management, architectural CPU state, VMEXIT handling and a confirmed boot of a real Linux.

Previously, an attempt to build an AMD-compatible Hypervisor.framework would have required solving two fundamental tasks at once: implementing the hypervisor itself and reproducing Apple's software interface. Now the first task already has a working prototype, and the second can be researched independently.

But here it is important to define the boundaries of what is achievable correctly.

Creating a custom library compatible with the basic part of the x86 Hypervisor.framework is quite realistic. Getting the existing HVF backend of QEMU to work without rewriting its logic for SVM is significantly harder, but it is already a concrete research goal that can be verified with hardware experiments.

Achieving fully transparent system compatibility with all macOS applications is a completely different level of task. Some programs may check `kern.hv_support`, others use their own virtualization mechanisms, and Virtualization.framework is a separate high-level stack entirely. On top of that come the specifics of dyld, Hardened Runtime, library validation and system component signing.

It cannot be claimed in advance that implementing a compatible API will automatically make Parallels, VMware or all applications using Virtualization.framework work.

That is why the first stage of AMD-HVF should be considerably more modest: a separate library, a custom test application and a minimal set of virtual machine operations. After that — a virtual VMCS model, compatibility verification and an attempt to run the existing x86 HVF backend of QEMU through AMDVM.

If that experiment succeeds, the result will be genuinely interesting. We will have proven not only the possibility of AMD hardware virtualization under macOS, but also the possibility of providing existing software with a compatible interface backed by a different hardware architecture.

At the same time I do not want to turn AMDVM into a collection of VMX-specific workarounds. On the contrary, a proper architecture assumes separation of responsibilities. AMDVM should remain an independent hardware backend, `amdsvm` — a native QEMU accelerator, and AMD-HVF — a separate software compatibility layer. Such an approach will allow exploring the Apple interface without breaking the already proven properties of the world-switch and the memory management mechanisms.

This is exactly what I currently consider the most logical continuation of the project.
