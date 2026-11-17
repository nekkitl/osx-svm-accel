#include <assert.h>
#include <stdio.h>

#include "../svm-kext/amdvm_vm_abi_draft.h"

int
main(void)
{
	assert(sizeof(amdvm_vcpu_memory_range_t) == 32);
	assert(sizeof(amdvm_vm_abi_draft_negotiate_request_t) == 24);
	assert(sizeof(amdvm_vm_abi_draft_negotiate_result_t) == 40);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_NEGOTIATE == 13u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_SET_MEMORY_MAP == 14u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_SET_RESET_ROM == 15u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_CREATE == 16u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_STATE == 17u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN == 18u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_KICK == 19u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_MEMORY_TYPES == 20u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DESTROY == 21u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_VM_DESTROY == 22u);
	assert(AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DIAGNOSTIC == 23u);
	assert(amdvm_vm_abi_draft_memory_map_flags_valid(0));
	assert(amdvm_vm_abi_draft_memory_map_flags_valid(
	    AMDVM_VM_ABI_DRAFT_MEMORY_MAP_REPLACE));
	assert(!amdvm_vm_abi_draft_memory_map_flags_valid(2u));
	assert(!amdvm_vm_abi_draft_memory_map_flags_valid(
	    AMDVM_VM_ABI_DRAFT_MEMORY_MAP_REPLACE | 2u));
	assert(sizeof(amdvm_x86_guest_state_v1_t) == 4608);
	assert(offsetof(amdvm_x86_guest_state_v1_t, xstate) == 512);
	assert(sizeof(amdvm_vm_abi_draft_memory_map_request_t) == 2088);
	assert(sizeof(amdvm_vm_abi_draft_memory_map_result_t) == 40);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_create_request_t) == 48);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_create_result_t) == 40);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_lifecycle_request_t) == 24);
	assert(sizeof(amdvm_vm_abi_draft_vm_destroy_request_t) == 24);
	assert(sizeof(amdvm_vm_abi_draft_lifecycle_result_t) == 24);
	assert(sizeof(amdvm_vm_abi_draft_reset_rom_request_t) == 4120);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_state_request_t) == 4632);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_state_result_t) == 4632);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_run_request_t) == 4656);
	assert(offsetof(amdvm_vm_abi_draft_vcpu_run_request_t, tsc_offset) == 4648);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_run_result_t) == 4704);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_kick_request_t) == 32);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_kick_result_t) == 40);
	assert(sizeof(amdvm_vm_abi_draft_memory_type_range_t) == 24);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_memory_types_request_t) == 6184);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_memory_types_result_t) == 40);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_diagnostic_request_t) == 24);
	assert(sizeof(amdvm_vm_abi_draft_vcpu_diagnostic_result_t) == 160);
	puts("amdvm VM ABI draft layouts: PASS");
	return 0;
}
