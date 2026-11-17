#include "amdvm_vm_cpu_state.h"
#include <string.h>

int amdvm_vm_cpu_state_init(amdvm_vm_cpu_state_t *owner,
    const amdvm_vmcb_t *vmcb, const uint64_t gprs[15], const void *xstate)
{
	if (!owner || !vmcb || !gprs || !xstate) return -1;
	memset(owner, 0, sizeof(*owner));
	owner->architectural.abi_version = AMDVM_X86_STATE_ABI_VERSION;
	owner->architectural.xcr0 = 1;
	owner->architectural.xstate_size = AMDVM_X86_XSTATE_MAX_SIZE;
	if (amdvm_vmcb_store_x86_state_v1(vmcb, &owner->architectural, gprs,
	    xstate, AMDVM_X86_XSTATE_MAX_SIZE)) return -1;
	owner->valid = 1;
	return 0;
}

static int
amdvm_vm_cpu_state_set_impl(amdvm_vm_cpu_state_t *owner, amdvm_vmcb_t *vmcb,
    uint64_t gprs[15], void *xstate, const amdvm_x86_guest_state_v1_t *input,
    int already_validated)
{
	if (!owner || !owner->valid || !input) return -1;
	int status = already_validated ?
	    amdvm_vmcb_load_x86_state_v1_validated(vmcb, input, gprs, xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) :
	    amdvm_vmcb_load_x86_state_v1(vmcb, input, gprs, xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE);
	if (status) return -1;
	memcpy(&owner->architectural, input, sizeof(*input));
	return 0;
}

int
amdvm_vm_cpu_state_set(amdvm_vm_cpu_state_t *owner, amdvm_vmcb_t *vmcb,
    uint64_t gprs[15], void *xstate, const amdvm_x86_guest_state_v1_t *input)
{
	return amdvm_vm_cpu_state_set_impl(owner, vmcb, gprs, xstate, input, 0);
}

int
amdvm_vm_cpu_state_set_validated(amdvm_vm_cpu_state_t *owner, amdvm_vmcb_t *vmcb,
    uint64_t gprs[15], void *xstate, const amdvm_x86_guest_state_v1_t *input)
{
	return amdvm_vm_cpu_state_set_impl(owner, vmcb, gprs, xstate, input, 1);
}

int amdvm_vm_cpu_state_get(const amdvm_vm_cpu_state_t *owner,
    const amdvm_vmcb_t *vmcb, const uint64_t gprs[15], const void *xstate,
    amdvm_x86_guest_state_v1_t *output)
{
	if (!owner || !owner->valid || !output) return -1;
	memcpy(output, &owner->architectural, sizeof(*output));
	return amdvm_vmcb_store_x86_state_v1(vmcb, output, gprs, xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE);
}
