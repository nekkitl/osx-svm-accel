/* Offline NPT sizing check through the descriptor-table structural capacity. */
#include "../provider/amdvm_npt.h"
#include "../amdvm_probe_abi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void check_ram_size(uint64_t ram_size, unsigned int gib)
{
	const amdvm_npt_range_t ranges[] = {
		{ .guest_gpa = 0, .size = ram_size },
		{ .guest_gpa = AMDVM_VCPU_RESET_ROM_GPA,
		  .size = AMDVM_NPT_PAGE_SIZE },
	};
	size_t required_pages = 0;

	assert(amdvm_npt_required_pages(ranges,
	    sizeof(ranges) / sizeof(ranges[0]), 48, &required_pages) ==
	    AMDVM_NPT_OK);
	assert(required_pages > 1u);
	assert(required_pages <= UINT32_MAX / AMDVM_NPT_PAGE_SIZE);
	printf("%u GiB guest plus reset ROM: %zu NPT pages (%zu bytes): PASS\n",
	    gib, required_pages, required_pages * AMDVM_NPT_PAGE_SIZE);
}

int main(void)
{
	const uint64_t four_gib = UINT64_C(4) * 1024u * 1024u * 1024u;
	const uint64_t one_gib = UINT64_C(1) * 1024u * 1024u * 1024u;
	const uint64_t two_gib = UINT64_C(2) * 1024u * 1024u * 1024u;
	const amdvm_npt_range_t ranges[] = {
		{ .guest_gpa = 0,
		  .size = AMDVM_VCPU_RESET_ROM_GPA },
		{ .guest_gpa = AMDVM_VCPU_RESET_ROM_GPA,
		  .size = AMDVM_NPT_PAGE_SIZE },
		{ .guest_gpa = four_gib,
		  .size = AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES -
		      AMDVM_VCPU_RESET_ROM_GPA },
	};
	size_t required_pages = 0;

	assert(AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES ==
	    (uint64_t)AMDVM_VCPU_MEMORY_MAX_RANGES *
	    AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES);
	assert(AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES ==
	    AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES +
	    AMDVM_VCPU_MEMORY_ROM_ALIAS_ALLOWANCE_BYTES);
	check_ram_size(one_gib, 1);
	check_ram_size(two_gib, 2);
	assert(amdvm_npt_required_pages(ranges,
	    sizeof(ranges) / sizeof(ranges[0]), 48, &required_pages) ==
	    AMDVM_NPT_OK);
	assert(required_pages > 1u);
	assert(required_pages <= UINT32_MAX / AMDVM_NPT_PAGE_SIZE);
	printf("structural-capacity guest NPT sizing: %zu pages (%zu bytes): PASS\n",
	    required_pages, required_pages * AMDVM_NPT_PAGE_SIZE);
	return 0;
}
