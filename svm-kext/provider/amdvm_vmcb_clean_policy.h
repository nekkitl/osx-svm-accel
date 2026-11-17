#ifndef AMDVM_VMCB_CLEAN_POLICY_H
#define AMDVM_VMCB_CLEAN_POLICY_H

#include "amdvm_vmcb.h"

/* Tracks the processor/VMCB identity that owns a reusable state-cache entry. */
typedef struct amdvm_vmcb_clean_policy {
	uint32_t valid;
	int32_t cpu;
	uint32_t asid;
	uint64_t vmcb_pa;
} amdvm_vmcb_clean_policy_t;

static inline uint32_t
amdvm_vmcb_clean_policy_prepare(const amdvm_vmcb_clean_policy_t *policy,
	int32_t cpu, uint32_t asid, uint64_t vmcb_pa,
	uint32_t unchanged_groups)
{
	if (policy == 0 || !policy->valid || cpu < 0 || asid == 0 ||
	    policy->cpu != cpu || policy->asid != asid ||
	    policy->vmcb_pa != vmcb_pa) {
		return 0;
	}
	return AMDVM_VMCB_CLEAN_STABLE_RUN_MASK |
	    (unchanged_groups & (AMDVM_VMCB_CLEAN_SEG | AMDVM_VMCB_CLEAN_DT |
	    AMDVM_VMCB_CLEAN_CR | AMDVM_VMCB_CLEAN_DR | AMDVM_VMCB_CLEAN_CR2));
}

static inline void
amdvm_vmcb_clean_policy_complete(amdvm_vmcb_clean_policy_t *policy,
	int32_t cpu, uint32_t asid, uint64_t vmcb_pa, int vmrun_succeeded,
	uint64_t exit_code)
{
	if (policy == 0) return;
	if (!vmrun_succeeded || cpu < 0 || asid == 0 || vmcb_pa == 0 ||
	    exit_code == AMDVM_SVM_EXIT_ERR ||
	    exit_code == AMDVM_SVM_EXIT_NOT_RUN ||
	    exit_code == AMDVM_SVM_EXIT_INVALID) {
		policy->valid = 0;
		return;
	}
	policy->cpu = cpu;
	policy->asid = asid;
	policy->vmcb_pa = vmcb_pa;
	policy->valid = 1;
}

#endif
