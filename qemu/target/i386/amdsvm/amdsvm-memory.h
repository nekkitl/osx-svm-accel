/* Local AMD-SVM guest RAM enumeration from QEMU's active FlatView. */
#ifndef AMDSVM_MEMORY_H
#define AMDSVM_MEMORY_H

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "amdsvm-memory-map.h"
#include "amdsvm-msr.h"

typedef enum AMDVMGuestMTRRPolicy {
    /* Preserve guest MTRRs exactly when hPAT can represent their types. */
    AMDVM_GUEST_MTRR_EXACT,
    /* VirtualBox-style MVP profile: execute backed guest memory as WB. */
    AMDVM_GUEST_MTRR_COMPAT_ALL_WB,
} AMDVMGuestMTRRPolicy;

bool amdsvm_collect_guest_ram(FlatView *flatview,
                              AMDVMGuestMemoryMap *map,
                              Error **errp);
bool amdsvm_build_memory_map_request(
    const AMDVMGuestMemoryMap *map,
    amdvm_vcpu_memory_map_request_t *request,
    Error **errp);
bool amdsvm_build_reset_rom_request(
    const AMDVMGuestMemoryMap *map,
    amdvm_vcpu_set_reset_rom_request_t *request,
    Error **errp);
bool amdsvm_build_memory_type_request(
    const AMDVMGuestMemoryMap *map,
    const AMDVMMTRRState *mtrr,
    AMDVMGuestMTRRPolicy policy,
    amdvm_vcpu_set_memory_types_request_t *request,
    Error **errp);

/* Build against the exact map segmentation pinned by the VM provider. */
bool amdsvm_build_published_memory_type_request(
    const AMDVMGuestMemoryMap *current_map,
    const AMDVMGuestMemoryMap *pinned_map,
    bool provider_map_is_authoritative,
    const AMDVMMTRRState *mtrr,
    AMDVMGuestMTRRPolicy policy,
    amdvm_vcpu_set_memory_types_request_t *request,
    Error **errp);

#endif
