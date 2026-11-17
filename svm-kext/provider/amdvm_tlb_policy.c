#include "amdvm_tlb_policy.h"

#include <string.h>

void
amdvm_tlb_policy_init(amdvm_tlb_policy_t *policy)
{
	if (policy == NULL) return;
	memset(policy, 0, sizeof(*policy));
	policy->last_cpu = -1;
	policy->flush_generation = 1;
}

void
amdvm_tlb_policy_request_flush(amdvm_tlb_policy_t *policy)
{
	if (policy != NULL) __sync_fetch_and_add(&policy->flush_generation, 1u);
}

void
amdvm_tlb_policy_observe_pat(amdvm_tlb_policy_t *policy,
	uint64_t architectural_pat, uint64_t execution_pat)
{
	if (policy == NULL) return;
	if (!policy->pat_valid || policy->architectural_pat != architectural_pat ||
	    policy->execution_pat != execution_pat)
		amdvm_tlb_policy_request_flush(policy);
	policy->architectural_pat = architectural_pat;
	policy->execution_pat = execution_pat;
	policy->pat_valid = 1;
}

void
amdvm_tlb_policy_observe_generation(amdvm_tlb_policy_t *policy,
	uint64_t generation)
{
	if (policy == NULL) return;
	if (policy->translation_generation != generation)
		amdvm_tlb_policy_request_flush(policy);
	policy->translation_generation = generation;
}

int
amdvm_tlb_policy_prepare(amdvm_tlb_policy_t *policy, uint32_t asid,
	uint32_t max_asid, int cpu, int flush_by_asid, uint64_t *tlb_control)
{
	if (policy == NULL || tlb_control == NULL || !asid || asid > max_asid ||
	    cpu < 0) return -1;
	if (policy->last_cpu != cpu) amdvm_tlb_policy_request_flush(policy);
	policy->prepared_generation = __sync_val_compare_and_swap(
	    &policy->flush_generation, 0u, 0u);
	if (!flush_by_asid) {
		*tlb_control = ((uint64_t)AMDVM_TLB_FLUSH_ALL_ASIDS << 32) | asid;
		return 0;
	}
	/*
	 * ASID and TLB_CONTROL are adjacent 32-bit VMCB fields at offset 0x58.
	 * A zero flush command must still carry the non-zero ASID; returning zero
	 * here makes VMRUN reject the VMCB as invalid guest state.
	 */
	*tlb_control = asid;
	if (policy->prepared_generation != policy->completed_generation) {
		*tlb_control |= (uint64_t)AMDVM_TLB_FLUSH_ASID << 32;
	}
	return 0;
}

void
amdvm_tlb_policy_complete(amdvm_tlb_policy_t *policy, int cpu,
	int vmrun_succeeded, uint64_t exit_code)
{
	if (policy == NULL || !vmrun_succeeded ||
	    exit_code == AMDVM_TLB_EXIT_ERR ||
	    exit_code == AMDVM_TLB_EXIT_NOT_RUN ||
	    exit_code == AMDVM_TLB_EXIT_INVALID) return;
	policy->last_cpu = cpu;
	policy->completed_generation = policy->prepared_generation;
}

int
amdvm_tlb_asid_allocate(amdvm_tlb_asid_allocator_t *allocator,
	uint32_t max_asid, uint32_t *asid)
{
	if (allocator == NULL || asid == NULL || max_asid == 0 || max_asid > 32768u)
		return -1;
	if (max_asid > allocator->max_asid) allocator->max_asid = max_asid;
	for (uint32_t candidate = 1; candidate <= max_asid; ++candidate) {
		uint32_t word = candidate >> 6;
		uint64_t mask = UINT64_C(1) << (candidate & 63u);
		if (!(allocator->used[word] & mask)) {
			allocator->used[word] |= mask;
			*asid = candidate;
			return 0;
		}
	}
	return -1;
}

void
amdvm_tlb_asid_release(amdvm_tlb_asid_allocator_t *allocator, uint32_t asid)
{
	if (allocator == NULL || asid == 0 || asid > 32768u) return;
	allocator->used[asid >> 6] &= ~(UINT64_C(1) << (asid & 63u));
}
