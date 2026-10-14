#ifndef AMDVM_XSTATE_H
#define AMDVM_XSTATE_H

#include <stddef.h>
#include <stdint.h>

/* Provider image profile: x87, SSE, AVX. Disabled components are retained
 * across XSETBV, independently of the architectural guest XCR0. */
#define AMDVM_XSTATE_IMAGE_MASK UINT64_C(7)

/* Legacy SSE remains usable before CR4.OSXSAVE is enabled. The physical
 * execution mask is separate from the architectural guest XCR0. */
static inline uint64_t
amdvm_xstate_execution_mask(uint64_t xcr0, uint64_t cr4)
{
	return xcr0 | ((cr4 & (UINT64_C(1) << 18)) ? 0 : UINT64_C(2));
}

/* Validate the standard-format XSAVE header against a vCPU's enabled XCR0. */
int amdvm_xstate_validate_standard(const void *area, size_t size,
	uint64_t xcr0);

/* Validate and copy a standard-format XSAVE image to/from provider storage. */
int amdvm_xstate_copy_standard(void *destination, size_t destination_size,
	const void *source, size_t source_size, uint64_t xcr0);

#endif
