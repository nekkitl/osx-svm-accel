#ifndef AMDVM_VM_NEGOTIATE_H
#define AMDVM_VM_NEGOTIATE_H

#include <stddef.h>
#include <stdint.h>

#include "../amdvm_vm_abi_draft.h"
#include "amdvm_client_mode.h"

enum amdvm_vm_negotiate_result_code {
	AMDVM_VM_NEGOTIATE_OK = 0,
	AMDVM_VM_NEGOTIATE_INVALID = -1,
	AMDVM_VM_NEGOTIATE_UNSUPPORTED = -2,
	AMDVM_VM_NEGOTIATE_MODE_CONFLICT = -3
};

#ifdef __cplusplus
extern "C" {
#endif

int amdvm_vm_negotiate(volatile uint32_t *mode,
	const amdvm_vm_abi_draft_negotiate_request_t *request,
	size_t request_size,
	amdvm_vm_abi_draft_negotiate_result_t *result,
	size_t result_size);

#ifdef __cplusplus
}
#endif

#endif
