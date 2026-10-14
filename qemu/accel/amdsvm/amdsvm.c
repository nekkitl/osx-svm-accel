/*
 * Local AMD SVM provider integration for QEMU.
 *
 * This is an experimental, non-upstream integration. The legacy execution
 * path remains a single-vCPU reset smoke; the opt-in VM-scoped path is being
 * developed for standard QEMU RAM/topology settings and is not yet a
 * validated general-purpose accelerator.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/accel.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "accel/accel-ops.h"
#include "accel/accel-cpu-ops.h"
#include "accel/dummy-cpus.h"
#include "exec/cpu-common.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "system/cpus.h"
#include "system/ioport.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "qemu/guest-random.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../../svm-kext/amdvm_probe_abi.h"
#include "amdsvm-firmware-dispatch.h"
#include "amdsvm-provider-lifecycle.h"
#include "amdsvm-vm-provider.h"
#include "../../target/i386/amdsvm/amdsvm-memory.h"
#include "../../target/i386/amdsvm/amdsvm-state.h"
#include "../../target/i386/svm.h"

#define AMDSVM_CONTEXT_VMRUN_FAILED (-11)
#define AMDSVM_VMRUN_FPU_NOT_READY (-6)
#define AMDSVM_PREENTRY_FPU_RETRY_LIMIT 3u
#define AMDSVM_VMEXIT_TRACE_INITIAL_LIMIT 4096u
#define AMDSVM_VMEXIT_TRACE_SAMPLE_MASK 0xfffu

bool amdsvm_allowed = true;
static unsigned amdsvm_firmware_pci_io_diag_count;
static unsigned amdsvm_firmware_vga_io_diag_count;

typedef struct AMDVMAccelState AMDVMAccelState;

typedef struct AMDVMHostTLBReaderArgs {
    AMDVMAccelState *state;
    unsigned index;
} AMDVMHostTLBReaderArgs;

typedef struct AMDVMFaultMemorySection {
    uint64_t gpa;
    uint64_t range_gpa;
    uint64_t hva;
    uint64_t size;
    const char *name;
    bool found;
    bool is_ram;
    bool is_rom;
} AMDVMFaultMemorySection;

static bool amdsvm_find_fault_memory_section(Int128 start, Int128 len,
                                             const MemoryRegion *mr,
                                             hwaddr offset_in_region,
                                             void *opaque)
{
    AMDVMFaultMemorySection *section = opaque;
    uint64_t gpa, size, ram_offset;
    void *ram_ptr;

    if (int128_gethi(start) != 0 || int128_gethi(len) != 0 ||
        int128_getlo(len) == 0) {
        return false;
    }
    gpa = int128_get64(start);
    size = int128_get64(len);
    if (section->gpa < gpa || section->gpa - gpa >= size) {
        return false;
    }
    section->found = true;
    section->range_gpa = gpa;
    section->size = size;
    section->name = memory_region_name(mr);
    section->is_ram = memory_region_is_ram(mr);
    section->is_rom = memory_region_is_rom(mr);
    if (!section->is_ram || offset_in_region > UINT64_MAX -
            (section->gpa - gpa)) {
        return true;
    }
    ram_offset = offset_in_region + (section->gpa - gpa);
    ram_ptr = memory_region_get_ram_ptr((MemoryRegion *)mr);
    if (ram_ptr && (uintptr_t)ram_ptr <= UINT64_MAX - ram_offset) {
        section->hva = (uint64_t)(uintptr_t)ram_ptr + ram_offset;
    }
    return true;
}

static AMDVMFaultMemorySection amdsvm_current_memory_section(uint64_t gpa)
{
    AMDVMFaultMemorySection section = { .gpa = gpa };
    FlatView *flatview = address_space_to_flatview(&address_space_memory);

    flatview_for_each_range(flatview, amdsvm_find_fault_memory_section,
                            &section);
    return section;
}

/* Narrow bring-up trace: establish whether LAPIC MMIO is guest RAM at the
 * exact point when the provider receives its pinned RAM map. */
static void amdsvm_trace_smp_memory_map(const char *stage,
                                       const AMDVMGuestMemoryMap *map)
{
    AMDVMFaultMemorySection apic = amdsvm_current_memory_section(
        UINT64_C(0xfee00310));

    error_report("AMDVM_SMP_MAP stage=%s ranges=%u bytes=0x%llx apic_found=%u apic_region=%s apic_ram=%u apic_rom=%u apic_range=0x%llx+0x%llx",
                 stage, map ? map->count : 0,
                 (unsigned long long)(map ? map->total_ram : 0),
                 apic.found, apic.name ? apic.name : "<unmapped>",
                 apic.is_ram, apic.is_rom,
                 (unsigned long long)apic.range_gpa,
                 (unsigned long long)apic.size);
    if (!map) {
        return;
    }
    for (uint32_t i = 0; i < map->count; ++i) {
        const AMDVMGuestMemoryRange *range = &map->ranges[i];
        error_report("AMDVM_SMP_MAP_RANGE stage=%s index=%u gpa=0x%llx size=0x%llx flags=0x%x hva=0x%llx",
                     stage, i,
                     (unsigned long long)range->guest_physical_address,
                     (unsigned long long)range->size, range->flags,
                     (unsigned long long)range->host_virtual_address);
    }
}

struct AMDVMAccelState {
    AccelState parent_obj;
    bool confirm_no_kdp_watchpoints;
    bool confirm_unbounded_vmrun;
    bool confirm_finite_kick_test;
    bool confirm_finite_smp_test;
    bool confirm_experimental_run;
    bool finite_host_tlb_stress;
    bool trace_vmexit;
    bool diagnostic_only;
    bool diagnostic_finished;
    bool diagnostic_validate_ram_map;
    bool validate_finite_smp_rom;
    bool vm_api;
    bool vm_api_prepared;
    bool vm_api_prepare_failed;
    bool vm_api_fpu_preflight_done;
    unsigned vm_expected_cpus;
    AMDVMVMProvider vm_provider;
    uint64_t vm_kick_generation[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    char *debugcon;
    CharFrontend debugcon_frontend;
    bool debugcon_frontend_initialized;
    io_connect_t connection;
    AMDVMProviderLifecycle provider_lifecycle;
    bool provider_prepare_failed;
    bool guest_map_initialized;
    bool provider_halted[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    AMDVMGuestMemoryMap guest_map;
    uint64_t guest_page_count;
    bool memory_types_published[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    AMDVMMTRRState published_mtrr[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint64_t kick_generation;
    uint64_t trace_run_attempts;
    uint64_t trace_vcpu_run_attempts[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    QemuMutex kick_lock;
    bool kick_lock_initialized;
    QemuThread host_tlb_threads[16];
    AMDVMHostTLBReaderArgs host_tlb_reader_args[15];
    volatile uint8_t *host_tlb_page;
    size_t host_tlb_page_size;
    unsigned host_tlb_threads_started;
    int host_tlb_stop;
    int host_tlb_ready;
    int host_tlb_error;
    uint64_t host_tlb_mprotect_cycles;
    uint64_t host_tlb_reader_reads[15];
    bool host_tlb_stress_started;
    AMDVMEventState pending_events[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    amdvm_probe_result_t cpu_state_before;
};

static void *amdsvm_host_tlb_mprotect_thread(void *opaque)
{
    AMDVMAccelState *state = opaque;

    qatomic_inc(&state->host_tlb_ready);
    while (!qatomic_read(&state->host_tlb_stop)) {
        if (mprotect((void *)state->host_tlb_page, state->host_tlb_page_size,
                     PROT_READ) != 0 ||
            mprotect((void *)state->host_tlb_page, state->host_tlb_page_size,
                     PROT_READ | PROT_WRITE) != 0) {
            qatomic_set(&state->host_tlb_error, errno);
            break;
        }
        state->host_tlb_mprotect_cycles++;
    }
    return NULL;
}

static void *amdsvm_host_tlb_reader_thread(void *opaque)
{
    AMDVMHostTLBReaderArgs *arg = opaque;
    AMDVMAccelState *state = arg->state;
    uint64_t reads = 0;

    qatomic_inc(&state->host_tlb_ready);
    while (!qatomic_read(&state->host_tlb_stop)) {
        reads += *state->host_tlb_page;
    }
    qatomic_set(&state->host_tlb_reader_reads[arg->index], reads);
    return NULL;
}

static void amdsvm_host_tlb_stress_stop(AMDVMAccelState *state);

static int amdsvm_host_tlb_stress_start(AMDVMAccelState *state)
{
    long page_size;
    int64_t deadline;
    unsigned i;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return -EINVAL;
    }
    state->host_tlb_page_size = (size_t)page_size;
    state->host_tlb_page = mmap(NULL, state->host_tlb_page_size,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANON, -1, 0);
    if (state->host_tlb_page == MAP_FAILED) {
        state->host_tlb_page = NULL;
        return -errno;
    }
    *state->host_tlb_page = 1;
    qatomic_set(&state->host_tlb_stop, 0);
    qatomic_set(&state->host_tlb_ready, 0);
    qatomic_set(&state->host_tlb_error, 0);
    qatomic_set(&state->host_tlb_mprotect_cycles, 0);
    for (i = 0; i < G_N_ELEMENTS(state->host_tlb_reader_args); i++) {
        state->host_tlb_reader_args[i].state = state;
        state->host_tlb_reader_args[i].index = i;
        qemu_thread_create(&state->host_tlb_threads[i], "amdsvm-tlb-read",
                           amdsvm_host_tlb_reader_thread,
                           &state->host_tlb_reader_args[i],
                           QEMU_THREAD_JOINABLE);
        state->host_tlb_threads_started++;
    }
    qemu_thread_create(&state->host_tlb_threads[15], "amdsvm-tlb-protect",
                       amdsvm_host_tlb_mprotect_thread, state,
                       QEMU_THREAD_JOINABLE);
    state->host_tlb_threads_started++;
    state->host_tlb_stress_started = true;
    deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (qatomic_read(&state->host_tlb_ready) != 16 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    if (qatomic_read(&state->host_tlb_ready) != 16) {
        amdsvm_host_tlb_stress_stop(state);
        return -ETIMEDOUT;
    }
    return 0;
}

static void amdsvm_host_tlb_stress_stop(AMDVMAccelState *state)
{
    unsigned i;

    if (!state->host_tlb_stress_started) {
        return;
    }
    qatomic_set(&state->host_tlb_stop, 1);
    for (i = 0; i < state->host_tlb_threads_started; i++) {
        qemu_thread_join(&state->host_tlb_threads[i]);
    }
    error_report("AMDVM_HOST_TLB_STRESS mprotect_cycles=%llu error=%d",
                 (unsigned long long)state->host_tlb_mprotect_cycles,
                 qatomic_read(&state->host_tlb_error));
    for (i = 0; i < G_N_ELEMENTS(state->host_tlb_reader_reads); i++) {
        error_report("AMDVM_HOST_TLB_STRESS reader=%u reads=%llu", i,
                     (unsigned long long)state->host_tlb_reader_reads[i]);
    }
    state->host_tlb_stress_started = false;
    state->host_tlb_threads_started = 0;
    if (state->host_tlb_page) {
        munmap((void *)state->host_tlb_page, state->host_tlb_page_size);
        state->host_tlb_page = NULL;
    }
}

static AMDVMAccelState *amdsvm_active_state;

static char *amdsvm_get_debugcon(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return g_strdup(s->debugcon);
}

static void amdsvm_set_debugcon(Object *obj, const char *value, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    g_free(s->debugcon);
    s->debugcon = g_strdup(value);
}

static bool amdsvm_get_confirm_no_kdp_watchpoints(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->confirm_no_kdp_watchpoints;
}

static void amdsvm_set_confirm_no_kdp_watchpoints(Object *obj, bool value,
                                                   Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->confirm_no_kdp_watchpoints = value;
}

static bool amdsvm_get_confirm_unbounded_vmrun(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->confirm_unbounded_vmrun;
}

static void amdsvm_set_confirm_unbounded_vmrun(Object *obj, bool value,
                                                Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->confirm_unbounded_vmrun = value;
}

static bool amdsvm_get_confirm_finite_kick_test(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->confirm_finite_kick_test;
}

static void amdsvm_set_confirm_finite_kick_test(Object *obj, bool value,
                                                Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->confirm_finite_kick_test = value;
}

static bool amdsvm_get_confirm_finite_smp_test(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->confirm_finite_smp_test;
}

static void amdsvm_set_confirm_finite_smp_test(Object *obj, bool value,
                                               Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->confirm_finite_smp_test = value;
}

static bool amdsvm_get_confirm_experimental_run(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->confirm_experimental_run;
}

static void amdsvm_set_confirm_experimental_run(Object *obj, bool value,
                                          Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->confirm_experimental_run = value;
}

static bool amdsvm_get_finite_host_tlb_stress(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->finite_host_tlb_stress;
}

static void amdsvm_set_finite_host_tlb_stress(Object *obj, bool value,
                                              Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->finite_host_tlb_stress = value;
}

static bool amdsvm_get_trace_vmexit(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->trace_vmexit;
}

static void amdsvm_set_trace_vmexit(Object *obj, bool value, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->trace_vmexit = value;
}

static bool amdsvm_get_diagnostic_only(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->diagnostic_only;
}

static void amdsvm_set_diagnostic_only(Object *obj, bool value, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->diagnostic_only = value;
}

static bool amdsvm_get_diagnostic_validate_ram_map(Object *obj,
                                                    Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->diagnostic_validate_ram_map;
}

static void amdsvm_set_diagnostic_validate_ram_map(Object *obj, bool value,
                                                    Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->diagnostic_validate_ram_map = value;
}

static bool amdsvm_get_validate_finite_smp_rom(Object *obj, Error **errp)
{
    (void)errp;
    return ((AMDVMAccelState *)obj)->validate_finite_smp_rom;
}

static void amdsvm_set_validate_finite_smp_rom(Object *obj, bool value,
                                                Error **errp)
{
    (void)errp;
    ((AMDVMAccelState *)obj)->validate_finite_smp_rom = value;
}

static bool amdsvm_get_vm_api(Object *obj, Error **errp)
{
    return ((AMDVMAccelState *)obj)->vm_api;
}

static void amdsvm_set_vm_api(Object *obj, bool value, Error **errp)
{
    ((AMDVMAccelState *)obj)->vm_api = value;
}

static int amdsvm_call(io_connect_t connection, uint32_t selector,
                       const void *input, size_t input_size,
                       void *output, size_t *output_size)
{
    size_t output_capacity = output_size ? *output_size : 0;
    IOReturn status = IOConnectCallStructMethod(connection, selector,
                                                 input, input_size,
                                                 output, output_size);
    if (status != kIOReturnSuccess) {
        error_report("AMD-SVM provider selector %u failed (IOKit 0x%08x, input %zu bytes, output capacity %zu bytes)",
                     selector, status, input_size, output_capacity);
        return -EIO;
    }
    return 0;
}

static bool amdsvm_reset_diagnostic_matches(
    const amdvm_vcpu_diagnostic_result_t *diag,
    const uint8_t expected[16], uint32_t expected_profile)
{
    char actual_hex[16 * 3 + 1] = { 0 };
    char expected_hex[16 * 3 + 1] = { 0 };
    size_t i;

    for (i = 0; i < 16; i++) {
        snprintf(actual_hex + i * 3, sizeof(actual_hex) - i * 3,
                 "%02x%s", diag->entry_bytes[i], i == 15 ? "" : " ");
        snprintf(expected_hex + i * 3, sizeof(expected_hex) - i * 3,
                 "%02x%s", expected[i], i == 15 ? "" : " ");
    }
    error_report("AMD-SVM pre-entry reset diagnostic: profile=%u GPA=0x%llx offset=0x%x bytes=[%s] CS=%04x attr=%04x limit=%08x base=0x%llx RIP=0x%llx RFLAGS=0x%llx CR0=0x%llx EFER=0x%llx flags=0x%x",
                 diag->guest_profile,
                 (unsigned long long)diag->guest_physical_address,
                 diag->rom_offset, actual_hex, diag->cs_selector,
                 diag->cs_attributes, diag->cs_limit,
                 (unsigned long long)diag->cs_base,
                 (unsigned long long)diag->rip,
                 (unsigned long long)diag->rflags,
                 (unsigned long long)diag->cr0,
                 (unsigned long long)diag->efer, diag->flags);
    if (diag->abi_version != AMDVM_VCPU_ABI_VERSION ||
        diag->context_status != 0 || diag->guest_profile != expected_profile ||
        diag->flags != (AMDVM_VCPU_DIAGNOSTIC_ROM_INSTALLED |
                        AMDVM_VCPU_DIAGNOSTIC_NPT_RX_VALID |
                        AMDVM_VCPU_DIAGNOSTIC_RESET_STATE) ||
        diag->guest_physical_address != AMDVM_VCPU_RESET_ROM_GPA ||
        diag->rom_offset != AMDVM_VCPU_RESET_ROM_OFFSET ||
        diag->cs_selector != 0xf000 || diag->cs_attributes != 0x009b ||
        diag->cs_limit != 0xffff ||
        diag->cs_base != AMDVM_VCPU_RESET_CS_BASE ||
        diag->rip != AMDVM_VCPU_RESET_VECTOR_RIP || diag->rflags != 2 ||
        diag->cr0 != 0x60000010ull || diag->efer != (1ull << 12) ||
        memcmp(diag->entry_bytes, expected, 16) != 0) {
        error_report("AMD-SVM pre-entry diagnostic mismatch; expected GPA=0x%llx offset=0x%x bytes=[%s] and the QEMU reset profile",
                     (unsigned long long)AMDVM_VCPU_RESET_ROM_GPA,
                     AMDVM_VCPU_RESET_ROM_OFFSET, expected_hex);
        return false;
    }
    return true;
}

/* Permit the early hardware stop experiment only with this exact ROM. */
static bool amdsvm_finite_kick_rom_matches(const uint8_t *reset_page)
{
    static const uint8_t payload_prefix[] = {
        0xb0, 0x4b, 0xe6, 0xe9, 0xfa, 0x66, 0xb9,
    };
    static const uint8_t payload_suffix[] = {
        0x66, 0x49, 0x75, 0xfc, 0xb0, 0x48, 0xe6, 0xe9, 0xf4,
    };
    static const uint8_t count_1m[] = { 0x40, 0x42, 0x0f, 0x00 };
    static const uint8_t count_5m[] = { 0x40, 0x4b, 0x4c, 0x00 };
    static const uint8_t count_25m[] = { 0x40, 0x78, 0x7d, 0x01 };
    static const uint8_t reset_jump[] = { 0xe9, 0x0d, 0xff };
    const uint8_t *payload;
    bool known_finite_count;

    if (!reset_page) {
        return false;
    }
    payload = reset_page + 0xf00;
    known_finite_count =
        memcmp(payload + sizeof(payload_prefix), count_1m,
               sizeof(count_1m)) == 0 ||
        memcmp(payload + sizeof(payload_prefix), count_5m,
               sizeof(count_5m)) == 0 ||
        memcmp(payload + sizeof(payload_prefix), count_25m,
               sizeof(count_25m)) == 0;
    return memcmp(payload, payload_prefix, sizeof(payload_prefix)) == 0 &&
        known_finite_count &&
        memcmp(payload + sizeof(payload_prefix) + sizeof(count_5m),
               payload_suffix, sizeof(payload_suffix)) == 0 &&
        memcmp(reset_page + AMDVM_VCPU_RESET_ROM_OFFSET,
               reset_jump, sizeof(reset_jump)) == 0;
}

static bool amdsvm_guest_map_read(const AMDVMGuestMemoryMap *map,
                                 uint64_t gpa, void *buffer, size_t length)
{
    uint64_t end;

    if (!map || !buffer || !length || gpa > UINT64_MAX - length) {
        return false;
    }
    end = gpa + length;
    if (map->reset_rom_page_present &&
        gpa >= AMDVM_VCPU_RESET_ROM_GPA &&
        end <= AMDVM_VCPU_RESET_ROM_GPA + AMDVM_VCPU_RESET_ROM_SIZE) {
        memcpy(buffer, map->reset_rom_page +
               (gpa - AMDVM_VCPU_RESET_ROM_GPA), length);
        return true;
    }
    for (uint32_t i = 0; i < map->count; i++) {
        const AMDVMGuestMemoryRange *range = &map->ranges[i];
        uint64_t range_end;

        if (!(range->flags & AMDSVM_GUEST_RANGE_READ) || !range->size ||
            range->guest_physical_address > UINT64_MAX - range->size ||
            range->host_virtual_address > UINT64_MAX - range->size) {
            continue;
        }
        range_end = range->guest_physical_address + range->size;
        if (gpa >= range->guest_physical_address && end <= range_end) {
            uintptr_t host = (uintptr_t)(range->host_virtual_address +
                              (gpa - range->guest_physical_address));
            if (host == 0 || host > UINTPTR_MAX - length) {
                return false;
            }
            memcpy(buffer, (const void *)host, length);
            return true;
        }
    }
    return false;
}

/* Exact finite ROM gate for the first two-vCPU/AP-start hardware smoke. */
static bool amdsvm_finite_smp_rom_matches(const AMDVMGuestMemoryMap *map)
{
    static const uint8_t bsp_payload[] = {
        0xb0, 0x42, 0xe6, 0xe9, 0xfa, 0xb8, 0x00, 0xf0, 0x8e, 0xd8,
        0x0f, 0x01, 0x16, 0x00, 0x01, 0x0f, 0x20, 0xc0, 0x66, 0x83,
        0xc8, 0x01, 0x0f, 0x22, 0xc0, 0x66, 0xea, 0x21, 0xff, 0x0f,
        0x00, 0x08, 0x00, 0x66, 0xb8, 0x10, 0x00, 0x8e, 0xd8, 0x8e,
        0xc0, 0x8e, 0xd0, 0xbc, 0x00, 0x80, 0x00, 0x00, 0xb8, 0x00,
        0x00, 0x00, 0x01, 0xa3, 0x10, 0x03, 0xe0, 0xfe, 0xb8, 0x00,
        0xc5, 0x00, 0x00, 0xa3, 0x00, 0x03, 0xe0, 0xfe, 0xb9,
        0x00, 0x20, 0x00, 0x00, 0x49, 0x75, 0xfd, 0xb8, 0x00,
        0x85, 0x00, 0x00, 0xa3, 0x00, 0x03, 0xe0, 0xfe, 0xb9,
        0x00, 0x20, 0x00, 0x00, 0x49, 0x75, 0xfd, 0xb8, 0xff,
        0x06, 0x00, 0x00, 0xa3, 0x00, 0x03, 0xe0, 0xfe, 0xb9,
        0x40, 0x78, 0x7d, 0x01, 0x49, 0x75, 0xfd, 0xb0, 0x48,
        0xe6, 0xe9, 0xf4,
    };
    static const uint8_t ap_payload[] = {
        0xb0, 0x41, 0xe6, 0xe9, 0xfa,
        0x66, 0xb9, 0x40, 0x78, 0x7d, 0x01, 0x66, 0x49, 0x75, 0xfc,
        0xb0, 0x61, 0xe6, 0xe9, 0xf4,
    };
    static const uint8_t gdtr_payload[] = {
        0x17, 0x00, 0x08, 0x01, 0x0f, 0x00,
    };
    static const uint8_t gdt_payload[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0x00, 0x00, 0x00, 0x9b, 0xcf, 0x00,
        0xff, 0xff, 0x00, 0x00, 0x00, 0x93, 0xcf, 0x00,
    };
    static const uint8_t reset_jump[] = { 0xe9, 0x0d, 0xff };
    uint8_t actual_bsp[sizeof(bsp_payload)];
    uint8_t actual_ap[sizeof(ap_payload)];
    uint8_t actual_gdtr[sizeof(gdtr_payload)];
    uint8_t actual_gdt[sizeof(gdt_payload)];

    if (!map || !map->reset_rom_page_present ||
        memcmp(map->reset_rom_page + AMDVM_VCPU_RESET_ROM_OFFSET,
               reset_jump, sizeof(reset_jump)) != 0) {
        return false;
    }
    /* CS.base + 0xff00 places the BSP at GPA 0xffffff00, in the high
     * reset-ROM alias captured by reset_rom_page at offset 0xf00. */
    memcpy(actual_bsp, map->reset_rom_page + 0xf00, sizeof(actual_bsp));
    return memcmp(actual_bsp, bsp_payload, sizeof(bsp_payload)) == 0 &&
        amdsvm_guest_map_read(map, 0x000ff000, actual_ap,
                              sizeof(actual_ap)) &&
        memcmp(actual_ap, ap_payload, sizeof(ap_payload)) == 0 &&
        amdsvm_guest_map_read(map, 0x000f0100, actual_gdtr,
                              sizeof(actual_gdtr)) &&
        memcmp(actual_gdtr, gdtr_payload, sizeof(gdtr_payload)) == 0 &&
        amdsvm_guest_map_read(map, 0x000f0108, actual_gdt,
                              sizeof(actual_gdt)) &&
        memcmp(actual_gdt, gdt_payload, sizeof(gdt_payload)) == 0;
}

static bool amdsvm_control_diagnostic_matches(
    const amdvm_vcpu_control_diagnostic_result_t *diag)
{
    const uint32_t expected_flags =
        AMDVM_VCPU_CONTROL_DIAG_CPUID_INTERCEPT |
        AMDVM_VCPU_CONTROL_DIAG_HLT_INTERCEPT |
        AMDVM_VCPU_CONTROL_DIAG_IOIO_INTERCEPT |
        AMDVM_VCPU_CONTROL_DIAG_INTR_INTERCEPT |
        AMDVM_VCPU_CONTROL_DIAG_NPT_ENABLED |
        AMDVM_VCPU_CONTROL_DIAG_NPT_ROOT_VALID |
        AMDVM_VCPU_CONTROL_DIAG_IOPM_VALID |
        AMDVM_VCPU_CONTROL_DIAG_MSRPM_VALID |
        AMDVM_VCPU_CONTROL_DIAG_V_INTR_MASKING;
    /* ABI 13 reset profile delivers guest exceptions through its own IDT. */
    const uint32_t expected_intercept_exceptions = 0;
    const uint32_t expected_misc1 = (1u << 0) | (1u << 18) |
        (1u << 24) | (1u << 27);
    const uint32_t expected_interrupt_control = (1u << 24);

    error_report("AMD-SVM pre-entry VMCB controls: flags=0x%x CR=%04x/%04x DR=%04x/%04x exceptions=%08x misc1=%08x misc2=%08x iret_intercept=%u intn_intercept=%u int_ctl=%08x nested_paging=%08x ASID=%u TLB_CONTROL=%u",
                 diag->flags, diag->intercept_cr_read,
                 diag->intercept_cr_write, diag->intercept_dr_read,
                 diag->intercept_dr_write, diag->intercept_exceptions,
                 diag->intercept_misc1, diag->intercept_misc2,
                 (diag->intercept_misc1 & (1u << 20)) != 0,
                 (diag->intercept_misc1 & (1u << 21)) != 0,
                 diag->interrupt_control,
                 diag->nested_paging_control, diag->asid,
                 diag->tlb_control);
    return diag->abi_version == AMDVM_VCPU_ABI_VERSION &&
        diag->context_status == 0 && diag->flags == expected_flags &&
        diag->intercept_cr_read == UINT16_MAX &&
        diag->intercept_cr_write == UINT16_MAX &&
        diag->intercept_dr_read == UINT16_MAX &&
        diag->intercept_dr_write == UINT16_MAX &&
        diag->intercept_exceptions == expected_intercept_exceptions &&
        (diag->intercept_misc1 & expected_misc1) == expected_misc1 &&
        (diag->intercept_misc1 & (1u << 20)) == 0 &&
        (diag->intercept_misc1 & (1u << 21)) == 0 &&
        diag->interrupt_control == expected_interrupt_control &&
        diag->nested_paging_control == 1 && diag->asid != 0 &&
        diag->tlb_control == 1;
}

static int amdsvm_read_reset_diagnostic(
    io_connect_t connection, const uint8_t expected[16],
    uint32_t expected_profile)
{
    amdvm_vcpu_diagnostic_result_t diag = { 0 };
    amdvm_vcpu_control_diagnostic_result_t controls = { 0 };
    size_t output_size = sizeof(diag);

    if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_VCPU_DIAGNOSTIC,
                    NULL, 0, &diag, &output_size) < 0 ||
        output_size != sizeof(diag)) {
        error_report("AMD-SVM read-only reset diagnostic call failed");
        return -EIO;
    }
    if (!amdsvm_reset_diagnostic_matches(&diag, expected,
                                         expected_profile)) {
        return -EPROTO;
    }
    output_size = sizeof(controls);
    if (amdsvm_call(connection,
                    AMDVM_PROBE_SELECTOR_VCPU_CONTROL_DIAGNOSTIC,
                    NULL, 0, &controls, &output_size) < 0 ||
        output_size != sizeof(controls) ||
        !amdsvm_control_diagnostic_matches(&controls)) {
        error_report("AMD-SVM read-only VMCB control diagnostic failed");
        return -EPROTO;
    }
    return 0;
}

static int amdsvm_iokit_provider_transport(void *opaque, uint32_t selector,
                                           const void *input,
                                           size_t input_size,
                                           void *output,
                                           size_t *output_size)
{
    AMDVMAccelState *state = opaque;

    return amdsvm_call(state->connection, selector, input, input_size,
                       output, output_size);
}

/* A probe connection selects legacy mode; never mix it with VM negotiation. */
static int amdsvm_probe_host_state(amdvm_probe_result_t *result)
{
    io_connect_t connection = IO_OBJECT_NULL;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("AMDVMProbeService"));
    size_t size = sizeof(*result);
    if (service == IO_OBJECT_NULL) { return -ENODEV; }
    IOReturn opened = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (opened) { return -EACCES; }
    int rc = amdsvm_call(connection, AMDVM_PROBE_SELECTOR_QUERY,
                         NULL, 0, result, &size);
    IOReturn closed = IOServiceClose(connection);
    return rc || closed || size != sizeof(*result) ||
        result->abi_version != AMDVM_PROBE_ABI_VERSION ? -EIO : 0;
}

/* FPU preflight is a legacy selector and must use its own legacy connection. */
static int amdsvm_vm_api_fpu_preflight(void)
{
    amdvm_hlt_fpu_preflight_result_t fpu = { 0 };
    io_connect_t connection = IO_OBJECT_NULL;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("AMDVMProbeService"));
    size_t output_size = sizeof(fpu);
    int rc = -EIO;

    if (service == IO_OBJECT_NULL) {
        return -ENODEV;
    }
    IOReturn opened = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (opened != kIOReturnSuccess) {
        return -EACCES;
    }
    if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_HLT_FPU_PREFLIGHT,
                    NULL, 0, &fpu, &output_size) == 0 &&
        output_size == sizeof(fpu) &&
        fpu.abi_version == AMDVM_HLT_FPU_PREFLIGHT_ABI_VERSION &&
        fpu.flags == 0xffu && fpu.init_status == 0 &&
        fpu.first_xsave_status == 0 && fpu.second_xsave_status == 0 &&
        fpu.destroy_status == 0) {
        rc = 0;
    } else {
        error_report("AMD-SVM VM API XNU FPU preflight failed (flags 0x%x init=%d first=%d second=%d destroy=%d cpu=%u/%u ts=%u/%u em=%u/%u xcr0=0x%llx)",
                     fpu.flags, fpu.init_status, fpu.first_xsave_status,
                     fpu.second_xsave_status, fpu.destroy_status,
                     fpu.cpu_before, fpu.cpu_after, fpu.ts_before,
                     fpu.ts_after, fpu.em_before, fpu.em_after,
                     (unsigned long long)fpu.xcr0);
    }
    IOReturn closed = IOServiceClose(connection);
    return closed == kIOReturnSuccess ? rc : -EIO;
}

