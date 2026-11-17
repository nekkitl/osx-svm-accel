#include <assert.h>
#include <stdint.h>

#include "../svm-kext/provider/amdvm_pause_fastpath.h"

int
main(void)
{
	assert(amdvm_pause_masked_event_possible(0, 0));
	assert(amdvm_pause_masked_event_possible(AMDVM_PAUSE_RFLAGS_IF,
	    AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW));
	assert(amdvm_pause_masked_event_possible(AMDVM_PAUSE_RFLAGS_IF,
	    AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED));
	assert(!amdvm_pause_masked_event_possible(AMDVM_PAUSE_RFLAGS_IF, 0));
	assert(amdvm_pause_event_pending(AMDVM_PAUSE_EVENT_VALID, 0, 0));
	assert(amdvm_pause_event_pending(0, AMDVM_PAUSE_EVENT_VALID, 0));
	assert(amdvm_pause_event_pending(0, 0, 1));
	assert(!amdvm_pause_event_pending(0, 0, 0));
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, 0x1000, 0x1002,
	    0, 0, 0, 0) == AMDVM_PAUSE_CONTINUE);
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, 0x1000, 0x1003,
	    0, 0, 0, 0) == AMDVM_PAUSE_RETURN_UNHANDLED);
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, UINT64_MAX - 1, 1,
	    0, 0, 0, 0) == AMDVM_PAUSE_RETURN_UNHANDLED);
	/* Every non-PAUSE exit must return to the ordinary QEMU dispatcher. */
	static const uint64_t unhandled_exits[] = {
		UINT64_C(0x040), /* guest exception */
		UINT64_C(0x060), /* physical INTR */
		UINT64_C(0x061), /* NMI */
		UINT64_C(0x072), /* CPUID */
		UINT64_C(0x078), /* HLT */
		UINT64_C(0x013), /* CR3 write */
		UINT64_C(0x400)  /* NPF */
	};
	for (unsigned i = 0; i < sizeof(unhandled_exits) /
	    sizeof(unhandled_exits[0]); ++i) {
		assert(amdvm_pause_decide(unhandled_exits[i], 0x1000, 0x1002,
		    0, 0, 0, 0) == AMDVM_PAUSE_RETURN_UNHANDLED);
	}
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, 0x1000, 0x1002,
	    0, 0, 1, 0) == AMDVM_PAUSE_RETURN_EVENT);
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, 0x1000, 0x1002,
	    0, 0, 0, 1) == AMDVM_PAUSE_RETURN_EVENT);
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, 0x1000, 0x1002,
	    AMDVM_PAUSE_EXIT_BUDGET, 0, 0, 0) == AMDVM_PAUSE_RETURN_BUDGET);
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, 0x1000, 0x1002,
	    0, 1, 0, 0) == AMDVM_PAUSE_RETURN_BUDGET);
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, 0x1000, 0x1002,
	    AMDVM_PAUSE_EXIT_BUDGET - 1, 0, 0, 0) == AMDVM_PAUSE_CONTINUE);
	assert(amdvm_pause_decide(AMDVM_PAUSE_EXIT_CODE, 0x1000, 0x1002,
	    AMDVM_PAUSE_EXIT_BUDGET + 1, 0, 0, 0) == AMDVM_PAUSE_RETURN_BUDGET);
	assert(amdvm_pause_exit_is_event(AMDVM_PAUSE_EXIT_EXTERNAL_INTERRUPT));
	assert(amdvm_pause_exit_is_event(AMDVM_PAUSE_EXIT_NMI));
	assert(amdvm_pause_exit_is_event(AMDVM_PAUSE_EXIT_MACHINE_CHECK));
	assert(amdvm_pause_exit_is_event(0x04e));
	assert(!amdvm_pause_exit_is_event(AMDVM_PAUSE_EXIT_CODE));
	return 0;
}
