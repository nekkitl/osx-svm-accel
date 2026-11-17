#include "../svm-kext/amdvm_vm_shared_run.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	amdvm_vm_shared_run_slot_t slot;
	const uint64_t sequence = 17;
	uint32_t size = AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE;

	memset(&slot, 0, sizeof(slot));
	assert(amdvm_vm_shared_run_request_size_valid(
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V2_SIZE));
	assert(amdvm_vm_shared_run_request_size_valid(size));
	assert(!amdvm_vm_shared_run_request_size_valid(size - 1));

	assert(amdvm_vm_shared_run_client_acquire(&slot));
	assert(!amdvm_vm_shared_run_client_acquire(&slot));
	assert(!amdvm_vm_shared_run_provider_claim(&slot, sequence, size));
	uint8_t request_snapshot[AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE];
	for (size_t i = 0; i < sizeof(request_snapshot); ++i)
		slot.request[i] = request_snapshot[i] = (uint8_t)(i * 13u + 7u);
	assert(amdvm_vm_shared_run_client_publish(&slot, sequence, size));
	assert(!amdvm_vm_shared_run_provider_claim(&slot, sequence + 1, size));
	assert(!amdvm_vm_shared_run_provider_claim(&slot, sequence, size - 1));
	assert(amdvm_vm_shared_run_provider_claim(&slot, sequence, size));
	assert(!amdvm_vm_shared_run_provider_claim(&slot, sequence, size));
	/* PROVIDER ownership pins a zero-copy request until COMPLETE. */
	assert(!amdvm_vm_shared_run_client_acquire(&slot));
	assert(!amdvm_vm_shared_run_client_release(&slot, sequence));
	assert(!amdvm_vm_shared_run_client_abort_ready(&slot, sequence));
	static const uint8_t expected_result[] = { 0x52, 0x55, 0x4e, 0x21 };
	memcpy(slot.result, expected_result, sizeof(expected_result));
	assert(amdvm_vm_shared_run_provider_complete(&slot, sequence, 0));
	assert(memcmp(slot.result, expected_result, sizeof(expected_result)) == 0);
	assert(memcmp(slot.request, request_snapshot, sizeof(request_snapshot)) == 0);
	assert(!amdvm_vm_shared_run_client_release(&slot, sequence + 1));
	assert(amdvm_vm_shared_run_client_release(&slot, sequence));
	assert(__sync_val_compare_and_swap(&slot.owner, 0, 0) ==
	    AMDVM_VM_SHARED_RUN_FREE);

	/* A failed transport may release a published request only if nobody claimed it. */
	assert(amdvm_vm_shared_run_client_acquire(&slot));
	assert(amdvm_vm_shared_run_client_publish(&slot, sequence + 1,
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V2_SIZE));
	assert(amdvm_vm_shared_run_client_abort_ready(&slot, sequence + 1));
	assert(!amdvm_vm_shared_run_client_abort_ready(&slot, sequence + 1));

	puts("amdvm shared RUN ownership protocol: PASS");
	return 0;
}
