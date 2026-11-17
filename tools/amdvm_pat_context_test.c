/* Userspace test of the production PAT prepare/restore helpers. No entry. */
#include "../svm-kext/provider/amdvm_hlt_context.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    amdvm_hlt_context_t context = {0};
    amdvm_vmcb_t vmcb = {0};
    const uint64_t architectural = AMDVM_QEMU_RESET_PAT;
    context.guest_vmcb = &vmcb;
    context.guest_memory_mapped = 1;
    context.host_pat_valid = 1;
    context.host_pat_msr = AMDVM_GUEST_PAT_ALL_WB;
    context.memory_type_count = 2;
    amdvm_vmcb_set_u64(&vmcb, AMDVM_VMCB_GUEST_PAT, architectural);
    for (unsigned repetition = 0; repetition < 10; ++repetition) {
        assert(amdvm_hlt_context_prepare_guest_pat(&context) == 0);
        assert(context.execution_pat_active == 1);
        assert(amdvm_vmcb_get_u64(&vmcb, AMDVM_VMCB_GUEST_PAT) == 0);
        assert(context.architectural_guest_pat == architectural);
        assert(amdvm_hlt_context_prepare_guest_pat(&context) != 0);
        amdvm_hlt_context_restore_guest_pat(&context);
        assert(context.execution_pat_active == 0);
        assert(amdvm_vmcb_get_u64(&vmcb, AMDVM_VMCB_GUEST_PAT) == architectural);
        amdvm_hlt_context_restore_guest_pat(&context);
    }
    context.memory_types[1] = AMDVM_NPT_MEMTYPE_WB;
    assert(amdvm_hlt_context_prepare_guest_pat(&context) != 0);
    assert(context.execution_pat_active == 0);
    assert(amdvm_vmcb_get_u64(&vmcb, AMDVM_VMCB_GUEST_PAT) == architectural);
    puts("PASS: production PAT prepare/restore, repeat, rejection without mutation; no VMRUN");
    return 0;
}
