/* Local userspace-only integration fixture. Never communicates with AMDVM kext. */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../svm-kext/amdvm_probe_abi.h"

#define MOCK_SERVICE ((io_service_t)0xf001u)
#define MOCK_CONNECTION ((io_connect_t)0xf002u)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int context_alive;
static uint32_t profile;
static uint64_t guest_pages;
static uint8_t rom[AMDVM_VCPU_RESET_ROM_SIZE];

static int fail_at(const char *stage)
{
    const char *requested = getenv("AMDVM_MOCK_FAIL");
    return requested && strcmp(requested, stage) == 0;
}

static io_service_t mock_matching(mach_port_t port, CFDictionaryRef matching)
{
    CFTypeRef value = matching ? CFDictionaryGetValue(matching,
                                                    CFSTR("IOProviderClass")) : NULL;
    if (value && CFEqual(value, CFSTR("AMDVMProbeService"))) {
        CFRelease(matching);
        fprintf(stderr, "AMDVM_MOCK SERVICE\n");
        return MOCK_SERVICE;
    }
    io_service_t (*original)(mach_port_t, CFDictionaryRef) =
        dlsym(RTLD_NEXT, "IOServiceGetMatchingService");
    return original(port, matching);
}

static kern_return_t mock_open(io_service_t service, task_port_t task,
                               uint32_t type, io_connect_t *connection)
{
    if (service != MOCK_SERVICE) {
        kern_return_t (*original)(io_service_t, task_port_t, uint32_t, io_connect_t *) =
            dlsym(RTLD_NEXT, "IOServiceOpen");
        return original(service, task, type, connection);
    }
    *connection = MOCK_CONNECTION;
    fprintf(stderr, "AMDVM_MOCK OPEN\n");
    return kIOReturnSuccess;
}

static kern_return_t mock_release(io_object_t object)
{
    if (object == MOCK_SERVICE) return kIOReturnSuccess;
    kern_return_t (*original)(io_object_t) = dlsym(RTLD_NEXT, "IOObjectRelease");
    return original(object);
}

static kern_return_t mock_close(io_connect_t connection)
{
    if (connection != MOCK_CONNECTION) {
        kern_return_t (*original)(io_connect_t) = dlsym(RTLD_NEXT, "IOServiceClose");
        return original(connection);
    }
    fprintf(stderr, "AMDVM_MOCK CLOSE alive=%d\n", context_alive);
    return kIOReturnSuccess;
}

#define OUTPUT(type) \
    if (!output || !output_size || *output_size < sizeof(type)) abort(); \
    *output_size = sizeof(type); memset(output, 0, sizeof(type)); \
    type *result = output

