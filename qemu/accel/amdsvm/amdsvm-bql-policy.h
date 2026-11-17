/*
 * AMD-SVM BQL reacquisition experiment.
 *
 * A bounded number of non-blocking attempts may catch a short critical
 * section before sleeping. The caller must use the normal BQL lock if this
 * helper exhausts its budget.
 */
#ifndef AMDSVM_BQL_POLICY_H
#define AMDSVM_BQL_POLICY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct AMDVMBQLSpinResult {
    uint32_t attempts;
    bool acquired;
} AMDVMBQLSpinResult;

typedef bool (*AMDSVMBQLTryAcquireFn)(void *opaque);
typedef void (*AMDSVMBQLRelaxFn)(void *opaque);

static inline AMDVMBQLSpinResult
amdsvm_bql_try_acquire_bounded(uint32_t budget,
                               AMDSVMBQLTryAcquireFn try_acquire,
                               AMDSVMBQLRelaxFn relax, void *opaque)
{
    AMDVMBQLSpinResult result = { 0 };

    if (!budget || !try_acquire) {
        return result;
    }

    while (result.attempts < budget) {
        result.attempts++;
        if (try_acquire(opaque)) {
            result.acquired = true;
            return result;
        }
        if (relax && result.attempts < budget) {
            relax(opaque);
        }
    }
    return result;
}

#endif
