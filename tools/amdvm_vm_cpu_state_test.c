#include "../svm-kext/provider/amdvm_vm_cpu_state.h"
#include "../svm-kext/provider/amdvm_vmrun.h"
#include "../svm-kext/provider/amdvm_vmcb.h"
#include "../svm-kext/provider/amdvm_xstate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Explicit offline CPU model (XCR0 supported mask 7); no privileged entry. */
int amdvm_vmrun_validate_guest_xstate(const void *p, size_t size, uint64_t mask)
{
	return (mask & ~UINT64_C(7)) || amdvm_xstate_validate_standard(p, size, 7) ? -1 : 0;
}

int main(void)
{
	amdvm_vm_cpu_state_t *owners = calloc(4, sizeof(*owners));
	amdvm_vmcb_t *vmcbs;
	uint8_t *images;
	uint64_t gprs[4][15] = {{0}};
	amdvm_x86_guest_state_v1_t input, output, expected[4];
	assert(owners && !posix_memalign((void **)&vmcbs, 4096, 4 * sizeof(*vmcbs)));
	assert(!posix_memalign((void **)&images, 4096, 4 * AMDVM_X86_XSTATE_MAX_SIZE));
	memset(images, 0, 4 * AMDVM_X86_XSTATE_MAX_SIZE);
	for (unsigned v = 0; v < 4; ++v) {
		assert(!amdvm_vmcb_configure_probe(&vmcbs[v], 0x100000 + v * 4096,
		    0x200000 + v * 0x10000, 0x300000 + v * 0x10000, 1));
		assert(!amdvm_vmcb_configure_qemu_reset_vector_v1(&vmcbs[v]));
		assert(!amdvm_vm_cpu_state_init(&owners[v], &vmcbs[v], gprs[v], images + v * 4096));
		assert(!amdvm_vm_cpu_state_get(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &input));
		assert(input.efer == 0 && input.rip == 0xfff0 && input.xcr0 == 1);
		if (v == 0) {
			uint32_t misc, misc2, original_misc2, saved;
			memcpy(&misc, &vmcbs[v].bytes[AMDVM_VMCB_INTERCEPT_MISC1],
			    sizeof(misc));
			memcpy(&misc2, &vmcbs[v].bytes[AMDVM_VMCB_INTERCEPT_MISC2],
			    sizeof(misc2));
			original_misc2 = misc2;
			uint64_t offset = UINT64_C(0xfedcba9876543210);
			assert(misc & AMDVM_SVM_INTERCEPT_RDTSC);
			assert(misc2 & AMDVM_SVM_INTERCEPT_RDTSCP);
			/* Fail closed if RDTSCP could expose the host TSC_AUX. */
			amdvm_vmcb_set_u32(&vmcbs[v], AMDVM_VMCB_INTERCEPT_MISC2,
			    misc2 & ~AMDVM_SVM_INTERCEPT_RDTSCP);
			assert(amdvm_vmcb_prepare_native_rdtsc(&vmcbs[v], offset,
			    &saved) != 0);
			memcpy(&misc, &vmcbs[v].bytes[AMDVM_VMCB_INTERCEPT_MISC1],
			    sizeof(misc));
			assert(misc & AMDVM_SVM_INTERCEPT_RDTSC);
			amdvm_vmcb_set_u32(&vmcbs[v], AMDVM_VMCB_INTERCEPT_MISC2,
			    misc2);
			assert(!amdvm_vmcb_prepare_native_rdtsc(&vmcbs[v], offset, &saved));
			assert(amdvm_vmcb_get_u64(&vmcbs[v], AMDVM_VMCB_TSC_OFFSET) == offset);
			memcpy(&misc, &vmcbs[v].bytes[AMDVM_VMCB_INTERCEPT_MISC1],
			    sizeof(misc));
			assert(misc == (saved & ~AMDVM_SVM_INTERCEPT_RDTSC));
			memcpy(&misc2, &vmcbs[v].bytes[AMDVM_VMCB_INTERCEPT_MISC2],
			    sizeof(misc2));
			assert(misc2 == original_misc2);
			amdvm_vmcb_restore_rdtsc_intercept(&vmcbs[v], saved);
			memcpy(&misc, &vmcbs[v].bytes[AMDVM_VMCB_INTERCEPT_MISC1],
			    sizeof(misc));
			assert(misc == saved);
		}
		for (unsigned r = 0; r < 16; ++r) input.gpr[r] = 0x10000 * (v + 1) + r;
		input.rip += v; input.dr[0] = v + 100; input.dr[3] = v + 200;
		input.segments[AMDVM_X86_SEG_DS].base = 0x1000 * v;
		input.cr[2] = 0x2000 * v;
		input.star = 0x100 * v; input.lstar = 0x1000 * v;
		input.cstar = 0x2000 * v; input.sfmask = 0x100 * v;
		input.kernel_gs_base = 0x4000 * v;
		input.sysenter_cs = 8 * v; input.sysenter_esp = 0x8000 * v;
		input.sysenter_eip = 0x9000 * v;
		input.xcr0 = 3;
		uint64_t bv = 3;
		uint32_t mxcsr = 0x1f80;
		memcpy(input.xstate + 512, &bv, sizeof(bv));
		memcpy(input.xstate + 24, &mxcsr, sizeof(mxcsr));
		for (unsigned j = 160; j < 416; ++j) input.xstate[j] = (uint8_t)(j + v);
		assert(!amdvm_vm_cpu_state_set(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &input));
		expected[v] = input;
		/* RUN's prevalidated entry must stage exactly the same state without
		 * repeating the full architectural validation pass. */
		assert(!amdvm_vmcb_validate_x86_state_v1(&input));
		amdvm_vmcb_t staged = vmcbs[v];
		assert(!amdvm_vm_cpu_state_set_validated(&owners[v], &vmcbs[v], gprs[v],
		    images + v * 4096, &input));
		assert(!memcmp(&vmcbs[v], &staged, sizeof(staged)));
	}
	for (unsigned v = 0; v < 4; ++v) {
		assert(!amdvm_vm_cpu_state_get(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &output));
		assert(!memcmp(&output, &expected[v], sizeof(output)));
		assert(amdvm_vmcb_get_u64(&vmcbs[v], AMDVM_VMCB_NPT_ROOT) == 0x100000 + v * 4096);
		/* Invalid input cannot partially change VMCB, GPR, image or owner. */
		input = expected[v]; input.cr[0] = UINT64_C(1) << 31;
		amdvm_vmcb_t before = vmcbs[v];
		assert(amdvm_vm_cpu_state_set(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &input));
		assert(!memcmp(&vmcbs[v], &before, sizeof(before)));
		assert(!amdvm_vm_cpu_state_get(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &output));
		assert(!memcmp(&output, &expected[v], sizeof(output)));
	}
	/* VM-scoped owners must preserve NMI-blocking across hardware snapshots. */
	for (unsigned v = 0; v < 4; ++v) {
		input = expected[v];
		input.interruptibility = AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED;
		assert(!amdvm_vm_cpu_state_set(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &input));
		assert(!amdvm_vm_cpu_state_get(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &output));
		assert(output.interruptibility == input.interruptibility);
		uint32_t misc;
		memcpy(&misc, &vmcbs[v].bytes[AMDVM_VMCB_INTERCEPT_MISC1], sizeof(misc));
		assert(misc & AMDVM_SVM_INTERCEPT_IRET);
		input.interruptibility |= AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP;
		assert(!amdvm_vm_cpu_state_set(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &input));
		uint32_t saved;
		assert(!amdvm_vmcb_arm_nmi_iret_step(&vmcbs[v], &input, &saved));
		assert(amdvm_vmcb_get_u64(&vmcbs[v], AMDVM_VMCB_GUEST_RFLAGS) & AMDVM_X86_RFLAGS_TF);
		assert(!amdvm_vm_cpu_state_get(&owners[v], &vmcbs[v], gprs[v], images + v * 4096, &output));
		amdvm_vmcb_finish_nmi_iret_step(&vmcbs[v], saved, input.rflags, &output);
		assert(!(output.rflags & AMDVM_X86_RFLAGS_TF));
		assert(output.interruptibility == input.interruptibility);
	}
	free(owners); free(vmcbs); free(images);
	puts("PASS: four production per-vCPU state owners, GPR/segment/MSR/XSTATE roundtrip and invalid-state rollback; no VMRUN");
	return 0;
}
