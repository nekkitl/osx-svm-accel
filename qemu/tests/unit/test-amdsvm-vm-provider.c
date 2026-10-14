#include "qemu/osdep.h"
#include "../../accel/amdsvm/amdsvm-vm-provider.h"

typedef struct Mock {
    unsigned calls, fail_at;
    uint64_t generation;
    unsigned mask;
    bool map, rom;
    bool corrupt_map_result, corrupt_state_result;
    bool block_run, run_entered, release_run;
    GMutex run_mutex;
    GCond run_cond;
    uint64_t kick_generations[4];
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
        g_assert_cmpuint(r->flags, ==,
                         AMDVM_VM_ABI_DRAFT_VCPU_KICK_HOST_SIGNAL_SENT);
        g_assert_cmpuint(r->vcpu_id, <, 4);
        g_assert_true(m->mask & (1u << r->vcpu_id));
        g_assert_cmpuint(r->map_generation, ==, m->generation);
        g_assert_cmpuint(r->kick_generation, >=, m->kick_generations[r->vcpu_id]);
        m->kick_generations[r->vcpu_id] = r->kick_generation;
        *o = (amdvm_vm_abi_draft_vcpu_kick_result_t) {
            .abi_version = 2, .structure_size = sizeof(*o), .vcpu_id = r->vcpu_id,
            .map_generation = m->generation, .kick_generation = r->kick_generation,
        };
        break;
    }
    case AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN: {
        const amdvm_vm_abi_draft_vcpu_run_request_t *r = input;
        amdvm_vm_abi_draft_vcpu_run_result_t *o = output;
        g_assert_cmpuint(in_size, ==, sizeof(*r));
        g_assert_cmpuint(r->vcpu_id, <, 4);
        g_assert_true(m->mask & (1u << r->vcpu_id));
        g_assert_cmpuint(r->map_generation, ==, m->generation);
        g_assert_cmpuint(r->kick_generation, ==, m->kick_generations[r->vcpu_id]);
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
            .exit_code = 0x78, .state = r->state,
        };
        o->state.rip += 1;
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
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 1), ==, 0);
    uint32_t nmi = AMDVM_VCPU_EVENTINJ_VALID |
                   AMDVM_VCPU_EVENTINJ_TYPE_NMI | 2u;
    thread.state.interruptibility = AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED;
    unsigned int calls_before_nmi = m->calls;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, nmi, 0,
                    &thread.state, &thread.result), ==, -ENOTSUP);
    g_assert_cmpuint(m->calls, ==, calls_before_nmi);
    p.caps.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_NMI_EVENT_V1;
    g_assert_cmpint(amdsvm_vm_provider_run(&p, 0, 1, nmi, 0,
                    &thread.state, &thread.result), ==, 0);

    for (uint32_t id = 0; id < 4; ++id) {
        g_assert_cmpint(amdsvm_vm_provider_kick(&p, id, 1), ==, 0);
        amdvm_vm_abi_draft_vcpu_run_result_t result = {0};
        g_assert_cmpint(amdsvm_vm_provider_run(&p, id, 1, 0, 0,
                                                &thread.state, &result), ==, 0);
        g_assert_cmpuint(result.exit_code, ==, 0x78);
        g_assert_cmpuint(result.state.rip, ==, thread.state.rip + 1);
        g_assert_cmpint(amdsvm_vm_provider_kick(&p, id, 1), ==, 0);
    }

    m->block_run = true;
    worker = g_thread_new("amdsvm-run", run_thread, &thread);
    g_mutex_lock(&m->run_mutex);
    while (!m->run_entered) {
        g_cond_wait(&m->run_cond, &m->run_mutex);
    }
    g_mutex_unlock(&m->run_mutex);

    /* QEMU's host-thread signal is paired with a concurrent generation latch. */
    g_assert_cmpint(amdsvm_vm_provider_kick(&p, 0, 2), ==, 0);
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
                                            &thread.state, &thread.result), ==, -EAGAIN);

    g_cond_clear(&m->run_cond);
    g_mutex_clear(&m->run_mutex);
    g_free(m);
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
    g_assert_cmpint(amdsvm_vm_provider_replace_map(&p, &replacement, 1), ==, -EBUSY);
    g_assert_cmpint(amdsvm_vm_provider_destroy_vcpu(&p, 0), ==, 0);

    g_assert_cmpint(amdsvm_vm_provider_replace_map(&p, &replacement, 1), ==, 0);
    g_assert_cmpuint(p.generation, ==, 2);
    g_assert_cmpuint(m->generation, ==, 2);
    g_assert_cmpint(amdsvm_vm_provider_destroy(&p), ==, 0);
    g_assert_false(m->map);
    g_free(m);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/amdsvm/vm-provider/shared-map-four-cpus-faults", exercise);
    g_test_add_func("/amdsvm/vm-provider/run-kick-destroy-lifetime", run_kick_lifetime);
    g_test_add_func("/amdsvm/vm-provider/map-replace-after-vcpu-drain", map_replace);
    return g_test_run();
}
