#ifndef AMDVM_VM_SHARED_RUN_H
#define AMDVM_VM_SHARED_RUN_H

#include <stdint.h>

#include "amdvm_vm_abi_draft.h"

/* One fixed, cache-line-aligned handoff slot per vCPU. */
typedef struct __attribute__((aligned(64))) amdvm_vm_shared_run_slot {
	volatile uint32_t owner;
	uint32_t request_size;
	uint64_t sequence;
	uint64_t completed_sequence;
	int32_t completion_status;
	uint32_t reserved;
	uint8_t request[AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE];
	uint8_t result[sizeof(amdvm_vm_abi_draft_vcpu_run_result_t)];
} amdvm_vm_shared_run_slot_t;

typedef struct amdvm_vm_shared_run_area {
	amdvm_vm_shared_run_slot_t slots[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
} amdvm_vm_shared_run_area_t;

typedef struct amdvm_vm_shared_run_call {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t request_size;
	uint64_t sequence;
	uint64_t map_generation;
} amdvm_vm_shared_run_call_t;

typedef struct amdvm_vm_shared_run_ack {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	int32_t status;
	uint64_t sequence;
	uint64_t map_generation;
} amdvm_vm_shared_run_ack_t;

#define AMDVM_VM_SHARED_RUN_FREE      0u
#define AMDVM_VM_SHARED_RUN_WRITING   1u
#define AMDVM_VM_SHARED_RUN_READY     2u
#define AMDVM_VM_SHARED_RUN_PROVIDER  3u
#define AMDVM_VM_SHARED_RUN_COMPLETE  4u

static inline int
amdvm_vm_shared_run_request_size_valid(uint32_t size)
{
	return size == AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V2_SIZE ||
	    size == AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE;
}

static inline int
amdvm_vm_shared_run_client_acquire(amdvm_vm_shared_run_slot_t *slot)
{
	return slot != 0 && __sync_bool_compare_and_swap(&slot->owner,
	    AMDVM_VM_SHARED_RUN_FREE, AMDVM_VM_SHARED_RUN_WRITING);
}

static inline int
amdvm_vm_shared_run_client_publish(amdvm_vm_shared_run_slot_t *slot,
	uint64_t sequence, uint32_t request_size)
{
	if (slot == 0 || sequence == 0 ||
	    !amdvm_vm_shared_run_request_size_valid(request_size) ||
	    __sync_val_compare_and_swap(&slot->owner, 0, 0) !=
	    AMDVM_VM_SHARED_RUN_WRITING) {
		return 0;
	}
	slot->request_size = request_size;
	slot->sequence = sequence;
	slot->completed_sequence = 0;
	slot->completion_status = 0;
	__sync_synchronize();
	return __sync_bool_compare_and_swap(&slot->owner,
	    AMDVM_VM_SHARED_RUN_WRITING, AMDVM_VM_SHARED_RUN_READY);
}

static inline int
amdvm_vm_shared_run_provider_claim(amdvm_vm_shared_run_slot_t *slot,
	uint64_t sequence, uint32_t request_size)
{
	if (slot == 0 || sequence == 0 ||
	    !amdvm_vm_shared_run_request_size_valid(request_size) ||
	    slot->sequence != sequence || slot->request_size != request_size) {
		return 0;
	}
	if (!__sync_bool_compare_and_swap(&slot->owner,
	    AMDVM_VM_SHARED_RUN_READY, AMDVM_VM_SHARED_RUN_PROVIDER)) {
		return 0;
	}
	__sync_synchronize();
	return 1;
}

static inline int
amdvm_vm_shared_run_provider_complete(amdvm_vm_shared_run_slot_t *slot,
	uint64_t sequence, int32_t status)
{
	if (slot == 0 || sequence == 0 || slot->sequence != sequence ||
	    __sync_val_compare_and_swap(&slot->owner, 0, 0) !=
	    AMDVM_VM_SHARED_RUN_PROVIDER) {
		return 0;
	}
	slot->completion_status = status;
	slot->completed_sequence = sequence;
	__sync_synchronize();
	return __sync_bool_compare_and_swap(&slot->owner,
	    AMDVM_VM_SHARED_RUN_PROVIDER, AMDVM_VM_SHARED_RUN_COMPLETE);
}

static inline int
amdvm_vm_shared_run_client_release(amdvm_vm_shared_run_slot_t *slot,
	uint64_t sequence)
{
	if (slot == 0 || sequence == 0 || slot->sequence != sequence ||
	    slot->completed_sequence != sequence ||
	    __sync_val_compare_and_swap(&slot->owner, 0, 0) !=
	    AMDVM_VM_SHARED_RUN_COMPLETE) {
		return 0;
	}
	__sync_synchronize();
	return __sync_bool_compare_and_swap(&slot->owner,
	    AMDVM_VM_SHARED_RUN_COMPLETE, AMDVM_VM_SHARED_RUN_FREE);
}

/* Only abandon a request when transport failed before provider ownership. */
static inline int
amdvm_vm_shared_run_client_abort_ready(amdvm_vm_shared_run_slot_t *slot,
	uint64_t sequence)
{
	if (slot == 0 || sequence == 0 || slot->sequence != sequence) {
		return 0;
	}
	return __sync_bool_compare_and_swap(&slot->owner,
	    AMDVM_VM_SHARED_RUN_READY, AMDVM_VM_SHARED_RUN_FREE);
}

#endif /* AMDVM_VM_SHARED_RUN_H */
