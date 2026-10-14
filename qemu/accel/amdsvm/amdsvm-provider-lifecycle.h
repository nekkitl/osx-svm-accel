/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_AMDSVM_PROVIDER_LIFECYCLE_H
#define QEMU_AMDSVM_PROVIDER_LIFECYCLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../svm-kext/amdvm_probe_abi.h"

typedef int (*AMDVMProviderTransport)(void *opaque, uint32_t selector,
                                      const void *input, size_t input_size,
                                      void *output, size_t *output_size);

/* Thread-confined to the one QEMU vCPU thread; destroy is after RUN returns. */
typedef struct AMDVMProviderLifecycle {
    AMDVMProviderTransport transport;
    void *opaque;
    bool context_may_exist;
    bool context_ready;
    bool run_active;
} AMDVMProviderLifecycle;

void amdsvm_provider_lifecycle_init(AMDVMProviderLifecycle *provider,
                                    AMDVMProviderTransport transport,
                                    void *opaque);
int amdsvm_provider_prepare(AMDVMProviderLifecycle *provider,
                            const amdvm_vcpu_create_request_t *create,
                            const amdvm_vcpu_set_reset_rom_request_t *rom,
                            const amdvm_vcpu_memory_map_request_t *memory_map,
                            uint32_t expected_range_count,
                            amdvm_vcpu_memory_map_result_t *map_result);
/* Create and install a reset ROM without pinning guest RAM; diagnostics only. */
int amdsvm_provider_prepare_rom_only(
    AMDVMProviderLifecycle *provider,
    const amdvm_vcpu_create_request_t *create,
    const amdvm_vcpu_set_reset_rom_request_t *rom);
int amdsvm_provider_run(AMDVMProviderLifecycle *provider,
                        const amdvm_vcpu_run_request_t *request,
                        amdvm_vcpu_run_result_t *result);
int amdsvm_provider_destroy(AMDVMProviderLifecycle *provider);

#endif
