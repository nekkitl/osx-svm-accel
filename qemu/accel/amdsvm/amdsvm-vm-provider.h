#ifndef QEMU_AMDSVM_VM_PROVIDER_H
#define QEMU_AMDSVM_VM_PROVIDER_H
#include "amdsvm-provider-lifecycle.h"
#include "../../../svm-kext/amdvm_vm_abi_draft.h"

/* Resource preparation is BQL-serialized; RUN is one call per vCPU thread. */
typedef struct AMDVMVMProvider {
    AMDVMProviderTransport transport;
    void *opaque;
    amdvm_vm_abi_draft_negotiate_result_t caps;
    uint64_t generation;
    bool may_exist;
    bool mapped;
    bool cpus[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint64_t kick_generation[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint32_t run_active[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint32_t operation_gate;
} AMDVMVMProvider;
int amdsvm_vm_provider_init(AMDVMVMProvider *, AMDVMProviderTransport, void *);
int amdsvm_vm_provider_map(AMDVMVMProvider *, const uint8_t rom[4096],
                          const amdvm_vcpu_memory_range_t *, size_t count);
int amdsvm_vm_provider_replace_map(AMDVMVMProvider *,
                                  const amdvm_vcpu_memory_range_t *, size_t count);
int amdsvm_vm_provider_create(AMDVMVMProvider *, uint32_t id, uint32_t apic_id);
int amdsvm_vm_provider_destroy_vcpu(AMDVMVMProvider *, uint32_t id);
int amdsvm_vm_provider_state_roundtrip(AMDVMVMProvider *, uint32_t id,
                                      const amdvm_x86_guest_state_v1_t *);
int amdsvm_vm_provider_diagnostic(AMDVMVMProvider *, uint32_t id,
                                  amdvm_vm_abi_draft_vcpu_diagnostic_result_t *);
int amdsvm_vm_provider_kick(AMDVMVMProvider *, uint32_t id,
                            uint64_t kick_generation);
bool amdsvm_vm_provider_supports_kick(const AMDVMVMProvider *);
int amdsvm_vm_provider_run(AMDVMVMProvider *, uint32_t id,
                           uint64_t kick_generation,
                           uint32_t event_injection,
                           uint32_t event_injection_error_code,
                           const amdvm_x86_guest_state_v1_t *,
                           amdvm_vm_abi_draft_vcpu_run_result_t *);
int amdsvm_vm_provider_set_memory_types(
    AMDVMVMProvider *, uint32_t id,
    const amdvm_vcpu_set_memory_types_request_t *);
int amdsvm_vm_provider_destroy(AMDVMVMProvider *);
#endif
