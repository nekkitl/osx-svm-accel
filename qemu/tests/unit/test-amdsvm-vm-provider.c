#include "qemu/osdep.h"
#include "../../accel/amdsvm/amdsvm-vm-provider.h"

typedef struct Mock {
    unsigned calls, fail_at;
    uint64_t generation;
    unsigned mask;
    bool map, rom;
    bool corrupt_map_result, corrupt_state_result;
    bool fail_after_shared_complete;
    bool fail_after_shared_claim;
    unsigned pause_stats_reports;
    bool block_run, run_entered, release_run;
    GMutex run_mutex;
    GCond run_cond;
    uint64_t kick_generations[4];
    uint64_t kick_generation_floor[4];
    uint32_t last_run_flags;
    uint64_t run_exit_code;
    uint64_t last_tsc_offset;
    amdvm_vm_shared_run_area_t *shared_run_area;
    amdvm_x86_guest_state_v1_t states[4];
} Mock;

static int transport(void *opaque, uint32_t selector, const void *input,
                     size_t in_size, void *output, size_t *out_size)
{
    Mock *m = opaque;
    if (++m->calls == m->fail_at) { return -EIO; }
    g_assert_cmpuint(selector, >=, 13); /* VM-scoped ABI only. */
    switch (selector) {
    case AMDVM_VM_ABI_DRAFT_SELECTOR_NEGOTIATE: {
        const amdvm_vm_abi_draft_negotiate_request_t *r = input;
        amdvm_vm_abi_draft_negotiate_result_t *o = output;
        g_assert_cmpuint(in_size, ==, sizeof(*r));
        g_assert_cmpuint(*out_size, ==, sizeof(*o));
        *o = (amdvm_vm_abi_draft_negotiate_result_t) {
            .supported_version = 2, .structure_size = sizeof(*o),
            .max_vcpus = 4, .max_memory_ranges = 64,
            .feature_flags = 127 | AMDVM_VM_ABI_DRAFT_FEATURE_MAP_REPLACE_V1,
            .max_guest_bytes = 16ull << 30, .max_mapped_bytes = 17ull << 30,
        };
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_SET_RESET_ROM: {
        const amdvm_vm_abi_draft_reset_rom_request_t *r = input;
        amdvm_vm_abi_draft_lifecycle_result_t *o = output;
        g_assert_cmpuint(in_size, ==, sizeof(*r));
        g_assert_cmpuint(r->page[0xff0], ==, 0xf4);
        *o = (amdvm_vm_abi_draft_lifecycle_result_t) { .abi_version = 2, .structure_size = sizeof(*o) };
        m->rom = true;
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_SET_MEMORY_MAP: {
        const amdvm_vm_abi_draft_memory_map_request_t *r = input;
        amdvm_vm_abi_draft_memory_map_result_t *o = output;
        g_assert_true(m->rom);
        g_assert_cmpuint(r->range_count, >, 0);
        uint64_t pages = 0;
        for (unsigned i = 0; i < r->range_count; ++i) { pages += r->ranges[i].size / 4096; }
        if (r->flags & AMDVM_VM_ABI_DRAFT_MEMORY_MAP_REPLACE) {
            g_assert_true(m->map);
            g_assert_cmpuint(m->mask, ==, 0);
            g_assert_cmpuint(r->expected_generation, ==, m->generation);
            m->generation++;
        } else {
            g_assert_false(m->map);
            g_assert_cmpuint(r->flags, ==, 0);
            m->generation = 1;
            m->map = true;
        }
        *o = (amdvm_vm_abi_draft_memory_map_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o), .range_count = r->range_count,
            .map_generation = m->generation, .guest_page_count = pages, .pinned_page_count = pages,
        };
        if (m->corrupt_map_result) { o->pinned_page_count = 0; }
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_CREATE: {
        const amdvm_vm_abi_draft_vcpu_create_request_t *r = input;
        amdvm_vm_abi_draft_vcpu_create_result_t *o = output;
        g_assert_true(m->map); g_assert_cmpuint(r->vcpu_id, <, 4);
        g_assert_cmpuint(r->apic_id, ==, 2 * r->vcpu_id);
        g_assert_cmpuint(r->map_generation, ==, m->generation);
        m->kick_generations[r->vcpu_id] = 0;
        m->mask |= 1u << r->vcpu_id;
        *o = (amdvm_vm_abi_draft_vcpu_create_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o), .vcpu_id = r->vcpu_id,
            .map_generation = m->generation, .npt_page_count = 4620,
        };
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_STATE: {
        const amdvm_vm_abi_draft_vcpu_state_request_t *r = input;
        amdvm_vm_abi_draft_lifecycle_result_t *o = output;
        g_assert_cmpuint(r->vcpu_id, <, 4); g_assert_true(m->mask & (1u << r->vcpu_id));
        m->states[r->vcpu_id] = r->state;
        *o = (amdvm_vm_abi_draft_lifecycle_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o), .map_generation = m->generation,
        };
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DESTROY: {
        const amdvm_vm_abi_draft_vcpu_lifecycle_request_t *r = input;
        amdvm_vm_abi_draft_lifecycle_result_t *o = output;
        g_assert_cmpuint(r->vcpu_id, <, 4);
        g_assert_true(m->mask & (1u << r->vcpu_id));
        g_assert_cmpuint(r->map_generation, ==, m->generation);
        m->mask &= ~(1u << r->vcpu_id);
        m->kick_generations[r->vcpu_id] = 0;
        *o = (amdvm_vm_abi_draft_lifecycle_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o),
            .map_generation = m->generation,
        };
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_GET_STATE: {
        const amdvm_vm_abi_draft_vcpu_lifecycle_request_t *r = input;
        amdvm_vm_abi_draft_vcpu_state_result_t *o = output;
        g_assert_cmpuint(r->vcpu_id, <, 4);
        *o = (amdvm_vm_abi_draft_vcpu_state_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o), .vcpu_id = r->vcpu_id,
            .map_generation = m->generation, .state = m->states[r->vcpu_id],
        };
        if (m->corrupt_state_result) { o->state.gpr[0] ^= 1; }
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_KICK: {
        const amdvm_vm_abi_draft_vcpu_kick_request_t *r = input;
        amdvm_vm_abi_draft_vcpu_kick_result_t *o = output;
        g_assert_cmpuint(in_size, ==, sizeof(*r));
        g_assert_cmpuint(r->vcpu_id, <, 4);
        g_assert_true(m->mask & (1u << r->vcpu_id));
        g_assert_cmpuint(r->map_generation, ==, m->generation);
        if (r->flags == AMDVM_VM_ABI_DRAFT_VCPU_KICK_REPORT_PAUSE_STATS) {
            g_assert_cmpuint(r->kick_generation, ==,
                             m->kick_generations[r->vcpu_id]);
            m->pause_stats_reports++;
        } else {
            g_assert_cmpuint(r->flags, ==,
                             AMDVM_VM_ABI_DRAFT_VCPU_KICK_HOST_SIGNAL_SENT);
            g_assert_cmpuint(r->kick_generation, >=,
                             m->kick_generations[r->vcpu_id]);
            m->kick_generations[r->vcpu_id] = MAX(r->kick_generation,
                m->kick_generation_floor[r->vcpu_id]);
        }
        *o = (amdvm_vm_abi_draft_vcpu_kick_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o), .vcpu_id = r->vcpu_id,
            .map_generation = m->generation,
            .kick_generation = m->kick_generations[r->vcpu_id],
        };
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN: {
        const amdvm_vm_abi_draft_vcpu_run_request_t *r = input;
        amdvm_vm_abi_draft_vcpu_run_result_t *o = output;
        g_assert_true(in_size == sizeof(*r) || in_size ==
            AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V2_SIZE);
        g_assert_cmpuint(r->structure_size, ==, in_size);
        g_assert_cmpuint(r->vcpu_id, <, 4);
        g_assert_true(m->mask & (1u << r->vcpu_id));
        g_assert_cmpuint(r->map_generation, ==, m->generation);
        g_assert_cmpuint(r->kick_generation, ==, m->kick_generations[r->vcpu_id]);
        m->last_run_flags = r->flags;
        m->last_tsc_offset = in_size == sizeof(*r) ? r->tsc_offset : 0;
        if (m->block_run) {
            g_mutex_lock(&m->run_mutex);
            m->run_entered = true;
            g_cond_broadcast(&m->run_cond);
            while (!m->release_run) {
                g_cond_wait(&m->run_cond, &m->run_mutex);
            }
            g_mutex_unlock(&m->run_mutex);
        }
        *o = (amdvm_vm_abi_draft_vcpu_run_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o), .vcpu_id = r->vcpu_id,
            .flags = AMDVM_VCPU_RUN_CALLED |
                (m->kick_generations[r->vcpu_id] != r->kick_generation ?
                 AMDVM_VCPU_RUN_KICKED : 0),
            .map_generation = m->generation,
            .exit_code = m->run_exit_code ? m->run_exit_code : 0x78,
            .state = r->state,
        };
        o->state.rip += 1;
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN_SHARED: {
        const amdvm_vm_shared_run_call_t *call = input;
        amdvm_vm_shared_run_ack_t *ack = output;
        amdvm_vm_shared_run_slot_t *slot;
        amdvm_vm_abi_draft_vcpu_run_request_t request = {0};
        amdvm_vm_abi_draft_vcpu_run_result_t result = {0};
        g_assert_nonnull(m->shared_run_area);
        g_assert_cmpuint(in_size, ==, sizeof(*call));
        g_assert_cmpuint(*out_size, ==, sizeof(*ack));
        g_assert_cmpuint(call->vcpu_id, <, 4);
        slot = &m->shared_run_area->slots[call->vcpu_id];
        g_assert_true(amdvm_vm_shared_run_provider_claim(slot,
            call->sequence, call->request_size));
        if (m->fail_after_shared_claim) {
            return -EIO;
        }
        memcpy(&request, slot->request, call->request_size);
        g_assert_cmpuint(request.vcpu_id, ==, call->vcpu_id);
        g_assert_cmpuint(request.map_generation, ==, m->generation);
        m->last_run_flags = request.flags;
        m->last_tsc_offset = call->request_size == sizeof(request) ?
            request.tsc_offset : 0;
        result = (amdvm_vm_abi_draft_vcpu_run_result_t) {
            .abi_version = 2, .structure_size = sizeof(result),
            .vcpu_id = request.vcpu_id,
            .flags = AMDVM_VCPU_RUN_CALLED |
                (m->kick_generations[request.vcpu_id] !=
                 request.kick_generation ? AMDVM_VCPU_RUN_KICKED : 0),
            .map_generation = m->generation,
            .exit_code = 0x78, .state = request.state,
        };
        result.state.rip++;
        memcpy(slot->result, &result, sizeof(result));
        g_assert_true(amdvm_vm_shared_run_provider_complete(slot,
            call->sequence, 0));
        *ack = (amdvm_vm_shared_run_ack_t) {
            .abi_version = 2, .structure_size = sizeof(*ack),
            .vcpu_id = call->vcpu_id, .status = 0,
            .sequence = call->sequence,
            .map_generation = call->map_generation,
        };
        if (m->fail_after_shared_complete) {
            return -EIO;
        }
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VM_DESTROY: {
        const amdvm_vm_abi_draft_vm_destroy_request_t *r = input;
        amdvm_vm_abi_draft_lifecycle_result_t *o = output;
        g_assert_cmpuint(r->expected_generation, ==, m->generation);
        *o = (amdvm_vm_abi_draft_lifecycle_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o), .map_generation = m->generation,
        };
        m->rom = m->map = false; m->mask = 0;
        break;
    }
    default: g_assert_not_reached();
    }
    return 0;
}

static void exercise(void)
{
    uint8_t rom[4096] = { [0xff0] = 0xf4 };
    amdvm_vcpu_memory_range_t ranges[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        ranges[i].guest_physical_address = i ? (4ull << 30) + ((i - 1ull) * (3ull << 30)) : 0;
        ranges[i].user_virtual_address = (16ull << 30) + i * (3ull << 30);
        ranges[i].size = 3ull << 30; ranges[i].flags = 7;
    }
    for (unsigned fail = 0; fail <= 16; ++fail) {
        Mock *m = g_new0(Mock, 1);
        AMDVMVMProvider p;
        m->fail_at = fail;
        int rc = amdsvm_vm_provider_init(&p, transport, m);
        if (!rc) { rc = amdsvm_vm_provider_map(&p, rom, ranges, 3); }
        for (unsigned id = 0; !rc && id < 4; ++id) {
            amdvm_x86_guest_state_v1_t state = { .abi_version = 1, .rip = 0xfff0 + id };
            state.gpr[0] = id + 100; state.xstate[160] = id;
            rc = amdsvm_vm_provider_create(&p, id, 2 * id);
            if (!rc) { rc = amdsvm_vm_provider_state_roundtrip(&p, id, &state); }
        }
        int destroyed = amdsvm_vm_provider_destroy(&p);
        if (destroyed) {
            g_assert_true(p.may_exist);
            g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
        }
        g_assert_false(p.may_exist); g_assert_false(m->map); g_assert_cmpuint(m->mask, ==, 0);
        if (!fail) { g_assert_cmpint(rc, ==, 0); }
        g_free(m);
    }
    /* Capability rejection must not install ROM or pin a partial map. */
    Mock *m = g_new0(Mock, 1);
    AMDVMVMProvider p;
    g_assert_cmpint(amdsvm_vm_provider_init(&p, transport, m), ==, 0);
    p.caps.max_guest_bytes = 8ull << 30;
    g_assert_cmpint(amdsvm_vm_provider_map(&p, rom, ranges, 3), ==, -E2BIG);
    g_assert_false(p.may_exist); g_assert_cmpuint(m->calls, ==, 1);
    g_free(m);
    m = g_new0(Mock, 1);
    m->corrupt_map_result = true;
    g_assert_cmpint(amdsvm_vm_provider_init(&p, transport, m), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_map(&p, rom, ranges, 3), ==, -EIO);
    g_assert_true(p.may_exist); g_assert_false(p.mapped);
    g_assert_cmpuint(p.generation, ==, 1);
    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
    g_assert_false(m->map);
    g_free(m);
    m = g_new0(Mock, 1);
    g_assert_cmpint(amdsvm_vm_provider_init(&p, transport, m), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_map(&p, rom, ranges, 3), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_create(&p, 0, 0), ==, 0);
    m->corrupt_state_result = true;
    amdvm_x86_guest_state_v1_t state = {0};
    g_assert_cmpint(amdsvm_vm_provider_state_roundtrip(&p, 0, &state), ==, -EIO);
    g_assert_true(p.may_exist);
    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
    g_free(m);
}

typedef struct RunThread {
    AMDVMVMProvider *provider;
    amdvm_x86_guest_state_v1_t state;
    amdvm_vm_abi_draft_vcpu_run_result_t result;
    int status;
} RunThread;

static gpointer run_thread(gpointer opaque)
{
    RunThread *thread = opaque;
    thread->status = amdsvm_vm_provider_run(thread->provider, 0, 1, 0, 0,
                                             false, 0,
                                             &thread->state, &thread->result);
    return NULL;
}

static void run_kick_lifetime(void)
{
    uint8_t rom[4096] = { [0xff0] = 0xf4 };
    amdvm_vcpu_memory_range_t range = {
        .guest_physical_address = 0,
        .user_virtual_address = 16ull << 30,
        .size = 1ull << 30,
        .flags = 7,
    };
    Mock *m = g_new0(Mock, 1);
    AMDVMVMProvider p;
    GThread *worker;
    RunThread thread = { .provider = &p, .state = { .abi_version = AMDVM_X86_STATE_ABI_VERSION, .rip = 0x1000 } };

    g_mutex_init(&m->run_mutex);
    g_cond_init(&m->run_cond);
    g_assert_cmpint(amdsvm_vm_provider_init(&p, transport, m), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_map(&p, rom, &range, 1), ==, 0);
    for (uint32_t id = 0; id < 4; ++id) {
        g_assert_cmpint(amdsvm_vm_provider_create(&p, id, 2 * id), ==, 0);
    }
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 1, NULL), ==, 0);
    uint32_t nmi = AMDVM_VCPU_EVENTINJ_VALID |
                   AMDVM_VCPU_EVENTINJ_TYPE_NMI | 2u;
    thread.state.interruptibility = AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED;
    unsigned int calls_before_nmi = m->calls;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, nmi, 0,
                    false, 0,
                    &thread.state, &thread.result), ==, -ENOTSUP);
    g_assert_cmpuint(m->calls, ==, calls_before_nmi);
    p.caps.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_NMI_EVENT_V1;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, nmi, 0,
                    false, 0,
                    &thread.state, &thread.result), ==, 0);

    unsigned int calls_before_tsc = m->calls;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0,
                    true, UINT64_C(0x123456789abcdef0),
                    &thread.state, &thread.result), ==, -ENOTSUP);
    g_assert_cmpuint(m->calls, ==, calls_before_tsc);
    p.caps.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_RDTSC_V1;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0,
                    true, UINT64_C(0x123456789abcdef0),
                    &thread.state, &thread.result), ==, 0);
    g_assert_cmpuint(m->last_run_flags &
        AMDVM_VM_ABI_DRAFT_VCPU_RUN_NATIVE_RDTSC, !=, 0);
    g_assert_cmphex(m->last_tsc_offset, ==, UINT64_C(0x123456789abcdef0));

    unsigned int calls_before_native_cr3 = m->calls;
    g_assert_cmpint(amdsvm_vm_provider_run_ex(&p, 0, 1, 0, 0,
                    false, 0, true, &thread.state, &thread.result), ==,
                    -ENOTSUP);
    g_assert_cmpuint(m->calls, ==, calls_before_native_cr3);
    p.caps.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_CR3_V1;
    g_assert_cmpint(amdsvm_vm_provider_run_ex(&p, 0, 1, 0, 0,
                    false, 0, true, &thread.state, &thread.result), ==, 0);
    g_assert_cmpuint(m->last_run_flags &
        AMDVM_VM_ABI_DRAFT_VCPU_RUN_NATIVE_CR3, !=, 0);
    thread.state.cr[4] = UINT64_C(1) << 17;
    calls_before_native_cr3 = m->calls;
    g_assert_cmpint(amdsvm_vm_provider_run_ex(&p, 0, 1, 0, 0,
                    false, 0, true, &thread.state, &thread.result), ==,
                    -ENOTSUP);
    g_assert_cmpuint(m->calls, ==, calls_before_native_cr3);
    thread.state.cr[4] = 0;

    /* First-PF capture is opt-in and must fail closed unless the provider
     * advertises the appended diagnostic-intercept capability. */
    unsigned int calls_before_pf_diag = m->calls;
    p.diagnostic_pf_intercept = true;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, -ENOTSUP);
    g_assert_cmpuint(m->calls, ==, calls_before_pf_diag);
    p.caps.feature_flags |=
        AMDVM_VM_ABI_DRAFT_FEATURE_DIAGNOSTIC_PF_INTERCEPT;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, 0);
    g_assert_cmphex(m->last_run_flags, ==,
        AMDVM_VM_ABI_DRAFT_VCPU_RUN_DIAGNOSTIC_INTERCEPT_PF);
    p.diagnostic_pf_intercept = false;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, 0);
    g_assert_cmphex(m->last_run_flags, ==, 0);

    /* Optional RUN optimizations require their advertised capability. */
    unsigned before_optional = m->calls;
    p.mtrr_cache_enabled = true;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, -ENOTSUP);
    g_assert_cmpuint(m->calls, ==, before_optional);
    p.caps.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_MTRR_PAT_CACHE_V1;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, 0);
    g_assert_cmphex(m->last_run_flags, ==,
                    AMDVM_VM_ABI_DRAFT_VCPU_RUN_MTRR_CACHE);
    before_optional = m->calls;
    p.provider_pause_enabled = true;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, -ENOTSUP);
    g_assert_cmpuint(m->calls, ==, before_optional);
    p.caps.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, 0);
    g_assert_cmphex(m->last_run_flags, ==,
                    AMDVM_VM_ABI_DRAFT_VCPU_RUN_MTRR_CACHE |
                    AMDVM_VM_ABI_DRAFT_VCPU_RUN_PROVIDER_PAUSE);
    p.mtrr_cache_enabled = false;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, 0);
    g_assert_cmphex(m->last_run_flags, ==,
                    AMDVM_VM_ABI_DRAFT_VCPU_RUN_PROVIDER_PAUSE);
    p.provider_pause_enabled = false;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, 0);
    g_assert_cmphex(m->last_run_flags, ==, 0);

    for (uint32_t id = 0; id < 4; ++id) {
        g_assert_cmpint(amdsvm_vm_provider_kick(&p, id, 1, NULL), ==, 0);
        amdvm_vm_abi_draft_vcpu_run_result_t result = {0};
        g_assert_cmpint(amdsvm_vm_provider_run(&p, id, 1, 0, 0,
                                                false, 0,
                                                &thread.state, &result), ==, 0);
        g_assert_cmpuint(result.exit_code, ==, 0x78);
        g_assert_cmpuint(result.state.rip, ==, thread.state.rip + 1);
        g_assert_cmpint(amdsvm_vm_provider_kick(&p, id, 1, NULL), ==, 0);
    }

    /* UINT64_MAX is a completed SVM_EXIT_ERR, not a missing result. */
    m->run_exit_code = UINT64_MAX;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, 0);
    g_assert_cmphex(thread.result.exit_code, ==, UINT64_MAX);
    /* NOT_RUN is a distinct provider condition, never a completed exit. */
    m->run_exit_code = UINT64_MAX - 1u;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, -EAGAIN);
    m->run_exit_code = 0;
    m->fail_at = m->calls + 1;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
                    &thread.state, &thread.result), ==, -EIO);
    m->fail_at = 0;

    m->block_run = true;
    worker = g_thread_new("amdsvm-run", run_thread, &thread);
    g_mutex_lock(&m->run_mutex);
    while (!m->run_entered) {
        g_cond_wait(&m->run_cond, &m->run_mutex);
    }
    g_mutex_unlock(&m->run_mutex);

    /* QEMU's host-thread signal is paired with a concurrent generation latch. */
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 2, NULL), ==, 0);
    /* Destroy closes admission first and refuses to free a live RUN context. */
    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, -EBUSY);
    g_mutex_lock(&m->run_mutex);
    m->release_run = true;
    g_cond_broadcast(&m->run_cond);
    g_mutex_unlock(&m->run_mutex);
    g_thread_join(worker);
    g_assert_cmpint(thread.status, ==, 0);
    g_assert_cmpuint(thread.result.flags & AMDVM_VCPU_RUN_KICKED, !=, 0);
    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0,
                                            false, 0,
                                            &thread.state, &thread.result), ==, -EAGAIN);

    g_cond_clear(&m->run_cond);
    g_mutex_clear(&m->run_mutex);
    g_free(m);
}

