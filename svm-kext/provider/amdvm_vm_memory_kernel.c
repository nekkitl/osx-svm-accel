#include "amdvm_vm_memory.h"
#include "amdvm_hlt_context.h"

#include <IOKit/IOLib.h>

int
amdvm_vm_memory_kernel_limits(amdvm_vm_memory_limits_t *limits)
{
	uint32_t a, b, c, d;
	if (!limits) return -1;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x80000000u), "c"(0u));
	if (a < 0x80000008u) return -1;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x80000008u), "c"(0u));
	limits->host_phys_bits = (uint8_t)a;
	if (limits->host_phys_bits < 32 || limits->host_phys_bits > 52) return -1;
	/* Structural wire/descriptor bound, not a promise of available RAM.
	 * Pinning and metadata allocation provide resource admission at prepare. */
	limits->max_mapped_bytes = AMDVM_VCPU_MEMORY_MAX_RANGES *
	    AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES;
	limits->max_guest_bytes = limits->max_mapped_bytes;
	return 0;
}

static void *allocate(size_t bytes, void *opaque)
{
	(void)opaque;
	return IOMalloc(bytes);
}

static void deallocate(void *p, size_t bytes, void *opaque)
{
	(void)opaque;
	IOFree(p, bytes);
}

static int pin(amdvm_pinned_user_region_t *p, uint64_t address,
	size_t bytes, void *task, void *opaque)
{
	(void)opaque;
	return amdvm_physmem_pin_user_region(p, address, bytes, task);
}

static int unpin(amdvm_pinned_user_region_t *p, void *opaque)
{
	(void)opaque;
	return amdvm_physmem_unpin_user_region(p);
}

static int page_pa(const amdvm_pinned_user_region_t *p, size_t page,
	uint8_t bits, uint64_t *pa, void *opaque)
{
	(void)opaque;
	return amdvm_physmem_user_page_pa(p, page, bits, pa);
}

void
amdvm_vm_memory_kernel_ops(amdvm_vm_memory_ops_t *ops)
{
	if (!ops) return;
	ops->allocate = allocate;
	ops->deallocate = deallocate;
	ops->pin = pin;
	ops->unpin = unpin;
	ops->page_pa = page_pa;
	ops->quarantine = amdvm_hlt_context_quarantine_registry();
	ops->opaque = NULL;
}
