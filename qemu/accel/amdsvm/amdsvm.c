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
#include "exec/icount.h"
#include "system/ioport.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "qemu/guest-random.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/processor.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../../svm-kext/amdvm_probe_abi.h"
#include "amdsvm-firmware-dispatch.h"
#include "amdsvm-bql-policy.h"
#include "amdsvm-provider-lifecycle.h"
#include "amdsvm-vm-provider.h"
#include "../../target/i386/amdsvm/amdsvm-memory.h"
#include "../../target/i386/amdsvm/amdsvm-state.h"
#include "amdsvm-native-cr3-policy.h"
#include "../../target/i386/svm.h"

#define AMDSVM_CONTEXT_VMRUN_FAILED (-11)
#define AMDSVM_VMRUN_FPU_NOT_READY (-6)
#define AMDSVM_PREENTRY_FPU_RETRY_LIMIT 3u
#define AMDSVM_VMEXIT_TRACE_INITIAL_LIMIT 64u
#define AMDSVM_VMEXIT_TRACE_SAMPLE_MASK 0xfffu
#define AMDSVM_KERNEL_EVENT_TRACE_LIMIT 4096u
#define AMDSVM_KERNEL_EXCEPTION_TRACE_LIMIT 256u
#define AMDSVM_KERNEL_GPR_TRACE_LIMIT 512u

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

static void amdsvm_format_bytes(char *out, size_t out_size,
                                const uint8_t *bytes, size_t length)
{
    size_t used = 0;

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    for (size_t i = 0; i < length && used < out_size; i++) {
        int written = g_snprintf(out + used, out_size - used,
                                 i == 0 ? "%02x" : ":%02x", bytes[i]);
        if (written < 0 || (size_t)written >= out_size - used) {
            out[out_size - 1] = '\0';
            return;
        }
        used += written;
    }
}

typedef struct AMDVMGuestPageWalk {
    uint64_t entries[4];
    uint64_t gpa;
    int failed_level;
} AMDVMGuestPageWalk;

/* Diagnostic-only guest page-table walk. This uses the CR3 returned by the
 * provider, not CPUX86State's possibly stale software copy. */
static int amdsvm_debug_guest_walk_4level(uint64_t cr3, uint64_t cr4,
                                          uint64_t linear,
                                          AMDVMGuestPageWalk *walk)
{
    static const unsigned shifts[4] = { 39, 30, 21, 12 };
    uint64_t table = cr3 & UINT64_C(0x000ffffffffff000);

    memset(walk, 0, sizeof(*walk));
    walk->failed_level = -1;
    if ((cr4 & (UINT64_C(1) << 12)) != 0) {
        walk->failed_level = -2; /* LA57 needs a five-level walk. */
        return -1;
    }
    for (unsigned level = 0; level < 4; level++) {
        uint64_t raw = 0;
        uint64_t entry_gpa = table + ((linear >> shifts[level]) & 0x1ffu) * 8;
        MemTxResult status = address_space_read(&address_space_memory,
            entry_gpa, MEMTXATTRS_UNSPECIFIED, &raw, sizeof(raw));
        if (status != MEMTX_OK) {
            walk->failed_level = level;
            return -1;
        }
        walk->entries[level] = le64_to_cpu(raw);
        uint64_t entry = walk->entries[level];
        if ((entry & 1u) == 0) {
            walk->failed_level = level;
            return -1;
        }
        if (level == 1 && (entry & (UINT64_C(1) << 7))) {
            walk->gpa = (entry & UINT64_C(0x000fffffc0000000)) |
                        (linear & ((UINT64_C(1) << 30) - 1));
            return 0;
        }
        if (level == 2 && (entry & (UINT64_C(1) << 7))) {
            walk->gpa = (entry & UINT64_C(0x000fffffffe00000)) |
                        (linear & ((UINT64_C(1) << 21) - 1));
            return 0;
        }
        table = entry & UINT64_C(0x000ffffffffff000);
        if (level == 3) {
            walk->gpa = table | (linear & 0xfffu);
            return 0;
        }
    }
    walk->failed_level = 4;
    return -1;
}

static int amdsvm_debug_guest_virtual_read(uint64_t cr3, uint64_t cr4,
                                            uint64_t linear, void *buffer,
                                            size_t length,
                                            AMDVMGuestPageWalk *first_walk)
{
    uint8_t *bytes = buffer;
    bool first = true;

    while (length != 0) {
        AMDVMGuestPageWalk walk;
        size_t chunk;
        if (amdsvm_debug_guest_walk_4level(cr3, cr4, linear, &walk) != 0) {
            if (first_walk && first) {
                *first_walk = walk;
            }
            return -1;
        }
        if (first_walk && first) {
            *first_walk = walk;
        }
        first = false;
        chunk = MIN(length, (size_t)(0x1000 - (walk.gpa & 0xfffu)));
        if (address_space_read(&address_space_memory, walk.gpa,
                MEMTXATTRS_UNSPECIFIED, bytes, chunk) != MEMTX_OK) {
            return -2;
        }
        bytes += chunk;
        linear += chunk;
        length -= chunk;
    }
    return 0;
}

static void amdsvm_log_guest_virtual_read(const char *name, uint64_t linear,
                                           int status,
                                           const AMDVMGuestPageWalk *walk,
                                           const uint8_t *bytes,
                                           size_t length)
{
    char hex[160];
    amdsvm_format_bytes(hex, sizeof(hex), bytes, length);
    error_report("AMDVM_TRIPLE_FAULT_GUEST_VA name=%s linear=0x%llx status=%d level=%d gpa=0x%llx pml4e=0x%llx pdpte=0x%llx pde=0x%llx pte=0x%llx bytes=%s",
        name, (unsigned long long)linear, status,
        walk ? walk->failed_level : -3,
        (unsigned long long)(walk ? walk->gpa : 0),
        (unsigned long long)(walk ? walk->entries[0] : 0),
        (unsigned long long)(walk ? walk->entries[1] : 0),
        (unsigned long long)(walk ? walk->entries[2] : 0),
        (unsigned long long)(walk ? walk->entries[3] : 0), hex);
}

static bool amdsvm_is_transition16big_state(
    const amdvm_x86_guest_state_v1_t *state)
{
    return state->segments[AMDVM_X86_SEG_CS].selector == 0x50;
}

static void amdsvm_trace_transition_state(
    const char *stage, int vcpu,
    const amdvm_x86_guest_state_v1_t *state,
    uint64_t exit_code, uint32_t exit_int_info, uint64_t eventinj)
{
    const amdvm_x86_segment_state_t *cs =
        &state->segments[AMDVM_X86_SEG_CS];
    const amdvm_x86_segment_state_t *ss =
        &state->segments[AMDVM_X86_SEG_SS];
    const amdvm_x86_segment_state_t *ds =
        &state->segments[AMDVM_X86_SEG_DS];
    const amdvm_x86_segment_state_t *es =
        &state->segments[AMDVM_X86_SEG_ES];
    const amdvm_x86_segment_state_t *fs =
        &state->segments[AMDVM_X86_SEG_FS];
    const amdvm_x86_segment_state_t *gs =
        &state->segments[AMDVM_X86_SEG_GS];
    const amdvm_x86_segment_state_t *gdtr =
        &state->segments[AMDVM_X86_SEG_GDTR];
    const amdvm_x86_segment_state_t *idtr =
        &state->segments[AMDVM_X86_SEG_IDTR];
    char eventinj_text[32];

    if (eventinj == UINT64_MAX) {
        g_strlcpy(eventinj_text, "unavailable", sizeof(eventinj_text));
    } else {
        g_snprintf(eventinj_text, sizeof(eventinj_text), "0x%llx",
                   (unsigned long long)eventinj);
    }

    error_report("AMDVM_TRANSITION_STATE stage=%s vcpu=%d exit=0x%llx exit_int_info=0x%x eventinj=%s rip=0x%llx rsp=0x%llx rflags=0x%llx cr0=0x%llx pe=%u interruptibility=0x%x cs=%04x/%04x:%08x:0x%016llx ss=%04x/%04x:%08x:0x%016llx ds=%04x/%04x:%08x:0x%016llx es=%04x/%04x:%08x:0x%016llx fs=%04x/%04x:%08x:0x%016llx gs=%04x/%04x:%08x:0x%016llx gdtr=%04x/%04x:%08x:0x%016llx idtr=%04x/%04x:%08x:0x%016llx",
                 stage, vcpu, (unsigned long long)exit_code, exit_int_info,
                 eventinj_text,
                 (unsigned long long)state->rip,
                 (unsigned long long)state->gpr[AMDSVM_GPR_RSP],
                 (unsigned long long)state->rflags,
                 (unsigned long long)state->cr[0],
                 (unsigned)(state->cr[0] & 1u), state->interruptibility,
                 cs->selector, cs->attributes, cs->limit,
                 (unsigned long long)cs->base,
                 ss->selector, ss->attributes, ss->limit,
                 (unsigned long long)ss->base,
                 ds->selector, ds->attributes, ds->limit,
                 (unsigned long long)ds->base,
                 es->selector, es->attributes, es->limit,
                 (unsigned long long)es->base,
                 fs->selector, fs->attributes, fs->limit,
                 (unsigned long long)fs->base,
                 gs->selector, gs->attributes, gs->limit,
                 (unsigned long long)gs->base,
                 gdtr->selector, gdtr->attributes, gdtr->limit,
                 (unsigned long long)gdtr->base,
                 idtr->selector, idtr->attributes, idtr->limit,
                 (unsigned long long)idtr->base);
}

static void amdsvm_trace_pf_registers(
    const char *stage, int vcpu,
    const amdvm_x86_guest_state_v1_t *state)
{
    error_report("AMDVM_PF_EXIT_REGISTERS stage=%s vcpu=%d cr2=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx",
        stage, vcpu,
        (unsigned long long)state->cr[2],
        (unsigned long long)state->cr[3],
        (unsigned long long)state->cr[4],
        (unsigned long long)state->efer,
        (unsigned long long)state->gpr[AMDSVM_GPR_RAX],
        (unsigned long long)state->gpr[AMDSVM_GPR_RBX],
        (unsigned long long)state->gpr[AMDSVM_GPR_RCX],
        (unsigned long long)state->gpr[AMDSVM_GPR_RDX],
        (unsigned long long)state->gpr[AMDSVM_GPR_RSI],
        (unsigned long long)state->gpr[AMDSVM_GPR_RDI],
        (unsigned long long)state->gpr[AMDSVM_GPR_RBP],
        (unsigned long long)state->gpr[AMDSVM_GPR_RSP],
        (unsigned long long)state->gpr[AMDSVM_GPR_R8],
        (unsigned long long)state->gpr[AMDSVM_GPR_R9],
        (unsigned long long)state->gpr[AMDSVM_GPR_R10],
        (unsigned long long)state->gpr[AMDSVM_GPR_R11],
        (unsigned long long)state->gpr[AMDSVM_GPR_R12],
        (unsigned long long)state->gpr[AMDSVM_GPR_R13],
        (unsigned long long)state->gpr[AMDSVM_GPR_R14],
        (unsigned long long)state->gpr[AMDSVM_GPR_R15]);
}

