#include "../provider/amdvm_guest_memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE UINT64_C(4096)

typedef struct test_range {
	uint64_t size;
	uint32_t flags;
} test_range_t;

static void account_map(const test_range_t *ranges, size_t count,
	uint64_t *mapped_pages, uint64_t *writable_pages)
{
	*mapped_pages = 0;
	*writable_pages = 0;
	for (size_t i = 0; i < count; ++i) {
		assert(amdvm_guest_memory_account_range(ranges[i].size,
		    ranges[i].flags, mapped_pages, writable_pages) == 0);
	}
}

int main(void)
{
	const uint32_t ram = AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_WRITE |
	    AMDVM_VCPU_MEMORY_EXEC;
	const uint32_t rom = AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_EXEC;
	const test_range_t qemu_8g_pc[] = {
		{ UINT64_C(0xc0000), ram },
		{ UINT64_C(0x20000), rom },
		{ UINT64_C(0x20000), rom },
		{ UINT64_C(0xbff00000), ram },
		{ UINT64_C(0x3f000), rom },
		{ UINT64_C(0x140000000), ram },
	};
	const uint64_t writable_cap =
	    AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES / PAGE;
	const uint64_t mapped_cap = AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES / PAGE;
	uint64_t mapped = 0;
	uint64_t writable = 0;
	uint64_t saved_mapped;
	uint64_t saved_writable;

	account_map(qemu_8g_pc, sizeof(qemu_8g_pc) / sizeof(qemu_8g_pc[0]),
	    &mapped, &writable);
	assert(writable == UINT64_C(0x1fffc0000) / PAGE);
	assert(mapped == UINT64_C(0x20003f000) / PAGE);
	assert(writable < writable_cap && mapped < mapped_cap);
	puts("observed QEMU 8 GiB PC map: PASS");

	saved_mapped = mapped;
	saved_writable = writable;
	assert(writable < writable_cap);
	assert(amdvm_guest_memory_account_range(
	    (writable_cap - writable) * PAGE, ram, &mapped, &writable) == 0);
	assert(writable == writable_cap);
	assert(amdvm_guest_memory_account_range(PAGE, ram, &mapped, &writable) != 0);
	assert(mapped == saved_mapped + writable_cap - saved_writable);
	assert(writable == writable_cap);
	puts("writable RAM exact cap and fail-closed overflow: PASS");

	account_map(qemu_8g_pc, sizeof(qemu_8g_pc) / sizeof(qemu_8g_pc[0]),
	    &mapped, &writable);
	saved_mapped = mapped;
	saved_writable = writable;
	assert(amdvm_guest_memory_account_range((mapped_cap - mapped) * PAGE, rom,
	    &mapped, &writable) == 0);
	assert(mapped == mapped_cap && writable == saved_writable);
	assert(amdvm_guest_memory_account_range(PAGE, rom, &mapped, &writable) != 0);
	assert(mapped == mapped_cap && writable == saved_writable);
	puts("read-only ROM alias allowance exact cap and overflow: PASS");

	saved_mapped = mapped;
	saved_writable = writable;
	assert(amdvm_guest_memory_account_range(0, rom, &mapped, &writable) != 0);
	assert(amdvm_guest_memory_account_range(PAGE + 1u, rom,
	    &mapped, &writable) != 0);
	assert(amdvm_guest_memory_account_range(PAGE, AMDVM_VCPU_MEMORY_WRITE,
	    &mapped, &writable) != 0);
	assert(amdvm_guest_memory_account_range(PAGE, rom, &mapped, &mapped) != 0);
	assert(mapped == saved_mapped && writable == saved_writable);
	puts("invalid sizes, flags, and outputs leave accounting unchanged: PASS");
	return 0;
}
