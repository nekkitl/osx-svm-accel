#include "amdvm_guest_memory.h"

#define AMDVM_GUEST_MEMORY_PAGE_SIZE UINT64_C(4096)

int
amdvm_guest_memory_account_range(uint64_t size, uint32_t flags,
	uint64_t *total_mapped_pages, uint64_t *total_writable_pages)
{
	const uint32_t valid_flags = AMDVM_VCPU_MEMORY_READ |
	    AMDVM_VCPU_MEMORY_WRITE | AMDVM_VCPU_MEMORY_EXEC;
	uint64_t page_count;
	uint64_t mapped_limit_pages;
	uint64_t writable_limit_pages;
	uint64_t mapped_after;
	uint64_t writable_after;

	if (size == 0 || (size & (AMDVM_GUEST_MEMORY_PAGE_SIZE - 1u)) != 0 ||
	    total_mapped_pages == 0 || total_writable_pages == 0 ||
	    total_mapped_pages == total_writable_pages ||
	    (flags & AMDVM_VCPU_MEMORY_READ) == 0 || (flags & ~valid_flags) != 0) {
		return -1;
	}

	page_count = size / AMDVM_GUEST_MEMORY_PAGE_SIZE;
	mapped_limit_pages = AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES /
	    AMDVM_GUEST_MEMORY_PAGE_SIZE;
	writable_limit_pages = AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES /
	    AMDVM_GUEST_MEMORY_PAGE_SIZE;
	if (*total_mapped_pages > mapped_limit_pages ||
	    page_count > mapped_limit_pages - *total_mapped_pages) {
		return -1;
	}
	mapped_after = *total_mapped_pages + page_count;
	writable_after = *total_writable_pages;
	if (flags & AMDVM_VCPU_MEMORY_WRITE) {
		if (writable_after > writable_limit_pages ||
		    page_count > writable_limit_pages - writable_after) {
			return -1;
		}
		writable_after += page_count;
	}

	*total_mapped_pages = mapped_after;
	*total_writable_pages = writable_after;
	return 0;
}
