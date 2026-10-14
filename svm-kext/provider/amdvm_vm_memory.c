#include "amdvm_vm_memory.h"
#include "amdvm_npt.h"

#include <string.h>

#define PAGE_BYTES UINT64_C(4096)
#define GPA_LIMIT (UINT64_C(1) << 48)
#define RX_FLAGS (AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_EXEC)
static volatile uint32_t gMemoryOwners;

int
amdvm_vm_memory_validate(const amdvm_vcpu_memory_range_t *ranges,
	size_t count, const amdvm_vm_memory_limits_t *limits,
	size_t *page_count, uint64_t *writable_bytes)
{
	uint64_t mapped = 0, writable = 0, previous_end = 0;
	size_t i, j;
	if (!ranges || !limits || !page_count || !writable_bytes || !count ||
	    count > AMDVM_VCPU_MEMORY_MAX_RANGES || !limits->max_guest_bytes ||
	    limits->max_mapped_bytes < limits->max_guest_bytes ||
	    limits->host_phys_bits < 32 || limits->host_phys_bits > 52)
		return AMDVM_VM_MEMORY_INVALID;
	for (i = 0; i < count; ++i) {
		const amdvm_vcpu_memory_range_t *r = &ranges[i];
		if (r->reserved || !r->size ||
		    r->size > AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES ||
		    ((r->guest_physical_address | r->user_virtual_address | r->size) &
		    (PAGE_BYTES - 1u)) || !r->user_virtual_address ||
		    r->user_virtual_address > UINT64_MAX - r->size ||
		    r->guest_physical_address >= GPA_LIMIT ||
		    r->size > GPA_LIMIT - r->guest_physical_address ||
		    (i && r->guest_physical_address < previous_end) ||
		    !(r->flags & AMDVM_VCPU_MEMORY_READ) ||
		    (r->flags & ~(RX_FLAGS | AMDVM_VCPU_MEMORY_WRITE)))
			return AMDVM_VM_MEMORY_INVALID;
		previous_end = r->guest_physical_address + r->size;
		if (r->guest_physical_address < AMDVM_VCPU_RESET_ROM_GPA + PAGE_BYTES &&
		    AMDVM_VCPU_RESET_ROM_GPA < previous_end)
			return AMDVM_VM_MEMORY_INVALID;
		for (j = 0; j < i; ++j) {
			const amdvm_vcpu_memory_range_t *p = &ranges[j];
			if (r->user_virtual_address < p->user_virtual_address + p->size &&
			    p->user_virtual_address < r->user_virtual_address + r->size &&
			    (r->flags != RX_FLAGS || p->flags != RX_FLAGS))
				return AMDVM_VM_MEMORY_ALIAS;
		}
		if (r->size > limits->max_mapped_bytes - mapped)
			return AMDVM_VM_MEMORY_QUOTA;
		mapped += r->size;
		if (r->flags & AMDVM_VCPU_MEMORY_WRITE) {
			if (r->size > limits->max_guest_bytes - writable)
				return AMDVM_VM_MEMORY_QUOTA;
			writable += r->size;
		}
	}
	if (mapped / PAGE_BYTES > SIZE_MAX / sizeof(uint64_t))
		return AMDVM_VM_MEMORY_QUOTA;
	*page_count = (size_t)(mapped / PAGE_BYTES);
	*writable_bytes = writable;
	return AMDVM_VM_MEMORY_OK;
}

static int
unpin_callback(amdvm_pinned_user_region_t *pin, void *opaque)
{
	amdvm_vm_memory_t *m = opaque;
	return m->ops.unpin(pin, m->ops.opaque);
}

uint32_t
amdvm_vm_memory_owner_count(void)
{
	return __sync_val_compare_and_swap(&gMemoryOwners, 0u, 0u);
}

int
amdvm_vm_memory_close(amdvm_vm_memory_t **memory, int orphan)
{
	amdvm_vm_memory_t *m;
	amdvm_vm_memory_ops_t ops;
	size_t i;
	int failed = 0;
	if (!memory) return AMDVM_VM_MEMORY_INVALID;
	if (!(m = *memory)) return AMDVM_VM_MEMORY_OK;
	if (m->state != AMDVM_VM_MEMORY_QUARANTINED)
		m->state = AMDVM_VM_MEMORY_CLOSING;
	if (m->users) return AMDVM_VM_MEMORY_BUSY;
	if (m->state != AMDVM_VM_MEMORY_QUARANTINED) {
		for (i = 0; i < m->range_count; ++i) {
			if (m->pins[i].memory_descriptor &&
			    m->ops.unpin(&m->pins[i], m->ops.opaque)) failed = 1;
		}
		if (failed) {
			/* No allocations in cleanup: transfer every unresolved descriptor
			 * into the slot reserved before pinning. Successful transfer clears
			 * source references and preserves task/descriptor metadata. */
			int rc = amdvm_quarantine_registry_transfer(m->ops.quarantine,
			    m->quarantine_slot, m->quarantine_generation, m->pins, m->range_count);
			if (rc) return AMDVM_VM_MEMORY_CLEANUP_PENDING;
			m->state = AMDVM_VM_MEMORY_QUARANTINED;
		}
	}
	if (m->slot_reserved) {
		int rc;
		if (m->state == AMDVM_VM_MEMORY_QUARANTINED) {
			rc = orphan ? amdvm_quarantine_registry_forget(m->ops.quarantine,
			    m->quarantine_slot, m->quarantine_generation, unpin_callback, m) :
			    amdvm_quarantine_registry_recover(m->ops.quarantine,
			    m->quarantine_slot, m->quarantine_generation, unpin_callback, m);
		} else {
			rc = amdvm_quarantine_registry_release(m->ops.quarantine,
			    m->quarantine_slot, m->quarantine_generation);
		}
		if (rc) return AMDVM_VM_MEMORY_CLEANUP_PENDING;
	}
	ops = m->ops;
	if (m->page_pas) ops.deallocate(m->page_pas,
	    m->page_count * sizeof(*m->page_pas), ops.opaque);
	memset(m, 0, sizeof(*m));
	ops.deallocate(m, sizeof(*m), ops.opaque);
	*memory = NULL;
	__sync_fetch_and_sub(&gMemoryOwners, 1u);
	return AMDVM_VM_MEMORY_OK;
}

