#ifndef AMDVM_VM_NPT_H
#define AMDVM_VM_NPT_H
#include "amdvm_vm_memory.h"
#include "amdvm_npt.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Caller holds a VM-map borrow and discards the fresh NPT on failure.
 * check_pa validates host memory type and excludes all vCPU control memory. */
typedef int (*amdvm_vm_npt_check_pa_t)(uint64_t pa, void *opaque);
int amdvm_vm_npt_required_pages(const amdvm_vm_memory_t *, size_t *pages);
int amdvm_vm_npt_build(const amdvm_vm_memory_t *, amdvm_npt_t *,
    uint64_t reset_pa, amdvm_vm_npt_check_pa_t check_pa, void *opaque);
#ifdef __cplusplus
}
#endif
#endif
