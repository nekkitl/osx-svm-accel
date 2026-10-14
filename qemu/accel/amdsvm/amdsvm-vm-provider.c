#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "amdsvm-vm-provider.h"

#define AMDVM_OPERATION_GATE_CLOSING (1u << 31)
#define AMDVM_OPERATION_GATE_COUNT   (AMDVM_OPERATION_GATE_CLOSING - 1)

/* Hold the provider context across RUN/KICK transport calls. */
static int operation_enter(AMDVMVMProvider *p)
{
    uint32_t observed;

    for (;;) {
        observed = __sync_val_compare_and_swap(&p->operation_gate, 0, 0);
        if (observed & AMDVM_OPERATION_GATE_CLOSING) {
            return -ESHUTDOWN;
        }
        if ((observed & AMDVM_OPERATION_GATE_COUNT) == AMDVM_OPERATION_GATE_COUNT) {
            return -EOVERFLOW;
        }
        if (__sync_bool_compare_and_swap(&p->operation_gate, observed, observed + 1)) {
            return 0;
        }
    }
}

static void operation_leave(AMDVMVMProvider *p)
{
    __sync_fetch_and_sub(&p->operation_gate, 1);
}

static bool operation_close(AMDVMVMProvider *p)
{
    uint32_t observed;

    for (;;) {
        observed = __sync_val_compare_and_swap(&p->operation_gate, 0, 0);
        if (observed & AMDVM_OPERATION_GATE_CLOSING) {
            break;
        }
        if (__sync_bool_compare_and_swap(&p->operation_gate, observed,
                                         observed | AMDVM_OPERATION_GATE_CLOSING)) {
            observed |= AMDVM_OPERATION_GATE_CLOSING;
            break;
        }
    }
    return (observed & AMDVM_OPERATION_GATE_COUNT) == 0;
}

static bool valid_result(uint32_t version, uint32_t size, size_t expected)
{
    return version == AMDVM_VM_ABI_DRAFT_VERSION && size == expected;
}

int amdsvm_vm_provider_init(AMDVMVMProvider *p, AMDVMProviderTransport fn, void *opaque)
{
    amdvm_vm_abi_draft_negotiate_request_t request = {
        .requested_version = AMDVM_VM_ABI_DRAFT_VERSION,
        .structure_size = sizeof(request),
    };
    uint64_t required = AMDVM_VM_ABI_DRAFT_FEATURE_TRANSACTIONAL_MAP |
        AMDVM_VM_ABI_DRAFT_FEATURE_MAP_REPLACE_V1 |
        AMDVM_VM_ABI_DRAFT_FEATURE_PER_VCPU_NPT |
        AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_PREPARE |
        AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_STATE_V1 |
        AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_MEMORY_TYPES;
    size_t size;
    if (!p || !fn) { return -EINVAL; }
    memset(p, 0, sizeof(*p)); p->transport = fn; p->opaque = opaque;
    size = sizeof(p->caps);
    if (fn(opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_NEGOTIATE, &request,
           sizeof(request), &p->caps, &size) < 0 || size != sizeof(p->caps) ||
        !valid_result(p->caps.supported_version, p->caps.structure_size, size) ||
        (p->caps.feature_flags & required) != required || !p->caps.max_vcpus ||
        !p->caps.max_memory_ranges || !p->caps.max_guest_bytes ||
        p->caps.max_mapped_bytes < p->caps.max_guest_bytes) { return -ENOTSUP; }
    return 0;
}

