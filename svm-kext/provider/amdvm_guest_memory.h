#ifndef AMDVM_GUEST_MEMORY_H
#define AMDVM_GUEST_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "../amdvm_probe_abi.h"

/*
 * Account one already-address-validated guest range. This pure helper keeps
 * the provider's writable-RAM and total-mapped limits testable without XNU.
 * On failure neither counter is changed.
 */
int amdvm_guest_memory_account_range(uint64_t size, uint32_t flags,
	uint64_t *total_mapped_pages, uint64_t *total_writable_pages);

#endif
