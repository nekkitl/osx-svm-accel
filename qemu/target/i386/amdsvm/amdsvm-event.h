/* Local AMD-SVM guest event selection and delivery-retention helpers. */
#ifndef AMDSVM_EVENT_H
#define AMDSVM_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include "../../../../svm-kext/amdvm_probe_abi.h"

typedef struct AMDVMEventState {
    bool exception_pending;
    uint32_t exception_info;
    uint32_t exception_error_code;
    bool nmi_pending;
    bool nmi_iret_step;
    uint64_t nmi_iret_rip;
    uint64_t nmi_iret_guest_rflags;
    bool irq_pending;
    uint8_t irq_vector;
} AMDVMEventState;

static inline int amdsvm_event_valid(uint32_t event_info, uint32_t error_code)
{
    uint32_t type;
    uint32_t vector;
    bool error_valid;

    if (event_info == 0) {
        return error_code == 0;
    }
    if (!(event_info & AMDVM_VCPU_EVENTINJ_VALID) ||
        (event_info & AMDVM_VCPU_EVENTINJ_RESERVED_MASK)) {
        return 0;
    }
    type = event_info & AMDVM_VCPU_EVENTINJ_TYPE_MASK;
    vector = event_info & 0xffu;
    error_valid = (event_info & AMDVM_VCPU_EVENTINJ_ERROR_VALID) != 0;
    if (type == AMDVM_VCPU_EVENTINJ_TYPE_EXTERNAL) {
        return !error_valid && error_code == 0;
    }
    if (type == AMDVM_VCPU_EVENTINJ_TYPE_NMI) {
        return vector == 2u && !error_valid && error_code == 0;
    }
    return type == AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION &&
           vector == AMDVM_VCPU_EXCEPTION_GP && error_valid && error_code == 0;
}

static inline int amdsvm_event_queue_exception(AMDVMEventState *state,
                                               uint32_t event_info,
                                               uint32_t error_code)
{
    if (!state || state->exception_pending ||
        !amdsvm_event_valid(event_info, error_code) ||
        (event_info & AMDVM_VCPU_EVENTINJ_TYPE_MASK) !=
            AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION) {
        return -1;
    }
    state->exception_pending = true;
    state->exception_info = event_info;
    state->exception_error_code = error_code;
    return 0;
}

static inline int amdsvm_event_queue_nmi(AMDVMEventState *state)
{
    if (!state || state->nmi_pending) {
        return -1;
    }
    state->nmi_pending = true;
    return 0;
}

static inline int amdsvm_event_handle_iret(AMDVMEventState *state,
                                            uint64_t rip,
                                            uint32_t interruptibility,
                                            uint64_t guest_rflags)
{
    if (!state || state->nmi_iret_step ||
        !(interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED) ||
        /* Guest single-step is not yet separable from the internal NMI step. */
        (guest_rflags & (1ull << 8))) {
        return -1;
    }
    state->nmi_iret_step = true;
    state->nmi_iret_rip = rip;
    state->nmi_iret_guest_rflags = guest_rflags;
    return 0;
}

static inline void amdsvm_event_prepare(const AMDVMEventState *state,
                                        amdvm_vcpu_run_request_t *run)
{
    if (!state || !run) {
        return;
    }
    if (state->exception_pending) {
        run->event_injection = state->exception_info;
        run->event_injection_error_code = state->exception_error_code;
    } else if (state->nmi_pending) {
        run->event_injection = AMDVM_VCPU_EVENTINJ_VALID |
                               AMDVM_VCPU_EVENTINJ_TYPE_NMI | 2u;
    } else if (state->irq_pending) {
        run->event_injection = AMDVM_VCPU_EVENTINJ_VALID | state->irq_vector;
    }
    if (state->nmi_iret_step) {
        run->state.interruptibility |=
            AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED |
            AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP;
    }
}

/* EXITINTINFO with the same event means delivery was interrupted; retain it. */
static inline int amdsvm_event_complete(AMDVMEventState *state,
                                         uint64_t exit_code,
                                         uint32_t exit_int_info,
                                         amdvm_x86_guest_state_v1_t *guest_state)
{
    uint32_t expected;
    bool interrupted = false;
    bool synthetic_debug_exit = false;

    if (!state) {
        return -1;
    }
    expected = state->exception_pending ? state->exception_info :
               state->nmi_pending ? AMDVM_VCPU_EVENTINJ_VALID |
                   AMDVM_VCPU_EVENTINJ_TYPE_NMI | 2u :
               state->irq_pending ?
                   AMDVM_VCPU_EVENTINJ_VALID | state->irq_vector : 0;
    if (exit_int_info & AMDVM_VCPU_EXITINTINFO_VALID) {
        if (expected == 0 || exit_int_info != expected) {
            return -1;
        }
        interrupted = true;
    } else if (exit_int_info != 0) {
        return -1;
    }

    if (!interrupted) {
        if (state->exception_pending) {
            state->exception_pending = false;
        } else if (state->nmi_pending) {
            if (!guest_state) {
                return -1;
            }
            state->nmi_pending = false;
            guest_state->interruptibility |=
                AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED;
        } else if (state->irq_pending) {
            state->irq_pending = false;
        }
    }
    if (state->nmi_iret_step && guest_state) {
        if (guest_state->rip != state->nmi_iret_rip) {
            if (exit_code == AMDVM_VCPU_EXPECTED_DB_EXIT) {
                const uint64_t dr6_step = 1ull << 14;
                const uint64_t dr6_guest_causes = 0xfull | (1ull << 13) |
                    (1ull << 15);
                if (!(guest_state->dr[6] & dr6_step) ||
                    (guest_state->dr[6] & dr6_guest_causes)) {
                    return -1;
                }
                synthetic_debug_exit = true;
            }
            guest_state->interruptibility &=
                ~(AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED |
                  AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP);
            state->nmi_iret_step = false;
            state->nmi_iret_rip = 0;
            const uint64_t debug_flags = (1ull << 8) | (1ull << 16);
            guest_state->rflags = (guest_state->rflags & ~debug_flags) |
                (state->nmi_iret_guest_rflags & debug_flags);
            state->nmi_iret_guest_rflags = 0;
        } else {
            if (exit_code == AMDVM_VCPU_EXPECTED_DB_EXIT) {
                return -1;
            }
            guest_state->interruptibility |=
                AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED |
                AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP;
        }
    }
    return synthetic_debug_exit ? 1 : 0;
}

#endif