static void kick_generation_reconcile(void)
{
    uint8_t rom[4096] = { [0xff0] = 0xf4 };
    amdvm_vcpu_memory_range_t range = {
        .guest_physical_address = 0,
        .user_virtual_address = 16ull << 30,
        .size = 1ull << 30,
        .flags = 7,
    };
    Mock *m = g_new0(Mock, 1);
    AMDVMVMProvider p;
    amdvm_x86_guest_state_v1_t state = {
        .abi_version = AMDVM_X86_STATE_ABI_VERSION,
        .rip = 0x1000,
    };
    amdvm_vm_abi_draft_vcpu_run_result_t result = {0};
    uint64_t latched_generation = 0;

    g_assert_cmpint(amdsvm_vm_provider_init(&p, transport, m), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_map(&p, rom, &range, 1), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_create(&p, 0, 0), ==, 0);

    /* Simulate a provider generation newer than QEMU's lagging local value. */
    m->kick_generation_floor[0] = 2;
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 1,
                    &latched_generation), ==, 0);
    g_assert_cmpuint(latched_generation, ==, 2);
    g_assert_cmpuint(p.kick_generation[0], ==, 2);

    /* RUN must use that latched value even if its caller still says 1. */
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0,
                    false, 0,
                    &state, &result), ==, 0);
    g_assert_cmpuint(m->kick_generations[0], ==, 2);
    g_assert_cmpuint(result.exit_code, ==, 0x78);

    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
    g_free(m);
}

