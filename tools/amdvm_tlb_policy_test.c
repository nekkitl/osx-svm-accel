#include "../svm-kext/provider/amdvm_tlb_policy.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int
main(void)
{
	amdvm_tlb_policy_t policy;
	amdvm_tlb_asid_allocator_t allocator;
	uint64_t control = UINT64_MAX;
	uint32_t a, b;

	amdvm_tlb_policy_init(&policy);
	assert(policy.flush_generation != policy.completed_generation &&
	    policy.last_cpu == -1);
	assert(amdvm_tlb_policy_prepare(&policy, 7, 32768, 2, 1, &control) == 0);
	assert(control == (((uint64_t)AMDVM_TLB_FLUSH_ASID << 32) | 7));
	amdvm_tlb_policy_complete(&policy, 2, 1, 0x78);
	assert(amdvm_tlb_policy_prepare(&policy, 7, 32768, 2, 1, &control) == 0);
	assert(control == 7); /* no flush, but preserve the nonzero ASID */
	amdvm_tlb_policy_complete(&policy, 2, 1, 0x78);
	assert(amdvm_tlb_policy_prepare(&policy, 7, 32768, 3, 1, &control) == 0);
	assert(control == (((uint64_t)AMDVM_TLB_FLUSH_ASID << 32) | 7));
	amdvm_tlb_policy_complete(&policy, 3, 0, 0x78);
	assert(policy.flush_generation != policy.completed_generation &&
	    policy.last_cpu == 2);
	assert(amdvm_tlb_policy_prepare(&policy, 7, 32768, 3, 1, &control) == 0);
	assert(control == (((uint64_t)AMDVM_TLB_FLUSH_ASID << 32) | 7));
	amdvm_tlb_policy_complete(&policy, 3, 1, 0x78);
	amdvm_tlb_policy_observe_pat(&policy, 0x100, 0x200);
	assert(policy.flush_generation != policy.completed_generation);
	assert(amdvm_tlb_policy_prepare(&policy, 7, 32768, 3, 1, &control) == 0);
	assert(control == (((uint64_t)AMDVM_TLB_FLUSH_ASID << 32) | 7));
	amdvm_tlb_policy_complete(&policy, 3, 1, 0x78);
	amdvm_tlb_policy_observe_pat(&policy, 0x100, 0x200);
	assert(policy.flush_generation == policy.completed_generation);
	amdvm_tlb_policy_observe_pat(&policy, 0x101, 0x200);
	assert(policy.flush_generation != policy.completed_generation);
	amdvm_tlb_policy_observe_generation(&policy, 5);
	assert(policy.flush_generation != policy.completed_generation &&
	    policy.translation_generation == 5);
	assert(amdvm_tlb_policy_prepare(&policy, 7, 32768, 3, 0, &control) == 0);
	assert(control == (((uint64_t)AMDVM_TLB_FLUSH_ALL_ASIDS << 32) | 7));
	/* Legacy hardware must keep flushing even after a successful RUN. */
	amdvm_tlb_policy_complete(&policy, 3, 1, 0x78);
	assert(amdvm_tlb_policy_prepare(&policy, 7, 32768, 3, 0, &control) == 0);
	assert(control == (((uint64_t)AMDVM_TLB_FLUSH_ALL_ASIDS << 32) | 7));

	/* An invalidation after prepare must survive successful VMRUN completion. */
	amdvm_tlb_policy_init(&policy);
	assert(amdvm_tlb_policy_prepare(&policy, 11, 32768, 4, 1, &control) == 0);
	assert(control == (((uint64_t)AMDVM_TLB_FLUSH_ASID << 32) | 11));
	uint64_t prepared_generation = policy.prepared_generation;
	amdvm_tlb_policy_request_flush(&policy);
	amdvm_tlb_policy_complete(&policy, 4, 1, 0x78);
	assert(policy.completed_generation == prepared_generation);
	assert(policy.flush_generation != policy.completed_generation);
	assert(amdvm_tlb_policy_prepare(&policy, 11, 32768, 4, 1, &control) == 0);
	assert(control == (((uint64_t)AMDVM_TLB_FLUSH_ASID << 32) | 11));
	amdvm_tlb_policy_complete(&policy, 4, 1, 0x78);
	assert(policy.flush_generation == policy.completed_generation);

	/* A successful assembly return without a valid exit cannot consume a flush. */
	amdvm_tlb_policy_request_flush(&policy);
	assert(amdvm_tlb_policy_prepare(&policy, 11, 32768, 4, 1, &control) == 0);
	uint64_t invalid_generation = policy.prepared_generation;
	amdvm_tlb_policy_complete(&policy, 4, 1, AMDVM_TLB_EXIT_NOT_RUN);
	assert(policy.completed_generation != invalid_generation);
	amdvm_tlb_policy_complete(&policy, 4, 1, AMDVM_TLB_EXIT_INVALID);
	assert(policy.completed_generation != invalid_generation);
	/* UINT64_MAX is the valid SVM_EXIT_ERR exit code. */
	amdvm_tlb_policy_complete(&policy, 4, 1, UINT64_MAX);
	assert(policy.completed_generation != invalid_generation);

	assert(amdvm_tlb_policy_prepare(&policy, 0, 32768, 3, 1, &control) != 0);
	assert(amdvm_tlb_policy_prepare(&policy, 32769, 32768, 3, 1, &control) != 0);

	memset(&allocator, 0, sizeof(allocator));
	assert(amdvm_tlb_asid_allocate(&allocator, 2, &a) == 0 && a == 1);
	assert(amdvm_tlb_asid_allocate(&allocator, 2, &b) == 0 && b == 2);
	assert(amdvm_tlb_asid_allocate(&allocator, 2, &a) != 0);
	amdvm_tlb_asid_release(&allocator, 1);
	assert(amdvm_tlb_asid_allocate(&allocator, 2, &a) == 0 && a == 1);
	memset(&allocator, 0, sizeof(allocator));
	for (unsigned i = 0; i < 512; ++i) allocator.used[i] = UINT64_MAX;
	assert(amdvm_tlb_asid_allocate(&allocator, 32768, &a) == 0 && a == 32768);
	amdvm_tlb_asid_release(&allocator, 32768);
	assert(amdvm_tlb_asid_allocate(&allocator, 32768, &a) == 0 && a == 32768);
	return 0;
}