static int amdsvm_destroy_vcpu_context(AMDVMAccelState *state)
{
    if (!state->provider_lifecycle.context_may_exist) {
        return 0;
    }
    if (amdsvm_provider_destroy(&state->provider_lifecycle) < 0) {
        error_report("AMD-SVM QEMU vCPU destroy failed; preserving provider connection");
        return -EIO;
    }
    state->guest_map_initialized = false;
    memset(state->provider_halted, 0, sizeof(state->provider_halted));
    state->guest_page_count = 0;
    memset(state->memory_types_published, 0,
           sizeof(state->memory_types_published));
    memset(&state->guest_map, 0, sizeof(state->guest_map));
    memset(state->published_mtrr, 0, sizeof(state->published_mtrr));
    return 0;
}

static void amdsvm_kick_vcpu_thread(CPUState *cpu)
{
    AMDVMAccelState *state = amdsvm_active_state;
    amdvm_vcpu_kick_request_t request = { 0 };
    IOReturn status = kIOReturnSuccess;
    unsigned int attempt;

    /* Wake QEMU's worker and request provider cancellation. This POSIX signal
     * is not itself proof that XNU will deliver a physical interrupt to a
     * processor currently executing VMRUN; that host-exit path is a separate
     * hardware safety gate. */
    cpus_kick_thread(cpu);
    if (!state || state->connection == IO_OBJECT_NULL) {
        return;
    }

    if (state->vm_api) {
        uint64_t generation;
        int rc;

        /* The SIG_IPI above wakes QEMU's worker. The provider records the
         * generation only; bounded physical VMEXIT delivery is not established
         * by this selector. */
        if (!amdsvm_vm_provider_supports_kick(&state->vm_provider) ||
            cpu->cpu_index < 0 ||
            cpu->cpu_index >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
            return;
        }
        qemu_mutex_lock(&state->kick_lock);
        generation = qatomic_read(
            &state->vm_kick_generation[cpu->cpu_index]) + 1;
        if (generation == 0) {
            generation = 1;
        }
        rc = amdsvm_vm_provider_kick(&state->vm_provider,
                                     cpu->cpu_index, generation);
        if (rc == -EAGAIN || rc == -ESHUTDOWN) {
            /* A QEMU kick can race VM preparation or final destroy. */
        } else if (rc) {
            error_report("AMD-SVM VM API failed to latch KICK generation %" PRIu64
                         " for vCPU %d (%d)", generation,
                         cpu->cpu_index, rc);
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
        } else {
            qatomic_set(&state->vm_kick_generation[cpu->cpu_index], generation);
        }
        qemu_mutex_unlock(&state->kick_lock);
        return;
    }

    qemu_mutex_lock(&state->kick_lock);
    request.abi_version = AMDVM_VCPU_ABI_VERSION;
    request.generation = qatomic_read(&state->kick_generation) + 1;
    if (request.generation == 0) {
        request.generation = 1;
    }
    /* The provider latches the generation before sending the IPI. Retry that
     * same generation on a bounded ICR/lock timeout; never mint a new kick. */
    for (attempt = 1; attempt <= 3; attempt++) {
        status = IOConnectCallStructMethod(state->connection,
                                          AMDVM_PROBE_SELECTOR_VCPU_KICK,
                                          &request, sizeof(request), NULL, NULL);
        if (status != kIOReturnTimeout || attempt == 3) {
            break;
        }
    }
    if (status != kIOReturnSuccess) {
        error_report("AMD-SVM provider failed vCPU kick generation %" PRIu64
                     " after %u attempt(s) (IOKit 0x%08x)",
                     request.generation, attempt, status);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
    } else {
        /* Publish only after the provider has latched this generation. */
        qatomic_set(&state->kick_generation, request.generation);
    }
    qemu_mutex_unlock(&state->kick_lock);
}

static bool amdsvm_hardware_available(const amdvm_probe_result_t *result)
{
    return result->abi_version == AMDVM_PROBE_ABI_VERSION &&
           result->logical_cpus_seen != 0 &&
           result->cpu_index_overflow == 0 &&
           result->logical_cpus_seen == result->svm_cpuid_cpus &&
           result->logical_cpus_seen == result->npt_cpuid_cpus &&
           result->logical_cpus_seen == result->nrip_save_cpus &&
           result->vm_cr_svmdis_cpus == 0 &&
           result->efer_svme_cpus == 0 &&
           result->hsave_nonzero_cpus == 0;
}

static bool amdsvm_cpu_state_restored(const amdvm_probe_result_t *before,
                                      const amdvm_probe_result_t *after)
{
    uint32_t i;

    if (before->logical_cpus_seen != after->logical_cpus_seen ||
        before->nrip_save_cpus != after->nrip_save_cpus ||
        before->cpu_index_overflow != after->cpu_index_overflow ||
        after->efer_svme_cpus != 0 || after->hsave_nonzero_cpus != 0) {
        return false;
    }
    for (i = 0; i < before->logical_cpus_seen; i++) {
        const amdvm_probe_cpu_state_t *a = &before->cpus[i];
        const amdvm_probe_cpu_state_t *b = &after->cpus[i];

        if (a->cpu_index != b->cpu_index || a->flags != b->flags ||
            a->vm_cr != b->vm_cr || a->efer != b->efer ||
            a->hsave_pa != b->hsave_pa) {
            return false;
        }
    }
    return true;
}

