#ifndef AMDVM_ASID_CAPS_CACHE_H
#define AMDVM_ASID_CAPS_CACHE_H

#include <stdint.h>

/* CPUID SVM/ASID capabilities are immutable for a logical CPU after boot. */
typedef struct amdvm_asid_caps_cache {
	uint32_t cpu_id;
	uint32_t max_asid;
	int32_t flush_by_asid;
	uint32_t valid;
} amdvm_asid_caps_cache_t;

static inline int
amdvm_asid_caps_cache_lookup(const amdvm_asid_caps_cache_t *cache,
	uint32_t cpu_id, uint32_t *max_asid, int *flush_by_asid)
{
	if (cache == 0 || max_asid == 0 || flush_by_asid == 0 ||
	    cache->valid == 0 || cache->cpu_id != cpu_id ||
	    cache->max_asid == 0) return 0;
	*max_asid = cache->max_asid;
	*flush_by_asid = cache->flush_by_asid != 0;
	return 1;
}

static inline int
amdvm_asid_caps_cache_store(amdvm_asid_caps_cache_t *cache,
	uint32_t cpu_id, uint32_t max_asid, int flush_by_asid)
{
	if (cache == 0 || max_asid == 0) return -1;
	cache->valid = 0;
	cache->cpu_id = cpu_id;
	cache->max_asid = max_asid;
	cache->flush_by_asid = flush_by_asid != 0;
	__sync_synchronize();
	cache->valid = 1;
	return 0;
}

#endif
