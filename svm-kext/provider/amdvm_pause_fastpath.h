#ifndef AMDVM_PAUSE_FASTPATH_H
#define AMDVM_PAUSE_FASTPATH_H

#include <stdint.h>

#define AMDVM_PAUSE_EXIT_CODE UINT64_C(0x077)
#define AMDVM_PAUSE_EXIT_EXTERNAL_INTERRUPT UINT64_C(0x060)
#define AMDVM_PAUSE_EXIT_NMI UINT64_C(0x061)
#define AMDVM_PAUSE_EXIT_MACHINE_CHECK UINT64_C(0x062)
#define AMDVM_PAUSE_EXIT_EXCEPTION_FIRST UINT64_C(0x040)
#define AMDVM_PAUSE_EXIT_EXCEPTION_LAST UINT64_C(0x05f)
#define AMDVM_PAUSE_EXIT_BUDGET 64u
#define AMDVM_PAUSE_EVENT_VALID (UINT64_C(1) << 31)
#define AMDVM_PAUSE_RFLAGS_IF (UINT64_C(1) << 9)

#include "../amdvm_probe_abi.h"

typedef enum amdvm_pause_decision {
	AMDVM_PAUSE_CONTINUE = 0,
	AMDVM_PAUSE_RETURN_BUDGET = 1,
	AMDVM_PAUSE_RETURN_EVENT = 2,
	AMDVM_PAUSE_RETURN_UNHANDLED = 3
} amdvm_pause_decision_t;

static inline int
amdvm_pause_masked_event_possible(uint64_t rflags, uint32_t interruptibility)
{
	return (rflags & AMDVM_PAUSE_RFLAGS_IF) == 0 ||
	    (interruptibility & (AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW |
	    AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED)) != 0;
}

static inline int
amdvm_pause_exit_is_event(uint64_t exit_code)
{
	return (exit_code >= AMDVM_PAUSE_EXIT_EXCEPTION_FIRST &&
	    exit_code <= AMDVM_PAUSE_EXIT_EXCEPTION_LAST) ||
	    exit_code == AMDVM_PAUSE_EXIT_EXTERNAL_INTERRUPT ||
	    exit_code == AMDVM_PAUSE_EXIT_NMI ||
	    exit_code == AMDVM_PAUSE_EXIT_MACHINE_CHECK;
}

static inline int
amdvm_pause_event_pending(uint64_t event_injection, uint64_t exit_int_info,
	int masked_event_possible)
{
	return (event_injection & AMDVM_PAUSE_EVENT_VALID) != 0 ||
	    (exit_int_info & AMDVM_PAUSE_EVENT_VALID) != 0 ||
	    masked_event_possible;
}

static inline amdvm_pause_decision_t
amdvm_pause_decide(uint64_t exit_code, uint64_t rip, uint64_t next_rip,
	uint32_t pauses_handled, int deadline_expired, int kick_changed,
	int event_pending)
{
	if (kick_changed || event_pending) return AMDVM_PAUSE_RETURN_EVENT;
	if (exit_code != AMDVM_PAUSE_EXIT_CODE) return AMDVM_PAUSE_RETURN_UNHANDLED;
	if (rip > UINT64_MAX - 2 || next_rip != rip + 2)
		return AMDVM_PAUSE_RETURN_UNHANDLED;
	if (pauses_handled >= AMDVM_PAUSE_EXIT_BUDGET || deadline_expired)
		return AMDVM_PAUSE_RETURN_BUDGET;
	return AMDVM_PAUSE_CONTINUE;
}

#endif