int amdsvm_vm_provider_map(AMDVMVMProvider *p, const uint8_t rom[4096],
                          const amdvm_vcpu_memory_range_t *ranges, size_t count)
{
    g_autofree amdvm_vm_abi_draft_memory_map_request_t *request = NULL;
    g_autofree amdvm_vm_abi_draft_reset_rom_request_t *reset = NULL;
    amdvm_vm_abi_draft_lifecycle_result_t installed = {0};
    amdvm_vm_abi_draft_memory_map_result_t result = {0};
    uint64_t mapped = 0, writable = 0, end = 0;
    size_t size;
    if (!p || !p->transport || p->may_exist || !rom || !ranges || !count ||
        count > p->caps.max_memory_ranges || count > AMDVM_VCPU_MEMORY_MAX_RANGES) {
        return -EINVAL;
    }
    /* Capability accounting precedes any provider mutation. */
    for (size_t i = 0; i < count; ++i) {
        const amdvm_vcpu_memory_range_t *r = &ranges[i];
        if (!r->size || r->size > AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES ||
            ((r->guest_physical_address | r->user_virtual_address | r->size) & 4095) ||
            !r->user_virtual_address || r->reserved || !(r->flags & 1) || (r->flags & ~7u) ||
            r->guest_physical_address < end || r->guest_physical_address >= (1ull << 48) ||
            r->size > (1ull << 48) - r->guest_physical_address ||
            r->user_virtual_address > UINT64_MAX - r->size ||
            r->size > p->caps.max_mapped_bytes - mapped) { return -EINVAL; }
        end = r->guest_physical_address + r->size;
        if (r->guest_physical_address < AMDVM_VCPU_RESET_ROM_GPA + 4096 &&
            AMDVM_VCPU_RESET_ROM_GPA < end) { return -EINVAL; }
        mapped += r->size;
        if (r->flags & 2) {
            if (r->size > p->caps.max_guest_bytes - writable) { return -E2BIG; }
            writable += r->size;
        }
    }
    reset = g_new0(amdvm_vm_abi_draft_reset_rom_request_t, 1);
    reset->abi_version = AMDVM_VM_ABI_DRAFT_VERSION; reset->structure_size = sizeof(*reset);
    memcpy(reset->page, rom, sizeof(reset->page));
    p->may_exist = true; /* Failed copyout can still leave provider resources. */
    size = sizeof(installed);
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_SET_RESET_ROM,
        reset, sizeof(*reset), &installed, &size) < 0 || size != sizeof(installed) ||
        !valid_result(installed.abi_version, installed.structure_size, size) ||
        installed.context_status) { return -EIO; }
    request = g_new0(amdvm_vm_abi_draft_memory_map_request_t, 1);
    request->abi_version = AMDVM_VM_ABI_DRAFT_VERSION; request->structure_size = sizeof(*request);
    request->range_count = count;
    memcpy(request->ranges, ranges, count * sizeof(*ranges));
    size = sizeof(result);
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_SET_MEMORY_MAP,
        request, sizeof(*request), &result, &size) < 0) { return -EIO; }
    /* Retain a committed generation even if another result field is invalid. */
    if (result.map_generation) { p->generation = result.map_generation; }
    if (size != sizeof(result) || !valid_result(result.abi_version, result.structure_size, size) ||
        result.context_status || result.map_generation != 1 || result.range_count != count ||
        result.guest_page_count != mapped / 4096 || result.pinned_page_count != mapped / 4096) {
        return -EIO;
    }
    qatomic_set(&p->mapped, true);
    return 0;
}

int amdsvm_vm_provider_create(AMDVMVMProvider *p, uint32_t id, uint32_t apic_id)
{
    amdvm_vm_abi_draft_vcpu_create_request_t request = {
        .abi_version = AMDVM_VM_ABI_DRAFT_VERSION, .structure_size = sizeof(request),
        .vcpu_id = id, .apic_id = apic_id,
        .reset_profile = AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1,
    };
    amdvm_vm_abi_draft_vcpu_create_result_t result = {0};
    size_t size = sizeof(result);
    if (!p || !qatomic_read(&p->mapped) || id >= p->caps.max_vcpus ||
        id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS || qatomic_read(&p->cpus[id])) { return -EINVAL; }
    request.map_generation = p->generation;
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_CREATE,
        &request, sizeof(request), &result, &size) < 0 || size != sizeof(result) ||
        !valid_result(result.abi_version, result.structure_size, size) || result.context_status ||
        result.vcpu_id != id || result.map_generation != p->generation || !result.npt_page_count) {
        return -EIO;
    }
    qatomic_set(&p->cpus[id], true);
    return 0;
}

