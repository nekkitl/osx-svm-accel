/* Local translation of QEMU RAM ranges to bounded provider pin descriptors. */
#ifndef AMDSVM_MEMORY_MAP_H
#define AMDSVM_MEMORY_MAP_H

#include "qapi/error.h"
#include "../../../../svm-kext/amdvm_probe_abi.h"

#define AMDSVM_MAX_GUEST_RANGES 64u
#define AMDSVM_GUEST_PAGE_SIZE 4096u
#define AMDSVM_GUEST_RANGE_READ  (1u << 0)
#define AMDSVM_GUEST_RANGE_WRITE (1u << 1)
#define AMDSVM_GUEST_RANGE_EXEC  (1u << 2)

typedef struct AMDVMGuestMemoryRange {
    uint64_t guest_physical_address;
    uint64_t host_virtual_address;
    uint64_t size;
    uint32_t flags;
} AMDVMGuestMemoryRange;

typedef struct AMDVMGuestMemoryMap {
    AMDVMGuestMemoryRange ranges[AMDSVM_MAX_GUEST_RANGES];
    uint32_t count;
    uint64_t total_ram;
    uint64_t total_writable_bytes;
    bool reset_rom_page_present;
    uint8_t reset_rom_page[AMDVM_VCPU_RESET_ROM_SIZE];
} AMDVMGuestMemoryMap;

/* Compare map meaning, excluding C struct padding bytes. */
bool amdsvm_memory_map_equal(const AMDVMGuestMemoryMap *a,
                            const AMDVMGuestMemoryMap *b);

/* Same host pages may back multiple GPAs when the permissions also match. */
bool amdsvm_memory_backing_alias_allowed(uint32_t first_flags,
                                        uint32_t second_flags);

/* Returns 0, -EINVAL for invalid input, or -ENOSPC when ABI slots run out. */
int amdsvm_memory_map_append_range(
    amdvm_vcpu_memory_map_request_t *request,
    uint64_t guest_physical_address, uint64_t user_virtual_address,
    uint64_t size, uint32_t flags);

/* Add one validated FlatView RAM/ROM span, extracting the reset vector page. */
bool amdsvm_memory_map_append_flatview_range(
    AMDVMGuestMemoryMap *map, uint64_t guest_physical_address,
    uint64_t host_virtual_address, uint64_t size, uint32_t flags,
    const uint8_t *host_backing, Error **errp);

#endif
