#include "../svm-kext/provider/amdvm_vmcb_clean_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	amdvm_vmcb_clean_policy_t policy;
	memset(&policy, 0, sizeof(policy));
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 2, 7, 0x1000,
	    AMDVM_VMCB_CLEAN_SEG | AMDVM_VMCB_CLEAN_CR) == 0);

	amdvm_vmcb_clean_policy_complete(&policy, 2, 7, 0x1000, 1,
	    AMDVM_SVM_EXIT_HLT);
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 2, 7, 0x1000,
	    AMDVM_VMCB_CLEAN_SEG | AMDVM_VMCB_CLEAN_CR) ==
	    (AMDVM_VMCB_CLEAN_STABLE_RUN_MASK | AMDVM_VMCB_CLEAN_SEG |
	    AMDVM_VMCB_CLEAN_CR));
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 3, 7, 0x1000,
	    AMDVM_VMCB_CLEAN_SEG) == 0);
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 2, 8, 0x1000,
	    AMDVM_VMCB_CLEAN_SEG) == 0);
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 2, 7, 0x2000,
	    AMDVM_VMCB_CLEAN_SEG) == 0);

	amdvm_vmcb_clean_policy_complete(&policy, 2, 7, 0x1000, 0,
	    AMDVM_SVM_EXIT_HLT);
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 2, 7, 0x1000, 0) == 0);
	amdvm_vmcb_clean_policy_complete(&policy, 2, 7, 0x1000, 1,
	    AMDVM_SVM_EXIT_NOT_RUN);
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 2, 7, 0x1000, 0) == 0);
	amdvm_vmcb_clean_policy_complete(&policy, 2, 7, 0x1000, 1,
	    AMDVM_SVM_EXIT_INVALID);
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 2, 7, 0x1000, 0) == 0);
	amdvm_vmcb_clean_policy_complete(&policy, 2, 7, 0x1000, 1,
	    AMDVM_SVM_EXIT_ERR);
	assert(amdvm_vmcb_clean_policy_prepare(&policy, 2, 7, 0x1000, 0) == 0);

	puts("PASS: VMCB clean policy requires same CPU/ASID/page and successful prior run; only stable and input-equal groups are reused");
	return 0;
}
