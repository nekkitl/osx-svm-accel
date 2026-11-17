#include "../svm-kext/provider/amdvm_vm_memory.h"
#include "../svm-kext/provider/amdvm_vm_npt.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

typedef struct fixture {
	amdvm_quarantine_registry_t registry;
	unsigned allocations, live_allocations, fail_alloc;
	unsigned pin_calls, live_pins, fail_pin, fail_unpin;
	unsigned bad_pa, alias_pa;
} fixture_t;

static void *allocate(size_t bytes, void *opaque)
{
	fixture_t *f = opaque;
	if (++f->allocations == f->fail_alloc) return NULL;
	void *p = malloc(bytes);
	assert(p); ++f->live_allocations; return p;
}

static void deallocate(void *p, size_t bytes, void *opaque)
{
	fixture_t *f = opaque;
	assert(p && bytes && f->live_allocations);
	--f->live_allocations; free(p);
}

static int pin(amdvm_pinned_user_region_t *p, uint64_t address,
	size_t bytes, void *task, void *opaque)
{
	fixture_t *f = opaque;
	if (++f->pin_calls == f->fail_pin) return -1;
	p->memory_descriptor = malloc(1);
	assert(p->memory_descriptor);
	p->user_address = address; p->size = bytes; p->owning_task = task;
	++f->live_pins; return 0;
}

static int unpin(amdvm_pinned_user_region_t *p, void *opaque)
{
	fixture_t *f = opaque;
	if (f->fail_unpin) return -1;
	assert(p->memory_descriptor && f->live_pins);
	free(p->memory_descriptor); --f->live_pins;
	memset(p, 0, sizeof(*p)); return 0;
}

static int page_pa(const amdvm_pinned_user_region_t *p, size_t page,
	uint8_t bits, uint64_t *pa, void *opaque)
{
	fixture_t *f = opaque;
	assert(bits == 48 && page < p->size / 4096u);
	*pa = f->bad_pa ? 3u : f->alias_pa ? 0x800000u :
	    p->user_address + page * 4096u;
	return 0;
}

static amdvm_vm_memory_ops_t ops(fixture_t *f)
{
	amdvm_vm_memory_ops_t o = { allocate, deallocate, pin, unpin, page_pa,
	    &f->registry, f };
	return o;
}

static amdvm_vm_memory_limits_t limits = { UINT64_C(16) << 30,
	UINT64_C(17) << 30, 48 };
static const amdvm_vcpu_memory_range_t ranges[2] = {
	{ .guest_physical_address = 0, .user_virtual_address = 0x1000000,
	  .size = 8192, .flags = 7 },
	{ .guest_physical_address = 0x400000, .user_virtual_address = 0x2000000,
	  .size = 4096, .flags = 5 }
};

static int prepare(fixture_t *f, amdvm_vm_memory_t **m,
	const amdvm_vcpu_memory_range_t *map, size_t count)
{
	amdvm_vm_memory_ops_t o = ops(f);
	return amdvm_vm_memory_prepare(m, map, count, f, &limits, &o);
}

static void clean(fixture_t *f)
{
	assert(!f->live_allocations && !f->live_pins);
	assert(amdvm_vm_memory_owner_count() == 0);
	for (unsigned i = 0; i < AMDVM_QUARANTINE_MAX_SLOTS; ++i)
		assert(f->registry.slots[i].state == AMDVM_QUARANTINE_FREE);
}

static int check_npt_pa(uint64_t pa, void *opaque)
{
	return opaque && pa == *(uint64_t *)opaque;
}

