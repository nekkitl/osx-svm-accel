/* Offline, software-only allocation/translation test for 2-16 GiB guest RAM. */
#include "../provider/amdvm_npt.h"
#include "../amdvm_probe_abi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096u

static void
check_scatter_ram_size(uint64_t ram_bytes, unsigned gib)
{
	amdvm_npt_range_t ranges[3];
	size_t range_count = 0;
	uint64_t low_bytes = ram_bytes < AMDVM_VCPU_RESET_ROM_GPA ?
	    ram_bytes : AMDVM_VCPU_RESET_ROM_GPA;
	uint64_t high_bytes = ram_bytes - low_bytes;
	size_t pool_pages = 0;
	const size_t ram_pages = (size_t)(ram_bytes / PAGE_SIZE);
	uint8_t *pool = NULL;
	uint64_t *pool_pas = NULL;
	uint64_t *guest_pas = NULL;
	size_t *indexes = NULL;
	amdvm_npt_t npt = {0};
	amdvm_npt_translation_t translation = {0};
	const uint64_t pool_hpa_base = UINT64_C(0x10000000);
	const uint64_t guest_hpa_base = UINT64_C(0x800000000);
	size_t i;

	ranges[range_count++] = (amdvm_npt_range_t) {
		.guest_gpa = 0, .size = low_bytes
	};
	ranges[range_count++] = (amdvm_npt_range_t) {
		.guest_gpa = AMDVM_VCPU_RESET_ROM_GPA, .size = PAGE_SIZE
	};
	if (high_bytes != 0) {
		ranges[range_count++] = (amdvm_npt_range_t) {
			.guest_gpa = UINT64_C(0x100000000), .size = high_bytes
		};
	}
	assert(amdvm_npt_required_pages(ranges, range_count, 48u, &pool_pages) ==
	    AMDVM_NPT_OK);
	if (gib == 2u) assert(pool_pages == 1030u);
	else if (gib == 8u) assert(pool_pages == 4108u);
	else assert(gib >= 12u && pool_pages > 4108u);
	assert(posix_memalign((void **)&pool, PAGE_SIZE,
	    pool_pages * PAGE_SIZE) == 0);
	pool_pas = (uint64_t *)calloc(pool_pages, sizeof(*pool_pas));
	indexes = (size_t *)calloc(pool_pages, sizeof(*indexes));
	guest_pas = (uint64_t *)calloc(ram_pages, sizeof(*guest_pas));
	assert(pool_pas != NULL && indexes != NULL && guest_pas != NULL);
	memset(pool, 0, pool_pages * PAGE_SIZE);
	for (i = 0; i < pool_pages; ++i)
		pool_pas[i] = pool_hpa_base + (uint64_t)i * 2u * PAGE_SIZE;
	for (i = 0; i < ram_pages; ++i)
		guest_pas[i] = guest_hpa_base + (uint64_t)i * PAGE_SIZE;
	assert(amdvm_npt_init_scatter(&npt, pool, pool_pas, indexes,
	    pool_pages, 48u, 48u) == AMDVM_NPT_OK);
	size_t low_pages = (size_t)(low_bytes / PAGE_SIZE);
	assert(amdvm_npt_map_4k_pages(&npt, 0, guest_pas, low_pages,
	    AMDVM_NPT_READ | AMDVM_NPT_WRITE | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK);
	if (high_bytes != 0) {
		assert(amdvm_npt_map_4k_pages(&npt, UINT64_C(0x100000000),
		    guest_pas + low_pages, ram_pages - low_pages,
		    AMDVM_NPT_READ | AMDVM_NPT_WRITE | AMDVM_NPT_EXECUTE) ==
		    AMDVM_NPT_OK);
	}
	assert(amdvm_npt_map_4k(&npt, AMDVM_VCPU_RESET_ROM_GPA,
	    UINT64_C(0xc00000000), AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) ==
	    AMDVM_NPT_OK);
	assert(npt.pages_used == pool_pages);
	assert(amdvm_npt_translate_4k(&npt, 0, &translation) == AMDVM_NPT_OK);
	assert(translation.host_physical_address == guest_hpa_base);
	assert(amdvm_npt_translate_4k(&npt, ram_bytes / 2u &
	    ~(uint64_t)(PAGE_SIZE - 1u), &translation) == AMDVM_NPT_OK);
	uint64_t midpoint_gpa = ram_bytes / 2u &
	    ~(uint64_t)(PAGE_SIZE - 1u);
	uint64_t midpoint_offset = midpoint_gpa < AMDVM_VCPU_RESET_ROM_GPA ?
	    midpoint_gpa : midpoint_gpa - PAGE_SIZE;
	assert(translation.host_physical_address == guest_hpa_base + midpoint_offset);
	uint64_t last_gpa = high_bytes == 0 ? low_bytes - PAGE_SIZE :
	    UINT64_C(0x100000000) + high_bytes - PAGE_SIZE;
	assert(amdvm_npt_translate_4k(&npt, last_gpa, &translation) == AMDVM_NPT_OK);
	assert(translation.host_physical_address == guest_hpa_base + ram_bytes - PAGE_SIZE);
	assert(amdvm_npt_translate_4k(&npt, AMDVM_VCPU_RESET_ROM_GPA,
	    &translation) == AMDVM_NPT_OK);
	assert(translation.host_physical_address == UINT64_C(0xc00000000));
	printf("PASS: mapped %u GiB RAM plus reset ROM through %zu non-contiguous NPT pages (%zu bytes); all %zu guest pages mapped, root/midpoint/boundary translations verified.\n",
	    gib, pool_pages, pool_pages * PAGE_SIZE, ram_pages);
	free(guest_pas);
	free(indexes);
	free(pool_pas);
	free(pool);
	return;
}

int
main(void)
{
	check_scatter_ram_size(UINT64_C(2) * 1024u * 1024u * 1024u, 2u);
	check_scatter_ram_size(UINT64_C(8) * 1024u * 1024u * 1024u, 8u);
	check_scatter_ram_size(UINT64_C(12) * 1024u * 1024u * 1024u, 12u);
	check_scatter_ram_size(UINT64_C(16) * 1024u * 1024u * 1024u, 16u);
	return 0;
}
