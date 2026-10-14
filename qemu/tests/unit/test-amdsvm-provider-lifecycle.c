/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "../../accel/amdsvm/amdsvm-provider-lifecycle.h"

typedef struct MockProvider {
    AMDVMProviderLifecycle *lifecycle;
    uint32_t selectors[16];
    size_t selector_count;
    unsigned int run_count;
    bool inject_event_seen;
    bool reentrant_destroy_busy;
    bool fail_next_destroy;
    int fail_map_status;
} MockProvider;

static int mock_transport(void *opaque, uint32_t selector,
                          const void *input, size_t input_size,
                          void *output, size_t *output_size)
{
    MockProvider *mock = opaque;

    g_assert_cmpuint(mock->selector_count, <, G_N_ELEMENTS(mock->selectors));
    mock->selectors[mock->selector_count++] = selector;
    switch (selector) {
    case AMDVM_PROBE_SELECTOR_VCPU_CREATE:
    case AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM: {
        amdvm_vcpu_lifecycle_result_t *result = output;
        g_assert_nonnull(input);
        g_assert_cmpuint(input_size, >, 0);
        g_assert_cmpuint(*output_size, ==, sizeof(*result));
        memset(result, 0, sizeof(*result));
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        *output_size = sizeof(*result);
        return 0;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_MAP_MEMORY: {
        amdvm_vcpu_memory_map_result_t *result = output;
        g_assert_nonnull(input);
        g_assert_cmpuint(input_size, ==,
                         sizeof(amdvm_vcpu_memory_map_request_t));
        g_assert_cmpuint(*output_size, ==, sizeof(*result));
        memset(result, 0, sizeof(*result));
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        result->range_count = 1;
        result->guest_page_count = 2;
        result->npt_page_count = 1;
        if (mock->fail_map_status != 0) {
            result->context_status = mock->fail_map_status;
        }
        *output_size = sizeof(*result);
        return 0;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_RUN: {
        const amdvm_vcpu_run_request_t *request = input;
        amdvm_vcpu_run_result_t *result = output;
        g_assert_nonnull(request);
        g_assert_cmpuint(input_size, ==, sizeof(*request));
        g_assert_cmpuint(*output_size, ==, sizeof(*result));
        if (mock->lifecycle) {
            mock->reentrant_destroy_busy =
                amdsvm_provider_destroy(mock->lifecycle) == -EBUSY;
        }
        memset(result, 0, sizeof(*result));
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        result->flags = AMDVM_VCPU_RUN_CALLED;
        result->state = request->state;
        if (mock->run_count++ == 0) {
            result->exit_code = 0x78;
        } else {
            g_assert_cmpuint(request->event_injection, !=, 0);
            mock->inject_event_seen = true;
            result->exit_code = AMDVM_VCPU_EXPECTED_CPUID_EXIT;
        }
        *output_size = sizeof(*result);
        return 0;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_DESTROY: {
        amdvm_vcpu_lifecycle_result_t *result = output;
        g_assert_null(input);
        g_assert_cmpuint(input_size, ==, 0);
        g_assert_cmpuint(*output_size, ==, sizeof(*result));
        memset(result, 0, sizeof(*result));
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        if (mock->fail_next_destroy) {
            mock->fail_next_destroy = false;
            result->context_status = -EIO;
        }
        *output_size = sizeof(*result);
        return 0;
    }
    default:
        g_assert_not_reached();
    }
}

static void test_persistent_run_and_deferred_destroy(void)
{
    AMDVMProviderLifecycle lifecycle;
    MockProvider mock = { 0 };
    amdvm_vcpu_create_request_t create = { 0 };
    amdvm_vcpu_set_reset_rom_request_t rom = { 0 };
    amdvm_vcpu_memory_map_request_t map = { 0 };
    amdvm_vcpu_memory_map_result_t map_result = { 0 };
    amdvm_vcpu_run_request_t run = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const uint32_t expected[] = {
        AMDVM_PROBE_SELECTOR_VCPU_CREATE,
        AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM,
        AMDVM_PROBE_SELECTOR_VCPU_MAP_MEMORY,
        AMDVM_PROBE_SELECTOR_VCPU_RUN,
        AMDVM_PROBE_SELECTOR_VCPU_RUN,
        AMDVM_PROBE_SELECTOR_VCPU_DESTROY,
        AMDVM_PROBE_SELECTOR_VCPU_DESTROY,
    };

    amdsvm_provider_lifecycle_init(&lifecycle, mock_transport, &mock);
    mock.lifecycle = &lifecycle;
    create.abi_version = AMDVM_VCPU_ABI_VERSION;
    map.abi_version = AMDVM_VCPU_ABI_VERSION;
    map.range_count = 1;

    g_assert_cmpint(amdsvm_provider_prepare(&lifecycle, &create, &rom,
                  &map, 1, &map_result), ==, 0);
    g_assert_cmpint(amdsvm_provider_prepare(&lifecycle, &create, &rom,
                  &map, 1, &map_result), ==, 0);
    g_assert_true(lifecycle.context_may_exist);
    g_assert_true(lifecycle.context_ready);
    g_assert_cmpuint(mock.selector_count, ==, 3);

    run.abi_version = AMDVM_VCPU_ABI_VERSION;
    run.state.rip = 0xfffffff0;
    g_assert_cmpint(amdsvm_provider_run(&lifecycle, &run, &result), ==, 0);
    g_assert_cmpuint(result.exit_code, ==, 0x78);
    run.event_injection = 1;
    g_assert_cmpint(amdsvm_provider_run(&lifecycle, &run, &result), ==, 0);
    g_assert_cmpuint(result.exit_code, ==, AMDVM_VCPU_EXPECTED_CPUID_EXIT);
    g_assert_true(mock.inject_event_seen);
    g_assert_true(mock.reentrant_destroy_busy);

    mock.fail_next_destroy = true;
    g_assert_cmpint(amdsvm_provider_destroy(&lifecycle), ==, -EIO);
    g_assert_true(lifecycle.context_may_exist);
    g_assert_true(lifecycle.context_ready);
    g_assert_cmpint(amdsvm_provider_destroy(&lifecycle), ==, 0);
    g_assert_false(lifecycle.context_may_exist);
    g_assert_false(lifecycle.context_ready);
    g_assert_cmpuint(mock.selector_count, ==, G_N_ELEMENTS(expected));
    g_assert_cmpmem(mock.selectors, sizeof(expected), expected,
                    sizeof(expected));
}

static void test_ram_preparation_without_run(void)
{
    for (unsigned fail_map = 0; fail_map < 2; ++fail_map) {
        AMDVMProviderLifecycle lifecycle;
        MockProvider mock = { .fail_map_status = fail_map ? -14 : 0 };
        amdvm_vcpu_create_request_t create = { 0 };
        amdvm_vcpu_set_reset_rom_request_t rom = { 0 };
        amdvm_vcpu_memory_map_request_t map = { 0 };
        amdvm_vcpu_memory_map_result_t result = { 0 };
        const uint32_t expected[] = {
            AMDVM_PROBE_SELECTOR_VCPU_CREATE,
            AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM,
            AMDVM_PROBE_SELECTOR_VCPU_MAP_MEMORY,
            AMDVM_PROBE_SELECTOR_VCPU_DESTROY,
        };

        amdsvm_provider_lifecycle_init(&lifecycle, mock_transport, &mock);
        g_assert_cmpint(amdsvm_provider_prepare(&lifecycle, &create, &rom,
                       &map, 1, &result), ==, fail_map ? -EIO : 0);
        g_assert_true(lifecycle.context_may_exist);
        g_assert_cmpint(amdsvm_provider_destroy(&lifecycle), ==, 0);
        g_assert_false(lifecycle.context_may_exist);
        g_assert_false(lifecycle.context_ready);
        g_assert_cmpuint(mock.run_count, ==, 0);
        g_assert_cmpuint(mock.selector_count, ==, G_N_ELEMENTS(expected));
        g_assert_cmpmem(mock.selectors, sizeof(expected), expected,
                       sizeof(expected));
    }
}

static void test_ram_map_diagnostic_status_preserved(void)
{
    const int map_statuses[] = { -16, -17, -18 };

    for (size_t i = 0; i < G_N_ELEMENTS(map_statuses); ++i) {
        AMDVMProviderLifecycle lifecycle;
        MockProvider mock = { .fail_map_status = map_statuses[i] };
        amdvm_vcpu_create_request_t create = { 0 };
        amdvm_vcpu_set_reset_rom_request_t rom = { 0 };
        amdvm_vcpu_memory_map_request_t map = { 0 };
        amdvm_vcpu_memory_map_result_t result = { 0 };

        amdsvm_provider_lifecycle_init(&lifecycle, mock_transport, &mock);
        g_assert_cmpint(amdsvm_provider_prepare(&lifecycle, &create, &rom,
                       &map, 1, &result), ==, -EIO);
        g_assert_cmpint(result.context_status, ==, map_statuses[i]);
        g_assert_true(lifecycle.context_may_exist);
        g_assert_false(lifecycle.context_ready);
        g_assert_cmpuint(mock.run_count, ==, 0);
        g_assert_cmpint(amdsvm_provider_destroy(&lifecycle), ==, 0);
        g_assert_false(lifecycle.context_may_exist);
        g_assert_false(lifecycle.context_ready);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/amdsvm/provider/persistent-run-deferred-destroy",
                    test_persistent_run_and_deferred_destroy);
    g_test_add_func("/amdsvm/provider/ram-preparation-without-run",
                    test_ram_preparation_without_run);
    g_test_add_func("/amdsvm/provider/ram-map-diagnostic-status-preserved",
                    test_ram_map_diagnostic_status_preserved);
    return g_test_run();
}
