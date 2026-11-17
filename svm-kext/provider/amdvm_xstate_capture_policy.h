#ifndef AMDVM_XSTATE_CAPTURE_POLICY_H
#define AMDVM_XSTATE_CAPTURE_POLICY_H

#include <stdint.h>

#define AMDVM_XSTATE_POLICY_CR0_TS (UINT64_C(1) << 3)
#define AMDVM_XSTATE_POLICY_CR0_EM (UINT64_C(1) << 2)

typedef enum amdvm_xstate_capture_action {
	AMDVM_XSTATE_CAPTURE_REJECT = 0,
	AMDVM_XSTATE_CAPTURE_PINNED,
	AMDVM_XSTATE_CAPTURE_LAZY_RESTORE,
	AMDVM_XSTATE_CAPTURE_RETRY
} amdvm_xstate_capture_action_t;

/* A clear TS bit means the steady path can skip the speculative XSAVE before
 * taking runLock. A set TS bit must still use XNU's normal unpinned #NM path. */
static inline amdvm_xstate_capture_action_t
amdvm_xstate_capture_preflight_action(uint64_t cr0)
{
	if (cr0 & AMDVM_XSTATE_POLICY_CR0_EM)
		return AMDVM_XSTATE_CAPTURE_REJECT;
	return (cr0 & AMDVM_XSTATE_POLICY_CR0_TS) ?
	    AMDVM_XSTATE_CAPTURE_LAZY_RESTORE : AMDVM_XSTATE_CAPTURE_PINNED;
}

/* After runLock is acquired, only the same thread with TS/EM clear may proceed
 * to the single local-interrupt-off XSAVE. A newly set TS exits the lock first
 * so XNU's ordinary lazy restore can run safely. */
static inline amdvm_xstate_capture_action_t
amdvm_xstate_capture_pinned_action(uint64_t cr0, int same_thread)
{
	if (!same_thread)
		return AMDVM_XSTATE_CAPTURE_RETRY;
	if (cr0 & AMDVM_XSTATE_POLICY_CR0_EM)
		return AMDVM_XSTATE_CAPTURE_REJECT;
	if (cr0 & AMDVM_XSTATE_POLICY_CR0_TS)
		return AMDVM_XSTATE_CAPTURE_LAZY_RESTORE;
	return AMDVM_XSTATE_CAPTURE_PINNED;
}

#endif
