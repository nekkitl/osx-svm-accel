#ifndef AMDVM_RUN_BUFFER_H
#define AMDVM_RUN_BUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct amdvm_run_request_header {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
} amdvm_run_request_header_t;

/* IOUserClient in-band structureInput stays valid through its synchronous
 * externalMethod call. Descriptor-backed input must be staged while prepared. */
static inline int
amdvm_run_structure_requires_staging_copy(int descriptor_backed)
{
	return descriptor_backed != 0;
}

static inline int
amdvm_run_buffer_header_validate(const amdvm_run_request_header_t *header,
	size_t supplied_size, size_t size_v2, size_t size_v3,
	uint32_t max_vcpus, uint32_t *vcpu_id)
{
	if (header == NULL || vcpu_id == NULL ||
	    (supplied_size != size_v2 && supplied_size != size_v3) ||
	    header->structure_size != supplied_size ||
	    header->vcpu_id >= max_vcpus) {
		return -1;
	}
	*vcpu_id = header->vcpu_id;
	return 0;
}

/* expected_vcpu_id == -1 selects the legacy caller-agnostic RUN path. */
static inline int
amdvm_run_buffer_vcpu_matches(uint32_t vcpu_id, int32_t expected_vcpu_id,
	uint32_t max_vcpus)
{
	if (expected_vcpu_id < -1 ||
	    (expected_vcpu_id >= 0 &&
	    (uint32_t)expected_vcpu_id >= max_vcpus) || vcpu_id >= max_vcpus ||
	    (expected_vcpu_id >= 0 && vcpu_id != (uint32_t)expected_vcpu_id)) {
		return -1;
	}
	return 0;
}

static inline int
amdvm_run_buffer_try_reserve(volatile uint32_t *busy)
{
	return busy != NULL && __sync_bool_compare_and_swap(busy, 0u, 1u);
}

static inline void
amdvm_run_buffer_release(volatile uint32_t *busy)
{
	if (busy != NULL) __sync_lock_release(busy);
}

#endif
