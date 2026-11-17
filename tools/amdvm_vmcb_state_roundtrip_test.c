/* Userspace-only test for architectural state <-> VMCB/GPR mapping. */
#include "../svm-kext/provider/amdvm_vmcb.h"
#include "../svm-kext/provider/amdvm_npt.h"
#include "../svm-kext/provider/amdvm_vmrun.h"
#include "../svm-kext/provider/amdvm_xstate.h"
#include "../svm-kext/amdvm_probe_abi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Validation is a separate host-privileged boundary; exercise mapping only. */
int
amdvm_vmrun_validate_guest_xstate(const void *area, size_t size,
	uint64_t xcr0)
{
	/* Offline host model: this test CPU exposes x87, SSE, and AVX (XCR0=7). */
	return ((xcr0 & ~UINT64_C(7)) == 0 &&
	    amdvm_xstate_validate_standard(area, size, AMDVM_XSTATE_IMAGE_MASK) == 0) ? 0 : -1;
}

int
main(void)
{
	amdvm_vmcb_t *vmcb = NULL;
	amdvm_vmexit_snapshot_t exit_snapshot = {0};
	amdvm_x86_guest_state_v1_t input = {0};
	amdvm_x86_guest_state_v1_t output = {0};
	amdvm_x86_guest_state_v1_t output_before = {0};
	uint64_t gprs[15] = {0};
	uint64_t xstate_bv = 7;
	uint32_t intercept_misc1;
	void *guest_xstate = NULL;
	uint64_t expected_gpr[AMDVM_X86_GPR_COUNT];
	uint64_t expected_segments[AMDVM_X86_SEGMENT_COUNT][4];
	amdvm_vcpu_run_request_t staged_request = {0};
	uint64_t readonly_aliases[] = {
	    ((UINT64_C(0x12345000) >> 12) << 1) | 1u,
	    ((UINT64_C(0x12345000) >> 12) << 1) | 1u,
	    ((UINT64_C(0xabcde000) >> 12) << 1) | 1u,
	};
	uint64_t non_rom_alias[] = {
	    ((UINT64_C(0x12345000) >> 12) << 1) | 1u,
	    (UINT64_C(0x12345000) >> 12) << 1,
	};
	assert(amdvm_npt_validate_backing_aliases(readonly_aliases,
	    sizeof(readonly_aliases) / sizeof(readonly_aliases[0])) == AMDVM_NPT_OK);
	assert(amdvm_npt_validate_backing_aliases(non_rom_alias,
	    sizeof(non_rom_alias) / sizeof(non_rom_alias[0])) == AMDVM_NPT_CONFLICT);

	vmcb = aligned_alloc(AMDVM_PAGE_SIZE, sizeof(*vmcb));
	assert(vmcb != NULL);
	assert(amdvm_vmcb_configure_probe(vmcb, 0x1000, 0x2000, 0x5000, 1) == 0);
	memcpy(&intercept_misc1,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC1], sizeof(intercept_misc1));
	assert((intercept_misc1 & AMDVM_SVM_INTERCEPT_IRET) == 0);
	assert((intercept_misc1 & AMDVM_SVM_INTERCEPT_INTN) == 0);
	assert((intercept_misc1 & (AMDVM_SVM_INTERCEPT_INTR |
	    AMDVM_SVM_INTERCEPT_NMI | AMDVM_SVM_INTERCEPT_CPUID |
	    AMDVM_SVM_INTERCEPT_HLT | AMDVM_SVM_INTERCEPT_IOIO)) ==
	    (AMDVM_SVM_INTERCEPT_INTR | AMDVM_SVM_INTERCEPT_NMI |
	    AMDVM_SVM_INTERCEPT_CPUID | AMDVM_SVM_INTERCEPT_HLT |
	    AMDVM_SVM_INTERCEPT_IOIO));
	guest_xstate = aligned_alloc(AMDVM_PAGE_SIZE, AMDVM_X86_XSTATE_MAX_SIZE);
	assert(guest_xstate != NULL);
	amdvm_vmcb_clear(vmcb);
	assert(!amdvm_vmcb_external_interrupt_exit_enabled(vmcb));
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_INTERCEPT_MISC1,
	    AMDVM_SVM_INTERCEPT_INTR);
	assert(amdvm_vmcb_external_interrupt_exit_enabled(vmcb));
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_INTERCEPT_MISC1, 0);
	assert(!amdvm_vmcb_external_interrupt_exit_enabled(vmcb));
	assert(AMDVM_VMCB_NEXT_RIP == 0x0c8u);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_NEXT_RIP,
	    UINT64_C(0x123456789abcdef0));
	exit_snapshot.next_rip = amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_NEXT_RIP);
	assert(exit_snapshot.next_rip == UINT64_C(0x123456789abcdef0));
	assert(offsetof(amdvm_vmexit_snapshot_t, next_rip) == 40);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_CODE, 0x77);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_INFO1, 0x11);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_INFO2, 0x22);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RIP, 0x1000);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS, 0x202);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EVENT_INJECTION, 0x80000040);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_INT_INFO, 0x8000020e);
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_EXIT_INT_INFO_ERROR, 14);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_TLB_CONTROL, 0x300000007);
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_CLEAN_BITS, 0x1234);
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_INTERRUPT_STATE,
	    AMDVM_VMCB_INTERRUPT_SHADOW);
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_INTERCEPT_MISC1,
	    AMDVM_SVM_INTERCEPT_IRET);
	vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_LENGTH] = 2;
	vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_BYTES] = 0xf3;
	vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_BYTES + 1] = 0x90;
	amdvm_vmexit_snapshot_capture(vmcb, &exit_snapshot);
	assert(exit_snapshot.code == 0x77 && exit_snapshot.info1 == 0x11 &&
	    exit_snapshot.info2 == 0x22 && exit_snapshot.guest_rip == 0x1000);
	assert(exit_snapshot.event_injection == 0x80000040 &&
	    exit_snapshot.exit_int_info == 0x8000020e &&
	    exit_snapshot.exit_int_info_error == 14);
	assert(exit_snapshot.tlb_control == 0x300000007 &&
	    exit_snapshot.clean_bits == 0x1234);
	assert(exit_snapshot.guest_interruptibility ==
	    (AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW |
	     AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED));
	assert(exit_snapshot.instruction_length == 2 &&
	    exit_snapshot.instruction_bytes[0] == 0xf3 &&
	    exit_snapshot.instruction_bytes[1] == 0x90);
	/* Later VMCB cleanup cannot rewrite the preserved failure evidence. */
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_TLB_CONTROL, 7);
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_CLEAN_BITS, 0);
	assert(exit_snapshot.tlb_control == 0x300000007 &&
	    exit_snapshot.clean_bits == 0x1234);
	assert(amdvm_vmrun_status_for_exit(AMDVM_VMRUN_SUCCESS,
	    AMDVM_SVM_EXIT_ERR) == AMDVM_VMRUN_SUCCESS);
	assert(amdvm_vmrun_status_for_exit(AMDVM_VMRUN_SUCCESS,
	    AMDVM_SVM_EXIT_INVALID) == AMDVM_VMRUN_HARDWARE_INVALID);
	assert(amdvm_vmrun_status_for_exit(AMDVM_VMRUN_SUCCESS,
	    AMDVM_SVM_EXIT_NOT_RUN) == AMDVM_VMRUN_NO_EXIT);
	assert(amdvm_vmrun_status_for_exit(AMDVM_VMRUN_DEBUGGER_ACTIVE,
	    AMDVM_SVM_EXIT_ERR) == AMDVM_VMRUN_DEBUGGER_ACTIVE);
	input.abi_version = AMDVM_X86_STATE_ABI_VERSION;
	input.rflags = 2;
	input.cr[0] = 0x80000011;
	input.cr[2] = 0x22220000;
	input.cr[3] = 0x33330000;
	input.cr[4] = 0x64;
	input.efer = 0x501;
	input.pat = AMDVM_QEMU_RESET_PAT;
	input.xcr0 = 7;
	input.xstate_size = AMDVM_X86_XSTATE_MAX_SIZE;
	input.interruptibility = AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW;
	input.rip = 0x12345678;
	for (unsigned i = 0; i < AMDVM_X86_GPR_COUNT; ++i) {
		input.gpr[i] = UINT64_C(0x100000000) + i * 0x101u;
		expected_gpr[i] = input.gpr[i];
	}
	for (unsigned i = 0; i < AMDVM_X86_SEGMENT_COUNT; ++i) {
		input.segments[i].selector = (uint16_t)(0x10u + i);
		input.segments[i].attributes = 0x93;
		input.segments[i].limit = 0xffff;
		input.segments[i].base = UINT64_C(0x100000) * (i + 1u);
		expected_segments[i][0] = input.segments[i].selector;
		expected_segments[i][1] = input.segments[i].attributes;
		expected_segments[i][2] = input.segments[i].limit;
		expected_segments[i][3] = input.segments[i].base;
	}
	memset(input.xstate, 0, sizeof(input.xstate));
	memcpy(input.xstate + 512, &xstate_bv, sizeof(xstate_bv));
	/* QEMU's protected-mode CS/SS descriptors use valid DB/G bits 8..11. */
	input.segments[AMDVM_X86_SEG_CS].attributes = 0x049bu;
	input.segments[AMDVM_X86_SEG_SS].attributes = 0x0c93u;
	assert(amdvm_vmcb_validate_x86_state_v1(&input) == 0);
	input.segments[AMDVM_X86_SEG_CS].attributes |= 0x1000u;
	assert(amdvm_vmcb_validate_x86_state_v1(&input) != 0);
	input.segments[AMDVM_X86_SEG_CS].attributes &= (uint16_t)~0x1000u;
	assert(amdvm_vmcb_validate_x86_state_v1(&input) == 0);
	output = input;

	memset(guest_xstate, 0, AMDVM_X86_XSTATE_MAX_SIZE);
	assert(amdvm_vmcb_load_x86_state_v1(vmcb, &input, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	assert(memcmp(guest_xstate, input.xstate, sizeof(input.xstate)) == 0);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_EFER) ==
	    (input.efer | AMDVM_EFER_SVME));
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_PAT) == input.pat);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RSP) == input.gpr[4]);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RAX) == input.gpr[0]);
	{
		uint32_t unchanged = amdvm_vmcb_state_clean_candidates(vmcb, &input);
		const uint32_t expected = AMDVM_VMCB_CLEAN_SEG |
		    AMDVM_VMCB_CLEAN_DT | AMDVM_VMCB_CLEAN_CR |
		    AMDVM_VMCB_CLEAN_DR | AMDVM_VMCB_CLEAN_CR2;
		assert((unchanged & expected) == expected);
		amdvm_x86_guest_state_v1_t changed = input;
		changed.cr[3] ^= 0x1000u;
		assert((amdvm_vmcb_state_clean_candidates(vmcb, &changed) &
		    AMDVM_VMCB_CLEAN_CR) == 0);
		changed = input;
		changed.segments[AMDVM_X86_SEG_DS].base++;
		assert((amdvm_vmcb_state_clean_candidates(vmcb, &changed) &
		    AMDVM_VMCB_CLEAN_SEG) == 0);
		changed = input;
		changed.segments[AMDVM_X86_SEG_IDTR].base++;
		assert((amdvm_vmcb_state_clean_candidates(vmcb, &changed) &
		    AMDVM_VMCB_CLEAN_DT) == 0);
	}
	{
		uint16_t saved_cr_read = 0;
		uint16_t cr_read = 0;
		const uint16_t cr2_bit = (uint16_t)(1u << AMDVM_VMCB_CR2_INDEX);
		const uint16_t original_cr_read = (uint16_t)(0xa55au | cr2_bit);

		amdvm_vmcb_set_u16(vmcb, AMDVM_VMCB_INTERCEPT_CR_READ,
		    original_cr_read);
		assert(amdvm_vmcb_prepare_native_cr2_read(vmcb,
		    &saved_cr_read) == 0);
		memcpy(&cr_read, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
		    sizeof(cr_read));
		assert(saved_cr_read == original_cr_read);
		assert(cr_read == (uint16_t)(original_cr_read & ~cr2_bit));
		assert(amdvm_vmcb_prepare_native_cr2_read(vmcb,
		    &saved_cr_read) != 0);
		memcpy(&cr_read, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
		    sizeof(cr_read));
		assert(cr_read == (uint16_t)(original_cr_read & ~cr2_bit));
		amdvm_vmcb_restore_cr2_read_intercept(vmcb, saved_cr_read);
		memcpy(&cr_read, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
		    sizeof(cr_read));
		assert(cr_read == original_cr_read);
		/* Missing CR2 intercept must fail without changing other read bits. */
		saved_cr_read = 0x5aa5u;
		amdvm_vmcb_set_u16(vmcb, AMDVM_VMCB_INTERCEPT_CR_READ,
		    (uint16_t)(original_cr_read & ~cr2_bit));
		assert(amdvm_vmcb_prepare_native_cr2_read(vmcb,
		    &saved_cr_read) != 0);
		assert(saved_cr_read == 0x5aa5u);
		memcpy(&cr_read, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
		    sizeof(cr_read));
		assert(cr_read == (uint16_t)(original_cr_read & ~cr2_bit));
	}
	{
		uint16_t saved_cr_read = 0, saved_cr_write = 0;
		uint16_t cr_read = UINT16_MAX, cr_write = UINT16_MAX;
		const uint16_t native_read_mask = (uint16_t)~(1u << 3);
		const uint16_t native_write_mask = (uint16_t)~(1u << 3);

		amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR4, 0);
		amdvm_vmcb_set_u16(vmcb, AMDVM_VMCB_INTERCEPT_CR_READ, UINT16_MAX);
		amdvm_vmcb_set_u16(vmcb, AMDVM_VMCB_INTERCEPT_CR_WRITE, UINT16_MAX);
		assert(amdvm_vmcb_prepare_native_cr3(vmcb, &saved_cr_read,
		    &saved_cr_write) == 0);
		memcpy(&cr_read, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
		    sizeof(cr_read));
		memcpy(&cr_write, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_WRITE],
		    sizeof(cr_write));
		assert(saved_cr_read == UINT16_MAX && saved_cr_write == UINT16_MAX);
		assert(cr_read == native_read_mask && cr_write == native_write_mask);
		amdvm_vmcb_restore_native_cr3(vmcb, saved_cr_read, saved_cr_write);
		memcpy(&cr_read, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
		    sizeof(cr_read));
		memcpy(&cr_write, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_WRITE],
		    sizeof(cr_write));
		assert(cr_read == UINT16_MAX && cr_write == UINT16_MAX);
		amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR4, UINT64_C(1) << 17);
		assert(amdvm_vmcb_prepare_native_cr3(vmcb, &saved_cr_read,
		    &saved_cr_write) != 0);
		amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR4, 0);
	}
	{
		amdvm_x86_guest_state_v1_t v3 = input;
		uint16_t cr_read, cr_write;
		uint32_t int_ctl;

		v3.abi_version = AMDVM_X86_STATE_ABI_VERSION_V3;
		v3.cr[1] = 5;
		assert(amdvm_vmcb_validate_x86_state_v1(&v3) == 0);
		assert(amdvm_vmcb_load_x86_state_v1(vmcb, &v3, gprs,
		    guest_xstate, AMDVM_X86_XSTATE_MAX_SIZE) == 0);
		memcpy(&int_ctl, &vmcb->bytes[AMDVM_VMCB_INTERRUPT_CONTROL],
		    sizeof(int_ctl));
		assert((int_ctl & 0x0fu) == 5);
		memcpy(&cr_read, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
		    sizeof(cr_read));
		memcpy(&cr_write, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_WRITE],
		    sizeof(cr_write));
		assert((cr_read & (1u << 8)) == 0);
		assert((cr_write & (1u << 8)) == 0);
		assert(amdvm_vmcb_store_x86_state_v1(vmcb, &v3, gprs,
		    guest_xstate, AMDVM_X86_XSTATE_MAX_SIZE) == 0);
		assert(v3.cr[1] == 5);
		v3.cr[1] = 16;
		assert(amdvm_vmcb_validate_x86_state_v1(&v3) != 0);
		v3.abi_version = AMDVM_X86_STATE_ABI_VERSION_V2;
		v3.cr[1] = 1;
		assert(amdvm_vmcb_validate_x86_state_v1(&v3) != 0);
	}
	memcpy(&intercept_misc1,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC1], sizeof(intercept_misc1));
	assert((intercept_misc1 & AMDVM_SVM_INTERCEPT_IRET) == 0);
	input.interruptibility = AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED;
	assert(amdvm_vmcb_load_x86_state_v1(vmcb, &input, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	memcpy(&intercept_misc1,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC1], sizeof(intercept_misc1));
	assert(intercept_misc1 & AMDVM_SVM_INTERCEPT_IRET);
	input.interruptibility |= AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP;
	assert(amdvm_vmcb_load_x86_state_v1(vmcb, &input, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	memcpy(&intercept_misc1,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC1], sizeof(intercept_misc1));
	assert((intercept_misc1 & AMDVM_SVM_INTERCEPT_IRET) == 0);
	input.interruptibility = AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW;

	/* The provider temporarily traps the guest's post-IRET #DB and hides
	 * synthetic TF/RF from the state returned to QEMU. */
	uint32_t saved_exceptions = 0;
	uint32_t step_exceptions = (1u << 6);
	memcpy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_EXCEPTIONS], &step_exceptions,
	    sizeof(step_exceptions));
	input.interruptibility = AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED |
	    AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP;
	input.rflags = 2u | AMDVM_X86_RFLAGS_RF;
	assert(amdvm_vmcb_load_x86_state_v1(vmcb, &input, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	assert(amdvm_vmcb_arm_nmi_iret_step(vmcb, &input,
	    &saved_exceptions) == 0);
	uint32_t armed_exceptions = 0;
	memcpy(&armed_exceptions,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_EXCEPTIONS],
	    sizeof(armed_exceptions));
	assert(armed_exceptions == (step_exceptions | AMDVM_SVM_EXCEPTION_DB));
	assert((amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS) &
	    (AMDVM_X86_RFLAGS_TF | AMDVM_X86_RFLAGS_RF)) ==
	    (AMDVM_X86_RFLAGS_TF | AMDVM_X86_RFLAGS_RF));
	output.rflags = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS);
	amdvm_vmcb_finish_nmi_iret_step(vmcb, saved_exceptions, input.rflags,
	    &output);
	assert(output.rflags == (input.rflags |
	    (amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS) &
	     ~(AMDVM_X86_RFLAGS_TF | AMDVM_X86_RFLAGS_RF))));
	memcpy(&armed_exceptions,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_EXCEPTIONS],
	    sizeof(armed_exceptions));
	assert(armed_exceptions == step_exceptions);
	/* Diagnostic #PF interception must preserve and exactly restore the
	 * existing exception mask, including a concurrently armed #DB bit. */
	uint32_t diagnostic_saved_exceptions = 0;
	uint32_t diagnostic_pf_mask = UINT32_C(1) << AMDVM_VCPU_EXCEPTION_PF;
	assert(amdvm_vmcb_add_exception_intercepts(vmcb, diagnostic_pf_mask,
	    &diagnostic_saved_exceptions) == 0);
	memcpy(&armed_exceptions,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_EXCEPTIONS],
	    sizeof(armed_exceptions));
	assert(armed_exceptions == (step_exceptions | diagnostic_pf_mask));
	amdvm_vmcb_restore_exception_intercepts(vmcb,
	    diagnostic_saved_exceptions);
	memcpy(&armed_exceptions,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_EXCEPTIONS],
	    sizeof(armed_exceptions));
	assert(armed_exceptions == step_exceptions);
	assert(amdvm_vmcb_add_exception_intercepts(vmcb, 0,
	    &diagnostic_saved_exceptions) != 0);
	input.rflags |= AMDVM_X86_RFLAGS_TF;
	assert(amdvm_vmcb_arm_nmi_iret_step(vmcb, &input,
	    &saved_exceptions) != 0);
	input.rflags &= ~AMDVM_X86_RFLAGS_TF;

	/* The live vcpuRun path and this offline check share the staging helper. */
	staged_request.rip = AMDVM_VCPU_RESET_VECTOR_RIP;
	staged_request.rflags = 2;
	for (unsigned i = 0; i < 15u; ++i) {
		staged_request.gpr[i] = UINT64_C(0xa500000000000000) | i;
	}
	/* Seed stale outputs to prove staging clears them before every entry. */
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_NESTED_PAGING,
	    AMDVM_VMCB_NPT_ENABLE);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_CODE, 0x46u);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_INT_INFO, UINT64_C(0x80000b0d));
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_EXIT_INT_INFO_ERROR, 13);
	assert(amdvm_vmcb_stage_run_request(vmcb, gprs, guest_xstate,
	    &staged_request, 0) == 0);
	assert((amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_NESTED_PAGING) &
	    AMDVM_VMCB_NPT_ENABLE) != 0);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RIP) ==
	    AMDVM_VCPU_RESET_VECTOR_RIP);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS) == 2);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_EVENT_INJECTION) == 0);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_EXIT_INT_INFO) == 0);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_EXIT_CODE) ==
	    AMDVM_SVM_EXIT_NOT_RUN);
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_CLEAN_BITS, UINT32_MAX);
	amdvm_vmcb_clear_clean_bits(vmcb);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_CLEAN_BITS) == 0);
	assert(vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_LENGTH] == 0);
	for (unsigned i = 0; i < 15u; ++i) {
		assert(gprs[i] == staged_request.gpr[i]);
	}

	/* Model hardware-updated VMCB fields plus wrapper-saved guest GPRs. */
	for (unsigned i = 0; i < AMDVM_X86_GPR_COUNT; ++i) {
		expected_gpr[i] ^= UINT64_C(0xfeed000000000000) | (i + 1u);
	}
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RAX, expected_gpr[0]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RSP, expected_gpr[4]);
	gprs[1] = expected_gpr[3];
	gprs[2] = expected_gpr[1];
	gprs[3] = expected_gpr[2];
	gprs[4] = expected_gpr[6];
	gprs[5] = expected_gpr[7];
	gprs[6] = expected_gpr[5];
	for (unsigned i = 8; i < AMDVM_X86_GPR_COUNT; ++i) {
		gprs[i - 1u] = expected_gpr[i];
	}
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RIP, 0x87654321);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS, 0x246);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_INTERRUPT_STATE, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR0, 0x11);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR2, 0x2222);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR3, 0x3333);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR4, 0x64);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_EFER, 0x1501);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR6, 0x6666);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR7, 0x7777);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_STAR, 0x8888);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_LSTAR, 0x9999);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CSTAR, 0xaaaa);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_SFMASK, 0xbbbb);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_KERNEL_GS_BASE, 0xcccc);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_SYSENTER_CS, 0xdddd);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_SYSENTER_ESP, 0xeeee);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_SYSENTER_EIP, 0xffff);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_PAT, 0x0007040600070406);
	for (unsigned i = 0; i < AMDVM_X86_SEGMENT_COUNT; ++i) {
		uint32_t offset = AMDVM_VMCB_GUEST_ES + i * 0x10u;
		uint16_t selector = (uint16_t)(0x80u + i);
		uint16_t attributes = (uint16_t)(0x90u + i);
		uint32_t limit = 0x1000u + i;
		uint64_t base = UINT64_C(0xabc00000) + i;
		amdvm_vmcb_set_u16(vmcb, offset, selector);
		amdvm_vmcb_set_u16(vmcb, offset + 2u, attributes);
		amdvm_vmcb_set_u32(vmcb, offset + 4u, limit);
		amdvm_vmcb_set_u64(vmcb, offset + 8u, base);
		expected_segments[i][0] = selector;
		expected_segments[i][1] = attributes;
		expected_segments[i][2] = limit;
		expected_segments[i][3] = base;
	}
	/* Restore the GPR register order expected by the provider wrapper. */
	memset(guest_xstate, 0, AMDVM_X86_XSTATE_MAX_SIZE);
	memset(guest_xstate, 0x3c, 512);
	memcpy((uint8_t *)guest_xstate + 512, &xstate_bv, sizeof(xstate_bv));
	assert(amdvm_vmcb_store_x86_state_v1(vmcb, &output, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	assert(output.rip == 0x87654321 && output.rflags == 0x246);
	assert(output.interruptibility == 0);
	assert(output.cr[0] == 0x11 && output.cr[2] == 0x2222 &&
	    output.cr[3] == 0x3333 && output.cr[4] == 0x64);
	assert(output.efer == 0x501); /* Provider-only SVME must be hidden. */
	assert(output.dr[6] == 0x6666 && output.dr[7] == 0x7777);
	assert(output.star == 0x8888 && output.lstar == 0x9999 &&
	    output.cstar == 0xaaaa && output.sfmask == 0xbbbb);
	assert(output.kernel_gs_base == 0xcccc && output.sysenter_cs == 0xdddd &&
	    output.sysenter_esp == 0xeeee && output.sysenter_eip == 0xffff);
	assert(output.pat == 0x0007040600070406);
	for (unsigned i = 0; i < AMDVM_X86_GPR_COUNT; ++i) {
		assert(output.gpr[i] == expected_gpr[i]);
	}
	for (unsigned i = 0; i < AMDVM_X86_SEGMENT_COUNT; ++i) {
		assert(output.segments[i].selector == expected_segments[i][0]);
		assert(output.segments[i].attributes == expected_segments[i][1]);
		assert(output.segments[i].limit == expected_segments[i][2]);
		assert(output.segments[i].base == expected_segments[i][3]);
	}
	assert(memcmp(output.xstate, guest_xstate, sizeof(output.xstate)) == 0);
	/* Reproduce Linux's legacy SSE state at the RDTSC exit: XCR0=1,
	 * OSXSAVE clear, XSTATE_BV.SSE set. Preserve XMM bytes and architectural
	 * XCR0 through store -> load -> store, including an XCR0 shrink. */
	output.xcr0 = 1;
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR7,
	    output.dr[7] & ~(1ull << 13)); /* Remove mapping-only GD fixture. */
	xstate_bv = 2;
	memcpy((uint8_t *)guest_xstate + 512, &xstate_bv, sizeof(xstate_bv));
	assert(amdvm_xstate_execution_mask(1, 0x6b0) == 3);
	assert(amdvm_xstate_execution_mask(1, 1u << 18) == 1);
	assert(amdvm_vmcb_store_x86_state_v1(vmcb, &output, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	assert(output.xcr0 == 1 && output.xstate[160] == 0x3c);
	assert(amdvm_vmcb_load_x86_state_v1(vmcb, &output, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	assert(((uint8_t *)guest_xstate)[160] == 0x3c);
	output.cr[4] |= 1u << 18;
	xstate_bv = 6; /* Disabled SSE/AVX contents retained across XSETBV. */
	memcpy(output.xstate + 512, &xstate_bv, sizeof(xstate_bv));
	assert(amdvm_vmcb_load_x86_state_v1(vmcb, &output, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	assert(amdvm_vmcb_store_x86_state_v1(vmcb, &output, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) == 0);
	assert(output.xcr0 == 1);
	xstate_bv = 8; /* Unsupported component remains a hard rejection. */
	memcpy(output.xstate + 512, &xstate_bv, sizeof(xstate_bv));
	assert(amdvm_vmcb_validate_x86_state_v1(&output) != 0);
	xstate_bv = 6;
	memcpy(output.xstate + 512, &xstate_bv, sizeof(xstate_bv));
	output_before = output;
	((uint8_t *)guest_xstate)[528] = 1; /* Reserved XSAVE header byte. */
	assert(amdvm_vmcb_store_x86_state_v1(vmcb, &output, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) != 0);
	assert(memcmp(&output, &output_before, sizeof(output)) == 0);
	((uint8_t *)guest_xstate)[528] = 0;
	input.xstate[520] = 1; /* Compacted format is not accepted by this ABI. */
	amdvm_vmcb_clear(vmcb);
	memset(gprs, 0x5a, sizeof(gprs));
	assert(amdvm_vmcb_load_x86_state_v1(vmcb, &input, gprs, guest_xstate,
	    AMDVM_X86_XSTATE_MAX_SIZE) != 0);
	assert(amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RIP) == 0);
	assert(gprs[0] == UINT64_C(0x5a5a5a5a5a5a5a5a));
	free(guest_xstate);
	free(vmcb);
	puts("amdvm VMCB state round-trip mapping: PASS");
	return 0;
}
