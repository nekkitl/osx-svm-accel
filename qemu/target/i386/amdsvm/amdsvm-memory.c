/* Local AMD-SVM enumeration of QEMU's mapped guest RAM pages. */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "amdsvm-memory.h"
#include "amdsvm-memory-map.h"

typedef struct AMDVMCollectState {
    AMDVMGuestMemoryMap *map;
    Error **errp;
    bool failed;
} AMDVMCollectState;

/* Mapping diagnostics run on every map refresh; keep smoke logs bounded. */
static gint amdsvm_memory_region_log_count;
#define AMDSVM_MEMORY_REGION_LOG_LIMIT 512

/*
 * QEMU can expose immutable firmware at more than one GPA while both ranges
 * point into the same host ROM allocation. Preserve those guest-visible
 * aliases: the provider can map one immutable host page at multiple GPAs.
 * RAM aliases (including any ROM/RAM overlap) remain an error.
 */
static bool amdsvm_canonicalize_rom_aliases(AMDVMGuestMemoryMap *map,
                                            Error **errp)
{
    const uint32_t rom_flags = AMDSVM_GUEST_RANGE_READ |
                               AMDSVM_GUEST_RANGE_EXEC;
    AMDVMGuestMemoryMap normalized = { 0 };

    normalized.reset_rom_page_present = map->reset_rom_page_present;
    memcpy(normalized.reset_rom_page, map->reset_rom_page,
           sizeof(normalized.reset_rom_page));

    for (uint32_t i = 0; i < map->count; i++) {
        const AMDVMGuestMemoryRange *source = &map->ranges[i];
        uint64_t source_end = source->host_virtual_address + source->size;

        if (source->flags != rom_flags) {
            for (uint32_t j = 0; j < normalized.count; j++) {
                const AMDVMGuestMemoryRange *prior = &normalized.ranges[j];
                uint64_t prior_end = prior->host_virtual_address + prior->size;
                if (source->host_virtual_address < prior_end &&
                    prior->host_virtual_address < source_end) {
                    error_setg(errp,
                        "AMD-SVM rejects overlapping non-ROM host backing ranges: "
                        "range[%u] GPA=0x%llx HVA=0x%llx size=0x%llx flags=0x%x "
                        "overlaps range[%u] GPA=0x%llx HVA=0x%llx size=0x%llx flags=0x%x",
                        i,
                        (unsigned long long)source->guest_physical_address,
                        (unsigned long long)source->host_virtual_address,
                        (unsigned long long)source->size, source->flags,
                        j,
                        (unsigned long long)prior->guest_physical_address,
                        (unsigned long long)prior->host_virtual_address,
                        (unsigned long long)prior->size, prior->flags);
                    return false;
                }
            }
            if (!amdsvm_memory_map_append_flatview_range(&normalized,
                    source->guest_physical_address,
                    source->host_virtual_address, source->size,
                    source->flags, NULL, errp)) {
                return false;
            }
            continue;
        }

        for (uint32_t j = 0; j < normalized.count; j++) {
            const AMDVMGuestMemoryRange *prior = &normalized.ranges[j];
            uint64_t prior_end = prior->host_virtual_address + prior->size;

            if (source->host_virtual_address < prior_end &&
                prior->host_virtual_address < source_end &&
                prior->flags != rom_flags) {
                error_setg(errp,
                    "AMD-SVM ROM backing overlaps non-ROM guest memory");
                return false;
            }
        }
        if (!amdsvm_memory_map_append_flatview_range(&normalized,
                source->guest_physical_address,
                source->host_virtual_address, source->size,
                source->flags, NULL, errp)) {
            return false;
        }
    }

    *map = normalized;
    return true;
}

