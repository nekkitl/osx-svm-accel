#define _POSIX_C_SOURCE 200809L
#include "../svm-kext/provider/amdvm_vm_cpu_state.h"
#include "../svm-kext/provider/amdvm_vmrun.h"
#include "../svm-kext/provider/amdvm_vmcb.h"
#include "../svm-kext/provider/amdvm_xstate.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ITERATIONS 10000u
#define SAMPLES 9u

int
amdvm_vmrun_validate_guest_xstate(const void *p, size_t size, uint64_t mask)
{
	return (mask & ~UINT64_C(7)) || amdvm_xstate_validate_standard(p, size, 7) ?
	    -1 : 0;
}

static uint64_t
now_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static double
measure(amdvm_vm_cpu_state_t *owner, amdvm_vmcb_t *vmcb, uint64_t gprs[15],
	void *xstate, const amdvm_x86_guest_state_v1_t *input, int prevalidated)
{
	uint64_t start = now_ns();
	for (unsigned i = 0; i < ITERATIONS; ++i) {
		int rc = prevalidated ?
		    amdvm_vm_cpu_state_set_validated(owner, vmcb, gprs, xstate, input) :
		    amdvm_vm_cpu_state_set(owner, vmcb, gprs, xstate, input);
		if (rc != 0) abort();
	}
	return (double)(now_ns() - start) / (double)ITERATIONS;
}

static void
sort(double values[SAMPLES])
{
	for (unsigned i = 1; i < SAMPLES; ++i) {
		double value = values[i];
		unsigned j = i;
		while (j > 0 && values[j - 1] > value) {
			values[j] = values[j - 1];
			--j;
		}
		values[j] = value;
	}
}

int
main(void)
{
	amdvm_vm_cpu_state_t owner;
	amdvm_vmcb_t *vmcb = NULL;
	uint8_t *xstate = NULL;
	uint64_t gprs[15] = {0};
	amdvm_x86_guest_state_v1_t input;
	double safe[SAMPLES], prevalidated[SAMPLES];
	if (posix_memalign((void **)&vmcb, 4096, sizeof(*vmcb)) != 0 ||
	    posix_memalign((void **)&xstate, 4096, AMDVM_X86_XSTATE_MAX_SIZE) != 0)
		return 2;
	memset(vmcb, 0, sizeof(*vmcb));
	memset(xstate, 0, AMDVM_X86_XSTATE_MAX_SIZE);
	if (amdvm_vmcb_configure_probe(vmcb, 0x100000, 0x200000, 0x300000, 1) ||
	    amdvm_vmcb_configure_qemu_reset_vector_v1(vmcb) ||
	    amdvm_vm_cpu_state_init(&owner, vmcb, gprs, xstate) ||
	    amdvm_vm_cpu_state_get(&owner, vmcb, gprs, xstate, &input) ||
	    amdvm_vmcb_validate_x86_state_v1(&input) != 0) {
		fprintf(stderr, "benchmark fixture is invalid\n");
		free(vmcb);
		free(xstate);
		return 2;
	}
	for (unsigned i = 0; i < SAMPLES; ++i) {
		if ((i & 1u) == 0) {
			safe[i] = measure(&owner, vmcb, gprs, xstate, &input, 0);
			prevalidated[i] = measure(&owner, vmcb, gprs, xstate, &input, 1);
		} else {
			prevalidated[i] = measure(&owner, vmcb, gprs, xstate, &input, 1);
			safe[i] = measure(&owner, vmcb, gprs, xstate, &input, 0);
		}
	}
	sort(safe);
	sort(prevalidated);
	printf("samples=%u iterations_per_sample=%u safe_set_median_ns=%.1f prevalidated_set_median_ns=%.1f saved_ns_per_state_stage=%.1f reduction_pct=%.2f\n",
	    SAMPLES, ITERATIONS, safe[SAMPLES / 2], prevalidated[SAMPLES / 2],
	    safe[SAMPLES / 2] - prevalidated[SAMPLES / 2],
	    100.0 * (safe[SAMPLES / 2] - prevalidated[SAMPLES / 2]) /
	    safe[SAMPLES / 2]);
	free(vmcb);
	free(xstate);
	return 0;
}
