#include "../svm-kext/provider/amdvm_vm_memory.h"
#include "../svm-kext/provider/amdvm_vm_npt.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
	amdvm_quarantine_registry_t quarantine;
	uint64_t next_hva;
	size_t pins;
} fixture_t;

static void *allocate(size_t bytes, void *opaque)
{
	(void)opaque;
	return calloc(1, bytes);
}

static void deallocate(void *allocation, size_t bytes, void *opaque)
{
	(void)bytes;
	(void)opaque;
	free(allocation);
}

static int pin(amdvm_pinned_user_region_t *pin, uint64_t address,
	size_t bytes, void *task, void *opaque)
{
	fixture_t *f = opaque;
	pin->memory_descriptor = malloc(1);
	if (!pin->memory_descriptor) return -1;
	pin->user_address = address;
	pin->size = bytes;
	pin->owning_task = task;
	++f->pins;
	return 0;
}

static int unpin(amdvm_pinned_user_region_t *pin, void *opaque)
{
	fixture_t *f = opaque;
	if (!pin->memory_descriptor || !f->pins) return -1;
	free(pin->memory_descriptor);
	--f->pins;
	memset(pin, 0, sizeof(*pin));
	return 0;
}

static int page_pa(const amdvm_pinned_user_region_t *pin, size_t page,
	uint8_t bits, uint64_t *pa, void *opaque)
{
	(void)opaque;
	if (bits != 48 || page >= pin->size / 4096u) return -1;
	*pa = pin->user_address + (uint64_t)page * 4096u;
	return 0;
}

static int check_wb(uint64_t pa, void *opaque)
{
	(void)pa;
	(void)opaque;
	return 0;
}

static void build_and_check_vcpu(const amdvm_vm_memory_t *memory,
	unsigned vcpu_id, size_t required_pages)
{
	amdvm_npt_t npt = {0};
	void *pool = NULL;
	uint64_t *pool_pas = calloc(required_pages, sizeof(*pool_pas));
	size_t *pool_indexes = calloc(required_pages, sizeof(*pool_indexes));
	assert(pool_pas && pool_indexes);
	assert(posix_memalign(&pool, 4096, required_pages * 4096u) == 0);
	for (size_t i = 0; i < required_pages; ++i) {
		pool_pas[i] = UINT64_C(0x6000000000) +
		    (uint64_t)vcpu_id * UINT64_C(0x10000000) + i * 8192u;
	}
	assert(amdvm_npt_init_scatter(&npt, pool, pool_pas, pool_indexes,
	    required_pages, 48, 48) == AMDVM_NPT_OK);
	uint64_t reset_pa = UINT64_C(0x5000000000) + (uint64_t)vcpu_id * 4096u;
	assert(amdvm_vm_npt_build(memory, &npt, reset_pa, check_wb, NULL) ==
	    AMDVM_NPT_OK);
	assert(amdvm_npt_root_pa(&npt) == pool_pas[0]);
	amdvm_npt_translation_t translation = {0};
	assert(amdvm_npt_translate_4k_diagnostic(&npt,
	    AMDVM_VCPU_RESET_ROM_GPA, &translation) == AMDVM_NPT_OK);
	assert((translation.host_physical_address & ~UINT64_C(0xfff)) == reset_pa);
	assert((translation.permissions & (AMDVM_NPT_READ | AMDVM_NPT_WRITE |
	    AMDVM_NPT_EXECUTE)) == (AMDVM_NPT_READ | AMDVM_NPT_EXECUTE));
	assert(amdvm_npt_translate_4k_diagnostic(&npt,
	    UINT64_C(0xfee00000), &translation) == AMDVM_NPT_UNMAPPED);
	/* Check the low RAM, top of low RAM and first high-memory page. */
	const uint64_t guest_pages[] = {
		UINT64_C(0x00000000), UINT64_C(0xbffff000), UINT64_C(0x100000000)
	};
	for (size_t i = 0; i < sizeof(guest_pages) / sizeof(guest_pages[0]); ++i) {
		assert(amdvm_npt_translate_4k_diagnostic(&npt,
		    guest_pages[i], &translation) == AMDVM_NPT_OK);
		assert((translation.permissions & AMDVM_NPT_READ) != 0);
	}
	free(pool);
	free(pool_pas);
	free(pool_indexes);
}

int main(void)
{
	fixture_t fixture = {0};
	const uint64_t sizes[] = {
		UINT64_C(0xa0000), UINT64_C(0x20000), UINT64_C(0x10000),
		UINT64_C(0x10000), UINT64_C(0xbff00000), UINT64_C(0xf000),
		UINT64_C(0x40000000)
	};
	const uint64_t gpas[] = {
		UINT64_C(0), UINT64_C(0xc0000), UINT64_C(0xe0000),
		UINT64_C(0xf0000), UINT64_C(0x100000), UINT64_C(0xffff0000),
		UINT64_C(0x100000000)
	};
	const uint32_t flags[] = { 7, 5, 7, 5, 7, 5, 7 };
	amdvm_vcpu_memory_range_t ranges[7] = {0};
	uint64_t hva = UINT64_C(0x100000000);
	for (size_t i = 0; i < 7; ++i) {
		ranges[i].guest_physical_address = gpas[i];
		ranges[i].user_virtual_address = hva;
		ranges[i].size = sizes[i];
		ranges[i].flags = flags[i];
		hva += sizes[i] + 4096u;
	}
	amdvm_vm_memory_ops_t ops = {
		allocate, deallocate, pin, unpin, page_pa,
		&fixture.quarantine, &fixture
	};
	amdvm_vm_memory_limits_t limits = {
		UINT64_C(8) << 30, UINT64_C(8) << 30, 48
	};
	amdvm_vm_memory_t *memory = NULL;
	assert(amdvm_vm_memory_prepare(&memory, ranges, 7, &fixture,
	    &limits, &ops) == AMDVM_VM_MEMORY_OK);
	assert(memory->page_count > 1000000u);
	assert(amdvm_vm_memory_commit(memory, 1) == AMDVM_VM_MEMORY_OK);
	assert(amdvm_vm_memory_acquire(memory, 1) == AMDVM_VM_MEMORY_OK);
	size_t required_pages = 0;
	assert(amdvm_vm_npt_required_pages(memory, &required_pages) ==
	    AMDVM_NPT_OK);
	assert(required_pages > 1000u);
	build_and_check_vcpu(memory, 0, required_pages);
	build_and_check_vcpu(memory, 1, required_pages);
	assert(amdvm_vm_memory_release(memory) == AMDVM_VM_MEMORY_OK);
	assert(amdvm_vm_memory_close(&memory, 0) == AMDVM_VM_MEMORY_OK);
	assert(memory == NULL && fixture.pins == 0);
	assert(amdvm_vm_memory_owner_count() == 0);
	puts("4-GiB QEMU map NPT model: PASS (two roots, reset ROM RX, APIC hole, RAM edges, cleanup)");
	return 0;
}
