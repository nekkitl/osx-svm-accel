/* Build complete, page-aligned NPT memory-type maps from QEMU's MTRR state. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "amdsvm-memory.h"

static bool amdsvm_append_memory_type(
    amdvm_vcpu_set_memory_types_request_t *request,
    uint64_t gpa, uint8_t type, bool allow_extend, Error **errp)
{
    amdvm_vcpu_memory_type_range_t *last;

    if (allow_extend && request->range_count != 0) {
        last = &request->ranges[request->range_count - 1u];
        if (last->memory_type == type &&
            last->guest_physical_address + last->size == gpa) {
            if (last->size > UINT64_MAX - AMDSVM_GUEST_PAGE_SIZE) {
                error_setg(errp, "AMD-SVM memory-type range size overflowed");
                return false;
            }
            last->size += AMDSVM_GUEST_PAGE_SIZE;
            return true;
        }
    }
    if (request->range_count >= AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES) {
        error_setg(errp, "AMD-SVM MTRR map needs too many NPT memory-type ranges");
        return false;
    }
    request->ranges[request->range_count++] = (amdvm_vcpu_memory_type_range_t) {
        .guest_physical_address = gpa,
        .size = AMDSVM_GUEST_PAGE_SIZE,
        .memory_type = type,
    };
    return true;
}

bool amdsvm_build_memory_type_request(
    const AMDVMGuestMemoryMap *map,
    const AMDVMMTRRState *mtrr,
    AMDVMGuestMTRRPolicy policy,
    amdvm_vcpu_set_memory_types_request_t *request,
    Error **errp)
{
    bool reset_added = false;
    uint32_t i;

    if (!map || !mtrr || !request ||
        (policy != AMDVM_GUEST_MTRR_EXACT &&
         policy != AMDVM_GUEST_MTRR_COMPAT_ALL_WB) || map->count == 0 ||
        map->count > AMDVM_VCPU_MEMORY_MAX_RANGES ||
        !map->reset_rom_page_present) {
        error_setg(errp, "AMD-SVM memory-type map arguments are invalid");
        return false;
    }
    memset(request, 0, sizeof(*request));
    request->abi_version = AMDVM_VCPU_ABI_VERSION;

    for (i = 0; i < map->count; i++) {
        const AMDVMGuestMemoryRange *range = &map->ranges[i];
        uint64_t page, end;

        if (!range->size ||
            (range->guest_physical_address & (AMDSVM_GUEST_PAGE_SIZE - 1u)) ||
            (range->size & (AMDSVM_GUEST_PAGE_SIZE - 1u)) ||
            range->guest_physical_address > UINT64_MAX - range->size) {
            error_setg(errp, "AMD-SVM cannot derive MTRR types for an invalid RAM range");
            goto fail;
        }
        if (!reset_added &&
            AMDVM_VCPU_RESET_ROM_GPA < range->guest_physical_address) {
            uint8_t guest_type, type;
            if (!amdsvm_mtrr_effective_type(mtrr,
                    AMDVM_VCPU_RESET_ROM_GPA, &guest_type)) {
                error_setg(errp, "AMD-SVM cannot resolve reset-ROM MTRR type");
                goto fail;
            }
            type = policy == AMDVM_GUEST_MTRR_COMPAT_ALL_WB ?
                AMDVM_MTRR_TYPE_WB : guest_type;
            if (!amdsvm_append_memory_type(request,
                    AMDVM_VCPU_RESET_ROM_GPA, type, false, errp)) {
                goto fail;
            }
            reset_added = true;
        }
        end = range->guest_physical_address + range->size;
        for (page = range->guest_physical_address; page < end;
             page += AMDSVM_GUEST_PAGE_SIZE) {
            uint8_t guest_type, type;
            if (!amdsvm_mtrr_effective_type(mtrr, page, &guest_type)) {
                error_setg(errp,
                    "AMD-SVM guest MTRR state is undefined at GPA 0x%llx",
                    (unsigned long long)page);
                goto fail;
            }
            type = policy == AMDVM_GUEST_MTRR_COMPAT_ALL_WB ?
                AMDVM_MTRR_TYPE_WB : guest_type;
            if (!amdsvm_append_memory_type(request, page, type,
                    page != range->guest_physical_address, errp)) {
                goto fail;
            }
        }
    }
    if (!reset_added) {
        uint8_t guest_type, type;
        if (!amdsvm_mtrr_effective_type(mtrr,
                AMDVM_VCPU_RESET_ROM_GPA, &guest_type)) {
            error_setg(errp, "AMD-SVM cannot resolve reset-ROM MTRR type");
            goto fail;
        }
        type = policy == AMDVM_GUEST_MTRR_COMPAT_ALL_WB ?
            AMDVM_MTRR_TYPE_WB : guest_type;
        if (!amdsvm_append_memory_type(request, AMDVM_VCPU_RESET_ROM_GPA,
                type, false, errp)) {
            goto fail;
        }
    }
    return true;

fail:
    memset(request, 0, sizeof(*request));
    return false;
}