static bool amdsvm_collect_range(Int128 start, Int128 len,
                                 const MemoryRegion *mr,
                                 hwaddr offset_in_region, void *opaque)
{
    AMDVMCollectState *state = opaque;
    AMDVMGuestMemoryMap *map = state->map;
    uint64_t gpa, size, hva;
    uint32_t flags;
    int log_index;
    void *ram_ptr;
    bool rom;

    if (!memory_region_is_ram(mr)) {
        return false;
    }
    if (int128_gethi(start) != 0 || int128_gethi(len) != 0 ||
        int128_getlo(len) == 0 || memory_region_is_ram_device(mr) ||
        memory_region_is_protected(mr)) {
        error_setg(state->errp,
                   "AMD-SVM cannot pin this QEMU RAM section");
        state->failed = true;
        return true;
    }

    gpa = int128_get64(start);
    size = int128_get64(len);
    if ((gpa & (AMDSVM_GUEST_PAGE_SIZE - 1u)) != 0 ||
        (offset_in_region & (AMDSVM_GUEST_PAGE_SIZE - 1u)) != 0 ||
        (size & (AMDSVM_GUEST_PAGE_SIZE - 1u)) != 0 ||
        gpa > UINT64_MAX - size ||
        map->total_ram > UINT64_MAX - size) {
        error_setg(state->errp,
                   "AMD-SVM requires page-aligned, non-overflowing RAM sections");
        state->failed = true;
        return true;
    }

    ram_ptr = memory_region_get_ram_ptr(mr);
    if (ram_ptr == NULL || (uintptr_t)ram_ptr > UINT64_MAX - offset_in_region) {
        error_setg(state->errp, "AMD-SVM could not obtain QEMU RAM backing");
        state->failed = true;
        return true;
    }
    hva = (uint64_t)(uintptr_t)ram_ptr + offset_in_region;
    if ((hva & (AMDSVM_GUEST_PAGE_SIZE - 1u)) != 0 ||
        hva > UINT64_MAX - size) {
        error_setg(state->errp,
                   "AMD-SVM requires page-aligned QEMU host RAM backing");
        state->failed = true;
        return true;
    }

    rom = memory_region_is_rom(mr);
    flags = AMDSVM_GUEST_RANGE_READ | AMDSVM_GUEST_RANGE_EXEC;
    if (!rom) {
        flags |= AMDSVM_GUEST_RANGE_WRITE;
    }
    log_index = g_atomic_int_add(&amdsvm_memory_region_log_count, 1);
    if (log_index < AMDSVM_MEMORY_REGION_LOG_LIMIT) {
        error_report("AMDVM_MEMORY_REGION name=%s gpa=0x%llx size=0x%llx host=0x%llx ram=%u rom=%u flags=0x%x",
                     memory_region_name(mr), (unsigned long long)gpa,
                     (unsigned long long)size, (unsigned long long)hva,
                     memory_region_is_ram(mr), rom, flags);
    } else if (log_index == AMDSVM_MEMORY_REGION_LOG_LIMIT) {
        error_report("AMDVM_MEMORY_REGION further entries suppressed after %u lines",
                     AMDSVM_MEMORY_REGION_LOG_LIMIT);
    }
    if (log_index < AMDSVM_MEMORY_REGION_LOG_LIMIT && rom &&
        gpa == UINT64_C(0x000c0000) && size >= 4) {
        const uint8_t *rom_bytes = (const uint8_t *)ram_ptr + offset_in_region;
        uint64_t rom_image_size = (uint64_t)rom_bytes[2] * 512u;
        uint8_t checksum = 0;

        if (rom_image_size <= size) {
            for (uint64_t i = 0; i < rom_image_size; i++) {
                checksum += rom_bytes[i];
            }
        }
        error_report("AMDVM_VGA_ROM_MAP signature=%02x%02x size_units_512=%u checksum_valid=%u checksum8=0x%02x",
                     rom_bytes[0], rom_bytes[1], rom_bytes[2],
                     rom_image_size <= size, checksum);
    }
    if (!amdsvm_memory_map_append_flatview_range(map, gpa, hva, size, flags,
            (const uint8_t *)ram_ptr + offset_in_region, state->errp)) {
        state->failed = true;
        return true;
    }
    return false;
}

bool amdsvm_collect_guest_ram(FlatView *flatview,
                              AMDVMGuestMemoryMap *map,
                              Error **errp)
{
    AMDVMCollectState state = {
        .map = map,
        .errp = errp,
    };

    if (flatview == NULL || map == NULL) {
        error_setg(errp, "AMD-SVM guest RAM map arguments are invalid");
        return false;
    }
    memset(map, 0, sizeof(*map));
    flatview_for_each_range(flatview, amdsvm_collect_range, &state);
    if (state.failed) {
        memset(map, 0, sizeof(*map));
        return false;
    }
    if (!amdsvm_canonicalize_rom_aliases(map, errp)) {
        memset(map, 0, sizeof(*map));
        return false;
    }
    if (map->count == 0) {
        error_setg(errp, "QEMU FlatView contains no RAM for AMD-SVM");
        return false;
    }
    return true;
}

