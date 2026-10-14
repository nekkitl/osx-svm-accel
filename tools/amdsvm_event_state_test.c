#include "../qemu/target/i386/amdsvm/amdsvm-event.h"

#include <assert.h>
#include <string.h>

int
main(void)
{
	AMDVMEventState pending = { 0 };
	amdvm_vcpu_run_request_t run;
	amdvm_x86_guest_state_v1_t guest = { 0 };
	uint32_t gp = AMDVM_VCPU_EVENTINJ_VALID |
	    AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
	    AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_GP;
	uint32_t irq = AMDVM_VCPU_EVENTINJ_VALID | 0x41u;

	assert(amdsvm_event_queue_exception(&pending, gp, 0) == 0);
	assert(amdsvm_event_queue_exception(&pending, gp, 0) != 0);
	pending.irq_pending = true;
	pending.irq_vector = 0x41u;

	memset(&run, 0, sizeof(run));
	amdsvm_event_prepare(&pending, &run);
	assert(run.event_injection == gp);
	assert(run.event_injection_error_code == 0);

	/* Hardware reports the same event in EXITINTINFO: retain and retry it. */
	assert(amdsvm_event_complete(&pending, 0, gp, &guest) == 0);
	assert(pending.exception_pending);
	assert(pending.irq_pending);
	memset(&run, 0, sizeof(run));
	amdsvm_event_prepare(&pending, &run);
	assert(run.event_injection == gp);

	/* No EXITINTINFO means #GP was consumed; the IRQ remains queued. */
	assert(amdsvm_event_complete(&pending, 0, 0, &guest) == 0);
	assert(!pending.exception_pending);
	assert(pending.irq_pending);
	memset(&run, 0, sizeof(run));
	amdsvm_event_prepare(&pending, &run);
	assert(run.event_injection == irq);

	/* A mismatched interrupted event fails closed and retains the IRQ. */
	assert(amdsvm_event_complete(&pending, 0,
	    AMDVM_VCPU_EVENTINJ_VALID | 0x42u, &guest) != 0);
	assert(pending.irq_pending);
	assert(amdsvm_event_complete(&pending, 0, irq, &guest) == 0);
	assert(pending.irq_pending);
	assert(amdsvm_event_complete(&pending, 0, 0, &guest) == 0);
	assert(!pending.irq_pending);

	/* EXITINTINFO without a valid event, or with no queued event, is invalid. */
	assert(amdsvm_event_complete(&pending, 0, 1u, &guest) != 0);
	assert(amdsvm_event_complete(&pending, 0, irq, &guest) != 0);

	/* An NMI is injected once, then stays blocked until a native IRET retires. */
	assert(amdsvm_event_queue_nmi(&pending) == 0);
	memset(&run, 0, sizeof(run));
	amdsvm_event_prepare(&pending, &run);
	assert(run.event_injection == (AMDVM_VCPU_EVENTINJ_VALID |
	       AMDVM_VCPU_EVENTINJ_TYPE_NMI | 2u));
	/* Hardware EXITINTINFO says delivery was interrupted: retry same NMI. */
	assert(amdsvm_event_complete(&pending, 0,
	    run.event_injection, &guest) == 0);
	assert(pending.nmi_pending);
	assert(!(guest.interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED));
	assert(amdsvm_event_complete(&pending, 0, 0, &guest) == 0);
	assert(!pending.nmi_pending);
	assert(guest.interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED);
	guest.rflags = 2u;
	assert(amdsvm_event_handle_iret(&pending, 0x1000,
	       guest.interruptibility, guest.rflags) == 0);
	memset(&run, 0, sizeof(run));
	amdsvm_event_prepare(&pending, &run);
	assert(run.state.interruptibility &
	       AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP);
	guest.rip = 0x1001;
	guest.interruptibility = run.state.interruptibility;
	guest.rflags |= (1ull << 8) | (1ull << 16);
	guest.dr[6] = 1ull << 14;
	assert(amdsvm_event_complete(&pending, AMDVM_VCPU_EXPECTED_DB_EXIT,
	       0, &guest) == 1);
	assert(!pending.nmi_iret_step);
	assert(!(guest.interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED));
	assert((guest.rflags & ((1ull << 8) | (1ull << 16))) == 0);

	/* The internal #DB is accepted only for a BS single-step without guest causes. */
	pending.nmi_iret_step = true;
	pending.nmi_iret_rip = 0x2000;
	pending.nmi_iret_guest_rflags = 2u;
	guest.rip = 0x2001;
	guest.interruptibility = AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED |
	    AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP;
	guest.dr[6] = 1ull << 14 | 1u;
	assert(amdsvm_event_complete(&pending, AMDVM_VCPU_EXPECTED_DB_EXIT,
	       0, &guest) != 1);
	assert(pending.nmi_iret_step);
	assert(guest.interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED);

	/* Do not commandeer a guest's own TF state for the NMI window step. */
	pending.nmi_iret_step = false;
	assert(amdsvm_event_handle_iret(&pending, 0x3000,
	       AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED,
	       2u | (1ull << 8)) != 0);

	return 0;
}
