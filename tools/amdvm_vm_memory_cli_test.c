/* Exercise the real CLI flow without opening IOKit or executing SVM. */
#define IOConnectCallStructMethod mock_struct_method
#define IOServiceClose mock_service_close
#define main unused_probe_main
#include "../svm-kext/amdvm_probe.c"
#undef main
#include <assert.h>
#include "../svm-kext/provider/amdvm_vm_cpu_state.h"
#include "../svm-kext/provider/amdvm_xstate.h"

int amdvm_vmrun_validate_guest_xstate(const void *p, size_t size, uint64_t mask)
{
	return (mask & ~UINT64_C(7)) || amdvm_xstate_validate_standard(p, size, 7) ? -1 : 0;
}

static unsigned calls, closes, fault;
static uint64_t generation;
static int active, closed;
static unsigned cpu_mask;
static const uint8_t *mapping;
static amdvm_vm_cpu_state_t state_owners[4];
static amdvm_vmcb_t vmcbs[4];
static uint64_t cpu_gprs[4][15];
static uint8_t xstates[4][4096];

kern_return_t
mock_struct_method(mach_port_t connection, uint32_t selector,
    const void *input, size_t input_size, void *output, size_t *output_size)
{
	assert(connection == 42 && closed != 1);
	++calls;
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_NEGOTIATE) {
		const amdvm_vm_abi_draft_negotiate_request_t *r = input;
		amdvm_vm_abi_draft_negotiate_result_t *o = output;
		assert(input_size == sizeof(*r) && *output_size == sizeof(*o));
		assert(r->requested_version == AMDVM_VM_ABI_DRAFT_VERSION);
		if (fault == 1) return kIOReturnUnsupported;
		memset(o, 0, sizeof(*o));
		o->supported_version = r->requested_version;
		o->structure_size = sizeof(*o);
		o->max_memory_ranges = 64;
		o->max_guest_bytes = o->max_mapped_bytes = 4096 * 4;
		o->max_vcpus = 4;
		o->feature_flags = AMDVM_VM_ABI_DRAFT_FEATURE_TRANSACTIONAL_MAP |
		    AMDVM_VM_ABI_DRAFT_FEATURE_MAP_REPLACE_V1 |
		    AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_PREPARE | AMDVM_VM_ABI_DRAFT_FEATURE_PER_VCPU_NPT |
		    AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_STATE_V1;
		if (fault == 2) o->supported_version++;
		return kIOReturnSuccess;
	}
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_SET_RESET_ROM) {
		const amdvm_vm_abi_draft_reset_rom_request_t *r = input;
		amdvm_vm_abi_draft_lifecycle_result_t *o = output;
		assert(input_size == sizeof(*r) && *output_size == sizeof(*o));
		assert(r->page[0xff0] == 0xf4 && !active);
		memset(o, 0, sizeof(*o));
		o->abi_version = r->abi_version; o->structure_size = sizeof(*o);
		return kIOReturnSuccess;
	}
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_CREATE) {
		const amdvm_vm_abi_draft_vcpu_create_request_t *r = input;
		amdvm_vm_abi_draft_vcpu_create_result_t *o = output;
		assert(input_size == sizeof(*r) && *output_size == sizeof(*o));
		assert(active && r->vcpu_id < 4 && r->apic_id == r->vcpu_id);
		assert(r->map_generation == generation);
		if (cpu_mask & (1u << r->vcpu_id)) return kIOReturnBusy;
		memset(o, 0, sizeof(*o));
		o->abi_version = r->abi_version; o->structure_size = sizeof(*o);
		o->vcpu_id = r->vcpu_id; o->map_generation = generation;
		o->npt_page_count = 6;
		unsigned id = r->vcpu_id;
		memset(xstates[id], 0, 4096); memset(cpu_gprs[id], 0, sizeof(cpu_gprs[id]));
		assert(!amdvm_vmcb_configure_probe(&vmcbs[id], 0x100000, 0x200000, 0x300000, 1));
		assert(!amdvm_vmcb_configure_qemu_reset_vector_v1(&vmcbs[id]));
		assert(!amdvm_vm_cpu_state_init(&state_owners[id], &vmcbs[id], cpu_gprs[id], xstates[id]));
		cpu_mask |= 1u << r->vcpu_id;
		return kIOReturnSuccess;
	}
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_STATE) {
		const amdvm_vm_abi_draft_vcpu_state_request_t *r = input;
		amdvm_vm_abi_draft_lifecycle_result_t *o = output;
		assert(input_size == sizeof(*r) && *output_size == sizeof(*o));
		assert(r->vcpu_id < 4 && (cpu_mask & (1u << r->vcpu_id)));
		unsigned id = r->vcpu_id;
		memset(o, 0, sizeof(*o));
		o->abi_version = r->abi_version; o->structure_size = sizeof(*o);
		o->map_generation = generation;
		o->context_status = amdvm_vm_cpu_state_set(&state_owners[id], &vmcbs[id],
		    cpu_gprs[id], xstates[id], &r->state) ? -10 : 0;
		return kIOReturnSuccess;
	}
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_GET_STATE) {
		const amdvm_vm_abi_draft_vcpu_lifecycle_request_t *r = input;
		amdvm_vm_abi_draft_vcpu_state_result_t *o = output;
		assert(input_size == sizeof(*r) && *output_size == sizeof(*o));
		assert(r->vcpu_id < 4 && (cpu_mask & (1u << r->vcpu_id)));
		unsigned id = r->vcpu_id;
		memset(o, 0, sizeof(*o));
		o->abi_version = r->abi_version; o->structure_size = sizeof(*o);
		o->map_generation = generation; o->vcpu_id = id;
		o->context_status = amdvm_vm_cpu_state_get(&state_owners[id], &vmcbs[id],
		    cpu_gprs[id], xstates[id], &o->state) ? -10 : 0;
		return kIOReturnSuccess;
	}
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DESTROY) {
		const amdvm_vm_abi_draft_vcpu_lifecycle_request_t *r = input;
		amdvm_vm_abi_draft_lifecycle_result_t *o = output;
		assert(input_size == sizeof(*r) && *output_size == sizeof(*o));
		assert(cpu_mask & (1u << r->vcpu_id));
		cpu_mask &= ~(1u << r->vcpu_id);
		memset(o, 0, sizeof(*o));
		o->abi_version = r->abi_version; o->structure_size = sizeof(*o);
		o->map_generation = generation;
		return kIOReturnSuccess;
	}
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_SET_MEMORY_MAP) {
		const amdvm_vm_abi_draft_memory_map_request_t *r = input;
		amdvm_vm_abi_draft_memory_map_result_t *o = output;
		assert(input_size == sizeof(*r) && *output_size == sizeof(*o));
		assert(r->range_count == 1 && r->ranges[0].size == 16384);
		assert(amdvm_vm_abi_draft_memory_map_flags_valid(r->flags));
		mapping = (const uint8_t *)(uintptr_t)r->ranges[0].user_virtual_address;
		assert(mapping[0] == 0xa5 && mapping[16383] == 0xa5);
		if (closed == 2) return kIOReturnNotOpen;
		if (r->expected_generation != generation) return kIOReturnNotReady;
		memset(o, 0, sizeof(*o));
		o->abi_version = r->abi_version;
		o->structure_size = sizeof(*o);
		if (fault == 3) { o->context_status = -3; return kIOReturnSuccess; }
		if (r->flags & AMDVM_VM_ABI_DRAFT_MEMORY_MAP_REPLACE) {
			assert(active && !cpu_mask && generation == 1);
			assert(r->ranges[0].guest_physical_address == 0x100000);
			generation++;
		} else {
			if (active) return kIOReturnBusy;
			assert(r->ranges[0].guest_physical_address == 0);
			generation = 1; active = 1;
		}
		o->map_generation = generation; o->range_count = 1;
		o->guest_page_count = o->pinned_page_count = 4;
		if (fault == 4) o->pinned_page_count = 0;
		return kIOReturnSuccess;
	}
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VM_DESTROY) {
		const amdvm_vm_abi_draft_vm_destroy_request_t *r = input;
		amdvm_vm_abi_draft_lifecycle_result_t *o = output;
		assert(input_size == sizeof(*r) && *output_size == sizeof(*o));
		assert(r->expected_generation == generation);
		memset(o, 0, sizeof(*o));
		o->abi_version = r->abi_version; o->structure_size = sizeof(*o);
		o->map_generation = generation;
		if (fault == 5) { o->context_status = -8; return kIOReturnSuccess; }
		active = 0; closed = 2; cpu_mask = 0;
		return kIOReturnSuccess;
	}
	/* RUN and all unlisted selectors are forbidden. */
	abort();
}

kern_return_t
mock_service_close(mach_port_t connection)
{
	assert(connection == 42 && closes == 0);
	if (mapping) assert(mapping[0] == 0xa5 && mapping[16383] == 0xa5);
	++closes; active = 0; closed = 1; cpu_mask = 0;
	return kIOReturnSuccess;
}

int main(void)
{
	for (unsigned mode = 0; mode < 8; ++mode) {
		for (fault = 0; fault <= 5; ++fault) {
			io_connect_t connection = 42;
			calls = closes = cpu_mask = 0; generation = 0; active = closed = 0; mapping = NULL;
			int rc = amdvm_run_vm_memory_smoke(&connection, mode & 1,
			    mode & 2 ? 4 : 0, mode & 4);
			assert(rc == ((fault && !((mode & 1) && fault == 5)) ? 1 : 0));
			assert(connection == IO_OBJECT_NULL && closes == 1 && !active && !cpu_mask);
			assert(calls >= 1 && calls <= 33);
		}
	}
	puts("PASS: real VM RAM CLI destroy/close flows, error responses, close-before-unmap; no IOKit/VMRUN");
	return 0;
}
