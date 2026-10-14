#ifndef AMDVM_VM_CPU_STATE_H
#define AMDVM_VM_CPU_STATE_H
#include "../amdvm_probe_abi.h"
#include "amdvm_vmcb.h"

/* Caller holds the VM/vCPU lock and excludes active execution. Keep fields
 * absent from VMCB (XCR0, DR0–DR3, etc.) in an architectural per-vCPU owner. */
typedef struct amdvm_vm_cpu_state {
	amdvm_x86_guest_state_v1_t architectural;
	uint32_t valid;
} amdvm_vm_cpu_state_t;
#ifdef __cplusplus
extern "C" {
#endif
int amdvm_vm_cpu_state_init(amdvm_vm_cpu_state_t *, const amdvm_vmcb_t *,
    const uint64_t gprs[15], const void *xstate);
int amdvm_vm_cpu_state_set(amdvm_vm_cpu_state_t *, amdvm_vmcb_t *,
    uint64_t gprs[15], void *xstate, const amdvm_x86_guest_state_v1_t *);
int amdvm_vm_cpu_state_get(const amdvm_vm_cpu_state_t *, const amdvm_vmcb_t *,
    const uint64_t gprs[15], const void *xstate, amdvm_x86_guest_state_v1_t *);
#ifdef __cplusplus
}
#endif
#endif