static int amdsvm_run_microguest(io_connect_t connection,
                                 const char *debugcon_id,
                                 bool diagnostic_only)
{
    static const uint8_t guest_code[AMDVM_VCPU_TEST_ROM_SIZE] = {
        AMDVM_VCPU_TEST_ROM_CPUID_0,
        AMDVM_VCPU_TEST_ROM_CPUID_1,
        AMDVM_VCPU_TEST_ROM_MOV_AL_IMM8,
        AMDVM_VCPU_TEST_ROM_DEBUG_BYTE,
        AMDVM_VCPU_TEST_ROM_OUT_IMM8_AL,
        AMDVM_VCPU_TEST_ROM_DEBUG_PORT,
        AMDVM_VCPU_TEST_ROM_HLT,
    };
    amdvm_probe_result_t before = { 0 }, after = { 0 };
    amdvm_hlt_fpu_preflight_result_t fpu = { 0 };
    amdvm_vcpu_create_request_t create = { 0 };
    amdvm_vcpu_lifecycle_result_t lifecycle = { 0 };
    amdvm_vcpu_run_request_t run = { 0 };
    amdvm_vcpu_run_result_t exit = { 0 };
    uint8_t expected_entry[16] = { 0 };
    CharFrontend debugcon = { 0 };
    Chardev *debugchardev;
    Error *err = NULL;
    size_t output_size;
    bool context_created = false;
    bool debugcon_initialized = false;
    int ret = -EIO;

    if (!diagnostic_only && (!debugcon_id || !*debugcon_id)) {
        error_report("AMD-SVM requires debugcon=<QEMU chardev id> for guest OUT 0xe9");
        return -EINVAL;
    }
    debugchardev = diagnostic_only ? NULL : qemu_chr_find(debugcon_id);
    if (!diagnostic_only && (!debugchardev ||
        !qemu_chr_fe_init(&debugcon, debugchardev, &err))) {
        if (err) {
            error_report_err(err);
        } else {
            error_report("AMD-SVM debugcon chardev '%s' was not found",
                         debugcon_id);
        }
        return -ENODEV;
    }
    debugcon_initialized = !diagnostic_only;

    output_size = sizeof(before);
    if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_QUERY,
                    NULL, 0, &before, &output_size) < 0) {
        error_report("AMD-SVM provider query failed");
        ret = -EIO;
        goto out;
    }
    if (output_size != sizeof(before) ||
        before.abi_version != AMDVM_PROBE_ABI_VERSION) {
        error_report("AMD-SVM provider ABI mismatch (need v%u, found v%u, result size %zu)",
                     AMDVM_PROBE_ABI_VERSION, before.abi_version,
                     output_size);
        ret = -EPROTO;
        goto out;
    }
    if (!amdsvm_hardware_available(&before)) {
        error_report("AMD-SVM provider reports unsafe/unavailable hardware state");
        ret = -ENOTSUP;
        goto out;
    }

    output_size = sizeof(fpu);
    if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_HLT_FPU_PREFLIGHT,
                    NULL, 0, &fpu, &output_size) < 0 ||
        output_size != sizeof(fpu) ||
        fpu.abi_version != AMDVM_HLT_FPU_PREFLIGHT_ABI_VERSION ||
        fpu.flags != 0xffu || fpu.init_status != 0 ||
        fpu.first_xsave_status != 0 || fpu.second_xsave_status != 0 ||
        fpu.destroy_status != 0) {
        error_report("AMD-SVM XNU FPU preflight did not pass (flags 0x%x, status %d/%d/%d)",
                     fpu.flags, fpu.init_status, fpu.first_xsave_status,
                     fpu.second_xsave_status);
        ret = -ENOTSUP;
        goto out;
    }

    create.abi_version = AMDVM_VCPU_ABI_VERSION;
    create.guest_profile = AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1;
    create.code_size = sizeof(guest_code);
    memcpy(create.code, guest_code, sizeof(guest_code));
    output_size = sizeof(lifecycle);
    if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_VCPU_CREATE,
                    &create, sizeof(create), &lifecycle,
                    &output_size) < 0 || output_size != sizeof(lifecycle) ||
        lifecycle.abi_version != AMDVM_VCPU_ABI_VERSION ||
        lifecycle.context_status != 0) {
        error_report("AMD-SVM vCPU context creation failed (status %d)",
                     lifecycle.context_status);
        goto out;
    }
    context_created = true;

    memcpy(expected_entry, guest_code, sizeof(guest_code));
    if (amdsvm_read_reset_diagnostic(connection, expected_entry,
            AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1) < 0) {
        goto out;
    }
    if (diagnostic_only) {
        error_report("AMD-SVM diagnostic-only mode complete; VMRUN was not called");
        ret = 0;
        goto out;
    }

    /* CPUID in the guest exits to QEMU.  Supply a deterministic test leaf. */
    run.abi_version = AMDVM_VCPU_ABI_VERSION;
    run.flags = AMDVM_VCPU_RUN_CONFIRM_NO_KDP_WATCHPOINTS;
    run.rip = AMDVM_VCPU_TEST_ROM_CPUID_RIP;
    run.rflags = 2;
    output_size = sizeof(exit);
    if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_VCPU_RUN,
                    &run, sizeof(run), &exit, &output_size) < 0 ||
        output_size != sizeof(exit) ||
        exit.abi_version != AMDVM_VCPU_ABI_VERSION ||
        (exit.flags & AMDVM_VCPU_RUN_CALLED) == 0 ||
        (exit.flags & AMDVM_VCPU_RUN_LEASE_RETAINED) != 0 ||
        exit.context_status != 0 || exit.owner_status != 0 ||
        exit.vmrun_status != 0 ||
        exit.exit_code != AMDVM_VCPU_EXPECTED_CPUID_EXIT ||
        exit.next_rip != AMDVM_VCPU_TEST_ROM_CPUID_NEXT_RIP ||
        (exit.rip != AMDVM_VCPU_TEST_ROM_CPUID_RIP &&
         exit.rip != AMDVM_VCPU_TEST_ROM_CPUID_NEXT_RIP)) {
        error_report("AMD-SVM guest CPUID exit failed (VMEXIT 0x%llx, info1 0x%llx, info2 0x%llx, RIP 0x%llx, NRIP 0x%llx, RFLAGS 0x%llx, RAX 0x%llx, RCX 0x%llx, decode_len=%u decode_bytes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, status %d/%d/%d)",
                     (unsigned long long)exit.exit_code,
                     (unsigned long long)exit.exit_info1,
                     (unsigned long long)exit.exit_info2,
                     (unsigned long long)exit.rip,
                     (unsigned long long)exit.next_rip,
                     (unsigned long long)exit.rflags,
                     (unsigned long long)exit.gpr[AMDSVM_GPR_RAX],
                     (unsigned long long)exit.gpr[AMDSVM_GPR_RCX],
                     exit.instruction_length,
                     exit.instruction_bytes[0], exit.instruction_bytes[1],
                     exit.instruction_bytes[2], exit.instruction_bytes[3],
                     exit.instruction_bytes[4], exit.instruction_bytes[5],
                     exit.instruction_bytes[6], exit.instruction_bytes[7],
                     exit.instruction_bytes[8], exit.instruction_bytes[9],
                     exit.instruction_bytes[10], exit.instruction_bytes[11],
                     exit.instruction_bytes[12], exit.instruction_bytes[13],
                     exit.instruction_bytes[14],
                     exit.context_status,
                     exit.owner_status, exit.vmrun_status);
        error_report("AMD-SVM entry image: GPA=%016llx HPA=%016llx leaf=%016llx bytes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x CS=%04x/%04x:%08x:%016llx RIP=%016llx RFLAGS=%016llx CR0=%016llx CR3=%016llx CR4=%016llx EFER=%016llx NP=%08x N_CR3=%016llx ASID=%u CPUID-intercept=%u misc1=%08x misc2=%08x EVENTINJ=%016llx EXITINTINFO=%08x EXITCODE-sentinel=%016llx XCR0(host/guest)=%016llx/%016llx XSTATE-size=%u",
                     (unsigned long long)exit.entry.guest_physical_address,
                     (unsigned long long)exit.entry.host_physical_address,
                     (unsigned long long)exit.entry.npt_leaf_entry,
                     exit.entry.entry_bytes[0], exit.entry.entry_bytes[1],
                     exit.entry.entry_bytes[2], exit.entry.entry_bytes[3],
                     exit.entry.entry_bytes[4], exit.entry.entry_bytes[5],
                     exit.entry.entry_bytes[6], exit.entry.entry_bytes[7],
                     exit.entry.entry_bytes[8], exit.entry.entry_bytes[9],
                     exit.entry.entry_bytes[10], exit.entry.entry_bytes[11],
                     exit.entry.entry_bytes[12], exit.entry.entry_bytes[13],
                     exit.entry.entry_bytes[14], exit.entry.entry_bytes[15],
                     exit.entry.cs_selector, exit.entry.cs_attributes,
                     exit.entry.cs_limit,
                     (unsigned long long)exit.entry.cs_base,
                     (unsigned long long)exit.entry.rip,
                     (unsigned long long)exit.entry.rflags,
                     (unsigned long long)exit.entry.cr0,
                     (unsigned long long)exit.entry.cr3,
                     (unsigned long long)exit.entry.cr4,
                     (unsigned long long)exit.entry.efer,
                     exit.entry.nested_paging_control,
                     (unsigned long long)exit.entry.npt_root,
                     exit.entry.asid,
                     (exit.entry.flags & AMDVM_VCPU_ENTRY_CPUID_INTERCEPT) != 0,
                     exit.entry.intercept_misc1, exit.entry.intercept_misc2,
                     (unsigned long long)exit.entry.event_injection,
                     exit.exit_int_info,
                     (unsigned long long)exit.entry.exit_code_sentinel,
                     (unsigned long long)exit.entry.host_xcr0,
                     (unsigned long long)exit.entry.guest_xcr0,
                     exit.entry.guest_xstate_size);
        goto out;
    }

    memcpy(run.gpr, exit.gpr, sizeof(run.gpr));
    run.gpr[0] = 0; /* CPUID leaf 0: no basic leaves beyond leaf zero. */
    run.gpr[1] = 0x554d4551u; /* EBX: "QEMU" */
    run.gpr[2] = 0x204d5653u; /* ECX: "SVM " */
    run.gpr[3] = 0x20444d41u; /* EDX: "AMD " */
    run.rip = AMDVM_VCPU_TEST_ROM_CPUID_NEXT_RIP;
    run.rflags = exit.rflags;
    output_size = sizeof(exit);
    if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_VCPU_RUN,
                    &run, sizeof(run), &exit, &output_size) < 0 ||
        output_size != sizeof(exit) ||
        exit.abi_version != AMDVM_VCPU_ABI_VERSION ||
        (exit.flags & AMDVM_VCPU_RUN_CALLED) == 0 ||
        (exit.flags & AMDVM_VCPU_RUN_LEASE_RETAINED) != 0 ||
        exit.context_status != 0 || exit.owner_status != 0 ||
        exit.vmrun_status != 0 ||
        exit.exit_code != AMDVM_VCPU_EXPECTED_IOIO_EXIT ||
        exit.next_rip != AMDVM_VCPU_TEST_ROM_IO_NEXT_RIP ||
        (exit.rip != AMDVM_VCPU_TEST_ROM_IO_RIP &&
         exit.rip != AMDVM_VCPU_TEST_ROM_IO_NEXT_RIP) ||
        (exit.exit_info1 & 1u) != 0 || /* OUT, not IN */
        (exit.exit_info1 & (1u << 2)) != 0 || /* no string I/O */
        (exit.exit_info1 & (1u << 3)) != 0 || /* no REP */
        ((exit.exit_info1 >> 4) & 7u) != 1u || /* SVM encoding for byte width */
        ((exit.exit_info1 >> 16) & 0xffffu) != AMDVM_VCPU_TEST_ROM_DEBUG_PORT ||
        (exit.gpr[0] & 0xffu) != AMDVM_VCPU_TEST_ROM_DEBUG_BYTE) {
        error_report("AMD-SVM guest debug-port I/O exit failed (VMEXIT 0x%llx, info1 0x%llx, RIP 0x%llx, status %d/%d/%d)",
                     (unsigned long long)exit.exit_code,
                     (unsigned long long)exit.exit_info1,
                     (unsigned long long)exit.rip, exit.context_status,
                     exit.owner_status, exit.vmrun_status);
        goto out;
    }

    /* The fixed guest executed OUT DX/imm8 to 0xe9; forward AL to QEMU's chardev. */
    {
        uint8_t ch = (uint8_t)exit.gpr[0];
        if (qemu_chr_fe_write_all(&debugcon, &ch, 1) != 1) {
            error_report("AMD-SVM debugcon chardev write failed");
            goto out;
        }
    }

    memcpy(run.gpr, exit.gpr, sizeof(run.gpr));
    run.rip = AMDVM_VCPU_TEST_ROM_HLT_RIP;
    run.rflags = exit.rflags;
    output_size = sizeof(exit);
    if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_VCPU_RUN,
                    &run, sizeof(run), &exit, &output_size) < 0 ||
        output_size != sizeof(exit) ||
        exit.abi_version != AMDVM_VCPU_ABI_VERSION ||
        (exit.flags & AMDVM_VCPU_RUN_CALLED) == 0 ||
        (exit.flags & AMDVM_VCPU_RUN_LEASE_RETAINED) != 0 ||
        exit.context_status != 0 || exit.owner_status != 0 ||
        exit.vmrun_status != 0 || exit.exit_code != AMDVM_HLT_EXPECTED_EXIT) {
        error_report("AMD-SVM guest HLT exit failed (VMEXIT 0x%llx, RIP 0x%llx, status %d/%d/%d)",
                     (unsigned long long)exit.exit_code,
                     (unsigned long long)exit.rip, exit.context_status,
                     exit.owner_status, exit.vmrun_status);
        goto out;
    }
    ret = 0;

out:
    if (context_created) {
        memset(&lifecycle, 0, sizeof(lifecycle));
        output_size = sizeof(lifecycle);
        if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_VCPU_DESTROY,
                        NULL, 0, &lifecycle, &output_size) < 0 ||
            output_size != sizeof(lifecycle) ||
            lifecycle.context_status != 0) {
            error_report("AMD-SVM vCPU teardown failed; provider must remain loaded (status %d)",
                         lifecycle.context_status);
            ret = -EIO;
        }
    }
    if (ret == 0) {
        output_size = sizeof(after);
        if (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_QUERY,
                        NULL, 0, &after, &output_size) < 0 ||
            output_size != sizeof(after) ||
            !amdsvm_cpu_state_restored(&before, &after)) {
            error_report("AMD-SVM host CPU state differs after guest teardown");
            ret = -EIO;
        }
    }
    if (ret == 0 && !diagnostic_only) {
        error_report("AMD-SVM QEMU microguest passed: CPUID 0x72 -> OUT 0xe9 VMEXIT 0x7b -> HLT 0x78; host EFER.SVME/VM_HSAVE_PA restored on %u CPUs",
                     after.logical_cpus_seen);
    } else if (ret == 0) {
        error_report("AMD-SVM microguest pre-entry diagnostic completed; VMRUN was not called");
    }
    if (debugcon_initialized) {
        qemu_chr_fe_deinit(&debugcon, false);
    }
    return ret;
}

typedef struct AMDVMFirmwareDispatchContext {
    CPUState *cpu;
    AMDVMAccelState *accel_state;
    AMDVMEventState *pending_events;
    CharFrontend *debugcon;
    bool debugcon_initialized;
} AMDVMFirmwareDispatchContext;

static int amdsvm_firmware_store_state(
    void *opaque, const amdvm_x86_guest_state_v1_t *state)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;

    return amdsvm_state_store(ctx->cpu, state);
}

static int amdsvm_firmware_complete_event(void *opaque,
                                          uint64_t exit_code,
                                          uint32_t exit_int_info,
                                          amdvm_x86_guest_state_v1_t *state)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;

    return amdsvm_event_complete(ctx->pending_events, exit_code,
                                 exit_int_info, state);
}

static int amdsvm_firmware_handle_iret(void *opaque, uint64_t guest_rip,
                                      uint32_t interruptibility,
                                      uint64_t guest_rflags)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;

    return amdsvm_event_handle_iret(ctx->pending_events, guest_rip,
                                    interruptibility, guest_rflags);
}

static int amdsvm_firmware_cpuid(void *opaque, uint32_t leaf,
                                 uint32_t subleaf, uint32_t *eax,
                                 uint32_t *ebx, uint32_t *ecx,
                                 uint32_t *edx)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;

    amdsvm_emulate_cpuid(ctx->cpu, leaf, subleaf, eax, ebx, ecx, edx);
    return 0;
}

static int amdsvm_firmware_rdtsc(void *opaque, uint64_t *value)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;

    return amdsvm_state_read_tsc(ctx->cpu, value);
}

