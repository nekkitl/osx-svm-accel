#include "amdvm_client_mode.h"

int
amdvm_client_mode_select(volatile uint32_t *mode, uint32_t requested_mode)
{
	uint32_t observed;

	if (mode == 0 || (requested_mode != AMDVM_CLIENT_MODE_LEGACY &&
	    requested_mode != AMDVM_CLIENT_MODE_VM_SCOPED)) {
		return AMDVM_CLIENT_MODE_INVALID;
	}
	observed = __sync_val_compare_and_swap(mode,
	    AMDVM_CLIENT_MODE_UNSELECTED, requested_mode);
	return observed == AMDVM_CLIENT_MODE_UNSELECTED ||
	    observed == requested_mode ? AMDVM_CLIENT_MODE_OK :
	    AMDVM_CLIENT_MODE_CONFLICT;
}

int
amdvm_client_mode_accepts(const volatile uint32_t *mode,
	uint32_t requested_mode)
{
	if (mode == 0 || (requested_mode != AMDVM_CLIENT_MODE_LEGACY &&
	    requested_mode != AMDVM_CLIENT_MODE_VM_SCOPED)) {
		return AMDVM_CLIENT_MODE_INVALID;
	}
	return __sync_val_compare_and_swap((volatile uint32_t *)mode,
	    AMDVM_CLIENT_MODE_UNSELECTED, AMDVM_CLIENT_MODE_UNSELECTED) ==
	    requested_mode ? AMDVM_CLIENT_MODE_OK : AMDVM_CLIENT_MODE_CONFLICT;
}