int amdsvm_vm_provider_destroy_vcpu(AMDVMVMProvider *p, uint32_t id)
{
    amdvm_vm_abi_draft_vcpu_lifecycle_request_t request = {
        .abi_version = AMDVM_VM_ABI_DRAFT_VERSION,
        .structure_size = sizeof(request),
        .vcpu_id = id,
    };
    amdvm_vm_abi_draft_lifecycle_result_t result = {0};
    size_t size = sizeof(result);

    if (!p || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
        !qatomic_read(&p->mapped) || !qatomic_read(&p->cpus[id])) {
        return -EINVAL;
    }
    if (operation_enter(p)) {
        return -ESHUTDOWN;
    }
    if (qatomic_read(&p->run_active[id])) {
        operation_leave(p);
        return -EBUSY;
    }
    request.map_generation = p->generation;
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DESTROY,
            &request, sizeof(request), &result, &size) < 0 ||
        size != sizeof(result) ||
        !valid_result(result.abi_version, result.structure_size, size) ||
        result.context_status || result.map_generation != p->generation) {
        operation_leave(p);
        return -EIO;
    }
    qatomic_set(&p->cpus[id], false);
    operation_leave(p);
    return 0;
}

static bool operation_begin_maintenance(AMDVMVMProvider *p)
{
    return __sync_bool_compare_and_swap(&p->operation_gate, 0,
                                         AMDVM_OPERATION_GATE_CLOSING);
}

static void operation_end_maintenance(AMDVMVMProvider *p)
{
    __sync_lock_test_and_set(&p->operation_gate, 0);
}

int amdsvm_vm_provider_replace_map(AMDVMVMProvider *p,
                                   const amdvm_vcpu_memory_range_t *ranges,
                                   size_t count)
{
    g_autofree amdvm_vm_abi_draft_memory_map_request_t *request = NULL;
    amdvm_vm_abi_draft_memory_map_result_t result = {0};
    uint64_t mapped = 0, writable = 0, end = 0;
    uint64_t old_generation;
    size_t size;
    int rc = -EINVAL;

    if (!p || !p->transport || !p->may_exist || !qatomic_read(&p->mapped) ||
        !ranges || !count || count > p->caps.max_memory_ranges ||
        count > AMDVM_VCPU_MEMORY_MAX_RANGES ||
        !(p->caps.feature_flags & AMDVM_VM_ABI_DRAFT_FEATURE_MAP_REPLACE_V1)) {
        return -EINVAL;
    }
    if (!operation_begin_maintenance(p)) {
        return -EBUSY;
    }
    for (unsigned i = 0; i < AMDVM_VM_ABI_DRAFT_MAX_VCPUS; ++i) {
        if (qatomic_read(&p->cpus[i]) || qatomic_read(&p->run_active[i])) {
            rc = -EBUSY;
            goto out;
        }
    }
    /* Validate the full replacement before asking the provider to pin it. */
    for (size_t i = 0; i < count; ++i) {
        const amdvm_vcpu_memory_range_t *r = &ranges[i];
        if (!r->size || r->size > AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES ||
            ((r->guest_physical_address | r->user_virtual_address | r->size) & 4095) ||
            !r->user_virtual_address || r->reserved || !(r->flags & 1) ||
            (r->flags & ~7u) || r->guest_physical_address < end ||
            r->guest_physical_address >= (1ull << 48) ||
            r->size > (1ull << 48) - r->guest_physical_address ||
            r->user_virtual_address > UINT64_MAX - r->size ||
            r->size > p->caps.max_mapped_bytes - mapped) {
            rc = -EINVAL;
            goto out;
        }
        end = r->guest_physical_address + r->size;
        if (r->guest_physical_address < AMDVM_VCPU_RESET_ROM_GPA + 4096 &&
            AMDVM_VCPU_RESET_ROM_GPA < end) {
            rc = -EINVAL;
            goto out;
        }
        mapped += r->size;
        if (r->flags & 2) {
            if (r->size > p->caps.max_guest_bytes - writable) {
                rc = -E2BIG;
                goto out;
            }
            writable += r->size;
        }
    }
    request = g_new0(amdvm_vm_abi_draft_memory_map_request_t, 1);
    request->abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
    request->structure_size = sizeof(*request);
    request->range_count = count;
    request->flags = AMDVM_VM_ABI_DRAFT_MEMORY_MAP_REPLACE;
    request->expected_generation = p->generation;
    memcpy(request->ranges, ranges, count * sizeof(*ranges));
    old_generation = p->generation;
    size = sizeof(result);
    rc = -EIO;
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_SET_MEMORY_MAP,
            request, sizeof(*request), &result, &size) < 0) {
        goto out;
    }
    if (result.map_generation != old_generation) {
        p->generation = result.map_generation;
    }
    if (size != sizeof(result) ||
        !valid_result(result.abi_version, result.structure_size, size) ||
        result.context_status || result.map_generation == old_generation ||
        result.range_count != count || result.guest_page_count != mapped / 4096 ||
        result.pinned_page_count != mapped / 4096) {
        goto out;
    }
    rc = 0;
