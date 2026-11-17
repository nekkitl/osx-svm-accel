/* Map QEMU FlatView memory and split it into provider pin descriptors. */
#include "qemu/osdep.h"

#include "amdsvm-memory-map.h"

bool amdsvm_memory_map_equal(const AMDVMGuestMemoryMap *a,
                             const AMDVMGuestMemoryMap *b)
{
    uint32_t ai = 0, bi = 0;
    uint64_t ao = 0, bo = 0;

    if (!a || !b || a->count > AMDSVM_MAX_GUEST_RANGES ||
        b->count > AMDSVM_MAX_GUEST_RANGES ||
        a->total_ram != b->total_ram ||
        a->total_writable_bytes != b->total_writable_bytes ||
        a->reset_rom_page_present != b->reset_rom_page_present) {
        return false;
    }
    if (a->reset_rom_page_present &&
        memcmp(a->reset_rom_page, b->reset_rom_page,
               sizeof(a->reset_rom_page))) {
        return false;
    }
    /* FlatView may split or coalesce a contiguous mapping as device regions
     * appear. Compare the translated GPA->HVA mapping, not its segmentation,
     * so an equivalent map does not trigger a full provider rebuild. */
    while (ai < a->count && bi < b->count) {
        const AMDVMGuestMemoryRange *ar = &a->ranges[ai];
        const AMDVMGuestMemoryRange *br = &b->ranges[bi];
        uint64_t agpa, ahva, bgpa, bhva;
        uint64_t remaining_a, remaining_b, span;

        if (ao >= ar->size || bo >= br->size ||
            ar->guest_physical_address > UINT64_MAX - ao ||
            ar->host_virtual_address > UINT64_MAX - ao ||
            br->guest_physical_address > UINT64_MAX - bo ||
            br->host_virtual_address > UINT64_MAX - bo) {
            return false;
        }
        agpa = ar->guest_physical_address + ao;
        ahva = ar->host_virtual_address + ao;
        bgpa = br->guest_physical_address + bo;
        bhva = br->host_virtual_address + bo;
        if (agpa != bgpa || ahva != bhva || ar->flags != br->flags) {
            return false;
        }
        remaining_a = ar->size - ao;
        remaining_b = br->size - bo;
        span = MIN(remaining_a, remaining_b);
        ao += span;
        bo += span;
        if (ao == ar->size) {
            ai++;
            ao = 0;
        }
        if (bo == br->size) {
            bi++;
            bo = 0;
        }
    }
    return ai == a->count && bi == b->count;
}

static bool amdsvm_memory_map_append_coalesced(
    AMDVMGuestMemoryMap *map, uint64_t gpa, uint64_t hva,
    uint64_t size, uint32_t flags, Error **errp)
{
    if (size == 0) {
        return true;
    }
    if (gpa > UINT64_MAX - size || hva > UINT64_MAX - size ||
        map->total_ram > UINT64_MAX - size ||
        ((flags & AMDSVM_GUEST_RANGE_WRITE) &&
         map->total_writable_bytes > UINT64_MAX - size)) {
        error_setg(errp, "AMD-SVM guest memory range overflows address space");
        return false;
    }
    if (map->count != 0) {
        AMDVMGuestMemoryRange *previous = &map->ranges[map->count - 1u];
        if (previous->flags == flags &&
            previous->guest_physical_address <= UINT64_MAX - previous->size &&
            previous->host_virtual_address <= UINT64_MAX - previous->size &&
            previous->guest_physical_address + previous->size == gpa &&
            previous->host_virtual_address + previous->size == hva) {
            if (previous->size > UINT64_MAX - size) {
                error_setg(errp, "AMD-SVM coalesced memory range overflows");
                return false;
            }
            previous->size += size;
            map->total_ram += size;
            if (flags & AMDSVM_GUEST_RANGE_WRITE) {
                map->total_writable_bytes += size;
            }
            return true;
        }
    }
    if (map->count >= AMDSVM_MAX_GUEST_RANGES) {
        error_setg(errp, "AMD-SVM guest RAM has too many FlatView ranges");
        return false;
    }
    map->ranges[map->count++] = (AMDVMGuestMemoryRange) {
        .guest_physical_address = gpa,
        .host_virtual_address = hva,
        .size = size,
        .flags = flags,
    };
    map->total_ram += size;
    if (flags & AMDSVM_GUEST_RANGE_WRITE) {
        map->total_writable_bytes += size;
    }
    return true;
}

