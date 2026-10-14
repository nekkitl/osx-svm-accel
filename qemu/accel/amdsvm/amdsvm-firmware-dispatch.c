/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "amdsvm-firmware-dispatch.h"

#include "target/i386/svm.h"
#include "target/i386/amdsvm/amdsvm-state.h"

#define AMDVM_CR4_TSD (UINT64_C(1) << 2)

static bool amdsvm_dispatch_next_rip_valid(
    const amdvm_vcpu_run_result_t *result)
{
    return result->next_rip > result->state.rip &&
           result->next_rip - result->state.rip <= 15;
}

static AMDVMFirmwareDispatchAction amdsvm_dispatch_store_state(
    const AMDVMFirmwareDispatchOps *ops, void *opaque,
    const amdvm_x86_guest_state_v1_t *state)
{
    return ops->store_state && ops->store_state(opaque, state) == 0 ?
           AMDVM_FIRMWARE_DISPATCH_CONTINUE : AMDVM_FIRMWARE_DISPATCH_ERROR;
}

AMDVMFirmwareDispatchAction amdsvm_dispatch_firmware_exit(
    const amdvm_vcpu_run_request_t *request,
    const amdvm_vcpu_run_result_t *result,
    const AMDVMFirmwareDispatchOps *ops, void *opaque)
{
    const uint32_t allowed_flags = AMDVM_VCPU_RUN_CALLED |
                                   AMDVM_VCPU_RUN_LEASE_RETAINED |
                                   AMDVM_VCPU_RUN_KICKED;
    amdvm_x86_guest_state_v1_t state;
    AMDVMFirmwareDispatchAction action;
    int event_status;

    if (!request || !result || !ops ||
        result->abi_version != AMDVM_VCPU_ABI_VERSION ||
        (result->flags & ~allowed_flags) ||
        (result->flags & AMDVM_VCPU_RUN_LEASE_RETAINED) ||
        result->context_status != 0 || result->owner_status != 0 ||
        result->vmrun_status != 0 ||
        !(result->flags & (AMDVM_VCPU_RUN_CALLED |
                           AMDVM_VCPU_RUN_KICKED))) {
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }

    if (!(result->flags & AMDVM_VCPU_RUN_CALLED)) {
        if (!(result->flags & AMDVM_VCPU_RUN_KICKED) ||
            result->exit_code != UINT64_MAX ||
            memcmp(&result->state, &request->state, sizeof(request->state))) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        return ops->handle_preentry_kick ?
            ops->handle_preentry_kick(opaque) :
            AMDVM_FIRMWARE_DISPATCH_ERROR;
    }

    state = result->state;
    if (!ops->complete_event) {
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    event_status = ops->complete_event(opaque, result->exit_code,
                                       result->exit_int_info, &state);
    if (event_status < 0 ||
        amdsvm_dispatch_store_state(ops, opaque, &state) !=
            AMDVM_FIRMWARE_DISPATCH_CONTINUE) {
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (event_status > 0) {
        return result->exit_code == AMDVM_VCPU_EXPECTED_DB_EXIT ?
               AMDVM_FIRMWARE_DISPATCH_CONTINUE :
               AMDVM_FIRMWARE_DISPATCH_ERROR;
    }

    if (result->exit_code == AMDVM_VCPU_EXPECTED_CPUID_EXIT) {
        uint32_t eax = (uint32_t)state.gpr[AMDSVM_GPR_RAX];
        uint32_t ecx = (uint32_t)state.gpr[AMDSVM_GPR_RCX];
        uint32_t ebx, edx;

        if (!amdsvm_dispatch_next_rip_valid(result) || !ops->emulate_cpuid ||
            ops->emulate_cpuid(opaque, eax, ecx, &eax, &ebx, &ecx, &edx) != 0) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        state.gpr[AMDSVM_GPR_RAX] = eax;
        state.gpr[AMDSVM_GPR_RBX] = ebx;
        state.gpr[AMDSVM_GPR_RCX] = ecx;
        state.gpr[AMDSVM_GPR_RDX] = edx;
        state.rip = result->next_rip;
        return amdsvm_dispatch_store_state(ops, opaque, &state);
    }

    if (result->exit_code == SVM_EXIT_RDTSC) {
        uint64_t tsc;

        /* RDTSC is the unprefixed two-byte instruction 0F 31. */
        if (result->exit_info1 != 0 || result->exit_info2 != 0 ||
            result->next_rip != state.rip + 2u ||
            ((state.cr[4] & AMDVM_CR4_TSD) &&
             (state.segments[AMDVM_X86_SEG_CS].selector & 3u)) ||
            !ops->emulate_rdtsc ||
            ops->emulate_rdtsc(opaque, &tsc) != 0) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        state.gpr[AMDSVM_GPR_RAX] = (uint32_t)tsc;
        state.gpr[AMDSVM_GPR_RDX] = (uint32_t)(tsc >> 32);
        state.rip = result->next_rip;
        return amdsvm_dispatch_store_state(ops, opaque, &state);
    }

    if (result->exit_code == AMDVM_VCPU_EXPECTED_IOIO_EXIT) {
        uint64_t info = result->exit_info1;
        uint64_t size_bits = (info >> 4) & 7u;
        uint32_t port;
        unsigned size;
        bool is_input = (info & 1u) != 0;
        bool is_string = (info & (1u << 2)) != 0;
        bool has_rep = (info & (1u << 3)) != 0;

        if ((info & UINT64_C(0xffffffff0000e002)) != 0 ||
            (size_bits != 1u && size_bits != 2u && size_bits != 4u) ||
            !amdsvm_dispatch_next_rip_valid(result) ||
            (is_string ? !ops->emulate_string_io : !ops->emulate_io)) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        port = (uint32_t)((info >> 16) & 0xffffu);
        size = size_bits == 1u ? 1u : size_bits == 2u ? 2u : 4u;
        if (is_string) {
            amdvm_x86_guest_state_v1_t updated_state = state;

            if (!ops->emulate_string_io ||
                ops->emulate_string_io(opaque, &state, info,
                                       result->next_rip,
                                       &updated_state) != 0) {
                return AMDVM_FIRMWARE_DISPATCH_ERROR;
            }
            return amdsvm_dispatch_store_state(ops, opaque, &updated_state);
        }
        if (has_rep) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        uint64_t original_rax = state.gpr[AMDSVM_GPR_RAX];
        uint64_t rax = original_rax;

        if (ops->emulate_io(opaque, port, is_input, size, &rax) != 0) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        if (is_input) {
            if (size == 1) {
                rax = (original_rax & ~UINT64_C(0xff)) |
                      (rax & UINT64_C(0xff));
            } else if (size == 2) {
                rax = (original_rax & ~UINT64_C(0xffff)) |
                      (rax & UINT64_C(0xffff));
            } else {
                rax = (uint32_t)rax;
            }
        }
        state.gpr[AMDSVM_GPR_RAX] = rax;
        state.rip = result->next_rip;
        return amdsvm_dispatch_store_state(ops, opaque, &state);
    }

    if (result->exit_code == SVM_EXIT_READ_CR0 ||
        result->exit_code == SVM_EXIT_READ_CR2 ||
        result->exit_code == SVM_EXIT_READ_CR3 ||
        result->exit_code == SVM_EXIT_READ_CR4 ||
        result->exit_code == SVM_EXIT_WRITE_CR0 ||
        result->exit_code == SVM_EXIT_WRITE_CR2 ||
        result->exit_code == SVM_EXIT_WRITE_CR3 ||
        result->exit_code == SVM_EXIT_WRITE_CR4) {
        return ops->handle_cr ? ops->handle_cr(opaque, result) :
               AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if ((result->exit_code >= SVM_EXIT_READ_DR0 &&
         result->exit_code <= SVM_EXIT_READ_DR0 + 7u) ||
        (result->exit_code >= SVM_EXIT_WRITE_DR0 &&
         result->exit_code <= SVM_EXIT_WRITE_DR0 + 7u)) {
        return ops->handle_dr ? ops->handle_dr(opaque, result) :
               AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (result->exit_code == AMDVM_VCPU_EXIT_MSR) {
        return ops->handle_msr ? ops->handle_msr(opaque, result) :
               AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (result->exit_code == SVM_EXIT_NPF) {
        return ops->handle_npf ? ops->handle_npf(opaque, result) :
               AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (result->exit_code == SVM_EXIT_XSETBV) {
        return ops->handle_xsetbv ? ops->handle_xsetbv(opaque, result) :
               AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (result->exit_code == SVM_EXIT_IRET) {
        if (!ops->handle_iret ||
            ops->handle_iret(opaque, state.rip,
                             state.interruptibility, state.rflags) != 0) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
    }
    if (result->exit_code == AMDVM_VCPU_EXIT_PHYSICAL_INTR) {
        return ops->handle_physical_intr ?
            ops->handle_physical_intr(opaque) :
            AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (result->exit_code == AMDVM_HLT_EXPECTED_EXIT) {
        if (!amdsvm_dispatch_next_rip_valid(result)) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        state.rip = result->next_rip;
        /* Emulated HLT completes the instruction following STI. */
        state.interruptibility &= ~AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW;
        action = amdsvm_dispatch_store_state(ops, opaque, &state);
        return action == AMDVM_FIRMWARE_DISPATCH_CONTINUE ?
               AMDVM_FIRMWARE_DISPATCH_HALTED : action;
    }
    if (result->exit_code == SVM_EXIT_PAUSE) {
        if (result->next_rip != state.rip + 2u) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        state.rip = result->next_rip;
        return amdsvm_dispatch_store_state(ops, opaque, &state);
    }
    if (result->exit_code == SVM_EXIT_WBINVD) {
        /* QEMU's x86 TCG models WBINVD as a no-op for its virtual CPU. */
        if (result->exit_info1 != 0 || result->exit_info2 != 0 ||
            result->next_rip != state.rip + 2u) {
            return AMDVM_FIRMWARE_DISPATCH_ERROR;
        }
        state.rip = result->next_rip;
        return amdsvm_dispatch_store_state(ops, opaque, &state);
    }
    return AMDVM_FIRMWARE_DISPATCH_ERROR;
}