static void pause_stats_report_is_read_only(void)
{
    uint8_t rom[4096] = { [0xff0] = 0xf4 };
    amdvm_vcpu_memory_range_t range = {
        .guest_physical_address = 0,
        .user_virtual_address = 16ull << 30,
        .size = 1ull << 30,
        .flags = 7,
    };
    Mock *m = g_new0(Mock, 1);
    AMDVMVMProvider p;

    g_assert_cmpint(amdsvm_vm_provider_init(&p, transport, m), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_map(&p, rom, &range, 1), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_create(&p, 0, 0), ==, 0);
    p.caps.feature_flags |=
        AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1;
    g_assert_cmpint(amdsvm_vm_provider_report_pause_stats(&p), ==, 0);
    g_assert_cmpuint(m->pause_stats_reports, ==, 1);
    g_assert_cmpuint(m->kick_generations[0], ==, 0);
    g_assert_cmpuint(p.kick_generation[0], ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 9, NULL), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_report_pause_stats(&p), ==, 0);
    g_assert_cmpuint(m->pause_stats_reports, ==, 2);
    g_assert_cmpuint(m->kick_generations[0], ==, 9);
    g_assert_cmpuint(p.kick_generation[0], ==, 9);
    p.caps.feature_flags &=
        ~AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1;
    g_assert_cmpint(amdsvm_vm_provider_report_pause_stats(&p), ==, -ENOTSUP);
    g_assert_cmpuint(m->pause_stats_reports, ==, 2);

    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
    g_free(m);
}