bool amdsvm_memory_map_append_flatview_range(
    AMDVMGuestMemoryMap *map, uint64_t gpa, uint64_t hva, uint64_t size,
    uint32_t flags, const uint8_t *host_backing, Error **errp)
{
    const uint64_t reset_gpa = AMDVM_VCPU_RESET_ROM_GPA;
    const uint64_t reset_end = reset_gpa + AMDSVM_GUEST_PAGE_SIZE;
    AMDVMGuestMemoryMap original;
    uint64_t end;

    if (!map || (size != 0 && (gpa > UINT64_MAX - size ||
                              hva > UINT64_MAX - size))) {
        error_setg(errp, "AMD-SVM FlatView range arguments are invalid");
        return false;
    }
    if (size == 0) {
        return true;
    }
    end = gpa + size;
    if (gpa >= reset_end || end <= reset_gpa) {
        return amdsvm_memory_map_append_coalesced(map, gpa, hva,
                                                  size, flags, errp);
    }
    if (flags != (AMDSVM_GUEST_RANGE_READ | AMDSVM_GUEST_RANGE_EXEC) ||
        gpa > reset_gpa || end < reset_end ||
        map->reset_rom_page_present || host_backing == NULL) {
        error_setg(errp,
                   "AMD-SVM reset-vector page must be backed by one ROM section");
        return false;
    }

    original = *map;
    if (!amdsvm_memory_map_append_coalesced(map, gpa, hva,
            reset_gpa - gpa, flags, errp) ||
        !amdsvm_memory_map_append_coalesced(map, reset_end,
            hva + (reset_end - gpa), end - reset_end, flags, errp)) {
        *map = original;
        return false;
    }
    memcpy(map->reset_rom_page, host_backing + (reset_gpa - gpa),
           sizeof(map->reset_rom_page));
    map->reset_rom_page_present = true;
    return true;
}

bool amdsvm_memory_backing_alias_allowed(uint32_t first_flags,
                                        uint32_t second_flags)
{
    const uint32_t valid_flags = AMDVM_VCPU_MEMORY_READ |
                                 AMDVM_VCPU_MEMORY_WRITE |
                                 AMDVM_VCPU_MEMORY_EXEC;

    return first_flags == second_flags &&
           (first_flags & AMDVM_VCPU_MEMORY_READ) != 0 &&
           (first_flags & ~valid_flags) == 0;
}

int amdsvm_memory_map_append_range(
    amdvm_vcpu_memory_map_request_t *request,
    uint64_t guest_physical_address, uint64_t user_virtual_address,
    uint64_t size, uint32_t flags)
{
    const uint32_t valid_flags = AMDVM_VCPU_MEMORY_READ |
                                 AMDVM_VCPU_MEMORY_WRITE |
                                 AMDVM_VCPU_MEMORY_EXEC;
    uint64_t needed;
    uint64_t offset = 0;
    uint64_t total = 0;
    uint64_t writable = 0;
    uint32_t i;

    if (!request || request->range_count > AMDVM_VCPU_MEMORY_MAX_RANGES ||
        size == 0 ||
        ((guest_physical_address | user_virtual_address | size) & 4095u) ||
        user_virtual_address == 0 ||
        guest_physical_address > UINT64_MAX - size ||
        user_virtual_address > UINT64_MAX - size ||
        !(flags & AMDVM_VCPU_MEMORY_READ) || (flags & ~valid_flags)) {
        return -EINVAL;
    }

    for (i = 0; i < request->range_count; i++) {
        if (request->ranges[i].size >
            AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES - total) {
            return -EINVAL;
        }
        total += request->ranges[i].size;
        if (request->ranges[i].flags & AMDVM_VCPU_MEMORY_WRITE) {
            if (request->ranges[i].size >
                AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES - writable) {
                return -EINVAL;
            }
            writable += request->ranges[i].size;
        }
    }
    if (size > AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES - total) {
        return -E2BIG;
    }
    if ((flags & AMDVM_VCPU_MEMORY_WRITE) &&
        size > AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES - writable) {
        return -E2BIG;
    }

    needed = size / AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES +
             (size % AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES != 0);
    if (needed > AMDVM_VCPU_MEMORY_MAX_RANGES - request->range_count) {
        return -ENOSPC;
    }

    while (offset < size) {
        uint64_t chunk = MIN(size - offset,
                             AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES);
        amdvm_vcpu_memory_range_t *range =
            &request->ranges[request->range_count++];

        range->guest_physical_address = guest_physical_address + offset;
        range->user_virtual_address = user_virtual_address + offset;
        range->size = chunk;
        range->flags = flags;
        range->reserved = 0;
        offset += chunk;
    }
    return 0;
}
