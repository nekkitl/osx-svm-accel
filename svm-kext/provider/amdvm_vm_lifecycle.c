#include "amdvm_vm_lifecycle.h"

#include <string.h>

static amdvm_vm_vcpu_slot_t *
amdvm_vm_lifecycle_get_slot(amdvm_vm_lifecycle_t *vm, uint32_t slot)
{
	if (vm == NULL || slot >= AMDVM_VM_LIFECYCLE_MAX_VCPUS ||
	    vm->vcpus[slot].allocated == 0) {
		return NULL;
	}
	return &vm->vcpus[slot];
}

int
amdvm_vm_lifecycle_open(amdvm_vm_lifecycle_t *vm)
{
	if (vm == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	memset(vm, 0, sizeof(*vm));
	vm->state = AMDVM_VM_STATE_OPEN;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_map_begin(amdvm_vm_lifecycle_t *vm)
{
	if (vm == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_OPEN || vm->map_committed != 0 ||
	    vm->resources_owned != 0) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	vm->state = AMDVM_VM_STATE_MAP_PREPARING;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_map_abort(amdvm_vm_lifecycle_t *vm)
{
	if (vm == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_MAP_PREPARING) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	vm->state = AMDVM_VM_STATE_OPEN;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_map_commit(amdvm_vm_lifecycle_t *vm)
{
	if (vm == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_MAP_PREPARING ||
	    vm->map_generation == UINT64_MAX) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	vm->map_generation++;
	vm->map_committed = 1;
	vm->resources_owned = 1;
	vm->state = AMDVM_VM_STATE_READY;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_vcpu_create(amdvm_vm_lifecycle_t *vm, uint32_t vcpu_id,
	uint32_t *slot_out)
{
	uint32_t empty = AMDVM_VM_LIFECYCLE_MAX_VCPUS;
	uint32_t i;

	if (vm == NULL || slot_out == NULL || vcpu_id >=
	    AMDVM_VM_LIFECYCLE_MAX_VCPUS) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_READY || vm->map_committed == 0) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	for (i = 0; i < AMDVM_VM_LIFECYCLE_MAX_VCPUS; ++i) {
		if (vm->vcpus[i].allocated == 0) {
			if (empty == AMDVM_VM_LIFECYCLE_MAX_VCPUS) {
				empty = i;
			}
		} else if (vm->vcpus[i].id == vcpu_id) {
			return AMDVM_VM_LIFECYCLE_EXISTS;
		}
	}
	if (empty == AMDVM_VM_LIFECYCLE_MAX_VCPUS ||
	    vm->vcpu_count == UINT32_MAX) {
		return AMDVM_VM_LIFECYCLE_FULL;
	}
	vm->vcpus[empty].id = vcpu_id;
	vm->vcpus[empty].state = AMDVM_VM_VCPU_READY;
	vm->vcpus[empty].active_runs = 0;
	vm->vcpus[empty].allocated = 1;
	vm->vcpus[empty].map_generation = vm->map_generation;
	vm->vcpu_count++;
	*slot_out = empty;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_run_begin(amdvm_vm_lifecycle_t *vm, uint32_t slot,
	uint64_t map_generation, bool event_pending)
{
	amdvm_vm_vcpu_slot_t *vcpu = amdvm_vm_lifecycle_get_slot(vm, slot);
	bool state_can_run;

	if (vm == NULL || vcpu == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_READY || vm->map_committed == 0) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	if (map_generation != vm->map_generation ||
	    vcpu->map_generation != vm->map_generation) {
		return AMDVM_VM_LIFECYCLE_STALE_MAP;
	}
	state_can_run = vcpu->state == AMDVM_VM_VCPU_READY ||
	    vcpu->state == AMDVM_VM_VCPU_STOPPED ||
	    (vcpu->state == AMDVM_VM_VCPU_HALTED && event_pending);
	if (!state_can_run || vcpu->active_runs != 0 ||
	    vm->active_runs == UINT32_MAX) {
		return AMDVM_VM_LIFECYCLE_BUSY;
	}
	vcpu->state = AMDVM_VM_VCPU_RUNNING;
	vcpu->active_runs = 1;
	vm->active_runs++;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_run_end(amdvm_vm_lifecycle_t *vm, uint32_t slot,
	bool guest_halted)
{
	amdvm_vm_vcpu_slot_t *vcpu = amdvm_vm_lifecycle_get_slot(vm, slot);

	if (vm == NULL || vcpu == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vcpu->state != AMDVM_VM_VCPU_RUNNING || vcpu->active_runs != 1 ||
	    vm->active_runs == 0) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	vcpu->state = guest_halted ? AMDVM_VM_VCPU_HALTED :
	    AMDVM_VM_VCPU_STOPPED;
	vcpu->active_runs = 0;
	vm->active_runs--;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_vcpu_destroy(amdvm_vm_lifecycle_t *vm, uint32_t slot)
{
	amdvm_vm_vcpu_slot_t *vcpu = amdvm_vm_lifecycle_get_slot(vm, slot);

	if (vm == NULL || vcpu == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vcpu->active_runs != 0 || vcpu->state == AMDVM_VM_VCPU_RUNNING) {
		return AMDVM_VM_LIFECYCLE_BUSY;
	}
	if (vm->state != AMDVM_VM_STATE_READY &&
	    vm->state != AMDVM_VM_STATE_CLOSING &&
	    vm->state != AMDVM_VM_STATE_QUARANTINED) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	memset(vcpu, 0, sizeof(*vcpu));
	vm->vcpu_count--;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_close(amdvm_vm_lifecycle_t *vm)
{
	if (vm == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_READY &&
	    vm->state != AMDVM_VM_STATE_OPEN) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	vm->state = AMDVM_VM_STATE_CLOSING;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_destroy(amdvm_vm_lifecycle_t *vm)
{
	if (vm == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_CLOSING) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	if (vm->active_runs != 0 || vm->vcpu_count != 0) {
		return AMDVM_VM_LIFECYCLE_BUSY;
	}
	vm->resources_owned = 0;
	vm->map_committed = 0;
	vm->state = AMDVM_VM_STATE_DESTROYED;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_quarantine(amdvm_vm_lifecycle_t *vm)
{
	if (vm == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_CLOSING) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	vm->state = AMDVM_VM_STATE_QUARANTINED;
	return AMDVM_VM_LIFECYCLE_OK;
}

int
amdvm_vm_lifecycle_finish_quarantine(amdvm_vm_lifecycle_t *vm,
	bool cleanup_succeeded)
{
	if (vm == NULL) {
		return AMDVM_VM_LIFECYCLE_INVALID;
	}
	if (vm->state != AMDVM_VM_STATE_QUARANTINED) {
		return AMDVM_VM_LIFECYCLE_BAD_STATE;
	}
	if (!cleanup_succeeded || vm->active_runs != 0 || vm->vcpu_count != 0) {
		return AMDVM_VM_LIFECYCLE_BUSY;
	}
	vm->resources_owned = 0;
	vm->map_committed = 0;
	vm->state = AMDVM_VM_STATE_DESTROYED;
	return AMDVM_VM_LIFECYCLE_OK;
}

bool
amdvm_vm_lifecycle_can_free(const amdvm_vm_lifecycle_t *vm)
{
	return vm != NULL && vm->state == AMDVM_VM_STATE_DESTROYED &&
	    vm->resources_owned == 0 && vm->map_committed == 0 &&
	    vm->active_runs == 0 && vm->vcpu_count == 0;
}
