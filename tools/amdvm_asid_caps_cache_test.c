#include "../svm-kext/provider/amdvm_asid_caps_cache.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	amdvm_asid_caps_cache_t cache;
	uint32_t max_asid = 0;
	int flush_by_asid = -1;

	memset(&cache, 0, sizeof(cache));
	assert(!amdvm_asid_caps_cache_lookup(&cache, 3, &max_asid,
	    &flush_by_asid));
	assert(amdvm_asid_caps_cache_store(&cache, 3, 32768, 1) == 0);
	assert(amdvm_asid_caps_cache_lookup(&cache, 3, &max_asid,
	    &flush_by_asid));
	assert(max_asid == 32768 && flush_by_asid == 1);
	assert(!amdvm_asid_caps_cache_lookup(&cache, 4, &max_asid,
	    &flush_by_asid));
	assert(amdvm_asid_caps_cache_store(&cache, 4, 1024, 0) == 0);
	assert(!amdvm_asid_caps_cache_lookup(&cache, 3, &max_asid,
	    &flush_by_asid));
	assert(amdvm_asid_caps_cache_lookup(&cache, 4, &max_asid,
	    &flush_by_asid));
	assert(max_asid == 1024 && flush_by_asid == 0);
	assert(amdvm_asid_caps_cache_store(&cache, 4, 0, 1) == -1);
	assert(amdvm_asid_caps_cache_lookup(&cache, 4, &max_asid,
	    &flush_by_asid));
	assert(max_asid == 1024 && flush_by_asid == 0);
	puts("amdvm per-CPU ASID capability cache: PASS");
	return 0;
}
