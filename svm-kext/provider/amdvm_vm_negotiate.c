#include "amdvm_vm_negotiate.h"

#include <string.h>

int
amdvm_vm_negotiate(volatile uint32_t *mode,
	const amdvm_vm_abi_draft_negotiate_request_t *request,
	size_t request_size,
	amdvm_vm_abi_draft_negotiate_result_t *result,
	size_t result_size)
{
	amdvm_vm_abi_draft_negotiate_result_t candidate;

	if (mode == NULL || request == NULL || result == NULL ||
	    request_size != sizeof(*request) ||
	    result_size < sizeof(*result) ||
	    request->structure_size != sizeof(*request) ||
	    request->reserved[0] != 0 || request->reserved[1] != 0) {
		return AMDVM_VM_NEGOTIATE_INVALID;
	}
	if (request->requested_version != AMDVM_VM_ABI_DRAFT_VERSION) {
		return AMDVM_VM_NEGOTIATE_UNSUPPORTED;
	}
	if (amdvm_client_mode_select(mode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK) {
		return AMDVM_VM_NEGOTIATE_MODE_CONFLICT;
	}

	memset(&candidate, 0, sizeof(candidate));
	candidate.supported_version = AMDVM_VM_ABI_DRAFT_VERSION;
	candidate.structure_size = sizeof(candidate);
	/* No VM operation is advertised until it has a complete implementation. */
	*result = candidate;
	return AMDVM_VM_NEGOTIATE_OK;
}
