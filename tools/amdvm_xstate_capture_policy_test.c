#include <assert.h>
#include <stdio.h>

#include "../svm-kext/provider/amdvm_xstate_capture_policy.h"

int
main(void)
{
	/* Steady state avoids the old speculative pre-lock XSAVE. */
	assert(amdvm_xstate_capture_preflight_action(0) ==
	    AMDVM_XSTATE_CAPTURE_PINNED);
	/* Lazy FPU ownership still goes through an XSAVE/#NM with interrupts on. */
	assert(amdvm_xstate_capture_preflight_action(
	    AMDVM_XSTATE_POLICY_CR0_TS) == AMDVM_XSTATE_CAPTURE_LAZY_RESTORE);
	assert(amdvm_xstate_capture_preflight_action(
	    AMDVM_XSTATE_POLICY_CR0_EM) == AMDVM_XSTATE_CAPTURE_REJECT);

	/* Recheck after taking runLock; thread switches and a newly asserted TS
	 * cannot reach the pinned XSAVE path. */
	assert(amdvm_xstate_capture_pinned_action(0, 0) ==
	    AMDVM_XSTATE_CAPTURE_RETRY);
	assert(amdvm_xstate_capture_pinned_action(0, 1) ==
	    AMDVM_XSTATE_CAPTURE_PINNED);
	assert(amdvm_xstate_capture_pinned_action(
	    AMDVM_XSTATE_POLICY_CR0_TS, 1) ==
	    AMDVM_XSTATE_CAPTURE_LAZY_RESTORE);
	assert(amdvm_xstate_capture_pinned_action(
	    AMDVM_XSTATE_POLICY_CR0_EM, 1) == AMDVM_XSTATE_CAPTURE_REJECT);

	puts("amdvm XSTATE capture policy: PASS");
	return 0;
}