int
amdvm_vm_memory_prepare(amdvm_vm_memory_t **candidate,
	const amdvm_vcpu_memory_range_t *ranges, size_t count, void *task,
	const amdvm_vm_memory_limits_t *limits, const amdvm_vm_memory_ops_t *ops)
{
	amdvm_vm_memory_t *m;
	size_t pages, i, j, cursor = 0;
	uint64_t writable, *aliases;
	int rc;
	if (!candidate || *candidate || !task || !ops || !ops->allocate ||
	    !ops->deallocate || !ops->pin || !ops->unpin || !ops->page_pa ||
	    !ops->quarantine) return AMDVM_VM_MEMORY_INVALID;
	rc = amdvm_vm_memory_validate(ranges, count, limits, &pages, &writable);
	if (rc) return rc;
	m = ops->allocate(sizeof(*m), ops->opaque);
	if (!m) return AMDVM_VM_MEMORY_ALLOC_FAILED;
	memset(m, 0, sizeof(*m));
	m->ops = *ops; m->state = AMDVM_VM_MEMORY_BUILDING;
	m->range_count = count; m->page_count = pages; m->writable_bytes = writable;
	__sync_fetch_and_add(&gMemoryOwners, 1u);
	*candidate = m;
	m->page_pas = ops->allocate(pages * sizeof(uint64_t), ops->opaque);
	aliases = ops->allocate(pages * sizeof(uint64_t), ops->opaque);
	rc = AMDVM_VM_MEMORY_ALLOC_FAILED;
	if (!m->page_pas || !aliases) goto fail;
	rc = AMDVM_VM_MEMORY_QUOTA;
	if (amdvm_quarantine_registry_reserve(ops->quarantine, &m->quarantine_slot,
	    &m->quarantine_generation, unpin_callback, m)) goto fail;
	m->slot_reserved = 1;
	/* Validation and all metadata allocation precede pinning. Nothing is
	 * published until the caller assigns a nonzero map generation. */
	for (i = 0; i < count; ++i) {
		m->ranges[i].map = ranges[i];
		m->ranges[i].first_page = cursor;
		if (ops->pin(&m->pins[i], ranges[i].user_virtual_address,
		    (size_t)ranges[i].size, task, ops->opaque)) {
			rc = AMDVM_VM_MEMORY_PIN_FAILED; goto fail;
		}
		for (j = 0; j < ranges[i].size / PAGE_BYTES; ++j) {
			uint64_t pa, limit = UINT64_C(1) << limits->host_phys_bits;
			if (ops->page_pa(&m->pins[i], j, limits->host_phys_bits,
			    &pa, ops->opaque) || !pa || (pa & (PAGE_BYTES - 1u)) ||
			    pa >= limit || limit - pa < PAGE_BYTES) {
				rc = AMDVM_VM_MEMORY_BAD_PA; goto fail;
			}
			m->page_pas[cursor] = pa;
			aliases[cursor++] = ((pa / PAGE_BYTES) << 1) |
			    (ranges[i].flags == RX_FLAGS);
		}
	}
	if (amdvm_npt_validate_backing_aliases(aliases, pages) != AMDVM_NPT_OK) {
		rc = AMDVM_VM_MEMORY_ALIAS; goto fail;
	}
	ops->deallocate(aliases, pages * sizeof(*aliases), ops->opaque);
	m->state = AMDVM_VM_MEMORY_READY;
	return AMDVM_VM_MEMORY_OK;
fail:
	if (aliases) ops->deallocate(aliases, pages * sizeof(*aliases), ops->opaque);
	if (amdvm_vm_memory_close(candidate, 0)) return AMDVM_VM_MEMORY_CLEANUP_PENDING;
	return rc;
}

int
amdvm_vm_memory_commit(amdvm_vm_memory_t *m, uint64_t generation)
{
	if (!m || m->state != AMDVM_VM_MEMORY_READY || !generation || m->generation)
		return AMDVM_VM_MEMORY_INVALID;
	m->generation = generation;
	return AMDVM_VM_MEMORY_OK;
}

int
amdvm_vm_memory_acquire(amdvm_vm_memory_t *m, uint64_t generation)
{
	if (!m || m->state != AMDVM_VM_MEMORY_READY || !generation || m->users == UINT32_MAX)
		return AMDVM_VM_MEMORY_BUSY;
	if (generation != m->generation) return AMDVM_VM_MEMORY_STALE;
	++m->users;
	return AMDVM_VM_MEMORY_OK;
}

int
amdvm_vm_memory_release(amdvm_vm_memory_t *m)
{
	if (!m || !m->users) return AMDVM_VM_MEMORY_INVALID;
	--m->users;
	return AMDVM_VM_MEMORY_OK;
}