static void shared_run_handoff(void)
{
    uint8_t rom[4096] = { [0xff0] = 0xf4 };
    amdvm_vcpu_memory_range_t range = {
        .guest_physical_address = 0,
        .user_virtual_address = 16ull << 30,
        .size = 1ull << 30,
        .flags = 7,
    };
    amdvm_vm_shared_run_area_t *area = g_new0(amdvm_vm_shared_run_area_t, 1);
    Mock *m = g_new0(Mock, 1);
    AMDVMVMProvider p;
    amdvm_x86_guest_state_v1_t state = {
        .abi_version = AMDVM_X86_STATE_ABI_VERSION,
        .rip = 0x1234,
    };
    amdvm_vm_abi_draft_vcpu_run_result_t result = {0};

    m->shared_run_area = area;
    g_assert_cmpint(amdsvm_vm_provider_init(&p, transport, m), ==, 0);
    p.caps.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_SHARED_VCPU_RUN_V1;
    p.shared_run_area = area;
    g_assert_cmpint(amdsvm_vm_provider_map(&p, rom, &range, 1), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_create(&p, 0, 0), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 1, NULL), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
        &state, &result), ==, 0);
    g_assert_cmpuint(result.exit_code, ==, 0x78);
    g_assert_cmpuint(result.state.rip, ==, state.rip + 1);
    g_assert_cmpuint(area->slots[0].owner, ==, AMDVM_VM_SHARED_RUN_FREE);
    g_assert_cmpuint(area->slots[0].completed_sequence, ==, 1);

    p.caps.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_MTRR_PAT_CACHE_V1 |
                            AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1;
    p.mtrr_cache_enabled = true;
    p.provider_pause_enabled = true;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
        &state, &result), ==, 0);
    g_assert_cmphex(m->last_run_flags, ==,
                    AMDVM_VM_ABI_DRAFT_VCPU_RUN_MTRR_CACHE |
                    AMDVM_VM_ABI_DRAFT_VCPU_RUN_PROVIDER_PAUSE);
    g_assert_cmpuint(area->slots[0].completed_sequence, ==, 2);

    /* A transport error before provider claim may safely abort READY. */
    m->fail_at = m->calls + 1;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
        &state, &result), ==, -EIO);
    g_assert_cmpuint(area->slots[0].owner, ==, AMDVM_VM_SHARED_RUN_FREE);
    m->fail_at = 0;
    m->fail_after_shared_complete = true;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
        &state, &result), ==, -EIO);
    g_assert_cmpuint(area->slots[0].owner, ==, AMDVM_VM_SHARED_RUN_FREE);
    g_assert_cmpuint(area->slots[0].completed_sequence, ==, 4);
    m->fail_after_shared_complete = false;

    /* Once the provider claims a slot, uncertain completion must fail closed. */
    m->fail_after_shared_claim = true;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
        &state, &result), ==, -EIO);
    g_assert_cmpuint(area->slots[0].owner, ==, AMDVM_VM_SHARED_RUN_PROVIDER);
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, 0, 0, false, 0,
        &state, &result), ==, -EBUSY);
    g_assert_cmpuint(area->slots[0].owner, ==, AMDVM_VM_SHARED_RUN_PROVIDER);
    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
    g_free(m);
    g_free(area);
}

