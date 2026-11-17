#include <assert.h>
#include <stdint.h>
#include "../svm-kext/provider/amdvm_run_buffer.h"
#include "../svm-kext/provider/amdvm_run_slot_lifetime.h"

int
main(void)
{
	volatile uint32_t busy = 0;
	volatile uint64_t run_state = 0;
	uint32_t vcpu_id = UINT32_MAX;
	amdvm_run_request_header_t header = { 3u, 64u, 7u };
	assert(!amdvm_run_structure_requires_staging_copy(0)); /* in-band/shared */
	assert(amdvm_run_structure_requires_staging_copy(1));  /* OOL descriptor */

	assert(amdvm_run_buffer_header_validate(&header, 64u, 48u, 64u,
	    8u, &vcpu_id) == 0);
	assert(vcpu_id == 7u);
	assert(amdvm_run_buffer_vcpu_matches(vcpu_id, 7, 8u) == 0);
	assert(amdvm_run_buffer_vcpu_matches(vcpu_id, 6, 8u) != 0);
	assert(amdvm_run_buffer_vcpu_matches(vcpu_id, -1, 8u) == 0);
	assert(amdvm_run_buffer_vcpu_matches(vcpu_id, -2, 8u) != 0);
	assert(amdvm_run_buffer_vcpu_matches(8u, 7, 8u) != 0);
	assert(amdvm_run_buffer_vcpu_matches(7u, 8, 8u) != 0);
	assert(amdvm_run_buffer_header_validate(&header, 48u, 48u, 64u,
	    8u, &vcpu_id) != 0);
	header.structure_size = 48u;
	assert(amdvm_run_buffer_header_validate(&header, 64u, 48u, 64u,
	    8u, &vcpu_id) != 0);
	header.structure_size = 64u;
	header.vcpu_id = 8u;
	assert(amdvm_run_buffer_header_validate(&header, 64u, 48u, 64u,
	    8u, &vcpu_id) != 0);

	assert(amdvm_run_buffer_try_reserve(&busy));
	assert(!amdvm_run_buffer_try_reserve(&busy));
	amdvm_run_buffer_release(&busy);
	assert(amdvm_run_buffer_try_reserve(&busy));
	amdvm_run_buffer_release(&busy);
	assert(busy == 0u);

	/* RUN reserves lifetime before staging state outside the global lock.
	 * Destroy rejects it during staging, and then until the buffer guard exits. */
	assert(amdvm_run_buffer_try_reserve(&busy));
	assert(amdvm_run_slot_try_acquire(&run_state) == 1);
	assert(!amdvm_run_slot_quiescent(&run_state, &busy));
	/* The active reference remains held throughout out-of-lock state staging. */
	assert(amdvm_run_slot_refcount(&run_state) == 1u && busy == 1u);
	assert(amdvm_run_slot_begin_destroy(&run_state) ==
	    AMDVM_RUN_SLOT_CLOSING + 1u);
	assert(amdvm_run_slot_release(&run_state) == 1);
	assert(amdvm_run_slot_refcount(&run_state) == 0u);
	assert(!amdvm_run_slot_quiescent(&run_state, &busy));
	amdvm_run_buffer_release(&busy);
	assert(amdvm_run_slot_quiescent(&run_state, &busy));
	assert(amdvm_run_slot_release(&run_state) != 0);
	return 0;
}