static void test_two_npts(amdvm_vm_memory_t *m)
{
	size_t count;
	assert(amdvm_vm_npt_required_pages(m, &count) == 0);
	amdvm_npt_t npts[2];
	void *pools[2];
	uint64_t *pas[2];
	size_t *indexes[2];
	for (unsigned v = 0; v < 2; ++v) {
		assert(posix_memalign(&pools[v], 4096, count * 4096) == 0);
		pas[v] = malloc(count * sizeof(uint64_t));
		indexes[v] = malloc(count * sizeof(size_t));
		assert(pas[v] && indexes[v]);
		for (size_t i = 0; i < count; ++i)
			pas[v][i] = 0x3000000u + v * 0x1000000u + i * 8192u;
		assert(amdvm_npt_init_scatter(&npts[v], pools[v], pas[v], indexes[v], count, 48, 48) == 0);
		assert(amdvm_vm_npt_build(m, &npts[v], 0x5000000u + v * 4096u, check_npt_pa, NULL) == 0);
		assert(amdvm_npt_validate_4k_mapping(&npts[v], 4096, m->page_pas[1], 7) == 0);
	}
	assert(amdvm_npt_root_pa(&npts[0]) != amdvm_npt_root_pa(&npts[1]));
	assert(amdvm_npt_set_4k_pat_index(&npts[0], 4096, 3) == 0);
	assert(amdvm_npt_validate_4k_pat_index(&npts[1], 4096, 0) == 0);
	/* A fresh rejected table is never published to another vCPU. */
	assert(amdvm_npt_init_scatter(&npts[0], pools[0], pas[0], indexes[0], count, 48, 48) == 0);
	uint64_t reject = m->page_pas[0];
	assert(amdvm_vm_npt_build(m, &npts[0], 0x5000000u, check_npt_pa, &reject) != 0);
	assert(amdvm_npt_validate_4k_mapping(&npts[1], 4096, m->page_pas[1], 7) == 0);
	assert(amdvm_npt_init_scatter(&npts[0], pools[0], pas[0], indexes[0], count, 48, 48) == 0);
	assert(amdvm_vm_npt_build(m, &npts[0], m->page_pas[0], check_npt_pa, NULL) != 0);
	for (unsigned v = 0; v < 2; ++v) { free(pools[v]); free(pas[v]); free(indexes[v]); }
}

typedef struct concurrent {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	amdvm_vm_memory_t *memory;
	unsigned ready, release;
} concurrent_t;

static void *borrower(void *opaque)
{
	concurrent_t *c = opaque;
	assert(pthread_mutex_lock(&c->lock) == 0);
	assert(amdvm_vm_memory_acquire(c->memory, 7) == 0);
	++c->ready;
	assert(pthread_cond_broadcast(&c->changed) == 0);
	while (!c->release) assert(pthread_cond_wait(&c->changed, &c->lock) == 0);
	assert(c->memory->page_pas[0] == 0x1000000);
	assert(c->memory->pins[0].memory_descriptor != NULL);
	assert(amdvm_vm_memory_release(c->memory) == 0);
	assert(pthread_mutex_unlock(&c->lock) == 0);
	return NULL;
}