static AMDVMFaultMemorySection amdsvm_current_memory_section(uint64_t gpa)
{
    AMDVMFaultMemorySection section = { .gpa = gpa };
    MemoryRegionSection mrs;
    MemoryRegion *mr;
    uint64_t ram_offset;
    void *ram_ptr;

    if (gpa == UINT64_MAX) {
        return section;
    }

    /*
     * NPF diagnostics call this helper on every fault.  Walking every
     * FlatView range made a late MMIO GPA such as HPET/APIC scan the whole
     * memory map for each exit.  memory_region_find() uses FlatView lookup
     * to select the covering range directly and returns the rest of that
     * mapping from @gpa, which is also the bound needed by ROM shadow copies.
     */
    mrs = memory_region_find(get_system_memory(), gpa, UINT64_MAX - gpa);
    mr = mrs.mr;
    if (!mr) {
        return section;
    }
    if (int128_gethi(mrs.size) != 0 || int128_getlo(mrs.size) == 0) {
        memory_region_unref(mr);
        return section;
    }

    section.found = true;
    section.range_gpa = mrs.offset_within_address_space;
    section.size = int128_getlo(mrs.size);
    section.name = memory_region_name(mr);
    section.is_ram = memory_region_is_ram(mr);
    section.is_rom = memory_region_is_rom(mr);
    if (section.is_ram && mrs.offset_within_region <= UINT64_MAX -
            (gpa - section.range_gpa)) {
        ram_offset = mrs.offset_within_region + (gpa - section.range_gpa);
        ram_ptr = memory_region_get_ram_ptr(mr);
        if (ram_ptr && (uintptr_t)ram_ptr <= UINT64_MAX - ram_offset) {
            section.hva = (uint64_t)(uintptr_t)ram_ptr + ram_offset;
        }
    }
    memory_region_unref(mr);
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

#define AMDSVM_PERF_EXIT_CODES 0x1000u
#define AMDSVM_BQL_SPIN_BUDGET 128u

typedef struct AMDVMPerfVCPU {
    uint64_t runs;
    uint64_t run_us;
    uint64_t provider_call_us;
    uint64_t bql_reacquire_us;
    uint64_t bql_spin_attempts;
    uint64_t bql_spin_acquired;
    uint64_t bql_spin_fallback;
    uint64_t state_load_us;
    uint64_t state_store_us;
    uint64_t dispatch_us;
    uint64_t exit_count[AMDSVM_PERF_EXIT_CODES];
    uint64_t exit_us[AMDSVM_PERF_EXIT_CODES];
    uint64_t npf_hpet;
    uint64_t npf_lapic;
    uint64_t npf_other;
} AMDVMPerfVCPU;

typedef struct AMDVMPerfStats {
    AMDVMPerfVCPU vcpu[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
} AMDVMPerfStats;

struct AMDVMAccelState {
    AccelState parent_obj;
    bool confirm_no_kdp_watchpoints;
    bool confirm_unbounded_vmrun;
    bool confirm_finite_kick_test;
    bool confirm_finite_smp_test;
    bool confirm_experimental_run;
    bool finite_host_tlb_stress;
    bool perf_stats;
    AMDVMPerfStats *perf_stats_data;
    VMChangeStateEntry *perf_state_entry;
    bool trace_vmexit;
    bool diagnostic_cr0_window_trace;
    bool diagnostic_transition_irq_gate;
    bool diagnostic_gp_intercept;
    bool diagnostic_first_pf;
    bool diagnostic_only;
    bool diagnostic_finished;
    bool diagnostic_validate_ram_map;
    bool validate_finite_smp_rom;
    bool vm_api;
    bool bql_spin_enabled;
    bool mtrr_cache_enabled;
    bool provider_pause_enabled;
    bool shared_run_enabled;
    bool native_rdtsc_enabled;
    bool native_cr3_enabled;
    uint32_t host_phys_bits;
    bool vm_api_prepared;
    bool vm_api_prepare_failed;
    bool vm_api_fpu_preflight_done;
    unsigned vm_expected_cpus;
    AMDVMVMProvider vm_provider;
    mach_vm_address_t shared_run_mapping_address;
    mach_vm_size_t shared_run_mapping_size;
    bool shared_run_mapped;
    uint64_t vm_kick_generation[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    char *debugcon;
    CharFrontend debugcon_frontend;
    bool debugcon_frontend_initialized;
    io_connect_t connection;
    AMDVMProviderLifecycle provider_lifecycle;
    bool provider_prepare_failed;
    bool guest_map_initialized;
    bool provider_halted[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    bool diagnostic_gp_rearm_pending[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    unsigned diagnostic_gp_count[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    AMDVMGuestMemoryMap guest_map;
    uint64_t guest_page_count;
    bool memory_types_published[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    AMDVMMTRRState published_mtrr[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint64_t kick_generation;
    uint64_t trace_run_attempts;
    uint64_t trace_vcpu_run_attempts[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint64_t kernel_gpr_trace_count[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint64_t transition_event_trace_count[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint64_t kernel_event_trace_count[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint64_t kernel_exception_trace_count[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    uint64_t transition_irq_gate_count[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
    QemuMutex kick_lock;
    bool kick_lock_initialized;
    QemuMutex vm_context_lock;
    bool vm_context_lock_initialized;
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

static inline bool amdsvm_perf_enabled(const AMDVMAccelState *state)
{
    return state && state->perf_stats_data;
}

static bool amdsvm_bql_try_acquire(void *opaque)
{
    (void)opaque;
    return bql_trylock();
}

static void amdsvm_bql_relax(void *opaque)
{
    (void)opaque;
    cpu_relax();
}

static inline void amdsvm_perf_record_state_load(AMDVMAccelState *state,
                                                   int vcpu, int64_t us)
{
    if (amdsvm_perf_enabled(state) && vcpu >= 0 &&
        vcpu < AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
        qatomic_fetch_add(&state->perf_stats_data->vcpu[vcpu].state_load_us,
                          (uint64_t)us);
    }
}

static inline void amdsvm_perf_record_state_store(AMDVMAccelState *state,
                                                    int vcpu, int64_t us)
{
    if (amdsvm_perf_enabled(state) && vcpu >= 0 &&
        vcpu < AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
        qatomic_fetch_add(&state->perf_stats_data->vcpu[vcpu].state_store_us,
                          (uint64_t)us);
    }
}

static inline void amdsvm_perf_record_dispatch(AMDVMAccelState *state,
                                                int vcpu, int64_t us)
{
    if (amdsvm_perf_enabled(state) && vcpu >= 0 &&
        vcpu < AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
        qatomic_fetch_add(&state->perf_stats_data->vcpu[vcpu].dispatch_us,
                          (uint64_t)us);
    }
}

static inline void amdsvm_perf_record_run(AMDVMAccelState *state, int vcpu,
                                          uint64_t exit_code, int64_t us,
                                          int64_t provider_call_us,
                                          int64_t bql_reacquire_us,
                                          const AMDVMBQLSpinResult *bql_spin,
                                          uint64_t exit_gpa)
{
    AMDVMPerfVCPU *perf;

    if (!amdsvm_perf_enabled(state) || vcpu < 0 ||
        vcpu >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
        return;
    }
    perf = &state->perf_stats_data->vcpu[vcpu];
    qatomic_fetch_add(&perf->runs, 1);
    qatomic_fetch_add(&perf->run_us, (uint64_t)us);
    qatomic_fetch_add(&perf->provider_call_us,
                      (uint64_t)provider_call_us);
    qatomic_fetch_add(&perf->bql_reacquire_us,
                      (uint64_t)bql_reacquire_us);
    if (bql_spin) {
        qatomic_fetch_add(&perf->bql_spin_attempts, bql_spin->attempts);
        qatomic_fetch_add(&perf->bql_spin_acquired, bql_spin->acquired);
        qatomic_fetch_add(&perf->bql_spin_fallback, !bql_spin->acquired);
    }
    if (exit_code < AMDSVM_PERF_EXIT_CODES) {
        qatomic_fetch_add(&perf->exit_count[exit_code], 1);
        qatomic_fetch_add(&perf->exit_us[exit_code], (uint64_t)us);
    }
    if (exit_code == SVM_EXIT_NPF) {
        uint64_t page = exit_gpa & ~0xfffull;
        if (page == 0xfed00000ull) {
            perf->npf_hpet++;
        } else if (page == 0xfee00000ull) {
            perf->npf_lapic++;
        } else {
            perf->npf_other++;
        }
    }
}

static void amdsvm_perf_dump(AMDVMAccelState *state, const char *reason)
{
    const char *path;
    char *default_path = NULL;
    FILE *file;
    uint64_t *counts;
    uint64_t *times;
    uint64_t runs = 0;
    uint64_t run_us = 0;
    uint64_t provider_call_us = 0;
    uint64_t bql_reacquire_us = 0;
    uint64_t bql_spin_attempts = 0;
    uint64_t bql_spin_acquired = 0;
    uint64_t bql_spin_fallback = 0;
    uint64_t npf_hpet = 0;
    uint64_t npf_lapic = 0;
    uint64_t npf_other = 0;
    uint64_t state_load_us = 0;
    uint64_t state_store_us = 0;
    uint64_t dispatch_us = 0;
    unsigned i;

    if (!amdsvm_perf_enabled(state)) {
        return;
    }
    path = getenv("AMDVM_PERF_STATS_PATH");
    if (!path || !*path) {
        default_path = g_strdup_printf("/tmp/amdsvm-perf-%ld.txt",
                                       (long)getpid());
        path = default_path;
    }
    file = fopen(path, "w");
    if (!file) {
        error_report("AMDVM_PERF_STATS could not open %s: %s",
                     path, strerror(errno));
        g_free(default_path);
        return;
    }

    counts = g_new0(uint64_t, AMDSVM_PERF_EXIT_CODES);
    times = g_new0(uint64_t, AMDSVM_PERF_EXIT_CODES);
    for (i = 0; i < AMDVM_VM_ABI_DRAFT_MAX_VCPUS; i++) {
        AMDVMPerfVCPU *perf = &state->perf_stats_data->vcpu[i];
        unsigned code;

        runs += qatomic_read(&perf->runs);
        run_us += qatomic_read(&perf->run_us);
        provider_call_us += qatomic_read(&perf->provider_call_us);
        bql_reacquire_us += qatomic_read(&perf->bql_reacquire_us);
        bql_spin_attempts += qatomic_read(&perf->bql_spin_attempts);
        bql_spin_acquired += qatomic_read(&perf->bql_spin_acquired);
        bql_spin_fallback += qatomic_read(&perf->bql_spin_fallback);
        npf_hpet += perf->npf_hpet;
        npf_lapic += perf->npf_lapic;
        npf_other += perf->npf_other;
        state_load_us += qatomic_read(&perf->state_load_us);
        state_store_us += qatomic_read(&perf->state_store_us);
        dispatch_us += qatomic_read(&perf->dispatch_us);
        for (code = 0; code < AMDSVM_PERF_EXIT_CODES; code++) {
            counts[code] += qatomic_read(&perf->exit_count[code]);
            times[code] += qatomic_read(&perf->exit_us[code]);
        }
    }

    fprintf(file, "amdsvm_perf version=1 pid=%ld reason=%s\n",
            (long)getpid(), reason ? reason : "unknown");
    fprintf(file,
            "totals runs=%" PRIu64 " run_us=%" PRIu64
            " provider_call_us=%" PRIu64 " bql_reacquire_us=%" PRIu64
            " bql_spin_attempts=%" PRIu64 " bql_spin_acquired=%" PRIu64
            " bql_spin_fallback=%" PRIu64
            " npf_hpet=%" PRIu64 " npf_lapic=%" PRIu64
            " npf_other=%" PRIu64
            " state_load_us=%" PRIu64 " state_store_us=%" PRIu64
            " dispatch_us=%" PRIu64 "\n",
            runs, run_us, provider_call_us, bql_reacquire_us,
            bql_spin_attempts, bql_spin_acquired, bql_spin_fallback,
            npf_hpet, npf_lapic, npf_other,
            state_load_us, state_store_us, dispatch_us);
    for (i = 0; i < AMDVM_VM_ABI_DRAFT_MAX_VCPUS; i++) {
        AMDVMPerfVCPU *perf = &state->perf_stats_data->vcpu[i];

        if (!qatomic_read(&perf->runs)) {
            continue;
        }
        fprintf(file,
                "vcpu id=%u runs=%" PRIu64 " run_us=%" PRIu64
                " provider_call_us=%" PRIu64
                " bql_reacquire_us=%" PRIu64
                " bql_spin_attempts=%" PRIu64
                " bql_spin_acquired=%" PRIu64
                " bql_spin_fallback=%" PRIu64
                " state_load_us=%" PRIu64 " state_store_us=%" PRIu64
                " dispatch_us=%" PRIu64 "\n",
                i, qatomic_read(&perf->runs),
                qatomic_read(&perf->run_us),
                qatomic_read(&perf->provider_call_us),
                qatomic_read(&perf->bql_reacquire_us),
                qatomic_read(&perf->bql_spin_attempts),
                qatomic_read(&perf->bql_spin_acquired),
                qatomic_read(&perf->bql_spin_fallback),
                qatomic_read(&perf->state_load_us),
                qatomic_read(&perf->state_store_us),
                qatomic_read(&perf->dispatch_us));
    }
    for (i = 0; i < AMDSVM_PERF_EXIT_CODES; i++) {
        if (!counts[i]) {
            continue;
        }
        fprintf(file,
                "exit code=0x%03x count=%" PRIu64 " run_us=%" PRIu64
                " avg_us=%.2f\n",
                i, counts[i], times[i],
                (double)times[i] / (double)counts[i]);
    }
    fclose(file);
    error_report("AMDVM_PERF_STATS path=%s reason=%s runs=%" PRIu64
                 " run_us=%" PRIu64
                 " provider_call_us=%" PRIu64
                 " bql_reacquire_us=%" PRIu64
                 " state_load_us=%" PRIu64
                 " state_store_us=%" PRIu64 " dispatch_us=%" PRIu64,
                 path, reason ? reason : "unknown", runs, run_us,
                 provider_call_us, bql_reacquire_us,
                 state_load_us, state_store_us, dispatch_us);
    g_free(times);
    g_free(counts);
    g_free(default_path);
}

static void amdsvm_perf_vm_state_change(void *opaque, bool running,
                                        RunState state)
{
    AMDVMAccelState *accel = opaque;

    (void)state;
    if (!running) {
        if (accel->vm_api &&
            (accel->vm_provider.caps.feature_flags &
             AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1)) {
            int rc = amdsvm_vm_provider_report_pause_stats(&accel->vm_provider);
            if (rc && rc != -EAGAIN) {
                error_report("AMD-SVM PAUSE counters could not be reported on VM stop (%d)",
                             rc);
            }
        }
        amdsvm_perf_dump(accel, "vm_stop");
    }
}

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

static bool amdsvm_get_perf_stats(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->perf_stats;
}

static void amdsvm_set_perf_stats(Object *obj, bool value, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->perf_stats = value;
}

static bool amdsvm_get_diagnostic_cr0_window_trace(Object *obj,
                                                    Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->diagnostic_cr0_window_trace;
}

static void amdsvm_set_diagnostic_cr0_window_trace(Object *obj, bool value,
                                                    Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->diagnostic_cr0_window_trace = value;
}

static bool amdsvm_cr0_window_contains(
    const amdvm_x86_guest_state_v1_t *state)
{
    return state && state->rip >= 0x8a0 && state->rip <= 0x8ef;
}

static void amdsvm_trace_cr0_window(const char *stage, int vcpu,
                                    const amdvm_x86_guest_state_v1_t *state,
                                    uint64_t exit_code,
                                    const char *entry_status)
{
    if (!state) {
        return;
    }
    error_report("AMDVM_CR0_WINDOW stage=%s vcpu=%d rip=0x%llx cr0=0x%llx pe=%u exit=0x%llx vmcb_entry=%s",
                 stage, vcpu, (unsigned long long)state->rip,
                 (unsigned long long)state->cr[0],
                 (unsigned)(state->cr[0] & 1u),
                 (unsigned long long)exit_code, entry_status);
}

static bool amdsvm_get_diagnostic_transition_irq_gate(Object *obj,
                                                       Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->diagnostic_transition_irq_gate;
}

static void amdsvm_set_diagnostic_transition_irq_gate(Object *obj, bool value,
                                                       Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->diagnostic_transition_irq_gate = value;
}

static bool amdsvm_get_diagnostic_gp_intercept(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->diagnostic_gp_intercept;
}

static void amdsvm_set_diagnostic_gp_intercept(Object *obj, bool value,
                                               Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->diagnostic_gp_intercept = value;
}

static bool amdsvm_get_diagnostic_first_pf(Object *obj, Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    return s->diagnostic_first_pf;
}

static void amdsvm_set_diagnostic_first_pf(Object *obj, bool value,
                                           Error **errp)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    (void)errp;
    s->diagnostic_first_pf = value;
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

static bool amdsvm_get_bql_spin(Object *obj, Error **errp)
{
    (void)errp;
    return ((AMDVMAccelState *)obj)->bql_spin_enabled;
}

static void amdsvm_set_bql_spin(Object *obj, bool value, Error **errp)
{
    (void)errp;
    ((AMDVMAccelState *)obj)->bql_spin_enabled = value;
}

static bool amdsvm_get_mtrr_cache(Object *obj, Error **errp)
{
    (void)errp;
    return ((AMDVMAccelState *)obj)->mtrr_cache_enabled;
}

static void amdsvm_set_mtrr_cache(Object *obj, bool value, Error **errp)
{
    (void)errp;
    ((AMDVMAccelState *)obj)->mtrr_cache_enabled = value;
}

static bool amdsvm_get_provider_pause(Object *obj, Error **errp)
{
    (void)errp;
    return ((AMDVMAccelState *)obj)->provider_pause_enabled;
}

static void amdsvm_set_provider_pause(Object *obj, bool value, Error **errp)
{
    (void)errp;
    ((AMDVMAccelState *)obj)->provider_pause_enabled = value;
}

static bool amdsvm_get_shared_run(Object *obj, Error **errp)
{
    (void)errp;
    return ((AMDVMAccelState *)obj)->shared_run_enabled;
}

static void amdsvm_set_shared_run(Object *obj, bool value, Error **errp)
{
    (void)errp;
    ((AMDVMAccelState *)obj)->shared_run_enabled = value;
}

static bool amdsvm_get_native_rdtsc(Object *obj, Error **errp)
{
    (void)errp;
    return ((AMDVMAccelState *)obj)->native_rdtsc_enabled;
}

static void amdsvm_set_native_rdtsc(Object *obj, bool value, Error **errp)
{
    (void)errp;
    ((AMDVMAccelState *)obj)->native_rdtsc_enabled = value;
}

static bool amdsvm_get_native_cr3(Object *obj, Error **errp)
{
    (void)errp;
    return ((AMDVMAccelState *)obj)->native_cr3_enabled;
}

static void amdsvm_set_native_cr3(Object *obj, bool value, Error **errp)
{
    (void)errp;
    ((AMDVMAccelState *)obj)->native_cr3_enabled = value;
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

/* Permit the pre-Alpine hardware stop experiment only with this exact ROM. */
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
                                     cpu->cpu_index, generation, &generation);
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
    bool defer_external_irq_completion;
} AMDVMFirmwareDispatchContext;

static int amdsvm_firmware_store_state(
    void *opaque, const amdvm_x86_guest_state_v1_t *state)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    int64_t started_us = amdsvm_perf_enabled(ctx->accel_state) ?
        g_get_monotonic_time() : 0;
    int ret = amdsvm_state_store(ctx->cpu, state);

    if (started_us) {
        amdsvm_perf_record_state_store(ctx->accel_state, ctx->cpu->cpu_index,
                                       g_get_monotonic_time() - started_us);
    }
    return ret;
}

static int amdsvm_firmware_complete_event(void *opaque,
                                          uint64_t exit_code,
                                          uint32_t exit_int_info,
                                          amdvm_x86_guest_state_v1_t *state)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;

    /* A transition-window diagnostic can defer a pending external IRQ
     * without acknowledging or dropping it. Keep it queued until a later
     * RUN actually offers the event to the processor. */
    if (ctx->defer_external_irq_completion) {
        return 0;
    }

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

static int amdsvm_firmware_rdtscp(void *opaque, uint64_t *value,
                                  uint32_t *aux)
{
    AMDVMFirmwareDispatchContext *ctx = opaque;
    int ret;

    ret = amdsvm_state_read_tsc(ctx->cpu, value);
    if (ret == 0) {
        ret = amdsvm_state_read_tsc_aux(ctx->cpu, aux);
    }
    return ret;
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
    amdvm_x86_guest_state_v1_t qemu_before = { 0 };
    amdvm_x86_guest_state_v1_t qemu_after = { 0 };
    bool trace_cr0_write = ctx && ctx->accel_state &&
        ctx->accel_state->diagnostic_cr0_window_trace &&
        result && result->exit_code == SVM_EXIT_WRITE_CR0 &&
        (amdsvm_cr0_window_contains(&result->state) ||
         (result->next_rip >= 0x8a0 && result->next_rip <= 0x8ef));
    if (trace_cr0_write) {
        (void)amdsvm_state_load(ctx->cpu, &qemu_before);
    }

    if (!amdsvm_handle_cr_exit(ctx->cpu, result->exit_code,
                               result->exit_info1, result->next_rip,
                               &event_injection, &event_error_code, &error)) {
        if (error) {
            error_report_err(error);
        }
        return AMDVM_FIRMWARE_DISPATCH_ERROR;
    }
    if (trace_cr0_write) {
        if (amdsvm_state_load(ctx->cpu, &qemu_after) != 0) {
            error_report("AMDVM_CR0_WINDOW stage=CR0_WRITE vcpu=%d rip=0x%llx next_rip=0x%llx before_cr0=0x%llx before_pe=%u after_cr0=unavailable after_pe=unavailable qemu_state=unavailable vmcb_entry=unavailable",
                         ctx->cpu->cpu_index,
                         (unsigned long long)result->state.rip,
                         (unsigned long long)result->next_rip,
                         (unsigned long long)qemu_before.cr[0],
                         (unsigned)(qemu_before.cr[0] & 1u));
        } else {
            error_report("AMDVM_CR0_WINDOW stage=CR0_WRITE vcpu=%d rip=0x%llx next_rip=0x%llx vmexit_cr0=0x%llx vmexit_pe=%u qemu_before_cr0=0x%llx qemu_before_pe=%u qemu_after_cr0=0x%llx qemu_after_pe=%u vmcb_entry=unavailable",
                         ctx->cpu->cpu_index,
                         (unsigned long long)result->state.rip,
                         (unsigned long long)result->next_rip,
                         (unsigned long long)result->state.cr[0],
                         (unsigned)(result->state.cr[0] & 1u),
                         (unsigned long long)qemu_before.cr[0],
                         (unsigned)(qemu_before.cr[0] & 1u),
                         (unsigned long long)qemu_after.cr[0],
                         (unsigned)(qemu_after.cr[0] & 1u));
        }
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
    /* This is a physical host interrupt (for example an XNU IPI), not a
     * guest EVENTINJ. Always service QEMU and provider async events before
     * re-entering the guest. Deferring this path can starve host work. */
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
        if (cpu->cpu_index < 0) {
            error_report("AMD-SVM VM API invalid CPU index %d", cpu->cpu_index);
            goto failed;
        }
        /* APs are initialized on their first SIPI/run, not during BSP setup.
         * Early SeaBIOS map updates otherwise rebuild every selected vCPU's
         * pinned-memory/NPT context before the guest has started those APs. */
        if (cpu->cpu_index != 0) {
            continue;
        }
        if (amdsvm_vm_provider_create(&state->vm_provider, cpu->cpu_index,
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
    if (prepared != 1) {
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

/* Prepare an AP only when QEMU has made it runnable (typically after SIPI).
 * The VM map is already committed; context allocation and NPT construction
 * are serialized against sibling creation and map refresh. */
static int amdsvm_vm_api_ensure_vcpu(AMDVMAccelState *state, CPUState *cpu)
{
    amdvm_x86_guest_state_v1_t snapshot = {0};
    amdvm_vm_abi_draft_vcpu_diagnostic_result_t diagnostic = {0};
    amdvm_vcpu_set_memory_types_request_t memory_types = {0};
    AMDVMMTRRState mtrr;
    uint32_t required_npt_flags =
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ENABLED |
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ROOT_MATCH |
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_TRANSLATES |
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_BACKING_MATCH |
        AMDVM_VM_ABI_DRAFT_VCPU_DIAG_APIC_UNMAPPED;
    unsigned id;
    int rc = -EIO;

    if (!state || !cpu || !state->vm_api_prepared ||
        cpu->cpu_index < 0 ||
        cpu->cpu_index >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
        return -EINVAL;
    }
    id = cpu->cpu_index;
    qemu_mutex_lock(&state->vm_context_lock);
    if (qatomic_read(&state->vm_provider.cpus[id])) {
        rc = 0;
        goto out;
    }
    if (amdsvm_vm_provider_create(&state->vm_provider, id,
                                  amdsvm_state_apic_id(cpu)) ||
        amdsvm_state_load(cpu, &snapshot) ||
        amdsvm_vm_provider_state_roundtrip(&state->vm_provider, id,
                                           &snapshot)) {
        error_report("AMD-SVM VM API CPU %u lazy context/state preparation failed", id);
        goto out;
    }
    amdsvm_state_get_mtrr(cpu, &mtrr);
    Error *err = NULL;
    if (!amdsvm_build_memory_type_request(&state->guest_map, &mtrr,
            AMDVM_GUEST_MTRR_COMPAT_ALL_WB, &memory_types, &err)) {
        error_report_err(err);
        goto failed_vcpu;
    }
    if (amdsvm_vm_provider_set_memory_types(&state->vm_provider, id,
                                            &memory_types) ||
        amdsvm_vm_provider_diagnostic(&state->vm_provider, id,
                                      &diagnostic) != 0 ||
        diagnostic.context_status != 0 ||
        (diagnostic.flags & required_npt_flags) != required_npt_flags ||
        diagnostic.reset_translation_status !=
            AMDVM_VM_ABI_DRAFT_NPT_STATUS_MAPPED ||
        diagnostic.apic_translation_status !=
            AMDVM_VM_ABI_DRAFT_NPT_STATUS_UNMAPPED ||
        memcmp(diagnostic.snapshot.entry_bytes,
               state->guest_map.reset_rom_page + AMDVM_VCPU_RESET_ROM_OFFSET,
               sizeof(diagnostic.snapshot.entry_bytes)) != 0) {
        error_report("AMD-SVM VM API CPU %u lazy NPT preflight failed; VMRUN was not called",
                     id);
        goto failed_vcpu;
    }
    state->published_mtrr[id] = mtrr;
    state->memory_types_published[id] = true;
    error_report("AMD-SVM VM API CPU %u AP context prepared on demand", id);
    rc = 0;
    goto out;

failed_vcpu:
    if (amdsvm_vm_provider_destroy_vcpu(&state->vm_provider, id) != 0) {
        error_report("AMD-SVM VM API CPU %u failed lazy context cleanup; connection retained",
                     id);
    }
out:
    qemu_mutex_unlock(&state->vm_context_lock);
    return rc;
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
    bool native_rdtsc = state->native_rdtsc_enabled && !icount_enabled() &&
        (state->vm_provider.caps.feature_flags &
         AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_RDTSC_V1);
    bool native_cr3 = state->native_cr3_enabled &&
        (state->vm_provider.caps.feature_flags &
         AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_CR3_V1) &&
        amdsvm_native_cr3_policy_safe(amdsvm_state_phys_bits(cpu),
            state->host_phys_bits, run->state.cr[4]);
    uint64_t tsc_offset = 0;
    if (native_rdtsc && amdsvm_state_get_native_tsc_offset(cpu,
                                                           &tsc_offset) != 0) {
        native_rdtsc = false;
    }
    rc = amdsvm_vm_provider_run_ex(&state->vm_provider, cpu->cpu_index,
        run->kick_generation, run->event_injection,
        run->event_injection_error_code, native_rdtsc, tsc_offset,
        native_cr3, &run->state, &vm_result);
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
    int64_t refresh_started_us = g_get_monotonic_time();
    int64_t pause_done_us = 0, ranges_done_us = 0, destroy_done_us = 0;
    int64_t replace_done_us = 0, create_done_us = 0, kick_done_us = 0;
    int64_t provider_create_us = 0, state_roundtrip_us = 0;
    int64_t set_memory_types_us = 0;

    if (!state || !state->vm_api || !state->vm_api_prepared || !current ||
        !updated_map || !updated_map->reset_rom_page_present ||
        !state->guest_map.reset_rom_page_present ||
        memcmp(state->guest_map.reset_rom_page, updated_map->reset_rom_page,
               sizeof(updated_map->reset_rom_page))) {
        error_report("AMD-SVM map refresh requires an unchanged reset-ROM page");
        return -ENOTSUP;
    }
    /* Map replacement is expensive and happens only when FlatView meaning
     * changed at an IOIO boundary. Capture both sides here to identify which
     * GPA/HVA/permission delta made the refresh necessary. */
    amdsvm_trace_smp_memory_map("refresh-old", &state->guest_map);
    amdsvm_trace_smp_memory_map("refresh-new", updated_map);
    rc = amdsvm_vm_api_pause_siblings(state, current, paused);
    if (rc) {
        error_report("AMD-SVM could not drain sibling vCPUs within 5 seconds");
        return rc;
    }
    pause_done_us = g_get_monotonic_time();
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
    ranges_done_us = g_get_monotonic_time();
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
    destroy_done_us = g_get_monotonic_time();
    rc = amdsvm_vm_provider_replace_map(&state->vm_provider, ranges, range_count);
    if (rc) {
        error_report("AMD-SVM provider rejected replacement map (%d)", rc);
        goto out;
    }
    replace_done_us = g_get_monotonic_time();
    state->guest_map = provider_map;
    state->guest_page_count = updated_map->total_ram / AMDSVM_GUEST_PAGE_SIZE;
    if (state->trace_vmexit) {
        amdsvm_trace_smp_memory_map("refresh", &provider_map);
    }
    CPU_FOREACH(cpu) {
        AMDVMMTRRState mtrr;
        g_autofree amdvm_x86_guest_state_v1_t *snapshot =
            g_new0(amdvm_x86_guest_state_v1_t, 1);
        amdvm_vcpu_set_memory_types_request_t memory_types = {0};
        int id = cpu->cpu_index;
        int64_t phase_start_us;
        if (id < 0 || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
            error_report("AMD-SVM invalid vCPU index %d after map replace", id);
            rc = -EIO;
            goto out;
        }
        phase_start_us = g_get_monotonic_time();
        if (amdsvm_vm_provider_create(&state->vm_provider, id,
                                      amdsvm_state_apic_id(cpu))) {
            error_report("AMD-SVM failed to recreate vCPU %d after map replace", id);
            rc = -EIO;
            goto out;
        }
        provider_create_us += g_get_monotonic_time() - phase_start_us;
        phase_start_us = g_get_monotonic_time();
        if (amdsvm_state_load(cpu, snapshot) ||
            amdsvm_vm_provider_state_roundtrip(&state->vm_provider, id,
                                                snapshot)) {
            error_report("AMD-SVM failed to restore vCPU %d state after map replace", id);
            rc = -EIO;
            goto out;
        }
        state_roundtrip_us += g_get_monotonic_time() - phase_start_us;
        amdsvm_state_get_mtrr(cpu, &mtrr);
        if (!amdsvm_build_memory_type_request(&state->guest_map, &mtrr,
                AMDVM_GUEST_MTRR_COMPAT_ALL_WB, &memory_types, &err)) {
            error_report_err(err);
            rc = -ENOTSUP;
            goto out;
        }
        phase_start_us = g_get_monotonic_time();
        if (amdsvm_vm_provider_set_memory_types(&state->vm_provider, id,
                                                &memory_types)) {
            error_report("AMD-SVM failed to restore vCPU %d NPT memory types", id);
            rc = -EIO;
            goto out;
        }
        set_memory_types_us += g_get_monotonic_time() - phase_start_us;
        state->published_mtrr[id] = mtrr;
        state->memory_types_published[id] = true;
    }
    create_done_us = g_get_monotonic_time();
    /*
     * Recreated provider vCPUs have fresh kick-generation state. Re-latch a
     * new kick for each one before resuming workers, otherwise the first RUN
     * can be rejected forever as stale and spin at the reset vector.
     */
    CPU_FOREACH(cpu) {
        int id = cpu->cpu_index;
        uint64_t generation_before;

        if (id < 0 || id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
            !qatomic_read(&state->vm_provider.cpus[id])) {
            continue;
        }
        generation_before = qatomic_read(&state->vm_kick_generation[id]);
        amdsvm_kick_vcpu_thread(cpu);
        if (qatomic_read(&state->vm_kick_generation[id]) ==
            generation_before) {
            error_report("AMD-SVM failed to synchronize recreated vCPU %d kick generation",
                         id);
            rc = -EIO;
            goto out;
        }
    }
    kick_done_us = g_get_monotonic_time();
    resume_siblings = true;
    error_report("AMD-SVM VM RAM map replaced generation=%llu ranges=%u bytes=%llu",
                 (unsigned long long)state->vm_provider.generation, range_count,
                 (unsigned long long)updated_map->total_ram);
    rc = 0;
out:
    if (resume_siblings) {
        amdsvm_vm_api_resume_siblings(current, paused);
    }
    int64_t pause_us = pause_done_us ?
        pause_done_us - refresh_started_us : -1;
    int64_t ranges_us = (pause_done_us && ranges_done_us) ?
        ranges_done_us - pause_done_us : -1;
    int64_t destroy_us = (ranges_done_us && destroy_done_us) ?
        destroy_done_us - ranges_done_us : -1;
    int64_t replace_us = (destroy_done_us && replace_done_us) ?
        replace_done_us - destroy_done_us : -1;
    int64_t create_us = (replace_done_us && create_done_us) ?
        create_done_us - replace_done_us : -1;
    int64_t kick_us = (create_done_us && kick_done_us) ?
        kick_done_us - create_done_us : -1;
    int64_t total_us = g_get_monotonic_time() - refresh_started_us;
    error_report("AMDVM_VM_REFRESH_TIMING vcpu=%d ranges=%u pause_us=%lld ranges_us=%lld destroy_us=%lld replace_us=%lld create_us=%lld provider_create_us=%lld state_roundtrip_us=%lld set_memory_types_us=%lld kick_us=%lld total_us=%lld rc=%d",
                 current->cpu_index, range_count,
                 (long long)pause_us, (long long)ranges_us,
                 (long long)destroy_us, (long long)replace_us,
                 (long long)create_us,
                 (long long)provider_create_us,
                 (long long)state_roundtrip_us,
                 (long long)set_memory_types_us,
                 (long long)kick_us,
                 (long long)total_us, rc);
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
        .emulate_rdtscp = amdsvm_firmware_rdtscp,
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
    uint64_t diagnostic_pf_reinjected = 0;
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
        if (!amdsvm_memory_map_equal(&guest_map, &accel_state->guest_map)) {
            ret = amdsvm_vm_api_refresh_map(accel_state, cpu, &guest_map);
            if (ret) {
                goto out;
            }
        }
        /* The VM-wide preparation creates only the BSP.  AP contexts are
         * deliberately lazy, so create and initialize this vCPU after its
         * QEMU reset/SIPI state is ready and before the run loop publishes
         * per-vCPU MTRRs or attempts RUN. */
        if (amdsvm_vm_api_ensure_vcpu(accel_state, cpu) != 0) {
            error_report("AMD-SVM VM API CPU %d context preparation failed before RUN",
                         cpu->cpu_index);
            ret = -EIO;
            goto out;
        }
    } else if (accel_state->guest_map_initialized &&
        !amdsvm_memory_map_equal(&guest_map, &accel_state->guest_map)) {
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
            if (!amdsvm_build_published_memory_type_request(
                    &guest_map, &accel_state->guest_map,
                    accel_state->vm_api, &current_mtrr,
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
        int64_t state_load_started_us = amdsvm_perf_enabled(accel_state) ?
            g_get_monotonic_time() : 0;
        int state_load_rc = amdsvm_state_load(cpu, &run.state);
        if (state_load_started_us) {
            amdsvm_perf_record_state_load(accel_state, cpu->cpu_index,
                g_get_monotonic_time() - state_load_started_us);
        }
        if (state_load_rc != 0) {
            error_report("AMD-SVM rejected QEMU CPU state before VMRUN");
            ret = -EINVAL;
            goto out;
        }
        amdsvm_prepare_irq_injection(
            cpu, &accel_state->pending_events[cpu->cpu_index], &run);
        const uint64_t guest_cr0 = run.state.cr[0];
        const uint64_t cr0_pe_mask = 1ull;
        const uint64_t rflags_if_mask = 1ull << 9;
        const bool transition_window =
            run.state.segments[AMDVM_X86_SEG_CS].selector == 0x50 &&
            (guest_cr0 & cr0_pe_mask) != 0;
        const bool external_irq = run.event_injection != 0 &&
            (run.event_injection & AMDVM_VCPU_EVENTINJ_VALID) != 0 &&
            (run.event_injection & AMDVM_VCPU_EVENTINJ_TYPE_MASK) ==
                AMDVM_VCPU_EVENTINJ_TYPE_EXTERNAL;
        /* Keep bounded firmware and kernel budgets so boot IRQs cannot crowd
         * out the Windows kernel events this trace is meant to diagnose. */
        const bool kernel_event_trace =
            run.state.rip >= UINT64_C(0xfffff80000000000);
        const bool exception_event = run.event_injection != 0 &&
            (run.event_injection & AMDVM_VCPU_EVENTINJ_VALID) != 0 &&
            (run.event_injection & AMDVM_VCPU_EVENTINJ_TYPE_MASK) ==
                AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION;
        const bool kernel_exception_trace = kernel_event_trace &&
            exception_event &&
            accel_state->kernel_exception_trace_count[cpu->cpu_index] <
                AMDSVM_KERNEL_EXCEPTION_TRACE_LIMIT;
        const bool trace_kernel_event_this_run =
            accel_state->trace_vmexit && run.event_injection != 0 &&
            kernel_event_trace &&
            (accel_state->kernel_event_trace_count[cpu->cpu_index] <
                 AMDSVM_KERNEL_EVENT_TRACE_LIMIT || kernel_exception_trace);
        if (accel_state->trace_vmexit && run.event_injection != 0 &&
            (kernel_event_trace ?
             trace_kernel_event_this_run :
             accel_state->transition_event_trace_count[cpu->cpu_index] < 256)) {
            uint64_t event_index =
                ++accel_state->transition_event_trace_count[cpu->cpu_index];
            if (kernel_event_trace &&
                accel_state->kernel_event_trace_count[cpu->cpu_index] <
                    AMDSVM_KERNEL_EVENT_TRACE_LIMIT) {
                ++accel_state->kernel_event_trace_count[cpu->cpu_index];
            }
            if (kernel_exception_trace) {
                ++accel_state->kernel_exception_trace_count[cpu->cpu_index];
            }
            error_report("AMDVM_EVENT_INJECTION_DIAG index=%llu vcpu=%d event=0x%x vector=%u type=%u cs=%04x cs_base=0x%016llx ss=%04x ss_attr=0x%04x ss_base=0x%016llx rsp=0x%llx stack_width=%u rip=0x%llx cr0=0x%llx pe=%u if=%u interruptibility=0x%x irq_request=0x%x irq_pending=%u nmi_pending=%u exception_pending=%u",
                         (unsigned long long)event_index,
                         cpu->cpu_index, run.event_injection,
                         run.event_injection & 0xffu,
                         (run.event_injection &
                             AMDVM_VCPU_EVENTINJ_TYPE_MASK) >>
                             AMDVM_VCPU_EVENTINJ_TYPE_SHIFT,
                         run.state.segments[AMDVM_X86_SEG_CS].selector,
                         (unsigned long long)run.state.segments[
                             AMDVM_X86_SEG_CS].base,
                         run.state.segments[AMDVM_X86_SEG_SS].selector,
                         run.state.segments[AMDVM_X86_SEG_SS].attributes,
                         (unsigned long long)run.state.segments[
                             AMDVM_X86_SEG_SS].base,
                         (unsigned long long)run.state.gpr[AMDSVM_GPR_RSP],
                         (run.state.segments[AMDVM_X86_SEG_SS].attributes &
                          (1u << 10)) ? 32u : 16u,
                         (unsigned long long)run.state.rip,
                         (unsigned long long)guest_cr0,
                         (guest_cr0 & cr0_pe_mask) != 0,
                         (run.state.rflags & rflags_if_mask) != 0,
                         run.state.interruptibility,
                         qatomic_read(&cpu->interrupt_request),
                         accel_state->pending_events[cpu->cpu_index].irq_pending,
                         accel_state->pending_events[cpu->cpu_index].nmi_pending,
                         accel_state->pending_events[cpu->cpu_index].exception_pending);
        }
        if (accel_state->diagnostic_transition_irq_gate &&
            transition_window && external_irq &&
            !accel_state->pending_events[cpu->cpu_index].exception_pending &&
            !accel_state->pending_events[cpu->cpu_index].nmi_pending) {
            uint64_t gate_index =
                ++accel_state->transition_irq_gate_count[cpu->cpu_index];
            dispatch_context.defer_external_irq_completion = true;
            run.event_injection = 0;
            run.event_injection_error_code = 0;
            error_report("AMDVM_TRANSITION_IRQ_DEFERRED index=%llu vcpu=%d vector=%u cs=%04x rip=0x%llx cr0=0x%llx if=%u",
                         (unsigned long long)gate_index,
                         cpu->cpu_index,
                         accel_state->pending_events[cpu->cpu_index].irq_vector,
                         run.state.segments[AMDVM_X86_SEG_CS].selector,
                         (unsigned long long)run.state.rip,
                         (unsigned long long)guest_cr0,
                         (run.state.rflags & rflags_if_mask) != 0);
        }
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
        bool kernel_gpr_trace_this_run = false;
        bool transition_trace_this_run = false;
        uint64_t trace_run_index = 0;
        int64_t run_started_wall_us = g_get_real_time();
        int64_t run_started_us = g_get_monotonic_time();
        transition_trace_this_run = accel_state->trace_vmexit &&
            amdsvm_is_transition16big_state(&run.state);
        if (accel_state->diagnostic_cr0_window_trace &&
            amdsvm_cr0_window_contains(&run.state)) {
            amdsvm_trace_cr0_window("QEMU_PRE_RUN", cpu->cpu_index,
                                    &run.state, UINT64_MAX,
                                    "unavailable_before_RUN");
        }
        if (transition_trace_this_run) {
            amdsvm_trace_transition_state("QEMU_PRE_RUN", cpu->cpu_index,
                                          &run.state, UINT64_MAX, 0,
                                          run.event_injection);
        }
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
            if (run.state.rip >= UINT64_C(0xffffffff80000000) &&
                accel_state->kernel_gpr_trace_count[cpu->cpu_index] <
                    AMDSVM_KERNEL_GPR_TRACE_LIMIT) {
                kernel_gpr_trace_this_run = true;
                trace_this_run = true;
                ++accel_state->kernel_gpr_trace_count[cpu->cpu_index];
            }
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
        int64_t provider_started_us = amdsvm_perf_enabled(accel_state) ?
            g_get_monotonic_time() : 0;
        int provider_status = accel_state->vm_api ?
            amdsvm_vm_api_run_once(accel_state, cpu, &run, &result) :
            amdsvm_provider_run(&accel_state->provider_lifecycle,
                                &run, &result);
        int64_t provider_finished_us = provider_started_us ?
            g_get_monotonic_time() : 0;
        AMDVMBQLSpinResult bql_spin = { 0 };
        if (accel_state->bql_spin_enabled) {
            bql_spin = amdsvm_bql_try_acquire_bounded(
                AMDSVM_BQL_SPIN_BUDGET, amdsvm_bql_try_acquire,
                amdsvm_bql_relax, NULL);
        }
        if (!bql_spin.acquired) {
            bql_lock();
        }
        if (provider_started_us) {
            amdsvm_perf_record_run(accel_state, cpu->cpu_index,
                provider_status < 0 ? UINT64_MAX : result.exit_code,
                g_get_monotonic_time() - provider_started_us,
                provider_finished_us - provider_started_us,
                g_get_monotonic_time() - provider_finished_us,
                accel_state->bql_spin_enabled ? &bql_spin : NULL,
                result.exit_info2);
        }
        if (provider_status < 0) {
            /* An IOKit error can follow a completed hardware entry. */
            vmrun_attempted = true;
            error_report("AMD-SVM QEMU provider RUN call failed");
            ret = provider_status;
            goto out;
        }
        if (trace_kernel_event_this_run) {
            error_report("AMDVM_EVENT_RUN_RESULT vcpu=%d event=0x%x vector=%u type=%u entry_rip=0x%llx entry_rsp=0x%llx entry_rflags=0x%llx entry_interruptibility=0x%llx exit_code=0x%llx exit_rip=0x%llx exit_rsp=0x%llx exit_rflags=0x%llx exit_interruptibility=0x%llx exit_int_info=0x%x flags=0x%x context=%d owner=%d vmrun=%d",
                         cpu->cpu_index, run.event_injection,
                         run.event_injection & 0xffu,
                         (run.event_injection &
                             AMDVM_VCPU_EVENTINJ_TYPE_MASK) >>
                             AMDVM_VCPU_EVENTINJ_TYPE_SHIFT,
                         (unsigned long long)run.state.rip,
                         (unsigned long long)run.state.gpr[AMDSVM_GPR_RSP],
                         (unsigned long long)run.state.rflags,
                         (unsigned long long)run.state.interruptibility,
                         (unsigned long long)result.exit_code,
                         (unsigned long long)result.state.rip,
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RSP],
                         (unsigned long long)result.state.rflags,
                         (unsigned long long)result.state.interruptibility,
                         result.exit_int_info, result.flags,
                         result.context_status, result.owner_status,
                         result.vmrun_status);
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
        if (accel_state->diagnostic_cr0_window_trace &&
            amdsvm_cr0_window_contains(&run.state)) {
            if (accel_state->vm_api) {
                error_report("AMDVM_CR0_WINDOW stage=VMCB_ENTRY vcpu=%d rip=0x%llx qemu_cr0=0x%llx qemu_pe=%u vmcb_cr0=unavailable vmcb_pe=unavailable reason=VM_API_RUN_result_has_no_entry_snapshot",
                             cpu->cpu_index,
                             (unsigned long long)run.state.rip,
                             (unsigned long long)run.state.cr[0],
                             (unsigned)(run.state.cr[0] & 1u));
            } else {
                error_report("AMDVM_CR0_WINDOW stage=VMCB_ENTRY vcpu=%d rip=0x%llx qemu_cr0=0x%llx qemu_pe=%u vmcb_cr0=0x%llx vmcb_pe=%u",
                             cpu->cpu_index,
                             (unsigned long long)run.state.rip,
                             (unsigned long long)run.state.cr[0],
                             (unsigned)(run.state.cr[0] & 1u),
                             (unsigned long long)result.entry.cr0,
                             (unsigned)(result.entry.cr0 & 1u));
            }
        }
        {
            static unsigned waitloop_exit_logs;
            if (waitloop_exit_logs < 64 &&
                ((run.state.rip >= 0x41b000 && run.state.rip < 0x41c000) ||
                 (result.state.rip >= 0x41b000 &&
                  result.state.rip < 0x41c000))) {
                error_report("AMDVM_WAITLOOP entry_rip=0x%llx exit_rip=0x%llx "
                             "next_rip=0x%llx exit=0x%llx info1=0x%llx "
                             "info2=0x%llx int_info=0x%x vmrun=%d context=%d "
                             "owner=%d entry_cr0=0x%llx exit_cr0=0x%llx "
                             "entry_eventinj=0x%llx exit_eventinj=0x%llx "
                             "rsp=0x%llx rax=0x%llx rdx=0x%llx",
                             (unsigned long long)run.state.rip,
                             (unsigned long long)result.state.rip,
                             (unsigned long long)result.next_rip,
                             (unsigned long long)result.exit_code,
                             (unsigned long long)result.exit_info1,
                             (unsigned long long)result.exit_info2,
                             result.exit_int_info, result.vmrun_status,
                             result.context_status, result.owner_status,
                             (unsigned long long)run.state.cr[0],
                             (unsigned long long)result.state.cr[0],
                             (unsigned long long)run.event_injection,
                             (unsigned long long)result.entry.event_injection,
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RSP],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RAX],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RDX]);
                waitloop_exit_logs++;
            }
        }
        if (accel_state->diagnostic_cr0_window_trace &&
            amdsvm_cr0_window_contains(&result.state)) {
            amdsvm_trace_cr0_window("VMCB_EXIT", cpu->cpu_index,
                                    &result.state, result.exit_code,
                                    accel_state->vm_api ? "unavailable" :
                                                         "available");
        }
        transition_trace_this_run |=
            amdsvm_is_transition16big_state(&result.state) ||
            ((run.state.cr[0] ^ result.state.cr[0]) & 1u) != 0;
        if (accel_state->trace_vmexit && transition_trace_this_run) {
            const amdvm_x86_segment_state_t *qemu_cs =
                &run.state.segments[AMDVM_X86_SEG_CS];
            if (!amdsvm_is_transition16big_state(&run.state)) {
                amdsvm_trace_transition_state("QEMU_PRE_RUN", cpu->cpu_index,
                                              &run.state, UINT64_MAX, 0,
                                              run.event_injection);
            }
            if (accel_state->vm_api) {
                error_report("AMDVM_TRANSITION_VMCB_ENTRY_UNAVAILABLE vcpu=%d reason=VM_API_RUN_result_has_no_entry_snapshot qemu_cs=%04x/%04x:%08x:0x%016llx qemu_rip=0x%llx qemu_cr0=0x%llx qemu_eventinj=0x%x",
                             cpu->cpu_index,
                             qemu_cs->selector, qemu_cs->attributes,
                             qemu_cs->limit,
                             (unsigned long long)qemu_cs->base,
                             (unsigned long long)run.state.rip,
                             (unsigned long long)run.state.cr[0],
                             run.event_injection);
            } else {
                uint32_t vmcb_mismatch = 0;

                if (qemu_cs->selector != result.entry.cs_selector) vmcb_mismatch |= 1u << 0;
                if (qemu_cs->attributes != result.entry.cs_attributes) vmcb_mismatch |= 1u << 1;
                if (qemu_cs->limit != result.entry.cs_limit) vmcb_mismatch |= 1u << 2;
                if (qemu_cs->base != result.entry.cs_base) vmcb_mismatch |= 1u << 3;
                if (run.state.rip != result.entry.rip) vmcb_mismatch |= 1u << 4;
                if (run.state.rflags != result.entry.rflags) vmcb_mismatch |= 1u << 5;
                if (run.state.cr[0] != result.entry.cr0) vmcb_mismatch |= 1u << 6;
                if (run.event_injection != result.entry.event_injection) vmcb_mismatch |= 1u << 7;
                error_report("AMDVM_TRANSITION_QEMU_VMCB_ENTRY vcpu=%d mismatch=0x%x qemu_cs=%04x/%04x:%08x:0x%016llx vmcb_cs=%04x/%04x:%08x:0x%016llx qemu_rip=0x%llx vmcb_rip=0x%llx qemu_cr0=0x%llx vmcb_cr0=0x%llx qemu_eventinj=0x%x vmcb_eventinj=0x%llx vmcb_exit_int_info_before=0x%x",
                             cpu->cpu_index, vmcb_mismatch,
                             qemu_cs->selector, qemu_cs->attributes,
                             qemu_cs->limit,
                             (unsigned long long)qemu_cs->base,
                             result.entry.cs_selector, result.entry.cs_attributes,
                             result.entry.cs_limit,
                             (unsigned long long)result.entry.cs_base,
                             (unsigned long long)run.state.rip,
                             (unsigned long long)result.entry.rip,
                             (unsigned long long)run.state.cr[0],
                             (unsigned long long)result.entry.cr0,
                             run.event_injection,
                             (unsigned long long)result.entry.event_injection,
                             result.exit_int_info);
            }
            amdsvm_trace_transition_state("VMCB_EXIT", cpu->cpu_index,
                                          &result.state, result.exit_code,
                                          result.exit_int_info,
                                          accel_state->vm_api ? UINT64_MAX :
                                          result.entry.event_injection);
        }
        /* Always capture a first intercepted #GP, even when VMEXIT tracing
         * has moved past its sampled RUN window. The stack and caller bytes
         * are needed to diagnose the fault and are only read from guest RAM.
         */
        if (trace_this_run || result.exit_code == 0x4d) {
            int64_t run_completed_us = g_get_monotonic_time();
            int64_t run_completed_wall_us = g_get_real_time();
            if (accel_state->vm_api) {
                error_report("AMDVM_FINITE_KICK_VMEXIT code=0x%llx vcpu=%d rip=0x%llx next_rip=0x%llx info1=0x%llx info2=0x%llx flags=0x%x abi=%u context=%d owner=%d vmrun=%d exit_int_info=0x%x entry_snapshot=unavailable generation=%llu started_us=%lld completed_us=%lld run_elapsed_us=%lld started_wall_us=%lld completed_wall_us=%lld",
                             (unsigned long long)result.exit_code,
                             cpu->cpu_index,
                             (unsigned long long)result.state.rip,
                             (unsigned long long)result.next_rip,
                             (unsigned long long)result.exit_info1,
                             (unsigned long long)result.exit_info2,
                             result.flags,
                             result.abi_version, result.context_status,
                             result.owner_status, result.vmrun_status,
                             result.exit_int_info,
                             (unsigned long long)run.kick_generation,
                             (long long)run_started_us,
                             (long long)run_completed_us,
                             (long long)(run_completed_us - run_started_us),
                             (long long)run_started_wall_us,
                             (long long)run_completed_wall_us);
            } else {
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
            }
            if (trace_this_run && result.exit_code != SVM_EXIT_INTR) {
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
            if (kernel_gpr_trace_this_run) {
                error_report("AMDVM_KERNEL_GPR_STATE index=%llu vcpu=%d eventinj=0x%x exit_code=0x%llx exit_int_info=0x%x entry_rip=0x%llx entry_rax=0x%llx entry_rbx=0x%llx entry_rcx=0x%llx entry_rdx=0x%llx entry_rsi=0x%llx entry_rdi=0x%llx entry_rbp=0x%llx entry_rsp=0x%llx entry_r8=0x%llx entry_r9=0x%llx entry_r10=0x%llx entry_r11=0x%llx entry_r12=0x%llx entry_r13=0x%llx entry_r14=0x%llx entry_r15=0x%llx exit_rip=0x%llx exit_rax=0x%llx exit_rbx=0x%llx exit_rcx=0x%llx exit_rdx=0x%llx exit_rsi=0x%llx exit_rdi=0x%llx exit_rbp=0x%llx exit_rsp=0x%llx exit_r8=0x%llx exit_r9=0x%llx exit_r10=0x%llx exit_r11=0x%llx exit_r12=0x%llx exit_r13=0x%llx exit_r14=0x%llx exit_r15=0x%llx cr0=0x%llx cr2=0x%llx cr3=0x%llx cr4=0x%llx",
                             (unsigned long long)accel_state->kernel_gpr_trace_count[cpu->cpu_index],
                             cpu->cpu_index, run.event_injection,
                             (unsigned long long)result.exit_code,
                             result.exit_int_info,
                             (unsigned long long)run.state.rip,
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RAX],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RBX],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RCX],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RDX],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RSI],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RDI],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RBP],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_RSP],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_R8],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_R9],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_R10],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_R11],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_R12],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_R13],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_R14],
                             (unsigned long long)run.state.gpr[AMDSVM_GPR_R15],
                             (unsigned long long)result.state.rip,
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RAX],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RBX],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RCX],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RDX],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RSI],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RDI],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RBP],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_RSP],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_R8],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_R9],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_R10],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_R11],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_R12],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_R13],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_R14],
                             (unsigned long long)result.state.gpr[AMDSVM_GPR_R15],
                             (unsigned long long)result.state.cr[0],
                             (unsigned long long)result.state.cr[2],
                             (unsigned long long)result.state.cr[3],
                             (unsigned long long)result.state.cr[4]);
            }
            if (trace_this_run && result.exit_code != SVM_EXIT_INTR) {
                if (accel_state->vm_api) {
                    error_report("AMDVM_EXIT_DIAG code=0x%llx exit_rip=0x%llx nrip=0x%llx info1=0x%llx info2=0x%llx exit_int_info=0x%x decode_len=%u decode=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x entry_snapshot=unavailable",
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
                                 result.instruction_bytes[14]);
                } else {
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
                error_report("AMDVM_PHYSICAL_INTR_STATE vcpu=%d rip=0x%llx rflags=0x%llx cr0=0x%llx pe=%u cs=%04x/%04x:%08x:0x%llx ss=%04x/%04x:%08x:0x%llx code_status=0x%x code=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x irq_before_events=0x%x halted=%u",
                             cpu->cpu_index,
                             (unsigned long long)result.state.rip,
                             (unsigned long long)result.state.rflags,
                             (unsigned long long)result.state.cr[0],
                             (result.state.cr[0] & 1u) != 0,
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
                uint8_t frame_bytes[8] = { 0 };
                const uint64_t fault_offset = result.state.rip;
                const uint64_t cs_base = result.state.segments[AMDVM_X86_SEG_CS].base;
                const uint64_t stack_pointer = result.state.gpr[AMDSVM_GPR_RSP];
                const uint64_t frame_pointer = result.state.gpr[AMDSVM_GPR_RBP];
                const uint64_t ss_base = result.state.segments[AMDVM_X86_SEG_SS].base;
                const amdvm_x86_segment_state_t *ss =
                    &result.state.segments[AMDVM_X86_SEG_SS];
                const uint64_t cr0 = result.state.cr[0];
                const uint64_t efer = result.state.efer;
                const bool long_mode = (efer & (1ull << 10)) != 0;
                const bool stack32 = (ss->attributes & (1u << 10)) != 0;
                const uint64_t stack_mask = long_mode ? UINT64_MAX :
                    (stack32 ? UINT32_MAX : UINT16_MAX);
                const uint64_t effective_sp = stack_pointer & stack_mask;
                const uint64_t effective_bp = frame_pointer & stack_mask;
                uint64_t fault_linear = 0;
                uint64_t stack_linear = 0;
                uint64_t frame_linear = 0;
                MemTxResult read_status = MEMTX_ERROR;
                MemTxResult stack_status = MEMTX_ERROR;
                MemTxResult frame_status = MEMTX_ERROR;
                if (fault_offset <= UINT64_MAX - cs_base) {
                    fault_linear = cs_base + fault_offset;
                    if ((cr0 & (1ull << 31)) == 0) {
                        read_status = address_space_read(&address_space_memory,
                            fault_linear, MEMTXATTRS_UNSPECIFIED,
                            fault_bytes, sizeof(fault_bytes));
                    }
                }
                if (effective_sp <= UINT64_MAX - ss_base) {
                    stack_linear = ss_base + effective_sp;
                    if ((cr0 & (1ull << 31)) == 0 &&
                        (long_mode || effective_sp <= ss->limit)) {
                        size_t stack_read_len = sizeof(stack_bytes);
                        if (!long_mode && stack_read_len - 1 >
                            ss->limit - effective_sp) {
                            stack_read_len = ss->limit - effective_sp + 1;
                        }
                        stack_status = address_space_read(&address_space_memory,
                            stack_linear, MEMTXATTRS_UNSPECIFIED,
                            stack_bytes, stack_read_len);
                    }
                }
                if (effective_bp <= UINT64_MAX - ss_base - 2) {
                    frame_linear = ss_base + effective_bp + 2;
                    if ((cr0 & (1ull << 31)) == 0 &&
                        (long_mode || effective_bp <= ss->limit) &&
                        (long_mode || ss->limit - effective_bp >= 5)) {
                        frame_status = address_space_read(&address_space_memory,
                            frame_linear, MEMTXATTRS_UNSPECIFIED,
                            frame_bytes, sizeof(frame_bytes));
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
                error_report("AMDVM_GP_STACK_EFFECTIVE raw_rsp=0x%llx effective_sp=0x%llx stack_bits=%u ss=%04x/%04x:%08x:0x%llx linear=0x%llx read_status=0x%x bytes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x raw_rbp=0x%llx effective_bp=0x%llx return_slot=0x%llx frame_status=0x%x return_bytes=%02x:%02x:%02x:%02x",
                             (unsigned long long)stack_pointer,
                             (unsigned long long)effective_sp,
                             long_mode ? 64u : (stack32 ? 32u : 16u),
                             ss->selector, ss->attributes, ss->limit,
                             (unsigned long long)ss_base,
                             (unsigned long long)stack_linear, stack_status,
                             stack_bytes[0], stack_bytes[1], stack_bytes[2],
                             stack_bytes[3], stack_bytes[4], stack_bytes[5],
                             stack_bytes[6], stack_bytes[7], stack_bytes[8],
                             stack_bytes[9], stack_bytes[10], stack_bytes[11],
                             stack_bytes[12], stack_bytes[13], stack_bytes[14],
                             stack_bytes[15],
                             (unsigned long long)frame_pointer,
                             (unsigned long long)effective_bp,
                             (unsigned long long)frame_linear, frame_status,
                             frame_bytes[0], frame_bytes[1], frame_bytes[2],
                             frame_bytes[3]);
            }
        }
        /*
         * A successful RUN transport may report malformed output after the
         * provider already entered the guest. Always verify host SVM state on
         * this path, then let the production dispatcher validate the result.
         */
        vmrun_attempted = true;
        if (result.exit_code == 0x4d) {
            const amdvm_x86_segment_state_t *cs =
                &result.state.segments[AMDVM_X86_SEG_CS];
            const amdvm_x86_segment_state_t *ss =
                &result.state.segments[AMDVM_X86_SEG_SS];
            const amdvm_x86_segment_state_t *ds =
                &result.state.segments[AMDVM_X86_SEG_DS];
            const amdvm_x86_segment_state_t *es =
                &result.state.segments[AMDVM_X86_SEG_ES];
            uint8_t instruction[16] = { 0 };
            uint64_t linear = 0;
            MemTxResult instruction_status = MEMTX_ERROR;

            if (!(result.state.cr[0] & (1ull << 31)) &&
                result.state.rip <= UINT64_MAX - cs->base) {
                linear = cs->base + result.state.rip;
                instruction_status = address_space_read(
                    &address_space_memory, linear, MEMTXATTRS_UNSPECIFIED,
                    instruction, sizeof(instruction));
            }
            error_report("AMDVM_FIRST_GP_STATE vcpu=%d rip=0x%llx next_rip=0x%llx error=0x%llx info2=0x%llx rflags=0x%llx interruptibility=0x%x cs=%04x/%04x:%08x:0x%llx ds=%04x/%04x:%08x:0x%llx es=%04x/%04x:%08x:0x%llx ss=%04x/%04x:%08x:0x%llx gdtr=%08x:0x%llx idtr=%08x:0x%llx cr0=0x%llx cr2=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rsp=0x%llx rbp=0x%llx rsi=0x%llx rdi=0x%llx instruction_status=%d instruction=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                         cpu->cpu_index,
                         (unsigned long long)result.state.rip,
                         (unsigned long long)result.next_rip,
                         (unsigned long long)result.exit_info1,
                         (unsigned long long)result.exit_info2,
                         (unsigned long long)result.state.rflags,
                         result.state.interruptibility,
                         cs->selector, cs->attributes, cs->limit,
                         (unsigned long long)cs->base,
                         ds->selector, ds->attributes, ds->limit,
                         (unsigned long long)ds->base,
                         es->selector, es->attributes, es->limit,
                         (unsigned long long)es->base,
                         ss->selector, ss->attributes, ss->limit,
                         (unsigned long long)ss->base,
                         result.state.segments[AMDVM_X86_SEG_GDTR].limit,
                         (unsigned long long)result.state.segments[
                             AMDVM_X86_SEG_GDTR].base,
                         result.state.segments[AMDVM_X86_SEG_IDTR].limit,
                         (unsigned long long)result.state.segments[
                             AMDVM_X86_SEG_IDTR].base,
                         (unsigned long long)result.state.cr[0],
                         (unsigned long long)result.state.cr[2],
                         (unsigned long long)result.state.cr[3],
                         (unsigned long long)result.state.cr[4],
                         (unsigned long long)result.state.efer,
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RAX],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RCX],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RDX],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RBX],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RSP],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RBP],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RSI],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RDI],
                         instruction_status,
                         instruction[0], instruction[1], instruction[2],
                         instruction[3], instruction[4], instruction[5],
                         instruction[6], instruction[7], instruction[8],
                         instruction[9], instruction[10], instruction[11],
                         instruction[12], instruction[13], instruction[14],
                         instruction[15]);
        }
        /*
         * SVM_EXIT_SHUTDOWN is the architectural triple-fault exit. KVM
         * treats the same guest condition as a guest reset; do not pass it
         * through the firmware instruction dispatcher as malformed data.
         * EXITINTINFO is retained for diagnosis (often the #DF being
         * delivered when the guest reached shutdown).
         */
        if (result.exit_code == SVM_EXIT_SHUTDOWN) {
            uint8_t guest_code[16] = { 0 };
            uint8_t idt_df[8] = { 0 };
            uint8_t idt_gp[8] = { 0 };
            uint8_t idt_pf[8] = { 0 };
            uint8_t df_handler[16] = { 0 };
            uint8_t gp_handler[16] = { 0 };
            uint8_t gdt_cs[8] = { 0 };
            uint8_t gdt_ss[8] = { 0 };
            char guest_code_hex[48], idt_df_hex[24], idt_gp_hex[24];
            char idt_pf_hex[24], gdt_cs_hex[24], gdt_ss_hex[24];
            char df_handler_hex[48], gp_handler_hex[48];
            const amdvm_x86_segment_state_t *cs =
                &result.state.segments[AMDVM_X86_SEG_CS];
            const amdvm_x86_segment_state_t *ss =
                &result.state.segments[AMDVM_X86_SEG_SS];
            const amdvm_x86_segment_state_t *gdtr =
                &result.state.segments[AMDVM_X86_SEG_GDTR];
            const amdvm_x86_segment_state_t *idtr =
                &result.state.segments[AMDVM_X86_SEG_IDTR];
            uint64_t linear;
            MemTxResult code_status = MEMTX_ERROR;
            MemTxResult idt_df_status = MEMTX_ERROR;
            MemTxResult idt_gp_status = MEMTX_ERROR;
            MemTxResult idt_pf_status = MEMTX_ERROR;
            MemTxResult gdt_cs_status = MEMTX_ERROR;
            MemTxResult gdt_ss_status = MEMTX_ERROR;
            MemTxResult df_handler_status = MEMTX_ERROR;
            MemTxResult gp_handler_status = MEMTX_ERROR;
            uint16_t df_selector = 0, gp_selector = 0;
            uint32_t df_offset = 0, gp_offset = 0;
            uint8_t guest_virtual_code[16] = { 0 };
            uint8_t guest_virtual_idt_df[16] = { 0 };
            uint8_t guest_virtual_cr2[16] = { 0 };
            uint8_t guest_virtual_stack[32] = { 0 };
            uint8_t guest_virtual_df_handler[16] = { 0 };
            AMDVMGuestPageWalk code_walk = { 0 }, idt_walk = { 0 };
            AMDVMGuestPageWalk cr2_walk = { 0 }, stack_walk = { 0 };
            AMDVMGuestPageWalk handler_walk = { 0 };
            int guest_virtual_code_status = -3;
            int guest_virtual_idt_status = -3;
            int guest_virtual_cr2_status = -3;
            int guest_virtual_stack_status = -3;
            int guest_virtual_handler_status = -3;
            uint64_t guest_virtual_code_address = 0;
            uint64_t guest_virtual_idt_address = 0;
            uint64_t guest_virtual_stack_address = 0;
            uint64_t guest_virtual_handler_address = 0;

            /* With paging off, the firmware's linear descriptor bases are GPA. */
            if (!(result.state.cr[0] & (1ull << 31)) &&
                result.state.rip <= UINT64_MAX - cs->base) {
                linear = cs->base + result.state.rip;
                code_status = address_space_read(&address_space_memory,
                    linear, MEMTXATTRS_UNSPECIFIED, guest_code,
                    sizeof(guest_code));
                if (idtr->limit >= 8u * 8u + sizeof(idt_df) - 1) {
                    idt_df_status = address_space_read(&address_space_memory,
                        idtr->base + 8u * 8u, MEMTXATTRS_UNSPECIFIED,
                        idt_df, sizeof(idt_df));
                }
                if (idtr->limit >= 13u * 8u + sizeof(idt_gp) - 1) {
                    idt_gp_status = address_space_read(&address_space_memory,
                        idtr->base + 13u * 8u, MEMTXATTRS_UNSPECIFIED,
                        idt_gp, sizeof(idt_gp));
                }
                if (idtr->limit >= 14u * 8u + sizeof(idt_pf) - 1) {
                    idt_pf_status = address_space_read(&address_space_memory,
                        idtr->base + 14u * 8u, MEMTXATTRS_UNSPECIFIED,
                        idt_pf, sizeof(idt_pf));
                }
                if (idt_df_status == MEMTX_OK) {
                    df_selector = (uint16_t)idt_df[2] |
                        ((uint16_t)idt_df[3] << 8);
                    df_offset = (uint32_t)idt_df[0] |
                        ((uint32_t)idt_df[1] << 8) |
                        ((uint32_t)idt_df[6] << 16) |
                        ((uint32_t)idt_df[7] << 24);
                    if (df_selector == cs->selector &&
                        df_offset <= cs->limit) {
                        df_handler_status = address_space_read(
                            &address_space_memory, cs->base + df_offset,
                            MEMTXATTRS_UNSPECIFIED, df_handler,
                            sizeof(df_handler));
                    }
                }
                if (idt_gp_status == MEMTX_OK) {
                    gp_selector = (uint16_t)idt_gp[2] |
                        ((uint16_t)idt_gp[3] << 8);
                    gp_offset = (uint32_t)idt_gp[0] |
                        ((uint32_t)idt_gp[1] << 8) |
                        ((uint32_t)idt_gp[6] << 16) |
                        ((uint32_t)idt_gp[7] << 24);
                    if (gp_selector == cs->selector &&
                        gp_offset <= cs->limit) {
                        gp_handler_status = address_space_read(
                            &address_space_memory, cs->base + gp_offset,
                            MEMTXATTRS_UNSPECIFIED, gp_handler,
                            sizeof(gp_handler));
                    }
                }
                linear = cs->selector & ~7u;
                if (linear <= gdtr->limit && sizeof(gdt_cs) - 1 <=
                        gdtr->limit - linear) {
                    gdt_cs_status = address_space_read(&address_space_memory,
                        gdtr->base + linear, MEMTXATTRS_UNSPECIFIED,
                        gdt_cs, sizeof(gdt_cs));
                }
                linear = ss->selector & ~7u;
                if (linear <= gdtr->limit && sizeof(gdt_ss) - 1 <=
                        gdtr->limit - linear) {
                    gdt_ss_status = address_space_read(&address_space_memory,
                        gdtr->base + linear, MEMTXATTRS_UNSPECIFIED,
                        gdt_ss, sizeof(gdt_ss));
                }
            }
            if (result.state.cr[0] & (UINT64_C(1) << 31)) {
                /* The original physical dump below deliberately skips these
                 * reads while paging is enabled. Resolve the fault addresses
                 * using the CR3 from this exact VMCB exit instead. */
                if (result.state.rip <= UINT64_MAX - cs->base) {
                    guest_virtual_code_address = cs->base + result.state.rip;
                    guest_virtual_code_status =
                        amdsvm_debug_guest_virtual_read(result.state.cr[3],
                            result.state.cr[4], guest_virtual_code_address,
                            guest_virtual_code, sizeof(guest_virtual_code),
                            &code_walk);
                }
                if (idtr->base <= UINT64_MAX - 8u * 16u) {
                    guest_virtual_idt_address = idtr->base + 8u * 16u;
                    guest_virtual_idt_status =
                        amdsvm_debug_guest_virtual_read(result.state.cr[3],
                            result.state.cr[4], guest_virtual_idt_address,
                            guest_virtual_idt_df,
                            sizeof(guest_virtual_idt_df), &idt_walk);
                }
                guest_virtual_cr2_status =
                    amdsvm_debug_guest_virtual_read(result.state.cr[3],
                        result.state.cr[4], result.state.cr[2],
                        guest_virtual_cr2, sizeof(guest_virtual_cr2),
                        &cr2_walk);
                guest_virtual_stack_address =
                    result.state.gpr[AMDSVM_GPR_RSP] >=
                        sizeof(guest_virtual_stack) ?
                    result.state.gpr[AMDSVM_GPR_RSP] -
                        sizeof(guest_virtual_stack) :
                    result.state.gpr[AMDSVM_GPR_RSP];
                guest_virtual_stack_status =
                    amdsvm_debug_guest_virtual_read(result.state.cr[3],
                        result.state.cr[4], guest_virtual_stack_address,
                        guest_virtual_stack, sizeof(guest_virtual_stack),
                        &stack_walk);
                if (guest_virtual_idt_status == 0) {
                    uint64_t offset =
                        (uint64_t)guest_virtual_idt_df[0] |
                        ((uint64_t)guest_virtual_idt_df[1] << 8) |
                        ((uint64_t)guest_virtual_idt_df[6] << 16) |
                        ((uint64_t)guest_virtual_idt_df[7] << 24) |
                        ((uint64_t)guest_virtual_idt_df[8] << 32) |
                        ((uint64_t)guest_virtual_idt_df[9] << 40) |
                        ((uint64_t)guest_virtual_idt_df[10] << 48) |
                        ((uint64_t)guest_virtual_idt_df[11] << 56);
                    uint16_t selector = (uint16_t)guest_virtual_idt_df[2] |
                        ((uint16_t)guest_virtual_idt_df[3] << 8);
                    if (selector == cs->selector) {
                        guest_virtual_handler_address = offset;
                        guest_virtual_handler_status =
                            amdsvm_debug_guest_virtual_read(
                                result.state.cr[3], result.state.cr[4],
                                guest_virtual_handler_address,
                                guest_virtual_df_handler,
                                sizeof(guest_virtual_df_handler),
                                &handler_walk);
                    }
                }
                amdsvm_log_guest_virtual_read("rip",
                    guest_virtual_code_address,
                    guest_virtual_code_status, &code_walk,
                    guest_virtual_code, sizeof(guest_virtual_code));
                amdsvm_log_guest_virtual_read("idt_df",
                    guest_virtual_idt_address,
                    guest_virtual_idt_status, &idt_walk,
                    guest_virtual_idt_df, sizeof(guest_virtual_idt_df));
                amdsvm_log_guest_virtual_read("cr2",
                    result.state.cr[2], guest_virtual_cr2_status,
                    &cr2_walk, guest_virtual_cr2,
                    sizeof(guest_virtual_cr2));
                amdsvm_log_guest_virtual_read("stack",
                    guest_virtual_stack_address,
                    guest_virtual_stack_status, &stack_walk,
                    guest_virtual_stack, sizeof(guest_virtual_stack));
                if (guest_virtual_handler_address != 0) {
                    amdsvm_log_guest_virtual_read("df_handler",
                        guest_virtual_handler_address,
                        guest_virtual_handler_status, &handler_walk,
                        guest_virtual_df_handler,
                        sizeof(guest_virtual_df_handler));
                }
            }
            amdsvm_format_bytes(guest_code_hex, sizeof(guest_code_hex),
                                guest_code, sizeof(guest_code));
            amdsvm_format_bytes(idt_df_hex, sizeof(idt_df_hex), idt_df,
                                sizeof(idt_df));
            amdsvm_format_bytes(idt_gp_hex, sizeof(idt_gp_hex), idt_gp,
                                sizeof(idt_gp));
            amdsvm_format_bytes(idt_pf_hex, sizeof(idt_pf_hex), idt_pf,
                                sizeof(idt_pf));
            amdsvm_format_bytes(gdt_cs_hex, sizeof(gdt_cs_hex), gdt_cs,
                                sizeof(gdt_cs));
            amdsvm_format_bytes(gdt_ss_hex, sizeof(gdt_ss_hex), gdt_ss,
                                sizeof(gdt_ss));
            amdsvm_format_bytes(df_handler_hex, sizeof(df_handler_hex),
                                df_handler, sizeof(df_handler));
            amdsvm_format_bytes(gp_handler_hex, sizeof(gp_handler_hex),
                                gp_handler, sizeof(gp_handler));

            error_report("AMDVM_TRIPLE_FAULT_CONTEXT vcpu=%d rip=0x%llx rflags=0x%llx interruptibility=0x%x exit_int_info=0x%x qemu_eventinj=0x%llx cs=%04x/%04x:%08x:0x%llx ss=%04x/%04x:%08x:0x%llx gdtr=%04x/%04x:%08x:0x%llx idtr=%04x/%04x:%08x:0x%llx cr0=0x%llx cr2=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx rsp=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx rbp=0x%llx xcr0=0x%llx",
                         cpu->cpu_index,
                         (unsigned long long)result.state.rip,
                         (unsigned long long)result.state.rflags,
                         result.state.interruptibility,
                         result.exit_int_info,
                         (unsigned long long)run.event_injection,
                         cs->selector, cs->attributes, cs->limit,
                         (unsigned long long)cs->base,
                         ss->selector, ss->attributes, ss->limit,
                         (unsigned long long)ss->base,
                         gdtr->selector, gdtr->attributes, gdtr->limit,
                         (unsigned long long)gdtr->base,
                         idtr->selector, idtr->attributes, idtr->limit,
                         (unsigned long long)idtr->base,
                         (unsigned long long)result.state.cr[0],
                         (unsigned long long)result.state.cr[2],
                         (unsigned long long)result.state.cr[3],
                         (unsigned long long)result.state.cr[4],
                         (unsigned long long)result.state.efer,
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RSP],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RAX],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RBX],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RCX],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RDX],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RSI],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RDI],
                         (unsigned long long)result.state.gpr[AMDSVM_GPR_RBP],
                         (unsigned long long)result.state.xcr0);
            error_report("AMDVM_TRIPLE_FAULT_TABLES code_status=%d code=%s idt8_status=%d idt8=%s idt13_status=%d idt13=%s idt14_status=%d idt14=%s gdt_cs_status=%d gdt_cs=%s gdt_ss_status=%d gdt_ss=%s",
                         code_status, guest_code_hex,
                         idt_df_status, idt_df_hex,
                         idt_gp_status, idt_gp_hex,
                         idt_pf_status, idt_pf_hex,
                         gdt_cs_status, gdt_cs_hex,
                         gdt_ss_status, gdt_ss_hex);
            error_report("AMDVM_TRIPLE_FAULT_HANDLERS df_selector=0x%x df_offset=0x%x df_handler_status=%d df_handler=%s gp_selector=0x%x gp_offset=0x%x gp_handler_status=%d gp_handler=%s",
                         df_selector, df_offset, df_handler_status,
                         df_handler_hex, gp_selector, gp_offset,
                         gp_handler_status, gp_handler_hex);
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            ret = -EINTR;
            goto out;
        }
        if (result.exit_code == 0x4d &&
            accel_state->vm_provider.diagnostic_gp_intercept) {
            error_report("AMDVM_FIRST_GP_STOP vcpu=%d rip=0x%llx error=0x%llx; state captured, exception will not be reinjected",
                         cpu->cpu_index,
                         (unsigned long long)result.state.rip,
                         (unsigned long long)result.exit_info1);
            /* Keep the exact VMCB-exit state in QEMU and stop the VM before
             * any guest #GP handler can run or alter the evidence. */
            if (amdsvm_state_store(cpu, &result.state) != 0) {
                error_report("AMDVM_FIRST_GP_STOP state-store failed");
                ret = -EIO;
                goto out;
            }
            amdvm_x86_guest_state_v1_t qemu_after_gp = { 0 };
            if (amdsvm_state_load(cpu, &qemu_after_gp) == 0) {
                amdsvm_trace_transition_state("QEMU_STATE_AT_FIRST_GP_STOP",
                    cpu->cpu_index, &qemu_after_gp, result.exit_code,
                    result.exit_int_info, run.event_injection);
            }
            accel_state->vm_provider.diagnostic_gp_intercept = false;
            vm_stop(RUN_STATE_PAUSED);
            ret = -EINTR;
            goto out;
        }
        if (result.exit_code == SVM_EXIT_EXCP_PF &&
            accel_state->vm_provider.diagnostic_pf_intercept) {
            const amdvm_x86_segment_state_t *cs =
                &result.state.segments[AMDVM_X86_SEG_CS];
            /* SVM intercepts #PF before architectural CR2 is updated. The
             * faulting linear address is EXITINFO2; keep the incoming CR2
             * separately as evidence of its pre-fault value. */
            uint64_t fault_linear = result.exit_info2;
            uint64_t code_linear = cs->base + result.state.rip;
            AMDVMGuestPageWalk fault_walk = { 0 };
            int fault_walk_status = amdsvm_debug_guest_walk_4level(
                result.state.cr[3], result.state.cr[4], fault_linear,
                &fault_walk);
            int state_store_status;

            if (result.exit_info1 > UINT32_MAX) {
                error_report("AMDVM_DIAGNOSTIC_PF_STOP_INVALID_ERROR vcpu=%d rip=0x%llx error=0x%llx linear=0x%llx; state captured",
                    cpu->cpu_index,
                    (unsigned long long)result.state.rip,
                    (unsigned long long)result.exit_info1,
                    (unsigned long long)fault_linear);
                amdsvm_trace_transition_state("PROVIDER_EXIT_FIRST_PF",
                    cpu->cpu_index, &result.state, result.exit_code,
                    result.exit_int_info, UINT64_MAX);
                amdsvm_trace_pf_registers("PROVIDER_EXIT",
                    cpu->cpu_index, &result.state);
                accel_state->vm_provider.diagnostic_pf_intercept = false;
                vm_stop(RUN_STATE_PAUSED);
                ret = -EINTR;
                goto out;
            }

            /* A not-present PF whose current guest page tables resolve the
             * same address is the anomaly under investigation. Ordinary PFs
             * must still be delivered to Linux, or the diagnostic itself
             * would stop boot at its first demand/protection fault. This
             * diagnostic run is one-vCPU so the page-table walk cannot race
             * a sibling guest CPU. */
            if (!amdsvm_pf_mapping_mismatch((uint32_t)result.exit_info1,
                                             fault_walk_status == 0)) {
                amdvm_x86_guest_state_v1_t reinject_state = result.state;
                int event_status;

                if (dispatch_context.defer_external_irq_completion) {
                    error_report("AMDVM_DIAGNOSTIC_PF_STOP_UNHANDLED vcpu=%d reinjected=%llu rip=0x%llx error=0x%llx linear=0x%llx exit_int_info=0x%x pending_exception=%u; state was not modified",
                        cpu->cpu_index,
                        (unsigned long long)diagnostic_pf_reinjected,
                        (unsigned long long)result.state.rip,
                        (unsigned long long)result.exit_info1,
                        (unsigned long long)fault_linear,
                        result.exit_int_info,
                        accel_state->pending_events[cpu->cpu_index].exception_pending);
                    accel_state->vm_provider.diagnostic_pf_intercept = false;
                    vm_stop(RUN_STATE_PAUSED);
                    ret = -EINTR;
                    goto out;
                }
                event_status = amdsvm_firmware_complete_event(
                    &dispatch_context, result.exit_code,
                    result.exit_int_info, &reinject_state);
                if (event_status != 0 ||
                    accel_state->pending_events[cpu->cpu_index].exception_pending) {
                    error_report("AMDVM_DIAGNOSTIC_PF_STOP_PENDING_EVENT vcpu=%d reinjected=%llu rip=0x%llx error=0x%llx linear=0x%llx exit_int_info=0x%x complete=%d pending_exception=%u pending_nmi=%u pending_irq=%u; state was not modified",
                        cpu->cpu_index,
                        (unsigned long long)diagnostic_pf_reinjected,
                        (unsigned long long)result.state.rip,
                        (unsigned long long)result.exit_info1,
                        (unsigned long long)fault_linear,
                        result.exit_int_info, event_status,
                        accel_state->pending_events[cpu->cpu_index].exception_pending,
                        accel_state->pending_events[cpu->cpu_index].nmi_pending,
                        accel_state->pending_events[cpu->cpu_index].irq_pending);
                    accel_state->vm_provider.diagnostic_pf_intercept = false;
                    vm_stop(RUN_STATE_PAUSED);
                    ret = -EINTR;
                    goto out;
                }
                reinject_state.cr[2] = fault_linear;
                if (amdsvm_firmware_store_state(&dispatch_context,
                                                 &reinject_state) != 0 ||
                    amdsvm_event_queue_page_fault(
                        &accel_state->pending_events[cpu->cpu_index],
                        (uint32_t)result.exit_info1) != 0) {
                    error_report("AMDVM_DIAGNOSTIC_PF_STOP_REINJECT_FAILED vcpu=%d reinjected=%llu rip=0x%llx error=0x%llx linear=0x%llx; state captured, no retry",
                        cpu->cpu_index,
                        (unsigned long long)diagnostic_pf_reinjected,
                        (unsigned long long)result.state.rip,
                        (unsigned long long)result.exit_info1,
                        (unsigned long long)fault_linear);
                    accel_state->vm_provider.diagnostic_pf_intercept = false;
                    vm_stop(RUN_STATE_PAUSED);
                    ret = -EINTR;
                    goto out;
                }
                diagnostic_pf_reinjected++;
                continue;
            }

            error_report("AMDVM_PF_MAPPING_MISMATCH_STOP vcpu=%d reinjected=%llu rip=0x%llx error=0x%llx linear=0x%llx cr2_before_pf=0x%llx cr3=0x%llx walk_status=%d level=%d pml4e=0x%llx pdpte=0x%llx pde=0x%llx pte=0x%llx exit_int_info=0x%x run_eventinj=0x%x",
                cpu->cpu_index,
                (unsigned long long)diagnostic_pf_reinjected,
                (unsigned long long)result.state.rip,
                (unsigned long long)result.exit_info1,
                (unsigned long long)fault_linear,
                (unsigned long long)result.state.cr[2],
                (unsigned long long)result.state.cr[3],
                fault_walk_status, fault_walk.failed_level,
                (unsigned long long)fault_walk.entries[0],
                (unsigned long long)fault_walk.entries[1],
                (unsigned long long)fault_walk.entries[2],
                (unsigned long long)fault_walk.entries[3],
                result.exit_int_info, run.event_injection);
            /* Freeze the guest before walking its page tables: another vCPU
             * must not make a post-fault PTE look like the faulting value. */
            amdsvm_trace_transition_state("PROVIDER_EXIT_MAPPING_MISMATCH_PF",
                cpu->cpu_index, &result.state, result.exit_code,
                result.exit_int_info, UINT64_MAX);
            amdsvm_trace_pf_registers("PROVIDER_EXIT", cpu->cpu_index,
                &result.state);
            state_store_status = amdsvm_state_store(cpu, &result.state);
            accel_state->vm_provider.diagnostic_pf_intercept = false;
            error_report("AMDVM_PF_MAPPING_MISMATCH_STOP: captured exact #PF state; exception will not be reinjected");
            vm_stop(RUN_STATE_PAUSED);
            if (state_store_status != 0) {
                error_report("AMDVM_PF_MAPPING_MISMATCH_STOP state-store failed");
                ret = -EIO;
                goto out;
            }

            amdvm_x86_guest_state_v1_t qemu_after_pf = { 0 };
            if (amdsvm_state_load(cpu, &qemu_after_pf) == 0) {
                amdsvm_trace_transition_state("QEMU_STATE_AT_MAPPING_MISMATCH_PF",
                    cpu->cpu_index, &qemu_after_pf, result.exit_code,
                    result.exit_int_info, UINT64_MAX);
                amdsvm_trace_pf_registers("QEMU_AFTER_STORE",
                    cpu->cpu_index, &qemu_after_pf);
            }
            uint8_t code[16] = { 0 };
            AMDVMGuestPageWalk code_walk = { 0 };
            int code_read_status = amdsvm_debug_guest_virtual_read(
                result.state.cr[3], result.state.cr[4], code_linear,
                code, sizeof(code), &code_walk);

            /* The provider state is the hardware-exit snapshot; emit the
             * same architectural fields after storing them into QEMU so the
             * two sides can be compared without relying on a post-handler
             * guest snapshot. EVENTINJ is not part of the current RUN result
             * layout; retain request eventinj and EXITINTINFO. The provider's
             * one-time VMCB log is from the first intercepted PF, which may
             * precede this mismatch. */
            error_report("AMDVM_PF_MAPPING_MISMATCH_ADDRESS vcpu=%d rip=0x%llx next_rip=0x%llx error=0x%llx exitinfo2=0x%llx pf_linear=0x%llx cr2_before_pf=0x%llx run_request_eventinj=0x%x code_read=%d code=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                         cpu->cpu_index,
                         (unsigned long long)result.state.rip,
                         (unsigned long long)result.next_rip,
                         (unsigned long long)result.exit_info1,
                         (unsigned long long)result.exit_info2,
                         (unsigned long long)fault_linear,
                         (unsigned long long)result.state.cr[2],
                         run.event_injection,
                         code_read_status, code[0], code[1], code[2],
                         code[3], code[4], code[5], code[6], code[7],
                         code[8], code[9], code[10], code[11], code[12],
                         code[13], code[14], code[15]);
            error_report("AMDVM_PF_MAPPING_MISMATCH_WALK status=%d level=%d gpa=0x%llx pml4e=0x%llx pdpte=0x%llx pde=0x%llx pte=0x%llx code_walk=%d code_gpa=0x%llx code_pml4e=0x%llx code_pdpte=0x%llx code_pde=0x%llx code_pte=0x%llx",
                         fault_walk_status, fault_walk.failed_level,
                         (unsigned long long)fault_walk.gpa,
                         (unsigned long long)fault_walk.entries[0],
                         (unsigned long long)fault_walk.entries[1],
                         (unsigned long long)fault_walk.entries[2],
                         (unsigned long long)fault_walk.entries[3],
                         code_read_status,
                         (unsigned long long)code_walk.gpa,
                         (unsigned long long)code_walk.entries[0],
                         (unsigned long long)code_walk.entries[1],
                         (unsigned long long)code_walk.entries[2],
                         (unsigned long long)code_walk.entries[3]);
            ret = -EINTR;
            goto out;
        }
        int64_t dispatch_started_us = amdsvm_perf_enabled(accel_state) ?
            g_get_monotonic_time() : 0;
        AMDVMFirmwareDispatchAction dispatch_action =
            amdsvm_dispatch_firmware_exit(&run, &result, &dispatch_ops,
                                           &dispatch_context);
        if (dispatch_started_us) {
            amdsvm_perf_record_dispatch(accel_state, cpu->cpu_index,
                g_get_monotonic_time() - dispatch_started_us);
        }
        dispatch_context.defer_external_irq_completion = false;
        if (dispatch_action == AMDVM_FIRMWARE_DISPATCH_RESET) {
            error_report("AMD-SVM received completed SVM_EXIT_ERR; requesting guest reset");
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            ret = -EINTR;
            goto out;
        }
        switch (dispatch_action) {
        case AMDVM_FIRMWARE_DISPATCH_CONTINUE:
            if (accel_state->diagnostic_gp_rearm_pending[cpu->cpu_index]) {
                accel_state->vm_provider.diagnostic_gp_intercept = true;
                accel_state->diagnostic_gp_rearm_pending[cpu->cpu_index] =
                    false;
                error_report("AMDVM_DIAGNOSTIC_GP_REARMED vcpu=%d after guest event completion",
                             cpu->cpu_index);
            }
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
                if (!amdsvm_memory_map_equal(&guest_map, &updated_map)) {
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

    if (!state->vm_api &&
        (state->mtrr_cache_enabled || state->provider_pause_enabled)) {
        error_report("AMD-SVM mtrr-cache and provider-pause require vm-api=on");
        return -EINVAL;
    }

    if (state->perf_stats && !state->perf_stats_data) {
        state->perf_stats_data = g_new0(AMDVMPerfStats, 1);
        state->perf_state_entry = qemu_add_vm_change_state_handler(
            amdsvm_perf_vm_state_change, state);
    }

    /* Keep the publication's guest-independent opt-in and a bounded
     * resource envelope for this experimental build. */
    if (state->confirm_experimental_run) {
        state->vm_api = true;
    }
    if (state->confirm_experimental_run &&
        (state->diagnostic_only || state->confirm_finite_kick_test ||
         state->confirm_finite_smp_test || state->confirm_unbounded_vmrun ||
         machine->smp.cpus < 1 || machine->smp.cpus > 8 ||
         machine->smp.max_cpus != machine->smp.cpus ||
         machine->ram_size < UINT64_C(256) * 1024 * 1024 ||
         machine->ram_size > UINT64_C(16) * 1024 * 1024 * 1024)) {
        error_report("AMD-SVM experimental RUN supports 1..8 vCPUs, 256 MiB..16 GiB RAM, no CPU hotplug, and no other RUN confirmation");
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
    /* Multi-vCPU VM API RUN is allowed with the generic unbounded-VMRUN
     * confirmation; the finite and publication profiles keep their own checks. */
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
        if (amdsvm_vm_provider_init(&state->vm_provider,
                                    amdsvm_iokit_provider_transport, state)) {
            error_report("AMD-SVM VM API capability negotiation or host probe failed");
            IOServiceClose(connection); state->connection = IO_OBJECT_NULL; return -ENOTSUP;
        }
        fprintf(stderr,
                "AMD-SVM VM API negotiated state_abi=%u feature_flags=0x%016" PRIx64 "\n",
                state->vm_provider.state_abi,
                state->vm_provider.caps.feature_flags);
        if ((state->mtrr_cache_enabled && !(state->vm_provider.caps.feature_flags &
               AMDVM_VM_ABI_DRAFT_FEATURE_MTRR_PAT_CACHE_V1)) ||
            (state->provider_pause_enabled && !(state->vm_provider.caps.feature_flags &
               AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1)) ||
            (state->native_cr3_enabled && !(state->vm_provider.caps.feature_flags &
               AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_CR3_V1))) {
            error_report("AMD-SVM requested optimizer gate is not advertised by provider");
            IOServiceClose(connection);
            state->connection = IO_OBJECT_NULL;
            return -ENOTSUP;
        }
        state->vm_provider.mtrr_cache_enabled = state->mtrr_cache_enabled;
        state->vm_provider.provider_pause_enabled = state->provider_pause_enabled;
        fprintf(stderr, "AMD-SVM RUN gates mtrr-cache=%u provider-pause=%u bql-spin=%u\n",
                state->mtrr_cache_enabled, state->provider_pause_enabled,
                state->bql_spin_enabled);
        if ((state->vm_provider.caps.feature_flags &
             AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1) &&
            !state->perf_state_entry) {
            state->perf_state_entry = qemu_add_vm_change_state_handler(
                amdsvm_perf_vm_state_change, state);
        }
        if (machine->smp.max_cpus > state->vm_provider.caps.max_vcpus ||
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
        if (amdsvm_state_set_abi_version(state->vm_provider.state_abi) != 0) {
            error_report("AMD-SVM provider advertised an unsupported state ABI");
            IOServiceClose(connection);
            state->connection = IO_OBJECT_NULL;
            return -ENOTSUP;
        }
        if (state->diagnostic_gp_intercept) {
            if (!(state->vm_provider.caps.feature_flags &
                  AMDVM_VM_ABI_DRAFT_FEATURE_DIAGNOSTIC_GP_INTERCEPT)) {
                error_report("AMD-SVM provider lacks DIAGNOSTIC_GP_INTERCEPT; VMRUN was not called");
                IOServiceClose(connection);
                state->connection = IO_OBJECT_NULL;
                return -ENOTSUP;
            }
            state->vm_provider.diagnostic_gp_intercept = true;
        }
        if (state->diagnostic_first_pf) {
            if (!(state->vm_provider.caps.feature_flags &
                  AMDVM_VM_ABI_DRAFT_FEATURE_DIAGNOSTIC_PF_INTERCEPT)) {
                error_report("AMD-SVM provider lacks DIAGNOSTIC_PF_INTERCEPT; VMRUN was not called");
                IOServiceClose(connection);
                state->connection = IO_OBJECT_NULL;
                return -ENOTSUP;
            }
            state->vm_provider.diagnostic_pf_intercept = true;
        }
        if (!state->diagnostic_only && state->shared_run_enabled &&
            (state->vm_provider.caps.feature_flags &
             AMDVM_VM_ABI_DRAFT_FEATURE_SHARED_VCPU_RUN_V1)) {
            mach_vm_address_t address = 0;
            mach_vm_size_t mapped_size = 0;
            kern_return_t map_status = IOConnectMapMemory64(connection,
                AMDVM_VM_ABI_DRAFT_SHARED_RUN_MEMORY_TYPE,
                mach_task_self(), &address, &mapped_size, kIOMapAnywhere);
            if (map_status == KERN_SUCCESS &&
                mapped_size >= sizeof(amdvm_vm_shared_run_area_t)) {
                state->shared_run_mapping_address = address;
                state->shared_run_mapping_size = mapped_size;
                state->shared_run_mapped = true;
                state->vm_provider.shared_run_area =
                    (amdvm_vm_shared_run_area_t *)(uintptr_t)address;
                fprintf(stderr, "AMD-SVM shared RUN mapping active size=%llu\n",
                        (unsigned long long)mapped_size);
            } else {
                if (map_status == KERN_SUCCESS) {
                    (void)IOConnectUnmapMemory64(connection,
                        AMDVM_VM_ABI_DRAFT_SHARED_RUN_MEMORY_TYPE,
                        mach_task_self(), address);
                }
                error_report("AMD-SVM shared RUN mapping unavailable; using the legacy RUN selector");
            }
        }
        state->vm_expected_cpus = machine->smp.cpus;
    }
    amdsvm_state_init_emulator();
    amdsvm_provider_lifecycle_init(&state->provider_lifecycle,
                                   amdsvm_iokit_provider_transport, state);
    qatomic_set(&state->kick_generation, 0);
    qemu_mutex_init(&state->kick_lock);
    state->kick_lock_initialized = true;
    if (state->vm_api) {
        qemu_mutex_init(&state->vm_context_lock);
        state->vm_context_lock_initialized = true;
    }
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
    if (state->perf_state_entry) {
        qemu_del_vm_change_state_handler(state->perf_state_entry);
        state->perf_state_entry = NULL;
    }
    if (state->perf_stats_data) {
        amdsvm_perf_dump(state, "finalize");
        g_free(state->perf_stats_data);
        state->perf_stats_data = NULL;
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
    if (state->shared_run_mapped && !provider_context_retained &&
        state->connection != IO_OBJECT_NULL) {
        kern_return_t unmap_status = IOConnectUnmapMemory64(
            state->connection, AMDVM_VM_ABI_DRAFT_SHARED_RUN_MEMORY_TYPE,
            mach_task_self(), state->shared_run_mapping_address);
        if (unmap_status == KERN_SUCCESS) {
            state->shared_run_mapped = false;
            state->shared_run_mapping_address = 0;
            state->shared_run_mapping_size = 0;
            state->vm_provider.shared_run_area = NULL;
        } else {
            error_report("AMD-SVM shared RUN mapping could not be unmapped (mach 0x%x); preserving provider connection", unmap_status);
            provider_context_retained = true;
        }
    }
    if (state->kick_lock_initialized) {
        qemu_mutex_destroy(&state->kick_lock);
        state->kick_lock_initialized = false;
    }
    if (state->vm_context_lock_initialized) {
        qemu_mutex_destroy(&state->vm_context_lock);
        state->vm_context_lock_initialized = false;
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

    ac->name = "AMD SVM provider (experimental QEMU firmware)";
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
    object_class_property_add_bool(oc, "perf-stats",
                                   amdsvm_get_perf_stats,
                                   amdsvm_set_perf_stats);
    object_class_property_add_bool(oc, "diagnostic-cr0-window-trace",
                                   amdsvm_get_diagnostic_cr0_window_trace,
                                   amdsvm_set_diagnostic_cr0_window_trace);
    object_class_property_add_bool(oc, "diagnostic-transition-irq-gate",
                                   amdsvm_get_diagnostic_transition_irq_gate,
                                   amdsvm_set_diagnostic_transition_irq_gate);
    object_class_property_add_bool(oc, "diagnostic-gp-intercept",
                                   amdsvm_get_diagnostic_gp_intercept,
                                   amdsvm_set_diagnostic_gp_intercept);
    object_class_property_add_bool(oc, "diagnostic-first-pf",
                                   amdsvm_get_diagnostic_first_pf,
                                   amdsvm_set_diagnostic_first_pf);
    object_class_property_add_bool(oc, "diagnostic-only",
                                   amdsvm_get_diagnostic_only,
                                   amdsvm_set_diagnostic_only);
    object_class_property_add_bool(oc, "vm-api", amdsvm_get_vm_api, amdsvm_set_vm_api);
    object_class_property_add_bool(oc, "bql-spin", amdsvm_get_bql_spin,
                                   amdsvm_set_bql_spin);
    object_class_property_add_bool(oc, "mtrr-cache", amdsvm_get_mtrr_cache,
                                   amdsvm_set_mtrr_cache);
    object_class_property_add_bool(oc, "provider-pause", amdsvm_get_provider_pause,
                                   amdsvm_set_provider_pause);
    object_class_property_add_bool(oc, "shared-run", amdsvm_get_shared_run,
                                   amdsvm_set_shared_run);
    object_class_property_add_bool(oc, "native-rdtsc", amdsvm_get_native_rdtsc,
                                   amdsvm_set_native_rdtsc);
    object_class_property_add_bool(oc, "native-cr3",
                                   amdsvm_get_native_cr3,
                                   amdsvm_set_native_cr3);
    object_class_property_add_bool(oc, "diagnostic-validate-ram-map",
                                   amdsvm_get_diagnostic_validate_ram_map,
                                   amdsvm_set_diagnostic_validate_ram_map);
    object_class_property_add_bool(oc, "validate-finite-smp-rom",
                                   amdsvm_get_validate_finite_smp_rom,
                                   amdsvm_set_validate_finite_smp_rom);
    object_class_property_add_str(oc, "debugcon", amdsvm_get_debugcon,
                                  amdsvm_set_debugcon);
}

static void amdsvm_accel_instance_init(Object *obj)
{
    AMDVMAccelState *s = (AMDVMAccelState *)obj;

    s->shared_run_enabled = true;
    /* Native RDTSC is still experimental: on the 2026-10-29 Alpine 8-vCPU
     * profile it stalled in pv_native_safe_halt, while the same binary with
     * native-rdtsc=off reached login. Keep it explicit opt-in until the TSC
     * offset and timer interaction are validated across SMP and RAM sizes. */
    s->native_rdtsc_enabled = false;
    s->native_cr3_enabled = false;
    s->host_phys_bits = amdsvm_state_host_phys_bits();
    /* The bounded try-lock improved SMP guest progress in the 2026-10-28
     * Alpine A/B. Keep the regular BQL acquisition as a bounded fallback;
     * users can still opt out with bql-spin=off. */
    s->bql_spin_enabled = true;
}

static const TypeInfo amdsvm_accel_type = {
    .name = ACCEL_CLASS_NAME("amdsvm"),
    .parent = TYPE_ACCEL,
    .instance_size = sizeof(AMDVMAccelState),
    .instance_init = amdsvm_accel_instance_init,
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
