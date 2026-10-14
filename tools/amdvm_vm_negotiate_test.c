#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../svm-kext/provider/amdvm_vm_negotiate.h"

static amdvm_vm_abi_draft_negotiate_request_t
valid_request(void)
{
	amdvm_vm_abi_draft_negotiate_request_t request;
	memset(&request, 0, sizeof(request));
	request.requested_version = AMDVM_VM_ABI_DRAFT_VERSION;
	request.structure_size = sizeof(request);
	return request;
}

int
main(void)
{
	amdvm_vm_abi_draft_negotiate_request_t request = valid_request();
	amdvm_vm_abi_draft_negotiate_result_t result;
	volatile uint32_t mode = AMDVM_CLIENT_MODE_UNSELECTED;

	memset(&result, 0xa5, sizeof(result));
	assert(amdvm_vm_negotiate(&mode, &request, sizeof(request), &result,
	    sizeof(result)) == AMDVM_VM_NEGOTIATE_OK);
	assert(mode == AMDVM_CLIENT_MODE_VM_SCOPED);
	assert(result.supported_version == AMDVM_VM_ABI_DRAFT_VERSION);
	assert(result.structure_size == sizeof(result));
	assert(result.max_vcpus == 0);
	assert(result.max_memory_ranges == 0);
	assert(result.feature_flags == 0);
	assert(result.max_guest_bytes == 0 && result.max_mapped_bytes == 0);
	assert(amdvm_vm_negotiate(&mode, &request, sizeof(request), &result,
	    sizeof(result)) == AMDVM_VM_NEGOTIATE_OK);

	mode = AMDVM_CLIENT_MODE_LEGACY;
	assert(amdvm_vm_negotiate(&mode, &request, sizeof(request), &result,
	    sizeof(result)) == AMDVM_VM_NEGOTIATE_MODE_CONFLICT);
	assert(mode == AMDVM_CLIENT_MODE_LEGACY);

	mode = AMDVM_CLIENT_MODE_UNSELECTED;
	request.requested_version++;
	assert(amdvm_vm_negotiate(&mode, &request, sizeof(request), &result,
	    sizeof(result)) == AMDVM_VM_NEGOTIATE_UNSUPPORTED);
	assert(mode == AMDVM_CLIENT_MODE_UNSELECTED);
	request = valid_request();
	request.reserved[1] = 1;
	assert(amdvm_vm_negotiate(&mode, &request, sizeof(request), &result,
	    sizeof(result)) == AMDVM_VM_NEGOTIATE_INVALID);
	assert(mode == AMDVM_CLIENT_MODE_UNSELECTED);
	request = valid_request();
	assert(amdvm_vm_negotiate(&mode, &request, sizeof(request) - 1,
	    &result, sizeof(result)) == AMDVM_VM_NEGOTIATE_INVALID);
	assert(mode == AMDVM_CLIENT_MODE_UNSELECTED);
	assert(amdvm_vm_negotiate(&mode, &request, sizeof(request), &result,
	    sizeof(result) - 1) == AMDVM_VM_NEGOTIATE_INVALID);
	assert(mode == AMDVM_CLIENT_MODE_UNSELECTED);
	puts("amdvm VM ABI negotiation: PASS (version, mode latch, unsupported features)");
	return 0;
}