static void map_replace(void)
{
    uint8_t rom[4096] = { [0xff0] = 0xf4 };
    amdvm_vcpu_memory_range_t initial = {
        .guest_physical_address = 0,
        .user_virtual_address = 16ull << 30,
        .size = 1ull << 30,
        .flags = 7,
    };
    amdvm_vcpu_memory_range_t replacement = initial;
    replacement.user_virtual_address += 2ull << 30;
    Mock *m = g_new0(Mock, 1);
    AMDVMVMProvider p;

    g_assert_cmpint(amdsvm_vm_provider_init(&p, transport, m), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_map(&p, rom, &initial, 1), ==, 0);
    g_assert_cmpuint(p.generation, ==, 1);

    /* A live vCPU prevents replacement until it has been destroyed. */
    g_assert_cmpint(amdsvm_vm_provider_create(&p, 0, 0), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 7, NULL), ==, 0);
    g_assert_cmpuint(p.kick_generation[0], ==, 7);
    g_assert_cmpint(amdsvm_vm_provider_replace_map(&p, &replacement, 1), ==, -EBUSY);
    g_assert_cmpint(amdsvm_vm_provider_destroy_vcpu(&p, 0), ==, 0);
    g_assert_cmpuint(p.kick_generation[0], ==, 0);

    g_assert_cmpint(amdsvm_vm_provider_replace_map(&p, &replacement, 1), ==, 0);
    g_assert_cmpuint(p.generation, ==, 2);
    g_assert_cmpuint(m->generation, ==, 2);
    g_assert_cmpint(amdsvm_vm_provider_create(&p, 0, 0), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 1, NULL), ==, 0);
    g_assert_cmpuint(p.kick_generation[0], ==, 1);
    g_assert_cmpint(amdsvm_vm_provider_destroy_vcpu(&p, 0), ==, 0);
    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
    g_assert_false(m->map);
    g_free(m);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/amdsvm/vm-provider/shared-map-four-cpus-faults", exercise);
    g_test_add_func("/amdsvm/vm-provider/run-kick-destroy-lifetime", run_kick_lifetime);
    g_test_add_func("/amdsvm/vm-provider/kick-generation-reconcile", kick_generation_reconcile);
    g_test_add_func("/amdsvm/vm-provider/pause-stats-report-read-only",
                    pause_stats_report_is_read_only);
    g_test_add_func("/amdsvm/vm-provider/shared-run-handoff",
                    shared_run_handoff);
    g_test_add_func("/amdsvm/vm-provider/map-replace-after-vcpu-drain", map_replace);
    return g_test_run();
}