out:
    operation_end_maintenance(p);
    return rc;
}

int amdsvm_vm_provider_state_roundtrip(AMDVMVMProvider *p, uint32_t id,
                                      const amdvm_x86_guest_state_v1_t *state)
{
    g_autofree amdvm_vm_abi_draft_vcpu_state_request_t *set = NULL;
    g_autofree amdvm_vm_abi_draft_vcpu_state_result_t *result = NULL;
    amdvm_vm_abi_draft_lifecycle_result_t stored = {0};
    amdvm_vm_abi_draft_vcpu_lifecycle_request_t get = {
        .abi_version = AMDVM_VM_ABI_DRAFT_VERSION, .structure_size = sizeof(get), .vcpu_id = id,
    };
    size_t size = sizeof(stored);
    if (!p || !state || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
        !qatomic_read(&p->cpus[id])) { return -EINVAL; }
    set = g_new0(amdvm_vm_abi_draft_vcpu_state_request_t, 1);
    set->abi_version = AMDVM_VM_ABI_DRAFT_VERSION; set->structure_size = sizeof(*set);
    set->vcpu_id = id; set->map_generation = p->generation; set->state = *state;
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_STATE,
        set, sizeof(*set), &stored, &size) < 0 || size != sizeof(stored) ||
        !valid_result(stored.abi_version, stored.structure_size, size) || stored.context_status ||
        stored.map_generation != p->generation) { return -EIO; }
    result = g_new0(amdvm_vm_abi_draft_vcpu_state_result_t, 1);
    get.map_generation = p->generation; size = sizeof(*result);
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_GET_STATE,
        &get, sizeof(get), result, &size) < 0 || size != sizeof(*result) ||
        !valid_result(result->abi_version, result->structure_size, size) || result->context_status ||
        result->vcpu_id != id || result->map_generation != p->generation ||
        memcmp(&result->state, state, sizeof(*state))) { return -EIO; }
    return 0;
}

static bool generation_newer(uint64_t candidate, uint64_t current)
{
    uint64_t delta = candidate - current;
    return delta != 0 && delta < (UINT64_C(1) << 63);
}

static bool generation_at_least(uint64_t observed, uint64_t requested)
{
    return observed == requested || generation_newer(observed, requested);
}

static bool run_kick_supported(const AMDVMVMProvider *p)
{
    uint64_t required = AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_RUN_V1 |
        AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_KICK_V1;
    return p && (p->caps.feature_flags & required) == required;
}

bool amdsvm_vm_provider_supports_kick(const AMDVMVMProvider *p)
{
    return p && (p->caps.feature_flags &
                 AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_KICK_V1) != 0;
}