static kern_return_t mock_call(io_connect_t connection, uint32_t selector,
                               const void *input, size_t input_size,
                               void *output, size_t *output_size)
{
    if (connection != MOCK_CONNECTION) {
        kern_return_t (*original)(io_connect_t, uint32_t, const void *, size_t,
                                  void *, size_t *) =
            dlsym(RTLD_NEXT, "IOConnectCallStructMethod");
        return original(connection, selector, input, input_size, output, output_size);
    }
    pthread_mutex_lock(&lock);
    if (selector != AMDVM_PROBE_SELECTOR_VCPU_KICK)
        fprintf(stderr, "AMDVM_MOCK SELECTOR=%u\n", selector);
    switch (selector) {
    case AMDVM_PROBE_SELECTOR_QUERY: {
        OUTPUT(amdvm_probe_result_t);
        result->abi_version = AMDVM_PROBE_ABI_VERSION;
        result->logical_cpus_seen = result->svm_cpuid_cpus =
            result->npt_cpuid_cpus = result->nrip_save_cpus =
            result->vm_cr_lock_cpus = 16;
        for (unsigned i = 0; i < 16; ++i) {
            result->cpus[i].cpu_index = i;
            result->cpus[i].flags = 0x117;
            result->cpus[i].vm_cr = 8;
            result->cpus[i].efer = 0xd01;
        }
        break;
    }
    case AMDVM_PROBE_SELECTOR_HLT_FPU_PREFLIGHT: {
        OUTPUT(amdvm_hlt_fpu_preflight_result_t);
        result->abi_version = AMDVM_HLT_FPU_PREFLIGHT_ABI_VERSION;
        result->flags = 0xff;
        break;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_CREATE: {
        if (!input || input_size != sizeof(amdvm_vcpu_create_request_t)) abort();
        const amdvm_vcpu_create_request_t *request = input;
        OUTPUT(amdvm_vcpu_lifecycle_result_t);
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        profile = request->guest_profile;
        context_alive = 1;
        break;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM: {
        if (!input || input_size != sizeof(amdvm_vcpu_set_reset_rom_request_t)) abort();
        const amdvm_vcpu_set_reset_rom_request_t *request = input;
        OUTPUT(amdvm_vcpu_lifecycle_result_t);
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        memcpy(rom, request->page, sizeof(rom));
        break;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_MAP_MEMORY: {
        if (!input || input_size != sizeof(amdvm_vcpu_memory_map_request_t)) abort();
        const amdvm_vcpu_memory_map_request_t *request = input;
        OUTPUT(amdvm_vcpu_memory_map_result_t);
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        for (unsigned i = 0; i < request->range_count; ++i)
            guest_pages += request->ranges[i].size / 4096;
        fprintf(stderr, "AMDVM_MOCK MAP ranges=%u guest_pages=%" PRIu64 "\n",
                request->range_count, guest_pages);
        result->guest_page_count = guest_pages;
        result->npt_page_count = 1;
        result->range_count = request->range_count;
        if (fail_at("map")) result->context_status = -14;
        break;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_DIAGNOSTIC: {
        OUTPUT(amdvm_vcpu_diagnostic_result_t);
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        result->guest_profile = profile;
        result->flags = 7;
        result->guest_physical_address = AMDVM_VCPU_RESET_ROM_GPA;
        result->rom_offset = AMDVM_VCPU_RESET_ROM_OFFSET;
        memcpy(result->entry_bytes, rom + result->rom_offset, 16);
        result->cs_selector = 0xf000;
        result->cs_attributes = 0x9b;
        result->cs_limit = 0xffff;
        result->cs_base = AMDVM_VCPU_RESET_CS_BASE;
        result->rip = AMDVM_VCPU_RESET_VECTOR_RIP;
        result->rflags = 2;
        result->cr0 = 0x60000010;
        result->efer = 1u << 12;
        break;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_CONTROL_DIAGNOSTIC: {
        OUTPUT(amdvm_vcpu_control_diagnostic_result_t);
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        result->flags = (1u << 0) | (1u << 2) | (1u << 3) |
            (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
            (1u << 8) | (1u << 9);
        result->intercept_cr_read = result->intercept_cr_write =
            result->intercept_dr_read = result->intercept_dr_write = UINT16_MAX;
        result->intercept_exceptions = 0;
        result->intercept_misc1 = (1u << 0) | (1u << 18) | (1u << 24) | (1u << 27);
        result->interrupt_control = (1u << 24);
        result->nested_paging_control = result->asid = result->tlb_control = 1;
        break;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_SET_MEMORY_TYPES: {
        OUTPUT(amdvm_vcpu_lifecycle_result_t);
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        result->reserved[0] = (uint32_t)(guest_pages + 1);
        if (fail_at("types")) result->context_status = -14;
        break;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_DESTROY: {
        OUTPUT(amdvm_vcpu_lifecycle_result_t);
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        context_alive = 0;
        break;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_KICK:
        break;
    case AMDVM_PROBE_SELECTOR_VCPU_RUN:
    case AMDVM_PROBE_SELECTOR_HLT_RUN:
        fprintf(stderr, "AMDVM_MOCK FORBIDDEN_RUN\n");
        abort();
    default:
        abort();
    }
    pthread_mutex_unlock(&lock);
    return kIOReturnSuccess;
}

#define INTERPOSE(replacement, original) \
    __attribute__((used)) static const struct { const void *replacement; const void *original; } \
    interpose_##original __attribute__((section("__DATA,__interpose"))) = \
        { (const void *)&replacement, (const void *)&original }
INTERPOSE(mock_matching, IOServiceGetMatchingService);
INTERPOSE(mock_open, IOServiceOpen);
INTERPOSE(mock_release, IOObjectRelease);
INTERPOSE(mock_close, IOServiceClose);
INTERPOSE(mock_call, IOConnectCallStructMethod);

__attribute__((destructor)) static void fixture_end(void)
{
    fprintf(stderr, "AMDVM_MOCK END alive=%d\n", context_alive);
}