static AMDVMFirmwareDispatchAction amdsvm_firmware_xsetbv(
    void *opaque, const amdvm_vcpu_run_result_t *result)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    uint32_t event = 0;

    if (result->exit_info1 || result->exit_info2 ||
        result->next_rip != result->state.rip + 3 ||
        amdsvm_handle_xsetbv_exit(ctx->cpu, result->next_rip, &event) != 0) {
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (event && amdsvm_event_queue_exception(ctx->pending_events,
                                              event, 0) != 0) {
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

static int amdsvm_firmware_io(void *opaque, uint32_t port, bool is_input,
                              unsigned size, uint64_t *rax)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    uint8_t ch;
    bool vga_port = port >= 0x3b0 && port <= 0x3df;
    bool pci_port = port == 0xcf8 || (port >= 0xcfc && port <= 0xcff);

    if (is_input) {
        switch (size) {
        case 1:
            *rax = cpu_inb(port);
            break;
        case 2:
            *rax = cpu_inw(port);
            break;
        case 4:
            *rax = cpu_inl(port);
            break;
        default:
            return -EINVAL;
        }
        if (vga_port && amdsvm_firmware_vga_io_diag_count < 256) {
            error_report("AMDVM_FIRMWARE_IO_DIAG class=vga dir=in port=0x%x size=%u value=0x%llx",
                         port, size, (unsigned long long)*rax);
            amdsvm_firmware_vga_io_diag_count++;
        } else if (pci_port && amdsvm_firmware_pci_io_diag_count < 256) {
            error_report("AMDVM_FIRMWARE_IO_DIAG class=pci dir=in port=0x%x size=%u value=0x%llx",
                         port, size, (unsigned long long)*rax);
            amdsvm_firmware_pci_io_diag_count++;
        }
        return 0;
    }

    if (vga_port && amdsvm_firmware_vga_io_diag_count < 256) {
        error_report("AMDVM_FIRMWARE_IO_DIAG class=vga dir=out port=0x%x size=%u value=0x%llx",
                     port, size, (unsigned long long)*rax);
        amdsvm_firmware_vga_io_diag_count++;
    } else if (pci_port && amdsvm_firmware_pci_io_diag_count < 256) {
        error_report("AMDVM_FIRMWARE_IO_DIAG class=pci dir=out port=0x%x size=%u value=0x%llx",
                     port, size, (unsigned long long)*rax);
        amdsvm_firmware_pci_io_diag_count++;
    }

    if (size == 1 && port == AMDVM_VCPU_TEST_ROM_DEBUG_PORT &&
        ctx->debugcon_initialized) {
        ch = (uint8_t)*rax;
        return qemu_chr_fe_write_all(ctx->debugcon, &ch, 1) == 1 ? 0 : -EIO;
    }
    switch (size) {
    case 1:
        cpu_outb(port, (uint8_t)*rax);
        break;
    case 2:
        cpu_outw(port, (uint16_t)*rax);
        break;
    case 4:
        cpu_outl(port, (uint32_t)*rax);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static int amdsvm_firmware_string_io(
    void *opaque, const amdvm_x86_guest_state_v1_t *state,
    uint64_t exit_info1, uint64_t next_rip,
    amdvm_x86_guest_state_v1_t *updated_state)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    Error *error = NULL;

    /* The dispatcher has already installed this state in CPUX86State. */
    if (!state || !updated_state ||
        amdsvm_handle_ioio_string_exit(ctx->cpu, exit_info1, next_rip,
                                       updated_state, &error) != 0) {
        if (error) {
            error_report_err(error);
        }
        return -EINVAL;
    }
    return 0;
}

static AMDVMFirmwareDispatchAction amdsvm_firmware_cr(
    void *opaque, const amdvm_vcpu_run_result_t *result)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    Error *error = NULL;
    uint32_t event_injection = 0, event_error_code = 0;

    if (!amdsvm_handle_cr_exit(ctx->cpu, result->exit_code,
                               result->exit_info1, result->next_rip,
                               &event_injection, &event_error_code, &error)) {
        if (error) {
            error_report_err(error);
        }
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (event_injection != 0 &&
        amdsvm_event_queue_exception(ctx->pending_events, event_injection,
                                     event_error_code) != 0) {
        error_report("AMD-SVM CR VMEXIT produced invalid or overlapping exception injection");
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

static AMDVMFirmwareDispatchAction amdsvm_firmware_dr(
    void *opaque, const amdvm_vcpu_run_result_t *result)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    Error *error = NULL;

    if (!amdsvm_handle_dr_exit(ctx->cpu, result->exit_code,
                               result->exit_info1, result->next_rip,
                               &error)) {
        if (error) {
            error_report_err(error);
        }
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

static AMDVMFirmwareDispatchAction amdsvm_firmware_msr(
    void *opaque, const amdvm_vcpu_run_result_t *result)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    Error *error = NULL;
    uint32_t event_injection = 0, event_error_code = 0;

    if (!amdsvm_handle_msr_exit(ctx->cpu, result->exit_info1,
                                result->next_rip, &event_injection,
                                &event_error_code, &error)) {
        if (error) {
            error_report_err(error);
        }
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (event_injection != 0 &&
        amdsvm_event_queue_exception(ctx->pending_events, event_injection,
                                     event_error_code) != 0) {
        error_report("AMD-SVM MSR VMEXIT produced invalid or overlapping exception injection");
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

static uint8_t amdsvm_firmware_read_committed_code(
    const AMDVMGuestMemoryMap *map,
    const amdvm_x86_guest_state_v1_t *state, uint8_t bytes[15])
{
    const amdvm_x86_segment_state_t *cs;
    uint64_t offset;
    uint64_t linear;
    uint8_t length = 0;

    if (!map || !state) {
        return 0;
    }
    cs = &state->segments[AMDVM_X86_SEG_CS];
    offset = state->rip;
    if ((state->cr[0] & (UINT64_C(1) << 31)) ||
        offset > cs->limit || cs->base > UINT64_MAX - offset) {
        return 0;
    }
    linear = cs->base + offset;
    while (length < 15 && length <= cs->limit - offset &&
           linear <= UINT64_MAX - length) {
        uint64_t gpa = linear + length;
        bool found = false;

        if (map->reset_rom_page_present &&
            gpa >= AMDVM_VCPU_RESET_ROM_GPA &&
            gpa < AMDVM_VCPU_RESET_ROM_GPA + AMDSVM_GUEST_PAGE_SIZE) {
            bytes[length] = map->reset_rom_page[
                gpa - AMDVM_VCPU_RESET_ROM_GPA];
            found = true;
        } else {
            for (uint32_t i = 0; i < map->count; i++) {
                const AMDVMGuestMemoryRange *range = &map->ranges[i];
                uint64_t end;

                if (!(range->flags & AMDSVM_GUEST_RANGE_READ) ||
                    !range->size ||
                    range->guest_physical_address > UINT64_MAX - range->size) {
                    continue;
                }
                end = range->guest_physical_address + range->size;
                if (gpa >= range->guest_physical_address && gpa < end &&
                    range->host_virtual_address <= UINT64_MAX -
                        (gpa - range->guest_physical_address)) {
                    uintptr_t hva = (uintptr_t)(range->host_virtual_address +
                                      (gpa - range->guest_physical_address));
                    bytes[length] = *(const uint8_t *)hva;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            break;
        }
        length++;
    }
    return length;
}

static AMDVMFirmwareDispatchAction amdsvm_firmware_npf(
    void *opaque, const amdvm_vcpu_run_result_t *result)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    Error *error = NULL;
    uint8_t instruction_bytes[15] = { 0 };
    uint8_t instruction_length = 0;
    const uint32_t immutable_rom_flags = AMDSVM_GUEST_RANGE_READ |
                                         AMDSVM_GUEST_RANGE_EXEC;
    uint64_t immutable_rom_bytes = 0;
    uint64_t immutable_rom_hva = 0;
    uint64_t shadow_backing_hva = 0;
    uint64_t shadow_backing_bytes = 0;
    AMDVMFaultMemorySection live_section = { 0 };
    static unsigned npf_write_trace_count;
    static unsigned npf_vga_trace_count;

    if (ctx->accel_state && ctx->accel_state->guest_map_initialized) {
        const AMDVMGuestMemoryMap *map = &ctx->accel_state->guest_map;

        /*
         * NPF RIP is a guest virtual address once paging is active, while the
         * committed RAM map is indexed by GPA. Prefer the bytes supplied by
         * AMD DecodeAssists; the map-based fetch remains a fallback for the
         * flat firmware phase where GPA and linear address coincide.
         */
        if (result->instruction_length >= 2 &&
            result->instruction_length <= sizeof(instruction_bytes)) {
            instruction_length = result->instruction_length;
            memcpy(instruction_bytes, result->instruction_bytes,
                   instruction_length);
        } else {
            instruction_length = amdsvm_firmware_read_committed_code(
                map, &result->state, instruction_bytes);
        }

        for (uint32_t i = 0; i < map->count; i++) {
            const AMDVMGuestMemoryRange *range = &map->ranges[i];
            uint64_t end;

            if (range->size == 0 ||
                range->guest_physical_address > UINT64_MAX - range->size) {
                continue;
            }
            end = range->guest_physical_address + range->size;
            if (range->flags == immutable_rom_flags &&
                result->exit_info2 >= range->guest_physical_address &&
                result->exit_info2 < end) {
                immutable_rom_bytes = end - result->exit_info2;
                immutable_rom_hva = range->host_virtual_address +
                    (result->exit_info2 - range->guest_physical_address);
                break;
            }
        }
        if (immutable_rom_bytes == 0 && map->reset_rom_page_present &&
            result->exit_info2 >= AMDVM_VCPU_RESET_ROM_GPA &&
            result->exit_info2 < AMDVM_VCPU_RESET_ROM_GPA +
                                 AMDSVM_GUEST_PAGE_SIZE) {
            immutable_rom_bytes = AMDVM_VCPU_RESET_ROM_GPA +
                                  AMDSVM_GUEST_PAGE_SIZE -
                                  result->exit_info2;
        }
        if (immutable_rom_bytes != 0) {
            live_section = amdsvm_current_memory_section(result->exit_info2);
            if (live_section.found && live_section.is_ram &&
                !live_section.is_rom && immutable_rom_hva != 0 &&
                live_section.hva != immutable_rom_hva) {
                shadow_backing_hva = immutable_rom_hva;
                shadow_backing_bytes = MIN(immutable_rom_bytes,
                    live_section.size - (result->exit_info2 -
                                         live_section.range_gpa));
            }
        }
    }
    if (!live_section.found) {
        live_section = amdsvm_current_memory_section(result->exit_info2);
    }

    if (ctx->accel_state && ctx->accel_state->guest_map_initialized &&
        (result->exit_info1 & (UINT64_C(1) << 1)) != 0) {
        bool vga_aperture = result->exit_info2 >= UINT64_C(0x000a0000) &&
                            result->exit_info2 < UINT64_C(0x000c0000);
        bool unassigned_candidate = result->exit_info1 ==
                                        ((UINT64_C(1) << 32) | 6u) &&
                                    !vga_aperture &&
                                    immutable_rom_bytes == 0;
        if (npf_write_trace_count < 32 ||
            (vga_aperture && npf_vga_trace_count < 32)) {
            if (!live_section.found) {
                live_section = amdsvm_current_memory_section(
                    result->exit_info2);
            }
            error_report("AMDVM_NPF_WRITE_TRACE index=%u rip=0x%llx gpa=0x%llx info1=0x%llx code_len=%u code=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x rom_remaining=0x%llx live_region=%s live_ram=%u live_rom=%u live_hva=0x%llx shadow_hva=0x%llx shadow_bytes=0x%llx vga_aperture=%u unassigned_candidate=%u eax=0x%llx ebx=0x%llx ecx=0x%llx edx=0x%llx esi=0x%llx edi=0x%llx ebp=0x%llx esp=0x%llx eflags=0x%llx",
                     npf_write_trace_count,
                     (unsigned long long)result->state.rip,
                     (unsigned long long)result->exit_info2,
                     (unsigned long long)result->exit_info1,
                     instruction_length,
                     instruction_bytes[0], instruction_bytes[1],
                     instruction_bytes[2], instruction_bytes[3],
                     instruction_bytes[4], instruction_bytes[5],
                     instruction_bytes[6], instruction_bytes[7],
                     instruction_bytes[8], instruction_bytes[9],
                     instruction_bytes[10], instruction_bytes[11],
                     instruction_bytes[12], instruction_bytes[13],
                     instruction_bytes[14],
                     (unsigned long long)immutable_rom_bytes,
                     live_section.found && live_section.name ?
                         live_section.name : "<unmapped>",
                     live_section.is_ram, live_section.is_rom,
                     (unsigned long long)live_section.hva,
                     (unsigned long long)shadow_backing_hva,
                     (unsigned long long)shadow_backing_bytes,
                     vga_aperture, unassigned_candidate,
                     (unsigned long long)result->state.gpr[AMDSVM_GPR_RAX],
                     (unsigned long long)result->state.gpr[AMDSVM_GPR_RBX],
                     (unsigned long long)result->state.gpr[AMDSVM_GPR_RCX],
                     (unsigned long long)result->state.gpr[AMDSVM_GPR_RDX],
                     (unsigned long long)result->state.gpr[AMDSVM_GPR_RSI],
                     (unsigned long long)result->state.gpr[AMDSVM_GPR_RDI],
                     (unsigned long long)result->state.gpr[AMDSVM_GPR_RBP],
                     (unsigned long long)result->state.gpr[AMDSVM_GPR_RSP],
                     (unsigned long long)result->state.rflags);
            if (vga_aperture) {
                npf_vga_trace_count++;
            }
        }
        npf_write_trace_count++;
    }

    if (amdsvm_handle_npf_write_exit(
            ctx->cpu, result->exit_info1, result->exit_info2,
            instruction_bytes, instruction_length,
            immutable_rom_bytes, shadow_backing_hva,
            shadow_backing_bytes, &error) != 0) {
        if (error) {
            error_report_err(error);
        }
        error_report("AMDVM_NPF_FAIL gpa=0x%llx info1=0x%llx region=%s ram=%u rom=%u live_hva=0x%llx rom_remaining=0x%llx shadow_hva=0x%llx shadow_bytes=0x%llx",
                     (unsigned long long)result->exit_info2,
                     (unsigned long long)result->exit_info1,
                     live_section.found && live_section.name ?
                         live_section.name : "<unmapped>",
                     live_section.is_ram, live_section.is_rom,
                     (unsigned long long)live_section.hva,
                     (unsigned long long)immutable_rom_bytes,
                     (unsigned long long)shadow_backing_hva,
                     (unsigned long long)shadow_backing_bytes);
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

static void amdsvm_process_async_events(CPUState *cpu,
                                       AMDVMAccelState *state)
{
    unsigned applied = amdsvm_state_process_async_events(
        cpu, state->trace_vmexit);
    bool was_halted = state->provider_halted[cpu->cpu_index];

    if (applied & AMDSVM_ASYNC_INIT_APPLIED) {
        /* INIT discards events retained from the previous execution context. */
        memset(&state->pending_events[cpu->cpu_index], 0,
               sizeof(state->pending_events[cpu->cpu_index]));
    }
    if (applied) {
        /* INIT/SIPI starts a new architectural context. A HLT from SeaBIOS
         * must not prevent entry at the subsequent Linux SIPI vector.
         * INIT itself still leaves the AP halted until a valid SIPI. */
        state->provider_halted[cpu->cpu_index] = false;
        if (state->trace_vmexit) {
            error_report("AMDVM_AP_RESET vcpu=%d applied=0x%x provider_halted_before=%u provider_halted_after=0",
                         cpu->cpu_index, applied, was_halted);
        }
    }
}

static AMDVMFirmwareDispatchAction amdsvm_firmware_process_events(
    AMDVMFirmwareDispatchContext *ctx)
{
    int pending;

    if (!ctx || !ctx->cpu) {
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    qemu_process_cpu_events(ctx->cpu);
    if (!cpu_can_run(ctx->cpu)) {
        return AMDVM_FIRMWARE_DISPATCH_YIELDED;
    }

    /* Target-specific helper mirrors QEMU KVM/HVF AP async-event handling. */
    amdsvm_process_async_events(ctx->cpu, ctx->accel_state);

    pending = amdsvm_pending_guest_interrupts(ctx->cpu);
    if (pending != 0) {
        error_report("AMD-SVM cannot yet inject pending QEMU guest interrupt (mask 0x%x)",
                     pending);
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

static AMDVMFirmwareDispatchAction amdsvm_firmware_physical_intr(void *opaque)
{
    return amdsvm_firmware_process_events(opaque);
}

static AMDVMFirmwareDispatchAction amdsvm_firmware_preentry_kick(void *opaque)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;

    qemu_process_cpu_events(ctx->cpu);
    if (!cpu_can_run(ctx->cpu)) {
        return AMDVM_FIRMWARE_DISPATCH_YIELDED;
    }
    return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

/*
 * QEMU firmware path: use QEMU's reset CPUState, actual reset ROM page, and
 * pinned board RAM. Unsupported exits fail closed and are never retried.
 */
static int amdsvm_vm_api_prepare(AMDVMAccelState *state)
{
    g_autofree AMDVMGuestMemoryMap *map = g_new0(AMDVMGuestMemoryMap, 1);
    g_autofree amdvm_vcpu_memory_range_t *ranges = NULL;
    g_autofree amdvm_x86_guest_state_v1_t *snapshot = g_new0(amdvm_x86_guest_state_v1_t, 1);
    Error *err = NULL;
    CPUState *cpu;
    unsigned prepared = 0;
    unsigned range_count = 0;

    if (!state || !state->vm_api ||
        state->vm_api_prepare_failed) {
        return -EPERM;
    }
    if (state->vm_api_prepared) {
        return 0;
    }
    if (!amdsvm_collect_guest_ram(address_space_to_flatview(&address_space_memory), map, &err)) {
        error_report_err(err);
        state->vm_api_prepare_failed = true;
        return -EINVAL;
    }
    if (!map->reset_rom_page_present) {
        state->vm_api_prepare_failed = true;
        return -EINVAL;
    }
    if (state->validate_finite_smp_rom) {
        if (!state->diagnostic_only || !state->diagnostic_validate_ram_map ||
            !state->vm_api || !amdsvm_finite_smp_rom_matches(map)) {
            error_report("AMD-SVM finite-SMP ROM mapping preflight failed; VMRUN was not called");
            state->vm_api_prepare_failed = true;
            return -EINVAL;
        }
        error_report("AMD-SVM finite-SMP ROM mapping preflight PASS: reset/BSP/AP/GDTR/GDT bytes match; VMRUN was not called");
    }
    ranges = g_new0(amdvm_vcpu_memory_range_t, AMDVM_VCPU_MEMORY_MAX_RANGES);
    for (unsigned i = 0; i < map->count; ++i) {
        uint64_t offset = 0;
        while (offset < map->ranges[i].size) {
            uint64_t bytes = MIN(map->ranges[i].size - offset, AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES);
            if (range_count == AMDVM_VCPU_MEMORY_MAX_RANGES) {
                state->vm_api_prepare_failed = true;
                return -E2BIG;
            }
            ranges[range_count++] = (amdvm_vcpu_memory_range_t) {
                .guest_physical_address = map->ranges[i].guest_physical_address + offset,
                .user_virtual_address = map->ranges[i].host_virtual_address + offset,
                .size = bytes, .flags = map->ranges[i].flags,
            };
            offset += bytes;
        }
    }
    /* The provider pins the split ranges above. Keep QEMU's later MTRR/NPT
     * map byte-for-byte segmented the same way; otherwise a >4 GiB FlatView
     * range is presented as one type range against multiple pinned ranges. */
    map->count = range_count;
    for (unsigned i = 0; i < range_count; ++i) {
        map->ranges[i] = (AMDVMGuestMemoryRange) {
            .guest_physical_address = ranges[i].guest_physical_address,
            .host_virtual_address = ranges[i].user_virtual_address,
            .size = ranges[i].size,
            .flags = ranges[i].flags,
        };
    }
    amdsvm_trace_smp_memory_map("prepare", map);
    if (amdsvm_vm_provider_map(&state->vm_provider, map->reset_rom_page, ranges, range_count)) {
        error_report("AMD-SVM VM API shared RAM preparation failed");
        goto failed;
    }
    CPU_FOREACH(cpu) {
        AMDVMMTRRState mtrr;
        amdvm_vcpu_set_memory_types_request_t memory_types = {0};
        if (cpu->cpu_index < 0 ||
            amdsvm_vm_provider_create(&state->vm_provider, cpu->cpu_index,
                                      amdsvm_state_apic_id(cpu)) ||
            amdsvm_state_load(cpu, snapshot) ||
            amdsvm_vm_provider_state_roundtrip(&state->vm_provider, cpu->cpu_index, snapshot)) {
            error_report("AMD-SVM VM API CPU %d resource/state preparation failed", cpu->cpu_index);
            goto failed;
        }
        amdsvm_state_get_mtrr(cpu, &mtrr);
        if (!amdsvm_build_memory_type_request(map, &mtrr,
                AMDVM_GUEST_MTRR_COMPAT_ALL_WB, &memory_types, &err)) {
            error_report_err(err);
            error_report("AMD-SVM VM API CPU %d MTRR map preparation failed", cpu->cpu_index);
            goto failed;
        }
        int memory_type_status = amdsvm_vm_provider_set_memory_types(
            &state->vm_provider, cpu->cpu_index, &memory_types);
        if (memory_type_status) {
            error_report("AMD-SVM VM API CPU %d memory-type update failed (status %d; ranges=%u; guest_pages=%" PRIu64 ")",
                         cpu->cpu_index, memory_type_status,
                         memory_types.range_count,
                         map->total_ram / AMDSVM_GUEST_PAGE_SIZE + 1u);
            goto failed;
        }
        amdvm_vm_abi_draft_vcpu_diagnostic_result_t npt_diag = {0};
        uint32_t required_npt_flags =
            AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ENABLED |
            AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ROOT_MATCH |
            AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_TRANSLATES |
            AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_BACKING_MATCH |
            AMDVM_VM_ABI_DRAFT_VCPU_DIAG_APIC_UNMAPPED;
        int npt_diag_status = amdsvm_vm_provider_diagnostic(
            &state->vm_provider, cpu->cpu_index, &npt_diag);
        const uint32_t expected_reset_flags =
            AMDVM_VCPU_DIAGNOSTIC_ROM_INSTALLED |
            AMDVM_VCPU_DIAGNOSTIC_NPT_RX_VALID |
            AMDVM_VCPU_DIAGNOSTIC_RESET_STATE;
        unsigned reset_bytes_first_diff = sizeof(npt_diag.snapshot.entry_bytes);
        for (unsigned i = 0; i < sizeof(npt_diag.snapshot.entry_bytes); ++i) {
            if (npt_diag.snapshot.entry_bytes[i] !=
                map->reset_rom_page[AMDVM_VCPU_RESET_ROM_OFFSET + i]) {
                reset_bytes_first_diff = i;
                break;
            }
        }
        error_report("AMDVM_VCPU_RESET_FIELDS vcpu=%d abi=%u context=%d profile=%u expected_profile=%u flags=0x%x expected_flags=0x%x GPA=0x%llx offset=0x%x CS=%04x attrib=0x%04x limit=0x%x base=0x%llx RIP=0x%llx RFLAGS=0x%llx CR0=0x%llx EFER=0x%llx first_byte_diff=%u expected_byte=0x%02x actual_byte=0x%02x",
                     cpu->cpu_index, npt_diag.snapshot.abi_version,
                     npt_diag.snapshot.context_status,
                     npt_diag.snapshot.guest_profile,
                     AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1,
                     npt_diag.snapshot.flags, expected_reset_flags,
                     (unsigned long long)npt_diag.snapshot.guest_physical_address,
                     npt_diag.snapshot.rom_offset,
                     npt_diag.snapshot.cs_selector,
                     npt_diag.snapshot.cs_attributes,
                     npt_diag.snapshot.cs_limit,
                     (unsigned long long)npt_diag.snapshot.cs_base,
                     (unsigned long long)npt_diag.snapshot.rip,
                     (unsigned long long)npt_diag.snapshot.rflags,
                     (unsigned long long)npt_diag.snapshot.cr0,
                     (unsigned long long)npt_diag.snapshot.efer,
                     reset_bytes_first_diff,
                     reset_bytes_first_diff < sizeof(npt_diag.snapshot.entry_bytes) ?
                         map->reset_rom_page[AMDVM_VCPU_RESET_ROM_OFFSET + reset_bytes_first_diff] : 0,
                     reset_bytes_first_diff < sizeof(npt_diag.snapshot.entry_bytes) ?
                         npt_diag.snapshot.entry_bytes[reset_bytes_first_diff] : 0);
        bool reset_state_matches =
            npt_diag.snapshot.abi_version == AMDVM_VCPU_ABI_VERSION &&
            npt_diag.snapshot.context_status == 0 &&
            npt_diag.snapshot.guest_profile ==
                AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1 &&
            npt_diag.snapshot.flags == expected_reset_flags &&
            npt_diag.snapshot.guest_physical_address ==
                AMDVM_VCPU_RESET_ROM_GPA &&
            npt_diag.snapshot.rom_offset == AMDVM_VCPU_RESET_ROM_OFFSET &&
            npt_diag.snapshot.cs_selector == 0xf000 &&
            npt_diag.snapshot.cs_attributes == 0x009b &&
            npt_diag.snapshot.cs_limit == 0xffff &&
            npt_diag.snapshot.cs_base == AMDVM_VCPU_RESET_CS_BASE &&
            npt_diag.snapshot.rip == AMDVM_VCPU_RESET_VECTOR_RIP &&
            npt_diag.snapshot.rflags == 2 &&
            npt_diag.snapshot.cr0 == 0x60000010ull &&
            npt_diag.snapshot.efer == (1ull << 12) &&
            memcmp(npt_diag.snapshot.entry_bytes,
                   map->reset_rom_page + AMDVM_VCPU_RESET_ROM_OFFSET,
                   sizeof(npt_diag.snapshot.entry_bytes)) == 0;
        error_report("AMDVM_VCPU_RESET_PREFLIGHT vcpu=%d match=%u flags=0x%x GPA=0x%llx offset=0x%x CS=%04x:%016llx RIP=0x%llx CR0=0x%llx EFER=0x%llx entry=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                     cpu->cpu_index, reset_state_matches,
                     npt_diag.snapshot.flags,
                     (unsigned long long)npt_diag.snapshot.guest_physical_address,
                     npt_diag.snapshot.rom_offset,
                     npt_diag.snapshot.cs_selector,
                     (unsigned long long)npt_diag.snapshot.cs_base,
                     (unsigned long long)npt_diag.snapshot.rip,
                     (unsigned long long)npt_diag.snapshot.cr0,
                     (unsigned long long)npt_diag.snapshot.efer,
                     npt_diag.snapshot.entry_bytes[0],
                     npt_diag.snapshot.entry_bytes[1],
                     npt_diag.snapshot.entry_bytes[2],
                     npt_diag.snapshot.entry_bytes[3],
                     npt_diag.snapshot.entry_bytes[4],
                     npt_diag.snapshot.entry_bytes[5],
                     npt_diag.snapshot.entry_bytes[6],
                     npt_diag.snapshot.entry_bytes[7],
                     npt_diag.snapshot.entry_bytes[8],
                     npt_diag.snapshot.entry_bytes[9],
                     npt_diag.snapshot.entry_bytes[10],
                     npt_diag.snapshot.entry_bytes[11],
                     npt_diag.snapshot.entry_bytes[12],
                     npt_diag.snapshot.entry_bytes[13],
                     npt_diag.snapshot.entry_bytes[14],
                     npt_diag.snapshot.entry_bytes[15]);
        error_report("AMDVM_VCPU_NPT_PREFLIGHT vcpu=%d status=%d flags=0x%x np_enable=0x%x root_match=%u reset_walk=%d reset_hpa=0x%llx apic_walk=%d npt_root=0x%llx reset_rip=0x%llx",
                     cpu->cpu_index, npt_diag_status, npt_diag.flags,
                     npt_diag.nested_paging_control, npt_diag.npt_root_matches,
                     npt_diag.reset_translation_status,
                     (unsigned long long)npt_diag.reset_translation_hpa,
                     npt_diag.apic_translation_status,
                     (unsigned long long)npt_diag.npt_root,
                     (unsigned long long)npt_diag.snapshot.rip);
        if (npt_diag_status != 0 || !reset_state_matches ||
            (npt_diag.flags & required_npt_flags) != required_npt_flags ||
            npt_diag.reset_translation_status !=
                AMDVM_VM_ABI_DRAFT_NPT_STATUS_MAPPED ||
            npt_diag.apic_translation_status !=
                AMDVM_VM_ABI_DRAFT_NPT_STATUS_UNMAPPED) {
            error_report("AMD-SVM vCPU %d NPT preflight failed; no VMRUN will be attempted",
                         cpu->cpu_index);
            goto failed;
        }
        state->published_mtrr[cpu->cpu_index] = mtrr;
        state->memory_types_published[cpu->cpu_index] = true;
        ++prepared;
    }
    if (prepared != state->vm_expected_cpus) {
        goto failed;
    }
    state->guest_map = *map;
    state->guest_map_initialized = true;
    state->guest_page_count = map->total_ram / AMDSVM_GUEST_PAGE_SIZE;
    state->vm_api_prepared = true;
    return 0;

failed:
    state->vm_api_prepare_failed = true;
    error_report("AMD-SVM VM API preparation failed; VMRUN was not called");
    if (amdsvm_vm_provider_destroy(&state->vm_provider)) {
        error_report("AMD-SVM VM API cleanup failed; connection retained");
    }
    return -EIO;
}

static int amdsvm_vm_api_diagnostic(AMDVMAccelState *state)
{
    amdvm_probe_result_t after = {0};
    int rc;

    if (!state->diagnostic_only || !state->diagnostic_validate_ram_map) {
        return -EPERM;
    }
    rc = amdsvm_vm_api_prepare(state);
    if (rc != 0) {
        return rc;
    }
    if (amdsvm_vm_provider_destroy(&state->vm_provider)) {
        error_report("AMD-SVM VM API cleanup failed; connection retained");
        rc = -EIO;
    } else {
        state->vm_api_prepared = false;
        state->guest_map_initialized = false;
        state->guest_page_count = 0;
    }
    if (amdsvm_probe_host_state(&after) ||
        !amdsvm_cpu_state_restored(&state->cpu_state_before, &after)) {
        error_report("AMD-SVM VM API host SVM/quarantine baseline mismatch");
        rc = -EIO;
    }
    if (!rc) {
        error_report("AMD-SVM VM API diagnostic PASS: cpus=%u mapped_bytes=%" PRIu64
                     " writable_bytes=%" PRIu64 "; state roundtrip and cleanup verified; VMRUN was not called",
                     state->vm_expected_cpus,
                     state->guest_map.total_ram,
                     state->guest_map.total_writable_bytes);
    }
    return rc;
}

/* Adapt the VM-scoped ABI result to the existing firmware exit dispatcher. */
static int amdsvm_vm_api_run_once(AMDVMAccelState *state, CPUState *cpu,
                                  const amdvm_vcpu_run_request_t *run,
                                  amdvm_vcpu_run_result_t *result)
{
    amdvm_vm_abi_draft_vcpu_run_result_t vm_result = { 0 };
    int rc;

    if (!state || !cpu || !run || !result || cpu->cpu_index < 0 ||
        cpu->cpu_index >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
        !state->vm_api_prepared) {
        error_report("AMD-SVM VM API RUN rejected before provider call (state=%p cpu=%p cpu_index=%d prepared=%u)",
                     state, cpu, cpu ? cpu->cpu_index : -1,
                     state ? state->vm_api_prepared : 0);
        return -EINVAL;
    }
    rc = amdsvm_vm_provider_run(&state->vm_provider, cpu->cpu_index,
        run->kick_generation, run->event_injection,
        run->event_injection_error_code, &run->state, &vm_result);
    if (rc == -ENOTSUP) {
        error_report("AMD-SVM provider RUN is fail-closed (ENTRY_GATED); no VMRUN was attempted");
    }
    if (rc < 0) {
        error_report("AMD-SVM VM API RUN provider rejected request (rc=%d vcpu=%d mapped=%u created=%u features=0x%llx kick=%llu event=0x%x state_abi=%u rip=0x%llx interruptibility=0x%llx flags=0x%x context=%d owner=%d run=%d exit=0x%llx result_state_abi=%u result_rip=0x%llx)",
                     rc, cpu->cpu_index,
                     qatomic_read(&state->vm_provider.mapped),
                     qatomic_read(&state->vm_provider.cpus[cpu->cpu_index]),
                     (unsigned long long)state->vm_provider.caps.feature_flags,
                     (unsigned long long)run->kick_generation,
                     run->event_injection, run->state.abi_version,
                     (unsigned long long)run->state.interruptibility,
                     (unsigned long long)run->state.rip, vm_result.flags,
                     vm_result.context_status, vm_result.owner_status,
                     vm_result.run_status,
                     (unsigned long long)vm_result.exit_code,
                     vm_result.state.abi_version,
                     (unsigned long long)vm_result.state.rip);
    }
    memset(result, 0, sizeof(*result));
    result->abi_version = AMDVM_VCPU_ABI_VERSION;
    result->flags = vm_result.flags;
    result->context_status = vm_result.context_status;
    result->owner_status = vm_result.owner_status;
    result->vmrun_status = vm_result.run_status;
    result->exit_int_info = vm_result.exit_int_info;
    memcpy(result->state.gpr, vm_result.state.gpr,
           sizeof(result->state.gpr));
    memcpy(result->gpr, vm_result.state.gpr, sizeof(result->gpr));
    result->rip = vm_result.state.rip;
    result->rflags = vm_result.state.rflags;
    result->exit_code = vm_result.exit_code;
    result->exit_info1 = vm_result.exit_info1;
    result->exit_info2 = vm_result.exit_info2;
    result->state = vm_result.state;
    result->next_rip = vm_result.next_rip;
    result->instruction_length = vm_result.instruction_length;
    memcpy(result->instruction_bytes, vm_result.instruction_bytes,
           sizeof(result->instruction_bytes));
    return rc;
}

/* Pause sibling workers with a finite deadline before VM-wide remapping. */
static int amdsvm_vm_api_pause_siblings(AMDVMAccelState *state,
                                       CPUState *current,
                                       bool requested[AMDVM_VM_ABI_DRAFT_MAX_VCPUS])
{
    CPUState *cpu;
    int64_t deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    memset(requested, 0, AMDVM_VM_ABI_DRAFT_MAX_VCPUS * sizeof(requested[0]));
    g_assert(bql_locked());
    CPU_FOREACH(cpu) {
        int id = cpu->cpu_index;
        if (cpu == current || id < 0 || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
            !qatomic_read(&state->vm_provider.cpus[id])) {
            continue;
        }
        requested[id] = true;
        cpu->stop = true;
        qemu_cpu_kick(cpu);
    }
    for (;;) {
        bool drained = true;
        CPU_FOREACH(cpu) {
            int id = cpu->cpu_index;
            if (id < 0 || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS || !requested[id]) {
                continue;
            }
            if (!cpu->stopped || qatomic_read(&state->vm_provider.run_active[id])) {
                drained = false;
                break;
            }
        }
        if (drained) {
            return 0;
        }
        if (g_get_monotonic_time() >= deadline) {
            CPU_FOREACH(cpu) {
                int id = cpu->cpu_index;
                if (id < 0 || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS || !requested[id]) {
                    continue;
                }
                if (cpu->stopped) {
                    int exception_index = cpu->exception_index;
                    cpu_resume(cpu);
                    cpu->exception_index = exception_index;
                } else {
                    cpu->stop = false;
                    qemu_cpu_kick(cpu);
                }
            }
            return -ETIMEDOUT;
        }
        bql_unlock();
        g_usleep(1000);
        bql_lock();
    }
}

static void amdsvm_vm_api_resume_siblings(CPUState *current,
                                         const bool requested[AMDVM_VM_ABI_DRAFT_MAX_VCPUS])
{
    CPUState *cpu;

    g_assert(bql_locked());
    CPU_FOREACH(cpu) {
        int id = cpu->cpu_index;
        if (cpu == current || id < 0 || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
            !requested[id]) {
            continue;
        }
        if (cpu->stopped) {
            int exception_index = cpu->exception_index;
            cpu_resume(cpu);
            cpu->exception_index = exception_index;
        } else {
            cpu->stop = false;
            qemu_cpu_kick(cpu);
        }
    }
}

static int amdsvm_vm_api_refresh_map(AMDVMAccelState *state, CPUState *current,
                                     const AMDVMGuestMemoryMap *updated_map)
{
    g_autofree amdvm_vcpu_memory_range_t *ranges = NULL;
    AMDVMGuestMemoryMap provider_map;
    bool paused[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    Error *err = NULL;
    CPUState *cpu;
    unsigned range_count = 0;
    int rc;
    bool resume_siblings = true;

    if (!state || !state->vm_api || !state->vm_api_prepared || !current ||
        !updated_map || !updated_map->reset_rom_page_present ||
        !state->guest_map.reset_rom_page_present ||
        memcmp(state->guest_map.reset_rom_page, updated_map->reset_rom_page,
               sizeof(updated_map->reset_rom_page))) {
        error_report("AMD-SVM map refresh requires an unchanged reset-ROM page");
        return -ENOTSUP;
    }
    rc = amdsvm_vm_api_pause_siblings(state, current, paused);
    if (rc) {
        error_report("AMD-SVM could not drain sibling vCPUs within 5 seconds");
        return rc;
    }
    ranges = g_new0(amdvm_vcpu_memory_range_t, AMDVM_VCPU_MEMORY_MAX_RANGES);
    for (unsigned i = 0; i < updated_map->count; ++i) {
        uint64_t offset = 0;
        while (offset < updated_map->ranges[i].size) {
            uint64_t bytes = MIN(updated_map->ranges[i].size - offset,
                                 AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES);
            if (range_count == AMDVM_VCPU_MEMORY_MAX_RANGES) {
                rc = -E2BIG;
                goto out;
            }
            ranges[range_count++] = (amdvm_vcpu_memory_range_t) {
                .guest_physical_address = updated_map->ranges[i].guest_physical_address + offset,
                .user_virtual_address = updated_map->ranges[i].host_virtual_address + offset,
                .size = bytes,
                .flags = updated_map->ranges[i].flags,
            };
            offset += bytes;
        }
    }
    provider_map = *updated_map;
    provider_map.count = range_count;
    for (unsigned i = 0; i < range_count; ++i) {
        provider_map.ranges[i] = (AMDVMGuestMemoryRange) {
            .guest_physical_address = ranges[i].guest_physical_address,
            .host_virtual_address = ranges[i].user_virtual_address,
            .size = ranges[i].size,
            .flags = ranges[i].flags,
        };
    }
    CPU_FOREACH(cpu) {
        int id = cpu->cpu_index;
        if (id < 0 || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
            !qatomic_read(&state->vm_provider.cpus[id])) {
            continue;
        }
        /* From the first destroy onward, failure leaves workers paused. */
        resume_siblings = false;
        rc = amdsvm_vm_provider_destroy_vcpu(&state->vm_provider, id);
        if (rc) {
            error_report("AMD-SVM failed to retire vCPU %d before map replace (%d)", id, rc);
            goto out;
        }
    }
    rc = amdsvm_vm_provider_replace_map(&state->vm_provider, ranges, range_count);
    if (rc) {
        error_report("AMD-SVM provider rejected replacement map (%d)", rc);
        goto out;
    }
    state->guest_map = provider_map;
    state->guest_page_count = updated_map->total_ram / AMDSVM_GUEST_PAGE_SIZE;
    CPU_FOREACH(cpu) {
        AMDVMMTRRState mtrr;
        g_autofree amdvm_x86_guest_state_v1_t *snapshot =
            g_new0(amdvm_x86_guest_state_v1_t, 1);
        amdvm_vcpu_set_memory_types_request_t memory_types = {0};
        int id = cpu->cpu_index;
        if (id < 0 || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
            amdsvm_vm_provider_create(&state->vm_provider, id,
                                      amdsvm_state_apic_id(cpu)) ||
            amdsvm_state_load(cpu, snapshot) ||
            amdsvm_vm_provider_state_roundtrip(&state->vm_provider, id, snapshot)) {
            error_report("AMD-SVM failed to recreate vCPU %d after map replace", id);
            rc = -EIO;
            goto out;
        }
        amdsvm_state_get_mtrr(cpu, &mtrr);
        if (!amdsvm_build_memory_type_request(&state->guest_map, &mtrr,
                AMDVM_GUEST_MTRR_COMPAT_ALL_WB, &memory_types, &err)) {
            error_report_err(err);
            rc = -ENOTSUP;
            goto out;
        }
        if (amdsvm_vm_provider_set_memory_types(&state->vm_provider, id,
                                                &memory_types)) {
            error_report("AMD-SVM failed to restore vCPU %d NPT memory types", id);
            rc = -EIO;
            goto out;
        }
        state->published_mtrr[id] = mtrr;
        state->memory_types_published[id] = true;
    }
    resume_siblings = true;
    error_report("AMD-SVM VM RAM map replaced generation=%llu ranges=%u bytes=%llu",
                 (unsigned long long)state->vm_provider.generation, range_count,
                 (unsigned long long)updated_map->total_ram);
    rc = 0;
out:
    if (resume_siblings) {
        amdsvm_vm_api_resume_siblings(current, paused);
    }
    return rc;
}

static int amdsvm_run_qemu_firmware(CPUState *cpu,
                                   AMDVMAccelState *accel_state)
{
    AMDVMGuestMemoryMap guest_map = { 0 };
    amdvm_vcpu_create_request_t create = { 0 };
    amdvm_vcpu_set_reset_rom_request_t reset_rom = { 0 };
    amdvm_vcpu_memory_map_request_t map_request = { 0 };
    amdvm_vcpu_memory_map_result_t map_result = { 0 };
    amdvm_vcpu_set_memory_types_request_t memory_types = { 0 };
    amdvm_vcpu_run_request_t run = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    amdvm_hlt_fpu_preflight_result_t fpu = { 0 };
    amdvm_probe_result_t after = { 0 };
    AMDVMFirmwareDispatchContext dispatch_context = { 0 };
    const AMDVMFirmwareDispatchOps dispatch_ops = {
        .store_state = amdsvm_firmware_store_state,
        .complete_event = amdsvm_firmware_complete_event,
        .handle_iret = amdsvm_firmware_handle_iret,
        .emulate_cpuid = amdsvm_firmware_cpuid,
        .emulate_rdtsc = amdsvm_firmware_rdtsc,
        .handle_xsetbv = amdsvm_firmware_xsetbv,
        .emulate_io = amdsvm_firmware_io,
        .emulate_string_io = amdsvm_firmware_string_io,
        .handle_cr = amdsvm_firmware_cr,
        .handle_dr = amdsvm_firmware_dr,
        .handle_msr = amdsvm_firmware_msr,
        .handle_npf = amdsvm_firmware_npf,
        .handle_physical_intr = amdsvm_firmware_physical_intr,
        .handle_preentry_kick = amdsvm_firmware_preentry_kick,
    };
    Chardev *debugchardev = NULL;
    Error *err = NULL;
    size_t output_size;
    bool vmrun_attempted = false;
    bool provider_prepared = false;
    unsigned int preentry_fpu_retry_count = 0;
    int ret = -EIO;

    dispatch_context.cpu = cpu;
    dispatch_context.accel_state = accel_state;
    if (!accel_state || cpu->cpu_index < 0 ||
        cpu->cpu_index >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
        accel_state->connection == IO_OBJECT_NULL) {
        return -EINVAL;
    }
    dispatch_context.debugcon = &accel_state->debugcon_frontend;
    if (!accel_state->vm_api && cpu->cpu_index != 0) {
        return -ENOTSUP;
    }
    dispatch_context.pending_events =
        &accel_state->pending_events[cpu->cpu_index];
    if (!accel_state->vm_api && accel_state->provider_prepare_failed) {
        return -EIO;
    }
    if (!accel_state->vm_api &&
        accel_state->provider_lifecycle.context_may_exist &&
        !accel_state->provider_lifecycle.context_ready) {
        error_report("AMD-SVM provider context exists but RAM setup did not commit");
        return -EIO;
    }
    if (accel_state->debugcon && *accel_state->debugcon &&
        !accel_state->debugcon_frontend_initialized) {
        debugchardev = qemu_chr_find(accel_state->debugcon);
        if (!debugchardev ||
            !qemu_chr_fe_init(&accel_state->debugcon_frontend,
                              debugchardev, &err)) {
            if (err) {
                error_report_err(err);
            } else {
                error_report("AMD-SVM debugcon chardev '%s' was not found",
                             accel_state->debugcon);
            }
            return -ENODEV;
        }
        /* vCPU workers enter this path with the BQL held, so only one shared
         * frontend is attached to the chardev for the lifetime of the accel. */
        accel_state->debugcon_frontend_initialized = true;
    }
    dispatch_context.debugcon_initialized =
        accel_state->debugcon_frontend_initialized;

    if (accel_state->vm_api) {
        if (!accel_state->vm_api_fpu_preflight_done) {
            if (amdsvm_vm_api_fpu_preflight() != 0) {
                ret = -ENOTSUP;
                goto out;
            }
            accel_state->vm_api_fpu_preflight_done = true;
        }
    } else {
        output_size = sizeof(fpu);
        if (amdsvm_call(accel_state->connection,
                    AMDVM_PROBE_SELECTOR_HLT_FPU_PREFLIGHT,
                    NULL, 0, &fpu, &output_size) < 0 ||
        output_size != sizeof(fpu) ||
        fpu.abi_version != AMDVM_HLT_FPU_PREFLIGHT_ABI_VERSION ||
        fpu.flags != 0xffu || fpu.init_status != 0 ||
        fpu.first_xsave_status != 0 || fpu.second_xsave_status != 0 ||
        fpu.destroy_status != 0) {
        error_report("AMD-SVM worker XNU FPU preflight failed (flags 0x%x init=%d first=%d second=%d destroy=%d cpu=%u/%u ts=%u/%u em=%u/%u xcr0=0x%llx)",
                     fpu.flags, fpu.init_status, fpu.first_xsave_status,
                     fpu.second_xsave_status, fpu.destroy_status,
                     fpu.cpu_before, fpu.cpu_after, fpu.ts_before,
                     fpu.ts_after, fpu.em_before, fpu.em_after,
                     (unsigned long long)fpu.xcr0);
            ret = -ENOTSUP;
            goto out;
        }
    }

    if (!amdsvm_collect_guest_ram(address_space_to_flatview(&address_space_memory),
                                  &guest_map, &err)) {
        error_report_err(err);
        ret = -EINVAL;
        goto out;
    }
    if (!guest_map.reset_rom_page_present) {
        error_report("AMD-SVM requires QEMU firmware mapped at reset GPA 0xfffff000");
        ret = -EINVAL;
        goto out;
    }
    if (accel_state->confirm_finite_kick_test &&
        !amdsvm_finite_kick_rom_matches(guest_map.reset_rom_page)) {
        error_report("AMD-SVM finite-kick mode rejected firmware: reset page does not contain the exact K/5M/H/HLT test ROM");
        ret = -EPERM;
        goto out;
    }
    if (accel_state->confirm_finite_smp_test &&
        !amdsvm_finite_smp_rom_matches(&guest_map)) {
        error_report("AMD-SVM finite-SMP mode rejected firmware: reset, BSP, or AP bytes do not match the fixed B/A/a/H ROM");
        ret = -EPERM;
        goto out;
    }
    if (accel_state->diagnostic_only &&
        !accel_state->diagnostic_validate_ram_map) {
        create.abi_version = AMDVM_VCPU_ABI_VERSION;
        create.guest_profile = AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1;
        if (!amdsvm_build_reset_rom_request(&guest_map, &reset_rom, &err)) {
            error_report_err(err);
            ret = -EINVAL;
            goto out;
        }
        if (amdsvm_provider_prepare_rom_only(
                &accel_state->provider_lifecycle, &create, &reset_rom) < 0) {
            error_report("AMD-SVM provider reset-ROM-only preparation failed");
            ret = -EIO;
            goto out;
        }
        if (amdsvm_read_reset_diagnostic(accel_state->connection,
                guest_map.reset_rom_page + AMDVM_VCPU_RESET_ROM_OFFSET,
                AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1) < 0) {
            ret = -EPROTO;
            goto out;
        }
        error_report("AMD-SVM firmware reset-ROM diagnostic completed; guest RAM was not pinned and VMRUN was not called");
        ret = 0;
        goto out;
    }
    if (accel_state->vm_api) {
        if (amdsvm_vm_api_prepare(accel_state) != 0) {
            error_report("AMD-SVM VM API persistent preparation failed");
            ret = -EIO;
            goto out;
        }
        if (memcmp(&guest_map, &accel_state->guest_map,
                   sizeof(guest_map)) != 0) {
            ret = amdsvm_vm_api_refresh_map(accel_state, cpu, &guest_map);
            if (ret) {
                goto out;
            }
        }
    } else if (accel_state->guest_map_initialized &&
        memcmp(&guest_map, &accel_state->guest_map,
               sizeof(guest_map)) != 0) {
        /*
         * SeaBIOS changes QEMU's FlatView when it enables PCI PAM/ROM shadowing.
         * The current provider ABI pins a complete map once and has no atomic
         * remap operation. We are between synchronous RUN calls here, so retire
         * the old context before pinning the new map. The next RUN supplies the
         * complete QEMU CPU state; if teardown or preparation fails, fail closed
         * without entering the guest against stale NPT translations.
         */
        error_report("AMD-SVM QEMU RAM map changed while vCPU was stopped; rebuilding pinned provider context");
        if (amdsvm_destroy_vcpu_context(accel_state) != 0) {
            ret = -EIO;
            goto out;
        }
    }
    if (!accel_state->vm_api && !accel_state->guest_map_initialized) {
        if (!amdsvm_build_memory_map_request(&guest_map, &map_request, &err)) {
            error_report_err(err);
            ret = -EINVAL;
            goto out;
        }

        create.abi_version = AMDVM_VCPU_ABI_VERSION;
        create.guest_profile = AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1;
        if (!amdsvm_build_reset_rom_request(&guest_map, &reset_rom, &err)) {
            error_report_err(err);
            ret = -EINVAL;
            goto out;
        }
        if (amdsvm_provider_prepare(&accel_state->provider_lifecycle,
                                    &create, &reset_rom, &map_request,
                                    map_request.range_count,
                                    &map_result) < 0) {
            error_report("AMD-SVM provider context/ROM/RAM preparation failed (status %d)",
                         map_result.context_status);
            accel_state->provider_prepare_failed = true;
            ret = -EIO;
            goto out;
        }
        accel_state->guest_map = guest_map;
        accel_state->guest_map_initialized = true;
        accel_state->guest_page_count = map_result.guest_page_count;
        provider_prepared = true;
    }

    if (!accel_state->vm_api && provider_prepared &&
        amdsvm_read_reset_diagnostic(accel_state->connection,
            guest_map.reset_rom_page + AMDVM_VCPU_RESET_ROM_OFFSET,
            AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1) < 0) {
        ret = -EPROTO;
        goto out;
    }

    if (accel_state->finite_host_tlb_stress &&
        !accel_state->host_tlb_stress_started) {
        ret = amdsvm_host_tlb_stress_start(accel_state);
        if (ret != 0) {
            error_report("AMD-SVM could not start same-process host TLB stress (%d)",
                         ret);
            goto out;
        }
    }

    for (;;) {
        /*
         * A kick may arrive after the previous VMEXIT while QEMU was
         * emulating it. Consume that request before entering the provider
         * again, rather than relying on a later physical interrupt.
         */
        if (qatomic_read(&cpu->thread_kicked) ||
            qatomic_read(&cpu->exit_request)) {
            qemu_process_cpu_events(cpu);
            if (!cpu_can_run(cpu)) {
                ret = -EINTR;
                goto out;
            }
        }

        AMDVMFirmwareDispatchAction event_action =
            amdsvm_firmware_process_events(&dispatch_context);
        if (event_action == AMDVM_FIRMWARE_DISPATCH_YIELDED) {
            ret = -EINTR;
            goto out;
        }
        if (event_action != AMDVM_FIRMWARE_DISPATCH_CONTINUE) {
            ret = -ENOTSUP;
            goto out;
        }

        /* Guest MTRRs have no hardware virtualization; publish them in NPT. */
        AMDVMMTRRState current_mtrr;
        amdsvm_state_get_mtrr(cpu, &current_mtrr);
        if (!accel_state->memory_types_published[cpu->cpu_index] ||
            memcmp(&current_mtrr,
                   &accel_state->published_mtrr[cpu->cpu_index],
                   sizeof(current_mtrr)) != 0) {
            memset(&memory_types, 0, sizeof(memory_types));
            amdvm_vcpu_lifecycle_result_t lifecycle = { 0 };
            if (!amdsvm_build_memory_type_request(&guest_map, &current_mtrr,
                                                 AMDVM_GUEST_MTRR_COMPAT_ALL_WB,
                                                 &memory_types, &err)) {
                error_report_err(err);
                ret = -ENOTSUP;
                goto out;
            }
            if (!accel_state->memory_types_published[cpu->cpu_index]) {
                error_report("AMD-SVM guest MTRR policy=compat-all-WB; guest MTRR MSRs remain virtualized, but backed guest pages execute WB");
            }
            int memory_type_status;
            if (accel_state->vm_api) {
                memory_type_status = amdsvm_vm_provider_set_memory_types(
                    &accel_state->vm_provider, cpu->cpu_index, &memory_types);
            } else {
                output_size = sizeof(lifecycle);
                memory_type_status =
                    amdsvm_call(accel_state->connection,
                        AMDVM_PROBE_SELECTOR_VCPU_SET_MEMORY_TYPES,
                        &memory_types, sizeof(memory_types), &lifecycle,
                        &output_size) < 0 || output_size != sizeof(lifecycle) ||
                    lifecycle.abi_version != AMDVM_VCPU_ABI_VERSION ||
                    lifecycle.context_status != 0 || lifecycle.reserved[1] != 0 ||
                    lifecycle.reserved[0] != guest_map.total_ram /
                        AMDSVM_GUEST_PAGE_SIZE + 1u;
            }
            if (memory_type_status) {
                if (accel_state->vm_api) {
                    error_report("AMD-SVM VM API could not apply guest MTRRs to vCPU %d NPT (status %d)",
                                 cpu->cpu_index, memory_type_status);
                } else {
                    error_report("AMD-SVM could not apply guest MTRRs to NPT (status %d, updated pages %u)",
                        lifecycle.context_status, lifecycle.reserved[0]);
                }
                error_report("AMD-SVM rejected guest MTRR input: default=0x%016" PRIx64
                    " phys_bits=%u fixed=%u guest_PAT=0x%016" PRIx64
                    " ranges=%u",
                    current_mtrr.default_type, current_mtrr.phys_bits,
                    current_mtrr.fixed_supported, amdsvm_state_get_pat(cpu),
                    memory_types.range_count);
                for (unsigned r = 0; r < memory_types.range_count; r++) {
                    error_report("AMD-SVM rejected MTRR range[%u]: GPA=0x%016" PRIx64
                        " size=0x%016" PRIx64 " type=%u",
                        r, memory_types.ranges[r].guest_physical_address,
                        memory_types.ranges[r].size,
                        memory_types.ranges[r].memory_type);
                }
                for (unsigned r = 0; r < AMDVM_MTRR_VARIABLE_COUNT; r++) {
                    error_report("AMD-SVM guest MTRR variable[%u]: base=0x%016" PRIx64
                        " mask=0x%016" PRIx64,
                        r, current_mtrr.variable[r].base,
                        current_mtrr.variable[r].mask);
                }
                ret = -ENOTSUP;
                goto out;
            }
            accel_state->published_mtrr[cpu->cpu_index] = current_mtrr;
            accel_state->memory_types_published[cpu->cpu_index] = true;
        }

        if (accel_state->diagnostic_only) {
            /* Explicit RAM diagnostics use the ordinary preparation path,
             * including SET_MEMORY_TYPES, but return before any RUN call.
             * Complete destroy now, rather than relying on process exit. */
            if (amdsvm_destroy_vcpu_context(accel_state) != 0) {
                ret = -EIO;
                goto out;
            }
            output_size = sizeof(after);
            if (amdsvm_call(accel_state->connection,
                    AMDVM_PROBE_SELECTOR_QUERY, NULL, 0,
                    &after, &output_size) < 0 ||
                output_size != sizeof(after) ||
                !amdsvm_cpu_state_restored(&accel_state->cpu_state_before,
                                            &after)) {
                error_report("AMD-SVM RAM diagnostic host state differs after destroy");
                ret = -EIO;
                goto out;
            }
            error_report("AMD-SVM RAM/NPT/MTRR pre-entry diagnostic completed; RAM pin/unpin and host state verified; VMRUN was not called");
            ret = 0;
            goto out;
        }

        memset(&run, 0, sizeof(run));
        run.abi_version = AMDVM_VCPU_ABI_VERSION;
        run.flags = AMDVM_VCPU_RUN_CONFIRM_NO_KDP_WATCHPOINTS;
        run.kick_generation = accel_state->vm_api ?
            qatomic_read(&accel_state->vm_kick_generation[cpu->cpu_index]) :
            qatomic_read(&accel_state->kick_generation);
        if (amdsvm_state_load(cpu, &run.state) != 0) {
            error_report("AMD-SVM rejected QEMU CPU state before VMRUN");
            ret = -EINVAL;
            goto out;
        }
        amdsvm_prepare_irq_injection(
            cpu, &accel_state->pending_events[cpu->cpu_index], &run);
        /* cpu_has_work also wakes for APIC polls and host work. Those do not
         * wake architectural HLT. Keep the provider halted until a real
         * guest event has been selected and retained for EVENTINJ. */
        if (accel_state->provider_halted[cpu->cpu_index] &&
            run.event_injection == 0) {
            cpu->halted = true;
            cpu->exception_index = EXCP_HLT;
            ret = -EINTR;
            goto out;
        }
        /*
         * A kick can complete after the loop-top event check but before the
         * generation snapshot. Recheck after that snapshot so a request
         * cannot acknowledge a new generation while stop work is still
         * pending. A kick after this check leaves the snapshot stale, which
         * the provider detects before entry or interrupts through VMRUN.
         */
        if (qatomic_read(&cpu->thread_kicked) ||
            qatomic_read(&cpu->exit_request)) {
            qemu_process_cpu_events(cpu);
            if (!cpu_can_run(cpu)) {
                ret = -EINTR;
                goto out;
            }
            continue;
        }
        bool trace_this_run = false;
        uint64_t trace_run_index = 0;
        int64_t run_started_wall_us = g_get_real_time();
        int64_t run_started_us = g_get_monotonic_time();
        if (accel_state->trace_vmexit) {
            trace_run_index = qatomic_fetch_inc(
                &accel_state->trace_run_attempts) + 1;
            uint64_t vcpu_run_index =
                ++accel_state->trace_vcpu_run_attempts[cpu->cpu_index];
            trace_this_run =
                vcpu_run_index <=
                    AMDSVM_VMEXIT_TRACE_INITIAL_LIMIT ||
                (trace_run_index &
                    AMDSVM_VMEXIT_TRACE_SAMPLE_MASK) == 0;
            if (trace_this_run) {
                error_report("AMDVM_FINITE_KICK_ENTRY index=%llu vcpu=%d generation=%llu guest_rip=0x%llx cs=%04x/%016llx cr0=0x%llx rflags=0x%llx halted=%u irq=0x%x started_us=%lld started_wall_us=%lld",
                             (unsigned long long)trace_run_index,
                             cpu->cpu_index,
                             (unsigned long long)run.kick_generation,
                             (unsigned long long)run.state.rip,
                             run.state.segments[AMDVM_X86_SEG_CS].selector,
                             (unsigned long long)
                                 run.state.segments[AMDVM_X86_SEG_CS].base,
                             (unsigned long long)run.state.cr[0],
                             (unsigned long long)run.state.rflags,
                             cpu->halted,
                             qatomic_read(&cpu->interrupt_request),
                             (long long)run_started_us,
                             (long long)run_started_wall_us);
            }
        }
        /*
         * QMP stop/pause must be able to acquire the BQL while the vCPU is
         * inside the synchronous IOUserClient call. Standard hardware
         * accelerators release it around guest execution; holding it here
         * prevents QMP from setting cpu->stop and can turn host INTR exits
         * into an unbounded re-entry loop.
         */
        bql_unlock();
        int provider_status = accel_state->vm_api ?
            amdsvm_vm_api_run_once(accel_state, cpu, &run, &result) :
            amdsvm_provider_run(&accel_state->provider_lifecycle,
                                &run, &result);
        bql_lock();
        if (provider_status < 0) {
            /* An IOKit error can follow a completed hardware entry. */
            vmrun_attempted = true;
            error_report("AMD-SVM QEMU provider RUN call failed");
            ret = -EIO;
            goto out;
        }
        if (result.abi_version == AMDVM_VCPU_ABI_VERSION &&
            result.flags == 0 &&
            result.context_status == AMDSVM_CONTEXT_VMRUN_FAILED &&
            result.owner_status == -1 &&
            result.vmrun_status == AMDSVM_VMRUN_FPU_NOT_READY &&
            result.exit_code == (accel_state->vm_api ? UINT64_MAX : 0) &&
            result.next_rip == 0 &&
            result.state.rip == run.state.rip &&
            memcmp(result.state.gpr, run.state.gpr,
                   sizeof(run.state.gpr)) == 0) {
            /*
             * XNU can leave lazy FPU ownership pending on this thread. The
             * provider refused entry before acquiring SVM, so retrying the
             * unchanged guest state cannot resume a partial VMRUN or reuse
             * a stale host XSTATE image. Keep the retry bounded and yield
             * the BQL so QMP stop/kick remains serviceable.
             */
            if (preentry_fpu_retry_count < AMDSVM_PREENTRY_FPU_RETRY_LIMIT) {
                preentry_fpu_retry_count++;
                error_report("AMD-SVM pre-entry XSTATE not ready; retry %u/%u without VMRUN context=%d owner=%d vmrun=%d flags=0x%x rip=0x%llx",
                             preentry_fpu_retry_count,
                             AMDSVM_PREENTRY_FPU_RETRY_LIMIT,
                             result.context_status, result.owner_status,
                             result.vmrun_status, result.flags,
                             (unsigned long long)run.state.rip);
                qemu_process_cpu_events(cpu);
                if (!cpu_can_run(cpu)) {
                    ret = -EINTR;
                    goto out;
                }
                bql_unlock();
                g_usleep(1000);
                bql_lock();
                continue;
            }
            error_report("AMD-SVM stopped after %u consecutive pre-entry XSTATE failures; no VMRUN was attempted",
                         AMDSVM_PREENTRY_FPU_RETRY_LIMIT);
            ret = -EAGAIN;
            goto out;
        }
        if (result.flags & AMDVM_VCPU_RUN_CALLED) {
            accel_state->provider_halted[cpu->cpu_index] = false;
            preentry_fpu_retry_count = 0;
        }
        if (trace_this_run) {
            int64_t run_completed_us = g_get_monotonic_time();
            int64_t run_completed_wall_us = g_get_real_time();
            error_report("AMDVM_FINITE_KICK_VMEXIT code=0x%llx vcpu=%d rip=0x%llx next_rip=0x%llx info1=0x%llx info2=0x%llx flags=0x%x abi=%u context=%d owner=%d vmrun=%d exit_int_info=0x%x entry_flags=0x%x entry_sentinel=0x%llx generation=%llu started_us=%lld completed_us=%lld run_elapsed_us=%lld started_wall_us=%lld completed_wall_us=%lld",
                         (unsigned long long)result.exit_code,
                         cpu->cpu_index,
                         (unsigned long long)result.state.rip,
                         (unsigned long long)result.next_rip,
                         (unsigned long long)result.exit_info1,
                         (unsigned long long)result.exit_info2,
                         result.flags,
                         result.abi_version, result.context_status,
                         result.owner_status, result.vmrun_status,
                         result.exit_int_info, result.entry.flags,
                         (unsigned long long)result.entry.exit_code_sentinel,
                         (unsigned long long)run.kick_generation,
                         (long long)run_started_us,
                         (long long)run_completed_us,
                         (long long)(run_completed_us - run_started_us),
                         (long long)run_started_wall_us,
                         (long long)run_completed_wall_us);
            if (result.exit_code != SVM_EXIT_INTR) {
                uint8_t code_bytes[16] = { 0 };
                uint8_t stack_bytes[16] = { 0 };
                uint8_t return_bytes[16] = { 0 };
                const uint64_t cs_base = result.state.segments[AMDVM_X86_SEG_CS].base;
                const uint64_t ss_base = result.state.segments[AMDVM_X86_SEG_SS].base;
                const uint64_t rsp = result.state.gpr[AMDSVM_GPR_RSP];
                const uint64_t cr0 = result.state.cr[0];
                uint64_t code_linear = 0;
                uint64_t stack_linear = 0;
                uint32_t return_target = 0;
                MemTxResult code_status = MEMTX_ERROR;
                MemTxResult stack_status = MEMTX_ERROR;
                MemTxResult return_status = MEMTX_ERROR;

                if (result.state.rip <= UINT64_MAX - cs_base) {
                    code_linear = cs_base + result.state.rip;
                    if ((cr0 & (1ull << 31)) == 0) {
                        code_status = address_space_read(&address_space_memory,
                            code_linear, MEMTXATTRS_UNSPECIFIED,
                            code_bytes, sizeof(code_bytes));
                    }
                }
                if (rsp <= UINT64_MAX - ss_base) {
                    stack_linear = ss_base + rsp;
                    if ((cr0 & (1ull << 31)) == 0) {
                        stack_status = address_space_read(&address_space_memory,
                            stack_linear, MEMTXATTRS_UNSPECIFIED,
                            stack_bytes, sizeof(stack_bytes));
                    }
                }
                if (stack_status == MEMTX_OK &&
                    (result.state.segments[AMDVM_X86_SEG_SS].attributes &
                     (1u << 10))) {
                    return_target = (uint32_t)stack_bytes[0] |
                        ((uint32_t)stack_bytes[1] << 8) |
                        ((uint32_t)stack_bytes[2] << 16) |
                        ((uint32_t)stack_bytes[3] << 24);
                    return_status = address_space_read(&address_space_memory,
                        return_target, MEMTXATTRS_UNSPECIFIED,
                        return_bytes, sizeof(return_bytes));
                }
                error_report("AMDVM_EXIT_CONTEXT code=0x%llx rip=0x%llx nrip=0x%llx rsp=0x%llx rflags=0x%llx cs=%04x/%04x:%08x:0x%llx ss=%04x/%04x:%08x:0x%llx cr0=0x%llx code_status=0x%x code=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x stack_status=0x%x stack=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x return_target=0x%x return_status=0x%x return_code=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                             (unsigned long long)result.exit_code,
                             (unsigned long long)result.state.rip,
                             (unsigned long long)result.next_rip,
                             (unsigned long long)rsp,
                             (unsigned long long)result.state.rflags,
                             result.state.segments[AMDVM_X86_SEG_CS].selector,
                             result.state.segments[AMDVM_X86_SEG_CS].attributes,
                             result.state.segments[AMDVM_X86_SEG_CS].limit,
                             (unsigned long long)cs_base,
                             result.state.segments[AMDVM_X86_SEG_SS].selector,
                             result.state.segments[AMDVM_X86_SEG_SS].attributes,
                             result.state.segments[AMDVM_X86_SEG_SS].limit,
                             (unsigned long long)ss_base,
                             (unsigned long long)cr0, code_status,
                             code_bytes[0], code_bytes[1], code_bytes[2],
                             code_bytes[3], code_bytes[4], code_bytes[5],
                             code_bytes[6], code_bytes[7], code_bytes[8],
                             code_bytes[9], code_bytes[10], code_bytes[11],
                             code_bytes[12], code_bytes[13], code_bytes[14],
                             code_bytes[15], stack_status, stack_bytes[0],
                             stack_bytes[1], stack_bytes[2], stack_bytes[3],
                             stack_bytes[4], stack_bytes[5], stack_bytes[6],
                             stack_bytes[7], stack_bytes[8], stack_bytes[9],
                             stack_bytes[10], stack_bytes[11], stack_bytes[12],
                             stack_bytes[13], stack_bytes[14], stack_bytes[15],
                             return_target, return_status, return_bytes[0],
                             return_bytes[1], return_bytes[2], return_bytes[3],
                             return_bytes[4], return_bytes[5], return_bytes[6],
                             return_bytes[7], return_bytes[8], return_bytes[9],
                             return_bytes[10], return_bytes[11], return_bytes[12],
                             return_bytes[13], return_bytes[14], return_bytes[15]);
            }
            if (result.exit_code != SVM_EXIT_INTR) {
                error_report("AMDVM_EXIT_DIAG code=0x%llx exit_rip=0x%llx nrip=0x%llx info1=0x%llx info2=0x%llx exit_int_info=0x%x decode_len=%u decode=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x entry_gpa=0x%llx entry_hpa=0x%llx entry_bytes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x entry_rip=0x%llx cs=%04x/%04x:%08x:0x%llx cr0=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx npt=%u n_cr3=0x%llx asid=%u intercept1=0x%x intercept2=0x%x eventinj=0x%llx",
                             (unsigned long long)result.exit_code,
                             (unsigned long long)result.state.rip,
                             (unsigned long long)result.next_rip,
                             (unsigned long long)result.exit_info1,
                             (unsigned long long)result.exit_info2,
                             result.exit_int_info, result.instruction_length,
                             result.instruction_bytes[0], result.instruction_bytes[1],
                             result.instruction_bytes[2], result.instruction_bytes[3],
                             result.instruction_bytes[4], result.instruction_bytes[5],
                             result.instruction_bytes[6], result.instruction_bytes[7],
                             result.instruction_bytes[8], result.instruction_bytes[9],
                             result.instruction_bytes[10], result.instruction_bytes[11],
                             result.instruction_bytes[12], result.instruction_bytes[13],
                             result.instruction_bytes[14],
                             (unsigned long long)result.entry.guest_physical_address,
                             (unsigned long long)result.entry.host_physical_address,
                             result.entry.entry_bytes[0], result.entry.entry_bytes[1],
                             result.entry.entry_bytes[2], result.entry.entry_bytes[3],
                             result.entry.entry_bytes[4], result.entry.entry_bytes[5],
                             result.entry.entry_bytes[6], result.entry.entry_bytes[7],
                             result.entry.entry_bytes[8], result.entry.entry_bytes[9],
                             result.entry.entry_bytes[10], result.entry.entry_bytes[11],
                             result.entry.entry_bytes[12], result.entry.entry_bytes[13],
                             result.entry.entry_bytes[14], result.entry.entry_bytes[15],
                             (unsigned long long)result.entry.rip,
                             result.entry.cs_selector, result.entry.cs_attributes,
                             result.entry.cs_limit,
                             (unsigned long long)result.entry.cs_base,
                             (unsigned long long)result.entry.cr0,
                             (unsigned long long)result.entry.cr3,
                             (unsigned long long)result.entry.cr4,
                             (unsigned long long)result.entry.efer,
                             result.entry.nested_paging_control,
                             (unsigned long long)result.entry.npt_root,
                             result.entry.asid, result.entry.intercept_misc1,
                             result.entry.intercept_misc2,
                             (unsigned long long)result.entry.event_injection);
            }
            if (result.exit_code == SVM_EXIT_INTR) {
                uint8_t intr_code[16] = { 0 };
                const uint64_t cs_base =
                    result.state.segments[AMDVM_X86_SEG_CS].base;
                uint64_t code_linear = UINT64_MAX;
                MemTxResult code_status = MEMTX_ERROR;

                if (result.state.rip <= UINT64_MAX - cs_base) {
                    code_linear = cs_base + result.state.rip;
                    if ((result.state.cr[0] & (1ull << 31)) == 0) {
                        code_status = address_space_read(
                            &address_space_memory, code_linear,
                            MEMTXATTRS_UNSPECIFIED, intr_code,
                            sizeof(intr_code));
                    }
                }
                error_report("AMDVM_PHYSICAL_INTR_STATE vcpu=%d rip=0x%llx rflags=0x%llx cs=%04x/%04x:%08x:0x%llx ss=%04x/%04x:%08x:0x%llx code_status=0x%x code=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x irq_before_events=0x%x halted=%u",
                             cpu->cpu_index,
                             (unsigned long long)result.state.rip,
                             (unsigned long long)result.state.rflags,
                             result.state.segments[AMDVM_X86_SEG_CS].selector,
                             result.state.segments[AMDVM_X86_SEG_CS].attributes,
                             result.state.segments[AMDVM_X86_SEG_CS].limit,
                             (unsigned long long)result.state.segments[AMDVM_X86_SEG_CS].base,
                             result.state.segments[AMDVM_X86_SEG_SS].selector,
                             result.state.segments[AMDVM_X86_SEG_SS].attributes,
                             result.state.segments[AMDVM_X86_SEG_SS].limit,
                             (unsigned long long)result.state.segments[AMDVM_X86_SEG_SS].base,
                             code_status, intr_code[0], intr_code[1],
                             intr_code[2], intr_code[3], intr_code[4],
                             intr_code[5], intr_code[6], intr_code[7],
                             intr_code[8], intr_code[9], intr_code[10],
                             intr_code[11], intr_code[12], intr_code[13],
                             intr_code[14], intr_code[15],
                             qatomic_read(&cpu->interrupt_request), cpu->halted);
            }
            if (result.exit_code == 0x4d) {
                uint8_t fault_bytes[16] = { 0 };
                uint8_t stack_bytes[16] = { 0 };
                const uint64_t fault_offset = result.state.rip;
                const uint64_t cs_base = result.state.segments[AMDVM_X86_SEG_CS].base;
                const uint64_t stack_pointer = result.state.gpr[AMDSVM_GPR_RSP];
                const uint64_t ss_base = result.state.segments[AMDVM_X86_SEG_SS].base;
                const uint64_t cr0 = result.state.cr[0];
                uint64_t fault_linear = 0;
                uint64_t stack_linear = 0;
                MemTxResult read_status = MEMTX_ERROR;
                MemTxResult stack_status = MEMTX_ERROR;
                if (fault_offset <= UINT64_MAX - cs_base) {
                    fault_linear = cs_base + fault_offset;
                    if ((cr0 & (1ull << 31)) == 0) {
                        read_status = address_space_read(&address_space_memory,
                            fault_linear, MEMTXATTRS_UNSPECIFIED,
                            fault_bytes, sizeof(fault_bytes));
                    }
                }
                if (stack_pointer <= UINT64_MAX - ss_base) {
                    stack_linear = ss_base + stack_pointer;
                    if ((cr0 & (1ull << 31)) == 0) {
                        stack_status = address_space_read(&address_space_memory,
                            stack_linear, MEMTXATTRS_UNSPECIFIED,
                            stack_bytes, sizeof(stack_bytes));
                    }
                }
                error_report("AMDVM_GP_DIAG rip=0x%llx cs_base=0x%llx linear=0x%llx cr0=0x%llx read_status=0x%x bytes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                             (unsigned long long)fault_offset,
                             (unsigned long long)cs_base,
                             (unsigned long long)fault_linear,
                             (unsigned long long)cr0, read_status,
                             fault_bytes[0], fault_bytes[1], fault_bytes[2],
                             fault_bytes[3], fault_bytes[4], fault_bytes[5],
                             fault_bytes[6], fault_bytes[7], fault_bytes[8],
                             fault_bytes[9], fault_bytes[10], fault_bytes[11],
                             fault_bytes[12], fault_bytes[13], fault_bytes[14],
                             fault_bytes[15]);
                error_report("AMDVM_GP_STACK rsp=0x%llx ss=%04x/%04x:%08x:0x%llx linear=0x%llx read_status=0x%x bytes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                             (unsigned long long)stack_pointer,
                             result.state.segments[AMDVM_X86_SEG_SS].selector,
                             result.state.segments[AMDVM_X86_SEG_SS].attributes,
                             result.state.segments[AMDVM_X86_SEG_SS].limit,
                             (unsigned long long)ss_base,
                             (unsigned long long)stack_linear, stack_status,
                             stack_bytes[0], stack_bytes[1], stack_bytes[2],
                             stack_bytes[3], stack_bytes[4], stack_bytes[5],
                             stack_bytes[6], stack_bytes[7], stack_bytes[8],
                             stack_bytes[9], stack_bytes[10], stack_bytes[11],
                             stack_bytes[12], stack_bytes[13], stack_bytes[14],
                             stack_bytes[15]);
            }
        }
        /*
         * A successful RUN transport may report malformed output after the
         * provider already entered the guest. Always verify host SVM state on
         * this path, then let the production dispatcher validate the result.
         */
        vmrun_attempted = true;
        switch (amdsvm_dispatch_firmware_exit(&run, &result, &dispatch_ops,
                                               &dispatch_context)) {
        case AMDVM_FIRMWARE_DISPATCH_CONTINUE:
            /*
             * SeaBIOS enables PCI option-ROM/PAM shadowing through config
             * I/O. That changes QEMU's FlatView while this execute call is
             * still active. The provider has direct NPT mappings to the old
             * host pages, so refresh them at this VMEXIT boundary before the
             * next VMRUN. The dispatcher has already committed the guest
             * state and completed the I/O side effect.
             */
            if (result.exit_code == AMDVM_VCPU_EXPECTED_IOIO_EXIT) {
                AMDVMGuestMemoryMap updated_map = { 0 };

                if (!amdsvm_collect_guest_ram(
                        address_space_to_flatview(&address_space_memory),
                        &updated_map, &err)) {
                    error_report_err(err);
                    ret = -EINVAL;
                    goto out;
                }
                if (memcmp(&guest_map, &updated_map,
                           sizeof(updated_map)) != 0) {
                    if (accel_state->vm_api) {
                        ret = amdsvm_vm_api_refresh_map(accel_state, cpu,
                                                        &updated_map);
                        if (ret) {
                            goto out;
                        }
                        guest_map = updated_map;
                    } else {
                    amdvm_vcpu_create_request_t updated_create = { 0 };
                    amdvm_vcpu_set_reset_rom_request_t updated_reset_rom = { 0 };
                    amdvm_vcpu_memory_map_request_t updated_map_request = { 0 };
                    amdvm_vcpu_memory_map_result_t updated_map_result = { 0 };

                    error_report("AMD-SVM QEMU RAM map changed during guest I/O; refreshing pinned provider context at VMEXIT");
                    if (amdsvm_destroy_vcpu_context(accel_state) != 0 ||
                        !amdsvm_build_memory_map_request(
                            &updated_map, &updated_map_request, &err) ||
                        !amdsvm_build_reset_rom_request(
                            &updated_map, &updated_reset_rom, &err)) {
                        if (err) {
                            error_report_err(err);
                        }
                        ret = -EIO;
                        goto out;
                    }
                    updated_create.abi_version = AMDVM_VCPU_ABI_VERSION;
                    updated_create.guest_profile =
                        AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1;
                    if (amdsvm_provider_prepare(
                            &accel_state->provider_lifecycle,
                            &updated_create, &updated_reset_rom,
                            &updated_map_request,
                            updated_map_request.range_count,
                            &updated_map_result) < 0) {
                        error_report("AMD-SVM could not commit the refreshed QEMU RAM map (status %d)",
                                     updated_map_result.context_status);
                        accel_state->provider_prepare_failed = true;
                        ret = -EIO;
                        goto out;
                    }
                    guest_map = updated_map;
                    accel_state->guest_map = updated_map;
                    accel_state->guest_map_initialized = true;
                    accel_state->guest_page_count =
                        updated_map_result.guest_page_count;
                    error_report("AMD-SVM refreshed guest RAM map: %u ranges, %llu pages",
                                 updated_map_result.range_count,
                                 (unsigned long long)
                                     updated_map_result.guest_page_count);
                    }
                }
            }
            continue;
        case AMDVM_FIRMWARE_DISPATCH_HALTED:
            accel_state->provider_halted[cpu->cpu_index] = true;
            cpu->halted = true;
            cpu->exception_index = EXCP_HLT;
            ret = -EINTR;
            goto out;
        case AMDVM_FIRMWARE_DISPATCH_YIELDED:
            if (accel_state->trace_vmexit) {
                error_report("AMDVM_FINITE_KICK_YIELDED vcpu=%d exit=0x%llx guest_rip=0x%llx",
                             cpu->cpu_index,
                             (unsigned long long)result.exit_code,
                             (unsigned long long)result.state.rip);
            }
            ret = -EINTR;
            goto out;
        case AMDVM_FIRMWARE_DISPATCH_ERROR:
        default: {
            uint8_t guest_instruction[15];
            unsigned guest_instruction_length =
                amdsvm_state_read_guest_instruction(cpu,
                                                    guest_instruction);

            error_report("AMD-SVM rejected malformed or unsupported firmware VMEXIT code=0x%llx rip=0x%llx nrip=0x%llx info1=0x%llx info2=0x%llx exit_int_info=0x%x flags=0x%x context=%d owner=%d vmrun=%d cr0=0x%llx cr2=0x%llx cr3=0x%llx cr4=0x%llx decode_len=%u decode=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x guest_fetch_len=%u guest_fetch=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                         (unsigned long long)result.exit_code,
                         (unsigned long long)result.state.rip,
                         (unsigned long long)result.next_rip,
                         (unsigned long long)result.exit_info1,
                         (unsigned long long)result.exit_info2,
                         result.exit_int_info, result.flags,
                         result.context_status, result.owner_status,
                         result.vmrun_status,
                         (unsigned long long)result.state.cr[0],
                         (unsigned long long)result.state.cr[2],
                         (unsigned long long)result.state.cr[3],
                         (unsigned long long)result.state.cr[4],
                         result.instruction_length,
                         result.instruction_bytes[0],
                         result.instruction_bytes[1],
                         result.instruction_bytes[2],
                         result.instruction_bytes[3],
                         result.instruction_bytes[4],
                         result.instruction_bytes[5],
                         result.instruction_bytes[6],
                         result.instruction_bytes[7],
                         result.instruction_bytes[8],
                         result.instruction_bytes[9],
                         result.instruction_bytes[10],
                         result.instruction_bytes[11],
                         result.instruction_bytes[12],
                         result.instruction_bytes[13],
                         result.instruction_bytes[14],
                         guest_instruction_length,
                         guest_instruction[0], guest_instruction[1],
                         guest_instruction[2], guest_instruction[3],
                         guest_instruction[4], guest_instruction[5],
                         guest_instruction[6], guest_instruction[7],
                         guest_instruction[8], guest_instruction[9],
                         guest_instruction[10], guest_instruction[11],
                         guest_instruction[12], guest_instruction[13],
                         guest_instruction[14]);
            ret = -EPROTO;
            goto out;
        }
        }
    }

out:
    amdsvm_host_tlb_stress_stop(accel_state);
    /* A failed preparation has no guest RUN to protect these resources. */
    if (accel_state->provider_lifecycle.context_may_exist &&
        !accel_state->provider_lifecycle.context_ready &&
        amdsvm_destroy_vcpu_context(accel_state) != 0) {
        error_report("AMD-SVM could not retire the uncommitted provider context");
        ret = -EIO;
    }
    if (accel_state->diagnostic_only &&
        amdsvm_destroy_vcpu_context(accel_state) != 0) {
        ret = -EIO;
    }
    if (!accel_state->vm_api &&
        (vmrun_attempted || accel_state->diagnostic_only)) {
        output_size = sizeof(after);
        if (amdsvm_call(accel_state->connection,
                        AMDVM_PROBE_SELECTOR_QUERY,
                        NULL, 0, &after, &output_size) < 0 ||
            output_size != sizeof(after) ||
            !amdsvm_cpu_state_restored(&accel_state->cpu_state_before,
                                       &after)) {
            error_report("AMD-SVM host SVM state did not match its pre-run snapshot");
            ret = -EIO;
        }
    }
    return ret;
}

static void amdsvm_ipi_signal_handler(int sig)
{
    (void)sig;
}

static void *amdsvm_vcpu_thread_fn(void *opaque)
{
    CPUState *cpu = opaque;
    int ret = 0;

#ifndef _WIN32
    sigset_t waitset;
    struct sigaction sigact;
    int wait_result;
    int mask_result;
    bool signal_ready = true;

    sigemptyset(&waitset);
    sigaddset(&waitset, SIG_IPI);
#endif

    rcu_register_thread();
    bql_lock();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

#ifndef _WIN32
    /* QEMU creates vCPU threads with signals blocked. */
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = amdsvm_ipi_signal_handler;
    sigemptyset(&sigact.sa_mask);
    if (sigaction(SIG_IPI, &sigact, NULL) != 0) {
        error_report("AMD-SVM could not install SIG_IPI handler: %s",
                     strerror(errno));
        signal_ready = false;
    } else {
        mask_result = pthread_sigmask(SIG_UNBLOCK, &waitset, NULL);
        if (mask_result != 0) {
            error_report("AMD-SVM could not unblock SIG_IPI: %s",
                         strerror(mask_result));
            signal_ready = false;
        }
    }
    if (!signal_ready) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
    }
#endif

    if (
#ifndef _WIN32
        signal_ready
#else
        true
#endif
       ) {
        do {
            qemu_process_cpu_events(cpu);
            bool diagnostic_finished = amdsvm_active_state &&
                amdsvm_active_state->diagnostic_only &&
                amdsvm_active_state->diagnostic_finished;
            if (!diagnostic_finished && cpu->halted && cpu_has_work(cpu)) {
                /* cpu_has_work may be true for a poll/host wake that does not
                 * architecturally release an AP from wait-for-SIPI or a guest
                 * from HLT. Apply x86 INIT/SIPI/IRQ semantics first and only
                 * run when that processing actually made the CPU runnable. */
                amdsvm_process_async_events(cpu, amdsvm_active_state);
                if (!cpu->halted) {
                    cpu->exception_index = -1;
                }
            }
            if (!diagnostic_finished && cpu_can_run(cpu) && !cpu->halted) {
                ret = amdsvm_active_state->vm_api &&
                      amdsvm_active_state->diagnostic_only ?
                    amdsvm_vm_api_diagnostic(amdsvm_active_state) :
                    amdsvm_run_qemu_firmware(cpu, amdsvm_active_state);
                if (ret == -EINTR) {
                    /* RUN yielded for a host event or the guest executed HLT. */
                } else if (ret == 0) {
                    if (amdsvm_active_state->diagnostic_only) {
                        amdsvm_active_state->diagnostic_finished = true;
                    }
                    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
                    /* Keep the worker for shutdown pause, but do not rerun a
                     * completed diagnostic or terminal guest result. */
                    cpu->halted = true;
                    cpu->exception_index = EXCP_HLT;
                    ret = -EINTR;
                } else {
                    if (amdsvm_active_state->diagnostic_only) {
                        amdsvm_active_state->diagnostic_finished = true;
                    }
                    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
                    /* A terminal provider error must not retry the same
                     * failed setup while QEMU processes the shutdown. */
                    cpu->halted = true;
                    cpu->exception_index = EXCP_HLT;
                    ret = -EINTR;
                }
            }
            /* A host-event yield is not an architectural halt. Likewise an
             * IRQ may already be pending when the previous kick is consumed.
             * Do not wait for a second signal while the vCPU has work. */
            diagnostic_finished = amdsvm_active_state &&
                amdsvm_active_state->diagnostic_only &&
                amdsvm_active_state->diagnostic_finished;
            bool runnable = !diagnostic_finished && cpu_can_run(cpu) &&
                            (!cpu->halted || cpu_has_work(cpu));
            bql_unlock();
            if (runnable) {
                bql_lock();
                continue;
            }
#ifndef _WIN32
            mask_result = pthread_sigmask(SIG_BLOCK, &waitset, NULL);
            if (mask_result != 0) {
                error_report("AMD-SVM could not block SIG_IPI for idle wait: %s",
                             strerror(mask_result));
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
                bql_lock();
                break;
            }
            if (!qatomic_read(&cpu->thread_kicked)) {
                int sig;

                do {
                    wait_result = sigwait(&waitset, &sig);
                } while (wait_result == EINTR);
                if (wait_result != 0) {
                    error_report("AMD-SVM sigwait failed: %s",
                                 strerror(wait_result));
                    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
                    bql_lock();
                    break;
                }
            }
            mask_result = pthread_sigmask(SIG_UNBLOCK, &waitset, NULL);
            if (mask_result != 0) {
                error_report("AMD-SVM could not unblock SIG_IPI after idle wait: %s",
                             strerror(mask_result));
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
                bql_lock();
                break;
            }
#else
            qemu_sem_wait(&cpu->sem);
#endif
            bql_lock();
        } while (!cpu->unplug);
    }

    if (amdsvm_active_state && !amdsvm_active_state->vm_api &&
        amdsvm_destroy_vcpu_context(amdsvm_active_state) != 0) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
    }

    amdsvm_state_free_emulator_buffer(cpu);
    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

static void amdsvm_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    amdsvm_state_alloc_emulator_buffer(cpu);

    snprintf(thread_name, sizeof(thread_name), "CPU %d/AMDSVM", cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, amdsvm_vcpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
#ifdef _WIN32
    qemu_sem_init(&cpu->sem, 0);
#endif
}

static int amdsvm_init_machine(AccelState *accel, MachineState *machine)
{
    io_service_t service;
    io_connect_t connection = IO_OBJECT_NULL;
    IOReturn status;
    int ret;
    size_t output_size;

    AMDVMAccelState *state = (AMDVMAccelState *)accel;

    /* Guest-independent experimental RUN. Selecting this profile enables
     * the VM-scoped API; resource choices remain bounded for this release. */
    if (state->confirm_experimental_run) {
        state->vm_api = true;
    }
    if (state->confirm_experimental_run &&
        (state->diagnostic_only || state->confirm_finite_kick_test ||
         state->confirm_finite_smp_test || state->confirm_unbounded_vmrun ||
         machine->smp.cpus < 1 || machine->smp.cpus > 2 ||
         machine->smp.max_cpus != machine->smp.cpus ||
         machine->ram_size < UINT64_C(256) * 1024 * 1024 ||
         machine->ram_size > UINT64_C(4) * 1024 * 1024 * 1024)) {
        error_report("AMD-SVM experimental RUN supports 1..2 vCPUs, 256 MiB..4 GiB RAM, no CPU hotplug, and no other RUN confirmation");
        return -EINVAL;
    }

    /* The finite ROM remains a separate exact-image safety probe. */
    if (state->confirm_finite_smp_test &&
        (!state->vm_api || state->diagnostic_only ||
         state->confirm_finite_kick_test || state->confirm_unbounded_vmrun ||
         machine->smp.cpus != 2 || machine->smp.max_cpus != 2 ||
         machine->ram_size != UINT64_C(4) * 1024 * 1024 * 1024)) {
        error_report("AMD-SVM finite-SMP mode is restricted to VM API, 2 vCPUs, 2 maxcpus, exactly 4 GiB, and no other RUN confirmation");
        return -EINVAL;
    }
    if (state->validate_finite_smp_rom &&
        (!state->vm_api || !state->diagnostic_only ||
         !state->diagnostic_validate_ram_map ||
         state->confirm_finite_smp_test || state->confirm_finite_kick_test ||
         state->confirm_unbounded_vmrun || machine->smp.cpus != 2 ||
         machine->smp.max_cpus != 2 ||
         machine->ram_size != UINT64_C(4) * 1024 * 1024 * 1024)) {
        error_report("AMD-SVM finite-SMP ROM validation is diagnostic-only and restricted to 2 vCPUs and exactly 4 GiB");
        return -EINVAL;
    }
    /* Diagnostic preparation is unrestricted by RUN policy. Guest execution
     * requires explicit experimental RUN or the separate finite ROM profile. */
    if (state->vm_api && !state->diagnostic_only &&
        machine->smp.cpus > 1 && !state->confirm_finite_smp_test &&
        !state->confirm_experimental_run) {
        error_report("AMD-SVM multi-vCPU execution requires the reviewed finite-SMP ROM profile or explicit experimental RUN profile");
        return -ENOTSUP;
    }
    /* CPU hotplug has no provider create/destroy hook yet. */
    if (state->vm_api && !state->diagnostic_only &&
        machine->smp.max_cpus != machine->smp.cpus) {
        error_report("AMD-SVM CPU hotplug is not supported; set maxcpus equal to cpus");
        return -ENOTSUP;
    }

    if (state->vm_api &&
        ((state->diagnostic_only &&
          (!state->diagnostic_validate_ram_map ||
           state->confirm_unbounded_vmrun || state->confirm_finite_kick_test ||
           state->confirm_finite_smp_test || state->confirm_experimental_run)) ||
         (!state->diagnostic_only &&
          ((!state->confirm_unbounded_vmrun &&
            !state->confirm_finite_kick_test &&
            !state->confirm_finite_smp_test &&
            !state->confirm_experimental_run) ||
           !state->confirm_no_kdp_watchpoints ||
           (state->confirm_unbounded_vmrun &&
            (state->confirm_finite_kick_test ||
             state->confirm_finite_smp_test ||
             state->confirm_experimental_run)) ||
           (state->confirm_finite_kick_test &&
            (state->confirm_finite_smp_test ||
             state->confirm_experimental_run)) ||
           (state->confirm_finite_smp_test && state->confirm_experimental_run))) ||
         machine->smp.cpus < 1 ||
         machine->smp.cpus > AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
         machine->smp.max_cpus > AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
         strcmp(object_get_typename(OBJECT(machine)),
                MACHINE_TYPE_NAME("none")) == 0)) {
        error_report("AMD-SVM vm-api requires PC firmware, 1..64 vCPUs, diagnostic RAM validation for no-RUN mode, or no-KDP-watchpoints plus an allowed finite ROM or explicit unbounded-VMRUN confirmation");
        return -EINVAL;
    }

    if (state->trace_vmexit && !state->confirm_finite_kick_test &&
        !state->confirm_finite_smp_test &&
        !state->confirm_experimental_run &&
        !state->confirm_unbounded_vmrun) {
        error_report("AMD-SVM trace-vmexit=on requires the finite-kick or explicit unbounded-VMRUN confirmation");
        return -EPERM;
    }
    if ((state->confirm_finite_kick_test || state->confirm_finite_smp_test ||
         state->confirm_experimental_run) &&
        state->confirm_unbounded_vmrun) {
        error_report("AMD-SVM finite-kick and unbounded VMRUN confirmations are mutually exclusive");
        return -EINVAL;
    }
    if (state->finite_host_tlb_stress && !state->confirm_finite_kick_test) {
        error_report("AMD-SVM in-process host TLB stress is restricted to the exact finite-kick ROM");
        return -EPERM;
    }
    if (!state->vm_api && !state->confirm_no_kdp_watchpoints) {
        error_report("AMD-SVM requires explicit confirm-no-kdp-watchpoints=on; do not run while KDP hardware watchpoints are active");
        return -EPERM;
    }
    service = IOServiceGetMatchingService(kIOMainPortDefault,
                                           IOServiceMatching("AMDVMProbeService"));
    if (service == IO_OBJECT_NULL) {
        error_report("AMD-SVM provider AMDVMProbeService is not loaded");
        return -ENODEV;
    }

    status = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (status != kIOReturnSuccess) {
        error_report("AMD-SVM provider open failed (IOKit 0x%08x); run QEMU with administrator privilege",
                     status);
        return -EACCES;
    }

    if (strcmp(object_get_typename(OBJECT(machine)),
               MACHINE_TYPE_NAME("none")) == 0) {
        if (state->confirm_finite_kick_test || state->confirm_finite_smp_test ||
            state->confirm_experimental_run ||
            state->trace_vmexit) {
            error_report("AMD-SVM finite-kick mode requires the PC firmware path, not the fixed microguest path");
            IOServiceClose(connection);
            return -EINVAL;
        }
        ret = amdsvm_run_microguest(connection, state->debugcon,
                                    state->diagnostic_only);
        IOServiceClose(connection);
        if (ret == 0) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        return ret;
    }

    if (!state->vm_api && machine->smp.cpus != 1) {
        error_report("AMD-SVM QEMU reset smoke currently requires exactly one vCPU");
        IOServiceClose(connection);
        return -ENOTSUP;
    }
    if (!state->confirm_unbounded_vmrun &&
        !state->confirm_finite_kick_test &&
        !state->confirm_finite_smp_test && !state->confirm_experimental_run &&
        !state->diagnostic_only) {
        error_report("AMD-SVM firmware execution is gated: use diagnostic-only=on, an exact finite test ROM, or the separately reviewed unbounded VMRUN confirmation");
        IOServiceClose(connection);
        return -EPERM;
    }
    output_size = sizeof(state->cpu_state_before);
    if (!state->vm_api && (amdsvm_call(connection, AMDVM_PROBE_SELECTOR_QUERY,
                    NULL, 0, &state->cpu_state_before, &output_size) < 0 ||
        output_size != sizeof(state->cpu_state_before) ||
        state->cpu_state_before.abi_version != AMDVM_PROBE_ABI_VERSION)) {
        error_report("AMD-SVM could not capture pre-run host SVM state");
        IOServiceClose(connection);
        return -EIO;
    }
    state->connection = connection;
    if (state->vm_api) {
        if (amdsvm_vm_provider_init(&state->vm_provider, amdsvm_iokit_provider_transport, state) ||
            machine->smp.max_cpus > state->vm_provider.caps.max_vcpus ||
            amdsvm_probe_host_state(&state->cpu_state_before)) {
            error_report("AMD-SVM VM API capability negotiation or host probe failed");
            IOServiceClose(connection); state->connection = IO_OBJECT_NULL; return -ENOTSUP;
        }
        if (!state->diagnostic_only &&
            (state->vm_provider.caps.feature_flags &
             (AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_RUN_V1 |
              AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_KICK_V1)) !=
             (AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_RUN_V1 |
              AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_KICK_V1)) {
            error_report("AMD-SVM VM API provider does not advertise both RUN and KICK");
            IOServiceClose(connection);
            state->connection = IO_OBJECT_NULL;
            return -ENOTSUP;
        }
        state->vm_expected_cpus = machine->smp.cpus;
    }
    amdsvm_state_init_emulator();
    amdsvm_provider_lifecycle_init(&state->provider_lifecycle,
                                   amdsvm_iokit_provider_transport, state);
    qatomic_set(&state->kick_generation, 0);
    qemu_mutex_init(&state->kick_lock);
    state->kick_lock_initialized = true;
    amdsvm_active_state = state;
    return 0;
}

static void amdsvm_accel_finalize(Object *obj)
{
    AMDVMAccelState *state = (AMDVMAccelState *)obj;
    bool provider_context_retained = false;
    bool vm_api_was_prepared = state->vm_api_prepared;

    if (amdsvm_active_state == state) {
        amdsvm_active_state = NULL;
    }
    amdsvm_host_tlb_stress_stop(state);
    if (state->vm_api) {
        if (amdsvm_vm_provider_destroy(&state->vm_provider)) {
            provider_context_retained = true;
        } else {
            state->vm_api_prepared = false;
            state->guest_map_initialized = false;
            if (vm_api_was_prepared) {
                amdvm_probe_result_t after = { 0 };
                if (amdsvm_probe_host_state(&after) ||
                    !amdsvm_cpu_state_restored(&state->cpu_state_before,
                                               &after)) {
                    error_report("AMD-SVM VM API host state did not match its pre-run baseline after destroy");
                }
            }
        }
    }
    if (state->provider_lifecycle.context_may_exist &&
        amdsvm_destroy_vcpu_context(state) != 0) {
        /* Keep the connection alive until process exit; clientClose must not
         * race a failed explicit destroy and free provider-owned backing. */
        provider_context_retained = true;
    }
    if (state->kick_lock_initialized) {
        qemu_mutex_destroy(&state->kick_lock);
        state->kick_lock_initialized = false;
    }
    if (state->connection != IO_OBJECT_NULL && !provider_context_retained) {
        IOServiceClose(state->connection);
        state->connection = IO_OBJECT_NULL;
    }
    if (state->debugcon_frontend_initialized) {
        qemu_chr_fe_deinit(&state->debugcon_frontend, false);
        state->debugcon_frontend_initialized = false;
    }
}

static void amdsvm_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);

    ac->name = "AMD SVM provider (experimental QEMU accelerator)";
    ac->init_machine = amdsvm_init_machine;
    ac->allowed = &amdsvm_allowed;
    object_class_property_add_bool(oc, "confirm-no-kdp-watchpoints",
                                   amdsvm_get_confirm_no_kdp_watchpoints,
                                   amdsvm_set_confirm_no_kdp_watchpoints);
    object_class_property_add_bool(oc, "confirm-unbounded-vmrun",
                                   amdsvm_get_confirm_unbounded_vmrun,
                                   amdsvm_set_confirm_unbounded_vmrun);
    object_class_property_add_bool(oc, "confirm-finite-kick-test",
                                   amdsvm_get_confirm_finite_kick_test,
                                   amdsvm_set_confirm_finite_kick_test);
    object_class_property_add_bool(oc, "confirm-finite-smp-test",
                                   amdsvm_get_confirm_finite_smp_test,
                                   amdsvm_set_confirm_finite_smp_test);
    object_class_property_add_bool(oc, "confirm-experimental-run",
                                   amdsvm_get_confirm_experimental_run,
                                   amdsvm_set_confirm_experimental_run);
    object_class_property_add_bool(oc, "finite-host-tlb-stress",
                                   amdsvm_get_finite_host_tlb_stress,
                                   amdsvm_set_finite_host_tlb_stress);
    object_class_property_add_bool(oc, "trace-vmexit",
                                   amdsvm_get_trace_vmexit,
                                   amdsvm_set_trace_vmexit);
    object_class_property_add_bool(oc, "diagnostic-only",
                                   amdsvm_get_diagnostic_only,
                                   amdsvm_set_diagnostic_only);
    object_class_property_add_bool(oc, "vm-api", amdsvm_get_vm_api, amdsvm_set_vm_api);
    object_class_property_add_bool(oc, "diagnostic-validate-ram-map",
                                   amdsvm_get_diagnostic_validate_ram_map,
                                   amdsvm_set_diagnostic_validate_ram_map);
    object_class_property_add_bool(oc, "validate-finite-smp-rom",
                                   amdsvm_get_validate_finite_smp_rom,
                                   amdsvm_set_validate_finite_smp_rom);
    object_class_property_add_str(oc, "debugcon", amdsvm_get_debugcon,
                                  amdsvm_set_debugcon);
}

static const TypeInfo amdsvm_accel_type = {
    .name = ACCEL_CLASS_NAME("amdsvm"),
    .parent = TYPE_ACCEL,
    .instance_size = sizeof(AMDVMAccelState),
    .instance_finalize = amdsvm_accel_finalize,
    .class_init = amdsvm_accel_class_init,
};

static void amdsvm_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = amdsvm_start_vcpu_thread;
    ops->kick_vcpu_thread = amdsvm_kick_vcpu_thread;
    ops->handle_interrupt = generic_handle_interrupt;
}

static const TypeInfo amdsvm_accel_ops_type = {
    .name = ACCEL_OPS_NAME("amdsvm"),
    .parent = TYPE_ACCEL_OPS,
    .class_init = amdsvm_accel_ops_class_init,
    .abstract = true,
};

static void amdsvm_type_init(void)
{
    type_register_static(&amdsvm_accel_type);
    type_register_static(&amdsvm_accel_ops_type);
}

type_init(amdsvm_type_init);
