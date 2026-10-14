/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_AMDSVM_FIRMWARE_DISPATCH_H
#define QEMU_AMDSVM_FIRMWARE_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "../../../svm-kext/amdvm_probe_abi.h"

typedef enum AMDVMFirmwareDispatchAction {
    AMDVM_FIRMWARE_DISPATCH_ERROR = -1,
    AMDVM_FIRMWARE_DISPATCH_CONTINUE = 0,
    AMDVM_FIRMWARE_DISPATCH_HALTED = 1,
    AMDVM_FIRMWARE_DISPATCH_YIELDED = 2,
} AMDVMFirmwareDispatchAction;

typedef struct AMDVMFirmwareDispatchOps {
    int (*store_state)(void *opaque,
                       const amdvm_x86_guest_state_v1_t *state);
    int (*complete_event)(void *opaque, uint64_t exit_code,
                          uint32_t exit_int_info,
                          amdvm_x86_guest_state_v1_t *state);
    int (*handle_iret)(void *opaque, uint64_t guest_rip,
                       uint32_t interruptibility, uint64_t guest_rflags);
    int (*emulate_cpuid)(void *opaque, uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx);
    int (*emulate_rdtsc)(void *opaque, uint64_t *value);
    int (*emulate_io)(void *opaque, uint32_t port, bool is_input,
                      unsigned size, uint64_t *rax);
    int (*emulate_string_io)(
        void *opaque, const amdvm_x86_guest_state_v1_t *state,
        uint64_t exit_info1, uint64_t next_rip,
        amdvm_x86_guest_state_v1_t *updated_state);
    AMDVMFirmwareDispatchAction (*handle_cr)(
        void *opaque, const amdvm_vcpu_run_result_t *result);
    AMDVMFirmwareDispatchAction (*handle_dr)(
        void *opaque, const amdvm_vcpu_run_result_t *result);
    AMDVMFirmwareDispatchAction (*handle_msr)(
        void *opaque, const amdvm_vcpu_run_result_t *result);
    AMDVMFirmwareDispatchAction (*handle_xsetbv)(
        void *opaque, const amdvm_vcpu_run_result_t *result);
    AMDVMFirmwareDispatchAction (*handle_npf)(
        void *opaque, const amdvm_vcpu_run_result_t *result);
    AMDVMFirmwareDispatchAction (*handle_physical_intr)(void *opaque);
    AMDVMFirmwareDispatchAction (*handle_preentry_kick)(void *opaque);
} AMDVMFirmwareDispatchOps;

AMDVMFirmwareDispatchAction amdsvm_dispatch_firmware_exit(
    const amdvm_vcpu_run_request_t *request,
    const amdvm_vcpu_run_result_t *result,
    const AMDVMFirmwareDispatchOps *ops, void *opaque);

#endif
