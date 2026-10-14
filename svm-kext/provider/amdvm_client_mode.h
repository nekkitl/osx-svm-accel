#ifndef AMDVM_CLIENT_MODE_H
#define AMDVM_CLIENT_MODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum amdvm_client_mode {
	AMDVM_CLIENT_MODE_UNSELECTED = 0,
	AMDVM_CLIENT_MODE_LEGACY = 1,
	AMDVM_CLIENT_MODE_VM_SCOPED = 2
};

enum amdvm_client_mode_result {
	AMDVM_CLIENT_MODE_OK = 0,
	AMDVM_CLIENT_MODE_INVALID = -1,
	AMDVM_CLIENT_MODE_CONFLICT = -2
};

/* The first dispatch selecting a mode permanently latches this client. */
int amdvm_client_mode_select(volatile uint32_t *mode, uint32_t requested_mode);
int amdvm_client_mode_accepts(const volatile uint32_t *mode,
	uint32_t requested_mode);

#ifdef __cplusplus
}
#endif

#endif
