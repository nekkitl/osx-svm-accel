/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "amdsvm-provider-lifecycle.h"

static int amdsvm_provider_call_lifecycle(AMDVMProviderLifecycle *provider,
                                          uint32_t selector,
                                          const void *input,
                                          size_t input_size,
                                          amdvm_vcpu_lifecycle_result_t *result)
{
    size_t output_size = sizeof(*result);

    memset(result, 0, sizeof(*result));
    if (provider->transport(provider->opaque, selector, input, input_size,
                            result, &output_size) < 0 ||
        output_size != sizeof(*result) ||
        result->abi_version != AMDVM_VCPU_ABI_VERSION ||
        result->context_status != 0) {
        return -EIO;
    }
    return 0;
}

void amdsvm_provider_lifecycle_init(AMDVMProviderLifecycle *provider,
                                    AMDVMProviderTransport transport,
                                    void *opaque)
{
    memset(provider, 0, sizeof(*provider));
    provider->transport = transport;
    provider->opaque = opaque;
}

int amdsvm_provider_prepare(AMDVMProviderLifecycle *provider,
                            const amdvm_vcpu_create_request_t *create,
                            const amdvm_vcpu_set_reset_rom_request_t *rom,
                            const amdvm_vcpu_memory_map_request_t *memory_map,
                            uint32_t expected_range_count,
                            amdvm_vcpu_memory_map_result_t *map_result)
{
    amdvm_vcpu_lifecycle_result_t lifecycle;
    size_t output_size;

    if (!provider || !provider->transport || !create || !rom ||
        !memory_map || !map_result || expected_range_count == 0) {
        return -EINVAL;
    }
    if (provider->context_ready) {
        return 0;
    }
    if (provider->context_may_exist || provider->run_active) {
        return -EBUSY;
    }

    /* An IOKit error can be returned after the operation reached the driver. */
    provider->context_may_exist = true;
    if (amdsvm_provider_call_lifecycle(provider,
            AMDVM_PROBE_SELECTOR_VCPU_CREATE, create, sizeof(*create),
            &lifecycle) < 0) {
        return -EIO;
    }
    if (amdsvm_provider_call_lifecycle(provider,
            AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM, rom, sizeof(*rom),
            &lifecycle) < 0) {
        return -EIO;
    }

    memset(map_result, 0, sizeof(*map_result));
    output_size = sizeof(*map_result);
    if (provider->transport(provider->opaque,
            AMDVM_PROBE_SELECTOR_VCPU_MAP_MEMORY, memory_map,
            sizeof(*memory_map), map_result, &output_size) < 0 ||
        output_size != sizeof(*map_result) ||
        map_result->abi_version != AMDVM_VCPU_ABI_VERSION ||
        map_result->context_status != 0 ||
        map_result->range_count != expected_range_count ||
        map_result->guest_page_count == 0 || map_result->npt_page_count == 0) {
        return -EIO;
    }
    provider->context_ready = true;
    return 0;
}

int amdsvm_provider_prepare_rom_only(
    AMDVMProviderLifecycle *provider,
    const amdvm_vcpu_create_request_t *create,
    const amdvm_vcpu_set_reset_rom_request_t *rom)
{
    amdvm_vcpu_lifecycle_result_t lifecycle;

    if (!provider || !provider->transport || !create || !rom) {
        return -EINVAL;
    }
    if (provider->context_ready || provider->context_may_exist ||
        provider->run_active) {
        return -EBUSY;
    }

    /* An IOKit error can follow a completed create; keep destroy available. */
    provider->context_may_exist = true;
    if (amdsvm_provider_call_lifecycle(provider,
            AMDVM_PROBE_SELECTOR_VCPU_CREATE, create, sizeof(*create),
            &lifecycle) < 0 ||
        amdsvm_provider_call_lifecycle(provider,
            AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM, rom, sizeof(*rom),
            &lifecycle) < 0) {
        return -EIO;
    }
    return 0;
}

int amdsvm_provider_run(AMDVMProviderLifecycle *provider,
                        const amdvm_vcpu_run_request_t *request,
                        amdvm_vcpu_run_result_t *result)
{
    size_t output_size;
    int ret;

    if (!provider || !provider->transport || !request || !result ||
        !provider->context_ready) {
        return -EINVAL;
    }
    if (provider->run_active) {
        return -EBUSY;
    }

    provider->run_active = true;
    memset(result, 0, sizeof(*result));
    output_size = sizeof(*result);
    ret = provider->transport(provider->opaque,
        AMDVM_PROBE_SELECTOR_VCPU_RUN, request, sizeof(*request),
        result, &output_size);
    provider->run_active = false;
    if (ret < 0 || output_size != sizeof(*result)) {
        return -EIO;
    }
    return 0;
}

int amdsvm_provider_destroy(AMDVMProviderLifecycle *provider)
{
    amdvm_vcpu_lifecycle_result_t lifecycle;

    if (!provider || !provider->transport) {
        return -EINVAL;
    }
    if (provider->run_active) {
        return -EBUSY;
    }
    if (!provider->context_may_exist) {
        return 0;
    }
    if (amdsvm_provider_call_lifecycle(provider,
            AMDVM_PROBE_SELECTOR_VCPU_DESTROY, NULL, 0, &lifecycle) < 0) {
        return -EIO;
    }
    provider->context_may_exist = false;
    provider->context_ready = false;
    return 0;
}
