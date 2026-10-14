/* Local experimental AMD-SVM <-> QEMU x86 architectural state mapping. */
#ifndef AMDSVM_STATE_H
#define AMDSVM_STATE_H

#include "../../../../svm-kext/amdvm_probe_abi.h"
#include "amdsvm-event.h"
#include "amdsvm-msr.h"

struct CPUState;
typedef struct Error Error;

/* QEMU's x86 GPR ABI order, kept explicit outside target-private CPU headers. */
#define AMDSVM_GPR_RAX 0u
#define AMDSVM_GPR_RCX 1u
#define AMDSVM_GPR_RDX 2u
#define AMDSVM_GPR_RBX 3u
#define AMDSVM_GPR_RSP 4u
#define AMDSVM_GPR_RBP 5u
#define AMDSVM_GPR_RSI 6u
#define AMDSVM_GPR_RDI 7u
#define AMDSVM_GPR_R8  8u
#define AMDSVM_GPR_R9  9u
#define AMDSVM_GPR_R10 10u
#define AMDSVM_GPR_R11 11u
#define AMDSVM_GPR_R12 12u
#define AMDSVM_GPR_R13 13u
#define AMDSVM_GPR_R14 14u
#define AMDSVM_GPR_R15 15u

static inline bool amdsvm_xsetbv_value_valid(uint64_t value, uint32_t index,
                                            unsigned cpl, uint64_t supported)
{
    return cpl == 0 && index == 0 && (value & 1) &&
           !(value & ~supported) && !(value & ~UINT64_C(7)) &&
           (!(value & 4) || (value & 2));
}

/* Snapshot QEMU architectural state into the provider ABI. Returns -EINVAL
 * before the provider is called when the state is outside the current profile.
 */
int amdsvm_state_load(struct CPUState *cpu,
                      amdvm_x86_guest_state_v1_t *state);
uint32_t amdsvm_state_apic_id(struct CPUState *cpu);

/* Apply a provider VMEXIT snapshot to QEMU's architectural state. */
int amdsvm_state_store(struct CPUState *cpu,
                       const amdvm_x86_guest_state_v1_t *state);
int amdsvm_handle_xsetbv_exit(struct CPUState *cpu, uint64_t next_rip,
                               uint32_t *event_injection);
int amdsvm_state_read_tsc(struct CPUState *cpu, uint64_t *value);
int amdsvm_handle_ioio_string_exit(
    struct CPUState *cpu, uint64_t exit_info1, uint64_t next_rip,
    amdvm_x86_guest_state_v1_t *updated_state, Error **errp);

/* Read instruction bytes at the current guest RIP for exit diagnostics. */
unsigned amdsvm_state_read_guest_instruction(
    struct CPUState *cpu, uint8_t instruction[15]);

/* Handle narrowly validated firmware ROM writes and VGA-aperture MMIO writes. */
int amdsvm_handle_npf_write_exit(
    struct CPUState *cpu, uint64_t exit_info1, uint64_t fault_gpa,
    const uint8_t instruction_bytes[15], uint8_t instruction_length,
    uint64_t immutable_rom_bytes, uint64_t shadow_backing_hva,
    uint64_t shadow_backing_bytes, Error **errp);

/* Validate state without mutating QEMU or entering the provider. */
int amdsvm_state_validate(const amdvm_x86_guest_state_v1_t *state);

/* Convert between QEMU SegmentCache flags and AMD VMCB packed attributes. */
uint16_t amdsvm_segment_attributes_from_qemu_flags(uint32_t flags);
uint32_t amdsvm_segment_flags_from_attributes(uint16_t attributes);

/* Snapshot QEMU's currently emulated guest MTRR registers. */
void amdsvm_state_get_mtrr(CPUState *cpu, AMDVMMTRRState *state);
uint64_t amdsvm_state_get_pat(CPUState *cpu);

/* Initialize QEMU x86 decode/emulation support for intercepted MSRs. */
void amdsvm_state_init_emulator(void);
void amdsvm_state_alloc_emulator_buffer(struct CPUState *cpu);
void amdsvm_state_free_emulator_buffer(struct CPUState *cpu);

/* Decode and emulate one hardware MSR VMEXIT, validating direction and NRIP. */
bool amdsvm_handle_msr_exit(struct CPUState *cpu, uint64_t exit_info1,
                            uint64_t next_rip, uint32_t *event_injection,
                            uint32_t *event_error_code, Error **errp);

/* Decode and emulate one supported hardware CR0/CR3/CR4 VMEXIT. */
bool amdsvm_handle_cr_exit(struct CPUState *cpu, uint64_t exit_code,
                           uint64_t exit_info1, uint64_t next_rip,
                           uint32_t *event_injection,
                           uint32_t *event_error_code, Error **errp);

/* Decode and emulate the ordinary intercepted DR0-DR7 moves used by guests. */
bool amdsvm_handle_dr_exit(struct CPUState *cpu, uint64_t exit_code,
                           uint64_t exit_info1, uint64_t next_rip,
                           Error **errp);

/* Return x86 interrupt requests this experimental backend cannot inject yet. */
int amdsvm_pending_guest_interrupts(struct CPUState *cpu);
/* Apply QEMU's INIT/SIPI/POLL/TPR handling before resuming a vCPU. */
enum {
    AMDSVM_ASYNC_INIT_APPLIED = 1u << 0,
    AMDSVM_ASYNC_SIPI_STARTED = 1u << 1,
};
unsigned amdsvm_state_process_async_events(struct CPUState *cpu, bool trace);

/* Cache an acknowledged PIC/APIC vector until SVM reports EVENTINJ consumed. */
void amdsvm_prepare_irq_injection(struct CPUState *cpu,
                                  AMDVMEventState *pending_events,
                                  amdvm_vcpu_run_request_t *run);

/* Execute QEMU's target x86 CPUID policy for one intercepted guest instruction. */
void amdsvm_emulate_cpuid(struct CPUState *cpu, uint32_t leaf,
                          uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx);

#endif