int amdsvm_vm_provider_kick(AMDVMVMProvider *p, uint32_t id,
                            uint64_t kick_generation)
{
    amdvm_vm_abi_draft_vcpu_kick_request_t request = {0};
    amdvm_vm_abi_draft_vcpu_kick_result_t result = {0};
    uint64_t current;
    size_t size = sizeof(result);

    if (!p) {
        return -EINVAL;
    }
    if (operation_enter(p)) {
        return -ESHUTDOWN;
    }
    if (!run_kick_supported(p) || !p->transport || !kick_generation ||
        id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
        operation_leave(p);
        return -EINVAL;
    }
    if (!qatomic_read(&p->mapped) || !qatomic_read(&p->cpus[id])) {
        operation_leave(p);
        return -EAGAIN;
    }
    current = __sync_val_compare_and_swap(&p->kick_generation[id], 0, 0);
    for (;;) {
        if (kick_generation != current && !generation_newer(kick_generation, current)) {
            operation_leave(p);
            return -EINVAL;
        }
        if (kick_generation == current ||
            __sync_bool_compare_and_swap(&p->kick_generation[id], current,
                                         kick_generation)) {
            break;
        }
        current = __sync_val_compare_and_swap(&p->kick_generation[id], 0, 0);
    }

    request.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
    request.structure_size = sizeof(request);
    request.vcpu_id = id;
    request.flags = AMDVM_VM_ABI_DRAFT_VCPU_KICK_HOST_SIGNAL_SENT;
    request.map_generation = p->generation;
    request.kick_generation = kick_generation;
    /* Publish before transport: an IOKit error may follow a latched KICK. */
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_KICK,
            &request, sizeof(request), &result, &size) < 0 ||
        size != sizeof(result) ||
        !valid_result(result.abi_version, result.structure_size, size) ||
        result.vcpu_id != id || result.context_status ||
        result.map_generation != p->generation ||
        !generation_at_least(result.kick_generation, kick_generation)) {
        operation_leave(p);
        return -EIO;
    }
    operation_leave(p);
    return 0;
}

int amdsvm_vm_provider_run(AMDVMVMProvider *p, uint32_t id,
                           uint64_t kick_generation,
                           uint32_t event_injection,
                           uint32_t event_injection_error_code,
                           const amdvm_x86_guest_state_v1_t *state,
                           amdvm_vm_abi_draft_vcpu_run_result_t *result)
{
    amdvm_vm_abi_draft_vcpu_run_request_t request = {0};
    amdvm_vm_abi_draft_vcpu_run_result_t output = {0};
    uint32_t allowed = AMDVM_VCPU_RUN_CALLED |
        AMDVM_VCPU_RUN_LEASE_RETAINED | AMDVM_VCPU_RUN_KICKED;
    size_t size = sizeof(output);
    int rc = 0;
    bool preentry_fpu_not_ready;
    bool run_entry_gated;

    if (!p) {
        return -EINVAL;
    }
    if (operation_enter(p)) {
        return -ESHUTDOWN;
    }
    if (!run_kick_supported(p) || !p->transport || !state ||
        !result || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
        state->abi_version != AMDVM_X86_STATE_ABI_VERSION) {
        operation_leave(p);
        return -EINVAL;
    }
    if (!qatomic_read(&p->mapped) || !qatomic_read(&p->cpus[id])) {
        operation_leave(p);
        return -EAGAIN;
    }
    if (((event_injection & AMDVM_VCPU_EVENTINJ_TYPE_MASK) ==
          AMDVM_VCPU_EVENTINJ_TYPE_NMI ||
         (state->interruptibility &
          (AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED |
           AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP))) &&
        !(p->caps.feature_flags & AMDVM_VM_ABI_DRAFT_FEATURE_NMI_EVENT_V1)) {
        operation_leave(p);
        return -ENOTSUP;
    }
    if (!__sync_bool_compare_and_swap(&p->run_active[id], 0, 1)) {
        operation_leave(p);
        return -EBUSY;
    }
    request.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
    request.structure_size = sizeof(request);
    request.vcpu_id = id;
    request.map_generation = p->generation;
    request.kick_generation = kick_generation;
    request.event_injection = event_injection;
    request.event_injection_error_code = event_injection_error_code;
    request.state = *state;

    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN,
            &request, sizeof(request), &output, &size) < 0) {
        rc = -EIO;
    }
    preentry_fpu_not_ready = output.flags == 0 &&
        output.context_status == -11 && output.owner_status == -1 &&
        output.run_status == -6 && output.exit_code == UINT64_MAX &&
        memcmp(&output.state, state, sizeof(*state)) == 0;
    run_entry_gated = output.flags == 0 &&
        output.context_status == -8 && output.owner_status == -1 &&
        output.run_status == -5 && output.exit_code == UINT64_MAX &&
        memcmp(&output.state, state, sizeof(*state)) == 0;
    if (rc ||
        size != sizeof(output) ||
        !valid_result(output.abi_version, output.structure_size, size) ||
        output.vcpu_id != id || output.map_generation != p->generation ||
        (output.flags & ~allowed) ||
        (!(output.flags & (AMDVM_VCPU_RUN_CALLED | AMDVM_VCPU_RUN_KICKED)) &&
         !preentry_fpu_not_ready && !run_entry_gated) ||
        ((output.flags & AMDVM_VCPU_RUN_KICKED) &&
         !(output.flags & AMDVM_VCPU_RUN_CALLED) &&
         (output.exit_code != UINT64_MAX ||
          memcmp(&output.state, state, sizeof(*state)) != 0)) ||
        ((output.flags & AMDVM_VCPU_RUN_CALLED) &&
         (output.exit_code == UINT64_MAX ||
          output.state.abi_version != AMDVM_X86_STATE_ABI_VERSION)) ||
        ((output.flags & AMDVM_VCPU_RUN_KICKED) &&
         output.state.abi_version != AMDVM_X86_STATE_ABI_VERSION)) {
        rc = -EIO;
    } else if (run_entry_gated) {
        /* The kernel intentionally kept RUN fail-closed; no hardware entry. */
        rc = -ENOTSUP;
    } else if (!preentry_fpu_not_ready &&
               (output.context_status || output.owner_status || output.run_status ||
                (output.flags & AMDVM_VCPU_RUN_LEASE_RETAINED))) {
        rc = -EIO;
    }
    *result = output;
    __sync_lock_release(&p->run_active[id]);
    operation_leave(p);
    return rc;
}

