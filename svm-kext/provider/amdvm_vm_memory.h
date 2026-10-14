#ifndef AMDVM_VM_MEMORY_H
#define AMDVM_VM_MEMORY_H

#include "amdvm_quarantine.h"

/* Real descriptor ownership, independent of a VMCB or a running vCPU.
 * The caller serializes prepare/commit/acquire/release/close with a sleeping
 * VM lock. No descriptor operation may run under an IRQ-disabled spinlock. */
typedef struct amdvm_vm_memory_ops {
	void *(*allocate)(size_t bytes, void *opaque);
	void (*deallocate)(void *allocation, size_t bytes, void *opaque);
	int (*pin)(amdvm_pinned_user_region_t *, uint64_t, size_t, void *, void *);
	int (*unpin)(amdvm_pinned_user_region_t *, void *);
	int (*page_pa)(const amdvm_pinned_user_region_t *, size_t, uint8_t,
	    uint64_t *, void *);
	amdvm_quarantine_registry_t *quarantine;
	void *opaque;
} amdvm_vm_memory_ops_t;

typedef struct amdvm_vm_memory_limits {
	uint64_t max_guest_bytes;
	uint64_t max_mapped_bytes;
	uint8_t host_phys_bits;
} amdvm_vm_memory_limits_t;

typedef struct amdvm_vm_memory_range {
	amdvm_vcpu_memory_range_t map;
	size_t first_page;
} amdvm_vm_memory_range_t;

enum amdvm_vm_memory_status {
	AMDVM_VM_MEMORY_OK = 0,
	AMDVM_VM_MEMORY_INVALID = -1,
	AMDVM_VM_MEMORY_ALLOC_FAILED = -2,
	AMDVM_VM_MEMORY_PIN_FAILED = -3,
	AMDVM_VM_MEMORY_BAD_PA = -4,
	AMDVM_VM_MEMORY_ALIAS = -5,
	AMDVM_VM_MEMORY_QUOTA = -6,
	AMDVM_VM_MEMORY_BUSY = -7,
	AMDVM_VM_MEMORY_CLEANUP_PENDING = -8,
	AMDVM_VM_MEMORY_STALE = -9
};

enum amdvm_vm_memory_state {
	AMDVM_VM_MEMORY_BUILDING = 1,
	AMDVM_VM_MEMORY_READY = 2,
	AMDVM_VM_MEMORY_CLOSING = 3,
	AMDVM_VM_MEMORY_QUARANTINED = 4
};

typedef struct amdvm_vm_memory {
	amdvm_vm_memory_ops_t ops;
	uint32_t state;
	uint32_t users;
	uint64_t generation;
	uint64_t writable_bytes;
	size_t range_count;
	size_t page_count;
	uint64_t *page_pas;
	uint32_t quarantine_slot;
	uint32_t slot_reserved;
	uint64_t quarantine_generation;
	amdvm_vm_memory_range_t ranges[AMDVM_VCPU_MEMORY_MAX_RANGES];
	amdvm_pinned_user_region_t pins[AMDVM_VCPU_MEMORY_MAX_RANGES];
} amdvm_vm_memory_t;

#ifdef __cplusplus
extern "C" {
#endif

int amdvm_vm_memory_validate(const amdvm_vcpu_memory_range_t *ranges,
	size_t count, const amdvm_vm_memory_limits_t *limits,
	size_t *page_count, uint64_t *writable_bytes);
/* On failure *candidate is NULL, except when cleanup is still pending.
 * That non-NULL object must be retained or orphaned through close(). */
int amdvm_vm_memory_prepare(amdvm_vm_memory_t **candidate,
	const amdvm_vcpu_memory_range_t *ranges, size_t count, void *task,
	const amdvm_vm_memory_limits_t *limits, const amdvm_vm_memory_ops_t *ops);
int amdvm_vm_memory_commit(amdvm_vm_memory_t *memory, uint64_t generation);
int amdvm_vm_memory_acquire(amdvm_vm_memory_t *memory, uint64_t generation);
int amdvm_vm_memory_release(amdvm_vm_memory_t *memory);
/* Block new borrowers first; pins remain alive until all users release.
 * orphan=1 transfers unresolved cleanup to process-independent quarantine. */
int amdvm_vm_memory_close(amdvm_vm_memory_t **memory, int orphan);
uint32_t amdvm_vm_memory_owner_count(void);
void amdvm_vm_memory_kernel_ops(amdvm_vm_memory_ops_t *ops);
int amdvm_vm_memory_kernel_limits(amdvm_vm_memory_limits_t *limits);

#ifdef __cplusplus
}
#endif
#endif