bool amdsvm_build_memory_map_request(
    const AMDVMGuestMemoryMap *map,
    amdvm_vcpu_memory_map_request_t *request,
    Error **errp)
{
    uint64_t previous_end = 0;
    uint64_t total_mapped_bytes = 0;
    uint64_t total_writable_bytes = 0;
    uint32_t i;

    if (!map || !request || map->count == 0 ||
        map->count > AMDVM_VCPU_MEMORY_MAX_RANGES ||
        map->total_ram > AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES ||
        map->total_writable_bytes > AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES) {
        error_setg(errp, "AMD-SVM QEMU RAM map cannot be represented by provider ABI");
        return false;
    }
    memset(request, 0, sizeof(*request));
    request->abi_version = AMDVM_VCPU_ABI_VERSION;
    for (i = 0; i < map->count; i++) {
        const AMDVMGuestMemoryRange *source = &map->ranges[i];
        uint64_t end;
        uint32_t flags = 0;

        if (!source->host_virtual_address || !source->size ||
            (source->guest_physical_address &
             (AMDSVM_GUEST_PAGE_SIZE - 1u)) ||
            (source->host_virtual_address &
             (AMDSVM_GUEST_PAGE_SIZE - 1u)) ||
            (source->size & (AMDSVM_GUEST_PAGE_SIZE - 1u)) ||
            source->guest_physical_address > UINT64_MAX - source->size ||
            source->host_virtual_address > UINT64_MAX - source->size ||
            (source->flags & AMDSVM_GUEST_RANGE_READ) == 0 ||
            (source->flags & ~(AMDSVM_GUEST_RANGE_READ |
                               AMDSVM_GUEST_RANGE_WRITE |
                               AMDSVM_GUEST_RANGE_EXEC)) ||
            (i != 0 && source->guest_physical_address < previous_end)) {
            error_setg(errp, "AMD-SVM QEMU RAM map contains an invalid or overlapping range");
            memset(request, 0, sizeof(*request));
            return false;
        }
        if (source->size >
            AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES - total_mapped_bytes) {
            error_setg(errp,
                       "AMD-SVM mapped memory exceeds the total provider limit");
            memset(request, 0, sizeof(*request));
            return false;
        }
        total_mapped_bytes += source->size;
        if (source->flags & AMDSVM_GUEST_RANGE_WRITE) {
            if (source->size > AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES -
                               total_writable_bytes) {
                error_setg(errp,
                           "AMD-SVM writable guest RAM exceeds the provider ABI capacity");
                memset(request, 0, sizeof(*request));
                return false;
            }
            total_writable_bytes += source->size;
        }
        end = source->guest_physical_address + source->size;
        if (source->guest_physical_address <
                AMDVM_VCPU_RESET_ROM_GPA + AMDSVM_GUEST_PAGE_SIZE &&
            AMDVM_VCPU_RESET_ROM_GPA < end) {
            error_setg(errp, "AMD-SVM memory map still includes reset ROM page");
            memset(request, 0, sizeof(*request));
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            const AMDVMGuestMemoryRange *prior = &map->ranges[j];
            uint64_t prior_end = prior->host_virtual_address + prior->size;
            uint64_t source_end = source->host_virtual_address + source->size;
            if (source->host_virtual_address < prior_end &&
                prior->host_virtual_address < source_end &&
                !amdsvm_memory_backing_alias_allowed(source->flags,
                                                       prior->flags)) {
                error_setg(errp,
                    "AMD-SVM host backing overlap: range[%u] GPA=0x%llx HVA=0x%llx size=0x%llx flags=0x%x overlaps range[%u] GPA=0x%llx HVA=0x%llx size=0x%llx flags=0x%x",
                    i,
                    (unsigned long long)source->guest_physical_address,
                    (unsigned long long)source->host_virtual_address,
                    (unsigned long long)source->size, source->flags,
                    j,
                    (unsigned long long)prior->guest_physical_address,
                    (unsigned long long)prior->host_virtual_address,
                    (unsigned long long)prior->size, prior->flags);
                memset(request, 0, sizeof(*request));
                return false;
            }
        }
        if (source->flags & AMDSVM_GUEST_RANGE_READ) {
            flags |= AMDVM_VCPU_MEMORY_READ;
        }
        if (source->flags & AMDSVM_GUEST_RANGE_WRITE) {
            flags |= AMDVM_VCPU_MEMORY_WRITE;
        }
        if (source->flags & AMDSVM_GUEST_RANGE_EXEC) {
            flags |= AMDVM_VCPU_MEMORY_EXEC;
        }
        if (amdsvm_memory_map_append_range(request,
                source->guest_physical_address,
                source->host_virtual_address, source->size, flags) != 0) {
            error_setg(errp,
                       "AMD-SVM RAM map exceeds provider descriptor capacity");
            memset(request, 0, sizeof(*request));
            return false;
        }
        previous_end = end;
    }
    if (total_mapped_bytes != map->total_ram ||
        total_writable_bytes != map->total_writable_bytes) {
        error_setg(errp,
                   "AMD-SVM QEMU RAM map totals do not match its ranges");
        memset(request, 0, sizeof(*request));
        return false;
    }
    return true;
}

bool amdsvm_build_reset_rom_request(
    const AMDVMGuestMemoryMap *map,
    amdvm_vcpu_set_reset_rom_request_t *request,
    Error **errp)
{
    if (!map || !request || !map->reset_rom_page_present) {
        error_setg(errp,
                   "QEMU FlatView has no complete firmware page at reset GPA 0xfffff000");
        return false;
    }
    memset(request, 0, sizeof(*request));
    request->abi_version = AMDVM_VCPU_ABI_VERSION;
    memcpy(request->page, map->reset_rom_page,
           sizeof(request->page));
    return true;
}