int amdsvm_vm_provider_diagnostic(AMDVMVMProvider *p, uint32_t id,
                                  amdvm_vm_abi_draft_vcpu_diagnostic_result_t *result)
{
    amdvm_vm_abi_draft_vcpu_diagnostic_request_t request = {0};
    amdvm_vm_abi_draft_vcpu_diagnostic_result_t output = {0};
    const uint32_t allowed_flags =
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ENABLED |
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ROOT_MATCH |
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_TRANSLATES |
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_APIC_UNMAPPED |
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_BACKING_MATCH;
    size_t size = sizeof(output);

    if (!p || !result || !p->transport || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
        operation_enter(p)) {
        return -EINVAL;
    }
    if (!qatomic_read(&p->mapped) || !qatomic_read(&p->cpus[id])) {
        operation_leave(p);
        return -EAGAIN;
    }
    request.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
    request.structure_size = sizeof(request);
    request.vcpu_id = id;
    request.map_generation = p->generation;
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DIAGNOSTIC,
            &request, sizeof(request), &output, &size) < 0 ||
        size != sizeof(output) ||
        !valid_result(output.abi_version, output.structure_size, size) ||
        output.vcpu_id != id || output.map_generation != p->generation ||
        output.context_status != 0 || output.reserved != 0 ||
        output.snapshot.abi_version != AMDVM_VCPU_ABI_VERSION ||
        output.snapshot.context_status != 0 || output.snapshot.reserved != 0 ||
        (output.flags & ~allowed_flags) ||
        output.npt_root_matches > 1u ||
        (!!(output.flags & AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ROOT_MATCH) !=
         !!output.npt_root_matches) ||
        (output.reset_translation_status !=
             AMDVM_VM_ABI_DRAFT_NPT_STATUS_MAPPED &&
         output.reset_translation_status !=
             AMDVM_VM_ABI_DRAFT_NPT_STATUS_INVALID) ||
        (output.apic_translation_status !=
             AMDVM_VM_ABI_DRAFT_NPT_STATUS_MAPPED &&
         output.apic_translation_status !=
             AMDVM_VM_ABI_DRAFT_NPT_STATUS_UNMAPPED &&
         output.apic_translation_status !=
             AMDVM_VM_ABI_DRAFT_NPT_STATUS_INVALID) ||
        (!!(output.flags & AMDVM_VM_ABI_DRAFT_VCPU_DIAG_APIC_UNMAPPED) !=
         (output.apic_translation_status ==
             AMDVM_VM_ABI_DRAFT_NPT_STATUS_UNMAPPED)) ||
        ((output.flags & AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_TRANSLATES) &&
         output.reset_translation_status !=
             AMDVM_VM_ABI_DRAFT_NPT_STATUS_MAPPED) ||
        ((output.flags & AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_BACKING_MATCH) &&
         (!(output.flags & AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_TRANSLATES) ||
          output.reset_translation_hpa == 0)) ||
        (!(output.flags & AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_TRANSLATES) &&
         output.reset_translation_hpa != 0)) {
        operation_leave(p);
        return -EIO;
    }
    *result = output;
    operation_leave(p);
    return 0;
}

