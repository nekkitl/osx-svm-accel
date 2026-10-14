#ifndef AMDVM_VM_LIFECYCLE_H
#define AMDVM_VM_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#define AMDVM_VM_LIFECYCLE_MAX_VCPUS 64u

enum amdvm_vm_lifecycle_state {
	AMDVM_VM_STATE_EMPTY = 0,
	AMDVM_VM_STATE_OPEN = 1,
	AMDVM_VM_STATE_MAP_PREPARING = 2,
	AMDVM_VM_STATE_READY = 3,
	AMDVM_VM_STATE_CLOSING = 4,
	AMDVM_VM_STATE_QUARANTINED = 5,
	AMDVM_VM_STATE_DESTROYED = 6
};

enum amdvm_vm_vcpu_state {
	AMDVM_VM_VCPU_EMPTY = 0,
	AMDVM_VM_VCPU_READY = 1,
	AMDVM_VM_VCPU_RUNNING = 2,
	AMDVM_VM_VCPU_STOPPED = 3,
	AMDVM_VM_VCPU_HALTED = 4
};

enum amdvm_vm_lifecycle_result {
	AMDVM_VM_LIFECYCLE_OK = 0,
	AMDVM_VM_LIFECYCLE_INVALID = -1,
	AMDVM_VM_LIFECYCLE_BAD_STATE = -2,
	AMDVM_VM_LIFECYCLE_EXISTS = -3,
	AMDVM_VM_LIFECYCLE_BUSY = -4,
	AMDVM_VM_LIFECYCLE_FULL = -5,
	AMDVM_VM_LIFECYCLE_STALE_MAP = -6
};

typedef struct amdvm_vm_vcpu_slot {
	uint32_t id;
	uint32_t state;
	uint32_t active_runs;
	uint32_t allocated;
	uint64_t map_generation;
} amdvm_vm_vcpu_slot_t;

/*
 * Metadata-only VM lifecycle model. Callers must serialize every operation
 * with the VM lock. The kernel has not integrated this model yet; it contains
 * no IOUserClient, memory descriptor, NPT, SVM lease, or VMRUN operation.
 */
typedef struct amdvm_vm_lifecycle {
	uint32_t state;
	uint32_t vcpu_count;
	uint32_t active_runs;
	uint32_t map_committed;
	uint32_t resources_owned;
	uint32_t reserved;
	uint64_t map_generation;
	amdvm_vm_vcpu_slot_t vcpus[AMDVM_VM_LIFECYCLE_MAX_VCPUS];
} amdvm_vm_lifecycle_t;

int amdvm_vm_lifecycle_open(amdvm_vm_lifecycle_t *vm);
int amdvm_vm_lifecycle_map_begin(amdvm_vm_lifecycle_t *vm);
int amdvm_vm_lifecycle_map_abort(amdvm_vm_lifecycle_t *vm);
int amdvm_vm_lifecycle_map_commit(amdvm_vm_lifecycle_t *vm);
int amdvm_vm_lifecycle_vcpu_create(amdvm_vm_lifecycle_t *vm, uint32_t vcpu_id,
	uint32_t *slot_out);
int amdvm_vm_lifecycle_run_begin(amdvm_vm_lifecycle_t *vm, uint32_t slot,
	uint64_t map_generation, bool event_pending);
int amdvm_vm_lifecycle_run_end(amdvm_vm_lifecycle_t *vm, uint32_t slot,
	bool guest_halted);
int amdvm_vm_lifecycle_vcpu_destroy(amdvm_vm_lifecycle_t *vm,
	uint32_t slot);
int amdvm_vm_lifecycle_close(amdvm_vm_lifecycle_t *vm);
int amdvm_vm_lifecycle_destroy(amdvm_vm_lifecycle_t *vm);
int amdvm_vm_lifecycle_quarantine(amdvm_vm_lifecycle_t *vm);
int amdvm_vm_lifecycle_finish_quarantine(amdvm_vm_lifecycle_t *vm,
	bool cleanup_succeeded);
bool amdvm_vm_lifecycle_can_free(const amdvm_vm_lifecycle_t *vm);

#endif
