#include "amdvm_vm_npt.h"
#include <string.h>

int amdvm_vm_npt_required_pages(const amdvm_vm_memory_t *m, size_t *pages)
{
	amdvm_npt_range_t ranges[AMDVM_VCPU_MEMORY_MAX_RANGES + 1u];
	size_t i, at = 0;
	if (!m || !pages || !m->generation || !m->users ||
	    m->state != AMDVM_VM_MEMORY_READY || !m->range_count ||
	    m->range_count > AMDVM_VCPU_MEMORY_MAX_RANGES) return AMDVM_NPT_INVALID;
	for (i = 0; i < m->range_count; ++i) {
		if (m->ranges[i].map.guest_physical_address < AMDVM_VCPU_RESET_ROM_GPA)
			++at;
		ranges[i].guest_gpa = m->ranges[i].map.guest_physical_address;
		ranges[i].size = m->ranges[i].map.size;
	}
	memmove(&ranges[at + 1], &ranges[at],
	    (m->range_count - at) * sizeof(ranges[0]));
	ranges[at].guest_gpa = AMDVM_VCPU_RESET_ROM_GPA;
	ranges[at].size = AMDVM_NPT_PAGE_SIZE;
	return amdvm_npt_required_pages(ranges, m->range_count + 1u, 48, pages);
}

int amdvm_vm_npt_build(const amdvm_vm_memory_t *m, amdvm_npt_t *npt,
    uint64_t reset_pa, amdvm_vm_npt_check_pa_t check_pa, void *opaque)
{
	size_t required, cursor = 0, i, j;
	if (!npt || !check_pa || !m || !m->page_pas ||
	    amdvm_vm_npt_required_pages(m, &required) ||
	    npt->pages_used != 1 || npt->pool_pages != required ||
	    !reset_pa || (reset_pa & (AMDVM_NPT_PAGE_SIZE - 1u)) ||
	    amdvm_npt_contains_hpa(npt, reset_pa) || check_pa(reset_pa, opaque))
		return AMDVM_NPT_INVALID;
	for (i = 0; i < m->range_count; ++i) {
		const amdvm_vm_memory_range_t *r = &m->ranges[i];
		size_t pages = (size_t)(r->map.size / AMDVM_NPT_PAGE_SIZE);
		if (r->first_page != cursor || pages > m->page_count - cursor)
			return AMDVM_NPT_INVALID;
		for (j = 0; j < pages; ++j) {
			uint64_t pa = m->page_pas[cursor++];
			if (pa == reset_pa || amdvm_npt_contains_hpa(npt, pa) ||
			    check_pa(pa, opaque) || amdvm_npt_map_4k(npt,
			    r->map.guest_physical_address + j * AMDVM_NPT_PAGE_SIZE,
			    pa, r->map.flags) != AMDVM_NPT_OK) return AMDVM_NPT_INVALID;
		}
	}
	if (cursor != m->page_count || amdvm_npt_map_4k(npt,
	    AMDVM_VCPU_RESET_ROM_GPA, reset_pa, AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) ||
	    npt->pages_used != required) return AMDVM_NPT_INVALID;
	/*
	 * Re-walking every 4 KiB leaf doubles the cost of building a multi-GiB
	 * NPT while only re-reading deterministic writes. Validate one leaf per
	 * page table plus both range boundaries, then the reset vector; the loop
	 * above already rejected raw-PA aliases, control memory and conflicts.
	 */
	for (i = 0; i < m->range_count; ++i) {
		const amdvm_vm_memory_range_t *r = &m->ranges[i];
		size_t pages = (size_t)(r->map.size / AMDVM_NPT_PAGE_SIZE);
		for (j = 0; j < pages; j += AMDVM_NPT_ENTRIES)
			if (amdvm_npt_validate_4k_mapping(npt,
			    r->map.guest_physical_address + j * AMDVM_NPT_PAGE_SIZE,
			    m->page_pas[r->first_page + j], r->map.flags)) return AMDVM_NPT_INVALID;
		if (pages > 1u && amdvm_npt_validate_4k_mapping(npt,
		    r->map.guest_physical_address + (pages - 1u) * AMDVM_NPT_PAGE_SIZE,
		    m->page_pas[r->first_page + pages - 1u], r->map.flags))
			return AMDVM_NPT_INVALID;
	}
	return amdvm_npt_validate_4k_mapping(npt, AMDVM_VCPU_RESET_ROM_GPA,
	    reset_pa, AMDVM_NPT_READ | AMDVM_NPT_EXECUTE);
}