int amdsvm_vm_provider_set_memory_types(
    AMDVMVMProvider *p, uint32_t id,
    const amdvm_vcpu_set_memory_types_request_t *types)
{
    g_autofree amdvm_vm_abi_draft_vcpu_memory_types_request_t *request = NULL;
    amdvm_vm_abi_draft_vcpu_memory_types_result_t result = {0};
    uint64_t expected_pages = 0;
    size_t size = sizeof(result);

    if (!p || !p->transport || !types || !p->mapped ||
        id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS || !p->cpus[id] ||
        types->abi_version != AMDVM_VCPU_ABI_VERSION ||
        !types->range_count ||
        types->range_count > AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES ||
        types->reserved[0] || types->reserved[1]) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < types->range_count; ++i) {
        const amdvm_vcpu_memory_type_range_t *range = &types->ranges[i];
        if (!range->size || (range->guest_physical_address & 4095) ||
            (range->size & 4095) || range->reserved ||
            range->size / 4096 > UINT64_MAX - expected_pages) {
            return -EINVAL;
        }
        expected_pages += range->size / 4096;
    }
    for (uint32_t i = types->range_count;
         i < AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES; ++i) {
        amdvm_vcpu_memory_type_range_t zero = {0};
        if (memcmp(&types->ranges[i], &zero, sizeof(zero))) {
            return -EINVAL;
        }
    }

    request = g_new0(amdvm_vm_abi_draft_vcpu_memory_types_request_t, 1);
    request->abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
    request->structure_size = sizeof(*request);
    request->vcpu_id = id;
    request->range_count = types->range_count;
    request->map_generation = p->generation;
    memcpy(request->ranges, types->ranges,
           types->range_count * sizeof(types->ranges[0]));
    if (p->transport(p->opaque,
            AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_MEMORY_TYPES,
            request, sizeof(*request), &result, &size) < 0) {
        return -EIO;
    }
    if (size != sizeof(result) ||
        !valid_result(result.abi_version, result.structure_size, size)) {
        return -EPROTO;
    }
    /* Preserve the provider's concrete failure code for diagnostics. */
    if (result.context_status) {
        return result.context_status;
    }
    if (result.vcpu_id != id ||
        result.map_generation != p->generation ||
        result.range_count != types->range_count ||
        result.updated_page_count != expected_pages) {
        error_report("AMD-SVM memory-type response mismatch: vcpu=%u/%u generation=%" PRIu64 "/%" PRIu64 " ranges=%u/%u updated_pages=%" PRIu64 "/%" PRIu64,
                    result.vcpu_id, id, result.map_generation, p->generation,
                    result.range_count, types->range_count,
                    result.updated_page_count, expected_pages);
        return -EPROTO;
    }
    return 0;
}

int amdsvm_vm_provider_destroy(AMDVMVMProvider *p)
{
    amdvm_vm_abi_draft_vm_destroy_request_t request = {
        .abi_version = AMDVM_VM_ABI_DRAFT_VERSION, .structure_size = sizeof(request),
    };
    amdvm_vm_abi_draft_lifecycle_result_t result = {0};
    size_t size = sizeof(result);
    if (!p || !p->may_exist) { return 0; }
    if (!operation_close(p)) {
        return -EBUSY;
    }
    request.expected_generation = p->generation;
    if (p->transport(p->opaque, AMDVM_VM_ABI_DRAFT_SELECTOR_VM_DESTROY,
        &request, sizeof(request), &result, &size) < 0 || size != sizeof(result) ||
        !valid_result(result.abi_version, result.structure_size, size) || result.context_status ||
        result.map_generation != p->generation) { return -EIO; }
    p->may_exist = false;
    qatomic_set(&p->mapped, false);
    for (unsigned i = 0; i < AMDVM_VM_ABI_DRAFT_MAX_VCPUS; ++i) {
        qatomic_set(&p->cpus[i], false);
    }
    memset(p->kick_generation, 0, sizeof(p->kick_generation));
    __sync_lock_test_and_set(&p->operation_gate, 0);
    return 0;
}