int main(void)
{
	fixture_t f = {0};
	amdvm_vm_memory_t *m = NULL;
	assert(prepare(&f, &m, ranges, 2) == 0);
	assert(m->page_count == 3 && m->writable_bytes == 8192);
	assert(m->page_pas[0] == 0x1000000 && m->page_pas[1] == 0x1001000 &&
	    m->page_pas[2] == 0x2000000 && m->ranges[1].first_page == 2);
	assert(amdvm_vm_memory_acquire(m, 1) == AMDVM_VM_MEMORY_STALE);
	assert(amdvm_vm_memory_commit(m, 42) == 0);
	assert(m->translation_generation == 1);
	assert(amdvm_vm_memory_commit(m, 43) == AMDVM_VM_MEMORY_INVALID);
	assert(amdvm_vm_memory_acquire(m, 41) == AMDVM_VM_MEMORY_STALE);
	assert(amdvm_vm_memory_acquire(m, 42) == 0);
	assert(amdvm_vm_memory_acquire(m, 42) == 0);
	test_two_npts(m);
	unsigned allocs = f.allocations;
	assert(amdvm_vm_memory_close(&m, 0) == AMDVM_VM_MEMORY_BUSY);
	assert(f.live_pins == 2 && m && m->users == 2);
	assert(amdvm_vm_memory_acquire(m, 42) == AMDVM_VM_MEMORY_BUSY);
	assert(amdvm_vm_memory_release(m) == 0);
	assert(amdvm_vm_memory_close(&m, 0) == AMDVM_VM_MEMORY_BUSY);
	assert(amdvm_vm_memory_release(m) == 0);
	assert(amdvm_vm_memory_release(m) == AMDVM_VM_MEMORY_INVALID);
	assert(amdvm_vm_memory_close(&m, 0) == 0 && !m);
	assert(f.allocations == allocs); clean(&f);

	/* Two real worker threads borrow the same retained pin set. */
	memset(&f, 0, sizeof(f));
	assert(prepare(&f, &m, ranges, 2) == 0);
	assert(amdvm_vm_memory_commit(m, 7) == 0);
	concurrent_t c = { .lock = PTHREAD_MUTEX_INITIALIZER,
	    .changed = PTHREAD_COND_INITIALIZER, .memory = m };
	pthread_t workers[2];
	for (unsigned i = 0; i < 2; ++i)
		assert(pthread_create(&workers[i], NULL, borrower, &c) == 0);
	assert(pthread_mutex_lock(&c.lock) == 0);
	while (c.ready != 2) assert(pthread_cond_wait(&c.changed, &c.lock) == 0);
	assert(amdvm_vm_memory_close(&m, 0) == AMDVM_VM_MEMORY_BUSY);
	assert(f.live_pins == 2 && m == c.memory);
	assert(amdvm_vm_memory_acquire(m, 7) == AMDVM_VM_MEMORY_BUSY);
	c.release = 1;
	assert(pthread_cond_broadcast(&c.changed) == 0);
	assert(pthread_mutex_unlock(&c.lock) == 0);
	for (unsigned i = 0; i < 2; ++i) assert(pthread_join(workers[i], NULL) == 0);
	assert(amdvm_vm_memory_close(&m, 0) == 0 && !m); clean(&f);
	assert(pthread_cond_destroy(&c.changed) == 0);
	assert(pthread_mutex_destroy(&c.lock) == 0);

	for (unsigned failure = 1; failure <= 3; ++failure) {
		memset(&f, 0, sizeof(f)); f.fail_alloc = failure;
		assert(prepare(&f, &m, ranges, 2) == AMDVM_VM_MEMORY_ALLOC_FAILED);
		assert(!m && !f.pin_calls); clean(&f);
	}
	for (unsigned failure = 1; failure <= 2; ++failure) {
		memset(&f, 0, sizeof(f)); f.fail_pin = failure;
		assert(prepare(&f, &m, ranges, 2) == AMDVM_VM_MEMORY_PIN_FAILED);
		assert(!m); clean(&f);
	}
	memset(&f, 0, sizeof(f)); f.bad_pa = 1;
	assert(prepare(&f, &m, ranges, 2) == AMDVM_VM_MEMORY_BAD_PA);
	assert(!m); clean(&f);

	/* Exhausting pre-reserved cleanup slots fails before any new pin. */
	memset(&f, 0, sizeof(f));
	amdvm_vm_memory_t *held[AMDVM_QUARANTINE_MAX_SLOTS] = {0};
	for (unsigned i = 0; i < AMDVM_QUARANTINE_MAX_SLOTS; ++i)
		assert(prepare(&f, &held[i], ranges, 2) == 0);
	unsigned pin_calls = f.pin_calls;
	assert(prepare(&f, &m, ranges, 2) == AMDVM_VM_MEMORY_QUOTA);
	assert(!m && f.pin_calls == pin_calls);
	for (unsigned i = 0; i < AMDVM_QUARANTINE_MAX_SLOTS; ++i)
		assert(amdvm_vm_memory_close(&held[i], 0) == 0);
	clean(&f);
	memset(&f, 0, sizeof(f)); f.alias_pa = 1;
	assert(prepare(&f, &m, ranges, 2) == AMDVM_VM_MEMORY_ALIAS);
	assert(!m); clean(&f);

	/* Failed complete retains descriptor/task metadata beyond owner close. */
	memset(&f, 0, sizeof(f)); f.fail_pin = 2; f.fail_unpin = 1;
	assert(prepare(&f, &m, ranges, 2) == AMDVM_VM_MEMORY_CLEANUP_PENDING);
	assert(m && m->state == AMDVM_VM_MEMORY_QUARANTINED && f.live_pins == 1);
	assert(amdvm_quarantine_registry_count(&f.registry) == 1);
	allocs = f.allocations;
	assert(amdvm_vm_memory_close(&m, 1) == 0 && !m);
	assert(!f.live_allocations && f.live_pins == 1 && f.allocations == allocs);
	f.fail_unpin = 0;
	amdvm_quarantine_registry_retry(&f.registry, unpin, &f); clean(&f);

	memset(&f, 0, sizeof(f));
	assert(prepare(&f, &m, ranges, 2) == 0); f.fail_unpin = 1;
	assert(amdvm_vm_memory_close(&m, 0) == AMDVM_VM_MEMORY_CLEANUP_PENDING);
	assert(m && f.live_pins == 2);
	f.fail_unpin = 0;
	assert(amdvm_vm_memory_close(&m, 0) == 0 && !m); clean(&f);

	/* Immutable ROM aliases may share backing, and writable aliases are safe
	 * only when they name the same user pages with identical permissions. */
	amdvm_vcpu_memory_range_t aliases[2] = { ranges[0], ranges[0] };
	aliases[0].flags = aliases[1].flags = 5;
	aliases[1].guest_physical_address = 0x800000;
	memset(&f, 0, sizeof(f));
	assert(prepare(&f, &m, aliases, 2) == 0);
	assert(amdvm_vm_memory_close(&m, 0) == 0); clean(&f);
	aliases[1].flags = 7;
	assert(prepare(&f, &m, aliases, 2) == AMDVM_VM_MEMORY_ALIAS);
	assert(!m); clean(&f);
	aliases[0] = ranges[0];
	aliases[1] = ranges[0];
	aliases[1].guest_physical_address = 0x800000;
	aliases[1].size = 4096;
	memset(&f, 0, sizeof(f));
	assert(prepare(&f, &m, aliases, 2) == 0);
	assert(m->page_count == 3 && m->page_pas[0] == m->page_pas[2]);
	assert(m->page_pas[0] != m->page_pas[1]);
	assert(amdvm_vm_memory_commit(m, 7) == 0);
	assert(amdvm_vm_memory_acquire(m, 7) == 0);
	amdvm_npt_t alias_npt = {0};
	size_t alias_pages = 0;
	assert(amdvm_vm_npt_required_pages(m, &alias_pages) == 0);
	void *alias_pool = NULL;
	uint64_t *alias_pool_pas = calloc(alias_pages, sizeof(*alias_pool_pas));
	size_t *alias_pool_indexes = calloc(alias_pages, sizeof(*alias_pool_indexes));
	assert(alias_pool_pas && alias_pool_indexes);
	assert(posix_memalign(&alias_pool, 4096, alias_pages * 4096) == 0);
	for (size_t i = 0; i < alias_pages; ++i)
		alias_pool_pas[i] = 0x3000000u + i * 8192u;
	assert(amdvm_npt_init_scatter(&alias_npt, alias_pool, alias_pool_pas,
	    alias_pool_indexes, alias_pages, 48, 48) == 0);
	assert(amdvm_vm_npt_build(m, &alias_npt, 0x5000000u,
	    check_npt_pa, NULL) == 0);
	assert(amdvm_npt_validate_4k_mapping(&alias_npt, 0x800000,
	    m->page_pas[2], 7) == 0);
	free(alias_pool); free(alias_pool_pas); free(alias_pool_indexes);
	assert(amdvm_vm_memory_release(m) == 0);
	assert(amdvm_vm_memory_close(&m, 0) == 0); clean(&f);
	aliases[1].user_virtual_address += 0x10000;
	memset(&f, 0, sizeof(f)); f.alias_pa = 1;
	assert(prepare(&f, &m, aliases, 2) == AMDVM_VM_MEMORY_ALIAS);
	assert(!m); clean(&f);

	/* New owner has no legacy 8-GiB accounting cap. Validate 9 GiB,
	 * split around reset-ROM GPA and the per-descriptor 32-bit limit. */
	amdvm_vcpu_memory_range_t large[3] = {0};
	for (unsigned i = 0; i < 3; ++i) {
		large[i].guest_physical_address = (uint64_t)i * (UINT64_C(4) << 30);
		large[i].user_virtual_address = (UINT64_C(1) << 40) +
		    (uint64_t)i * (UINT64_C(4) << 30);
		large[i].size = UINT64_C(3) << 30; large[i].flags = 7;
	}
	size_t pages; uint64_t writable;
	assert(amdvm_vm_memory_validate(large, 3, &limits, &pages, &writable) == 0);
	assert(writable == (UINT64_C(9) << 30) && pages == 2359296);
	amdvm_vm_memory_limits_t small = { 4096, 4096, 48 };
	assert(amdvm_vm_memory_validate(ranges, 2, &small, &pages, &writable) ==
	    AMDVM_VM_MEMORY_QUOTA);
	large[0].guest_physical_address = AMDVM_VCPU_RESET_ROM_GPA;
	assert(amdvm_vm_memory_validate(large, 3, &limits, &pages, &writable) ==
	    AMDVM_VM_MEMORY_INVALID);
	puts("PASS: production VM RAM owner prepare/commit, two borrowers, closing drain, allocation/pin/PA/alias rollback, durable orphan cleanup, and >8-GiB accounting; no VMRUN");
	return 0;
}
