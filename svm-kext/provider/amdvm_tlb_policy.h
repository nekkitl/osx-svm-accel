#ifndef AMDVM_TLB_POLICY_H
#define AMDVM_TLB_POLICY_H

#include <stdint.h>

#define AMDVM_TLB_FLUSH_ASID 3u
#define AMDVM_TLB_FLUSH_ALL_ASIDS 1u
#define AMDVM_TLB_EXIT_INVALID UINT64_C(0xffffffff)
#define AMDVM_TLB_EXIT_ERR UINT64_MAX
#define AMDVM_TLB_EXIT_NOT_RUN (UINT64_MAX - 1u)

typedef struct amdvm_tlb_policy {
	int32_t last_cpu;
	uint32_t reserved;
	uint32_t pat_valid;
	uint64_t architectural_pat;
	uint64_t execution_pat;
	uint64_t translation_generation;
	uint64_t flush_generation;
	uint64_t completed_generation;
	uint64_t prepared_generation;
} amdvm_tlb_policy_t;

void amdvm_tlb_policy_init(amdvm_tlb_policy_t *policy);
void amdvm_tlb_policy_request_flush(amdvm_tlb_policy_t *policy);
void amdvm_tlb_policy_observe_pat(amdvm_tlb_policy_t *policy,
	uint64_t architectural_pat, uint64_t execution_pat);
void amdvm_tlb_policy_observe_generation(amdvm_tlb_policy_t *policy,
	uint64_t generation);
int amdvm_tlb_policy_prepare(amdvm_tlb_policy_t *policy, uint32_t asid,
	uint32_t max_asid, int cpu, int flush_by_asid, uint64_t *tlb_control);
void amdvm_tlb_policy_complete(amdvm_tlb_policy_t *policy, int cpu,
	int vmrun_succeeded, uint64_t exit_code);

/* Caller serializes access to the allocator. ASID zero is reserved. */
typedef struct amdvm_tlb_asid_allocator {
	uint32_t max_asid;
	uint64_t used[513];
} amdvm_tlb_asid_allocator_t;

int amdvm_tlb_asid_allocate(amdvm_tlb_asid_allocator_t *allocator,
	uint32_t max_asid, uint32_t *asid);
void amdvm_tlb_asid_release(amdvm_tlb_asid_allocator_t *allocator,
	uint32_t asid);

#endif
