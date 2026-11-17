#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <mach/mach_time.h>
#include "amdvm_probe_abi.h"
#include "provider/amdvm_hlt_context.h"
#include "provider/amdvm_client_mode.h"
#include "provider/amdvm_vm_negotiate.h"
#include "provider/amdvm_vm_memory.h"
#include "provider/amdvm_vm_cpu_state.h"
#include "provider/amdvm_kick_logic.h"
#include "provider/amdvm_vcpu_lifecycle.h"
#include "provider/amdvm_vmcb.h"
#include "provider/amdvm_npt.h"
#include "provider/amdvm_xstate.h"
#include "provider/amdvm_xstate_capture_policy.h"
#include "provider/amdvm_pause_fastpath.h"
#include "provider/amdvm_run_buffer.h"
#include "provider/amdvm_run_slot_lifetime.h"
#include "amdvm_vm_shared_run.h"

extern "C" int cpu_number(void);

static amdvm_hlt_context_t gHltContext = {};
static volatile uint32_t gHltContextBusy = 0;
static volatile uint32_t gHltFpuPreflightPassed = 0;
static volatile uint32_t gVcpuFpuRejectLogged = 0;
static volatile uint32_t gVcpuContextCount = 0;
static volatile uint32_t gVcpuLeaseCount = 0;
static volatile uint32_t gVcpuRunNotReadyLogCount = 0;
static volatile uint32_t gInvalidVmcbFailureReported = 0;

static void
amdvm_pause_stats_report(uint32_t vcpu_id, uint64_t kick_generation,
	uint64_t entries, uint64_t handled, uint64_t budget, uint64_t events,
	uint64_t unhandled)
{
	IOLog("AMDVM_PAUSE_STATS vcpu=%u kick_generation=%llu fast_path_entries=%llu pause_handled=%llu returns_budget=%llu returns_event=%llu returns_unhandled=%llu\n",
	    vcpu_id, (unsigned long long)kick_generation,
	    (unsigned long long)entries, (unsigned long long)handled,
	    (unsigned long long)budget, (unsigned long long)events,
	    (unsigned long long)unhandled);
}

#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
static void
amdvm_provider_timing_report(uint32_t vcpu_id, uint64_t runs,
	uint64_t state_set_ticks, uint64_t xstate_preflight_ticks,
	uint64_t interrupt_off_ticks, uint64_t run_ticks, uint64_t state_get_ticks,
	uint64_t memory_type_ticks, uint64_t owner_acquire_ticks,
	uint64_t vmcb_prepare_ticks, uint64_t vmrun_xstate_ticks,
	uint64_t owner_release_ticks, uint64_t vmrun_samples)
{
	uint64_t state_set_ns = 0, xstate_preflight_ns = 0;
	uint64_t interrupt_off_ns = 0, run_ns = 0, state_get_ns = 0;
	uint64_t memory_type_ns = 0, owner_acquire_ns = 0;
	uint64_t vmcb_prepare_ns = 0, vmrun_xstate_ns = 0, owner_release_ns = 0;
	absolutetime_to_nanoseconds(state_set_ticks, &state_set_ns);
	absolutetime_to_nanoseconds(xstate_preflight_ticks, &xstate_preflight_ns);
	absolutetime_to_nanoseconds(interrupt_off_ticks, &interrupt_off_ns);
	absolutetime_to_nanoseconds(run_ticks, &run_ns);
	absolutetime_to_nanoseconds(state_get_ticks, &state_get_ns);
	absolutetime_to_nanoseconds(memory_type_ticks, &memory_type_ns);
	absolutetime_to_nanoseconds(owner_acquire_ticks, &owner_acquire_ns);
	absolutetime_to_nanoseconds(vmcb_prepare_ticks, &vmcb_prepare_ns);
	absolutetime_to_nanoseconds(vmrun_xstate_ticks, &vmrun_xstate_ns);
	absolutetime_to_nanoseconds(owner_release_ticks, &owner_release_ns);
	IOLog("AMDVM_PROVIDER_TIMING vcpu=%u runs=%llu vmrun_samples=%llu state_set_ns=%llu xstate_preflight_ns=%llu interrupt_off_ns=%llu context_run_ns=%llu state_get_ns=%llu memory_type_check_ns=%llu owner_acquire_ns=%llu vmcb_prepare_ns=%llu vmrun_xstate_ns=%llu owner_release_ns=%llu\n",
	    vcpu_id, (unsigned long long)runs,
	    (unsigned long long)vmrun_samples,
	    (unsigned long long)state_set_ns,
	    (unsigned long long)xstate_preflight_ns,
	    (unsigned long long)interrupt_off_ns,
	    (unsigned long long)run_ns,
	    (unsigned long long)state_get_ns,
	    (unsigned long long)memory_type_ns,
	    (unsigned long long)owner_acquire_ns,
	    (unsigned long long)vmcb_prepare_ns,
	    (unsigned long long)vmrun_xstate_ns,
	    (unsigned long long)owner_release_ns);
}
#endif

static int
amdvm_vmapi_run_context(amdvm_hlt_context_t *context, int *owner_status,
	amdvm_vmrun_status_t *vmrun_status,
	amdvm_vmexit_snapshot_t *exit_snapshot, bool nativeCr3)
{
#if defined(AMDVM_EXPERIMENTAL_NATIVE_CR3)
	if (nativeCr3) {
		uint16_t savedCrRead = 0, savedCrWrite = 0;
		int status;
		if (amdvm_vmcb_prepare_native_cr3(context->guest_vmcb,
		    &savedCrRead, &savedCrWrite) != 0) {
			*owner_status = AMDVM_SVM_OWNER_INVALID;
			*vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
			return AMDVM_HLT_CONTEXT_INVALID_GUEST;
		}
		status = amdvm_hlt_context_run(context, owner_status, vmrun_status,
		    exit_snapshot);
		amdvm_vmcb_restore_native_cr3(context->guest_vmcb,
		    savedCrRead, savedCrWrite);
		return status;
	}
#else
	if (nativeCr3) {
		*owner_status = AMDVM_SVM_OWNER_INVALID;
		*vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
		return AMDVM_HLT_CONTEXT_INVALID_GUEST;
	}
#endif
#if defined(AMDVM_EXPERIMENTAL_NATIVE_CR2_READ)
	uint16_t savedCrRead = 0;
	int status;

	if (amdvm_vmcb_prepare_native_cr2_read(context->guest_vmcb,
	    &savedCrRead) != 0) {
		*owner_status = AMDVM_SVM_OWNER_INVALID;
		*vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
		return AMDVM_HLT_CONTEXT_INVALID_GUEST;
	}
	status = amdvm_hlt_context_run(context, owner_status, vmrun_status,
	    exit_snapshot);
	amdvm_vmcb_restore_cr2_read_intercept(context->guest_vmcb, savedCrRead);
	return status;
#else
	return amdvm_hlt_context_run(context, owner_status, vmrun_status,
	    exit_snapshot);
#endif
}

static bool
amdvm_provider_pause_cycle_enabled(void)
{
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_PAUSE)
	return true;
#else
	return false;
#endif
}

class AMDVMObjectRetainGuard {
public:
	explicit AMDVMObjectRetainGuard(OSObject *object) : fObject(object)
	{
		fObject->retain();
	}
	~AMDVMObjectRetainGuard()
	{
		fObject->release();
	}
private:
	OSObject *fObject;
};

/*
 * On the steady CR0.TS=0 path, acquire runLock first and perform one XSAVE
 * after pinning the thread. If TS is set (including a race detected after
 * locking), release the lock and use an XSAVE with interrupts enabled so XNU's
 * ordinary #NM path can restore the PCB state. Never clear TS directly.
 *
 * Success returns with runLock held and local interrupts disabled, ready for
 * the caller's bounded operation. Failure always returns with both restored.
 */
static int
amdvm_capture_xstate_pinned(void *area, uint64_t *xcr0,
	thread_t expectedThread, IOSimpleLock *runLock,
	int *cpuBefore, int *cpuAfter, uint64_t *cr0Before, uint64_t *cr0After,
	IOInterruptState *interruptState, int *firstStatus, int *secondStatus,
	uint64_t *interruptOffStartTicks)
{
	const unsigned int retryLimit = 4;
	int status = AMDVM_VMRUN_FPU_NOT_READY;

	for (unsigned int attempt = 0; attempt < retryLimit; ++attempt) {
		uint64_t observedCr0;
		__asm__ volatile("mov %%cr0, %0" : "=r"(observedCr0));
		*cr0Before = observedCr0;
		*cr0After = observedCr0;
		amdvm_xstate_capture_action_t preflightAction =
		    amdvm_xstate_capture_preflight_action(observedCr0);
		if (preflightAction == AMDVM_XSTATE_CAPTURE_REJECT) {
			status = AMDVM_VMRUN_FPU_NOT_READY;
			*firstStatus = status;
			*secondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
			return status;
		}
		if (preflightAction == AMDVM_XSTATE_CAPTURE_LAZY_RESTORE) {
			uint64_t lazyXcr0 = 0;
			status = amdvm_vmrun_xstate_capture_current_thread(area, &lazyXcr0,
			    cr0Before, cr0After);
			if (status == AMDVM_VMRUN_SUCCESS) {
				*xcr0 = lazyXcr0;
				*firstStatus = AMDVM_VMRUN_SUCCESS;
				continue;
			}
			if (status == AMDVM_VMRUN_FPU_NOT_READY &&
			    (*cr0After & AMDVM_XSTATE_POLICY_CR0_TS) != 0 &&
			    (*cr0After & AMDVM_XSTATE_POLICY_CR0_EM) == 0) {
				*xcr0 = lazyXcr0;
				continue;
			}
			*firstStatus = status;
			*secondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
			return status;
		}

		IOSimpleLockLock(runLock);
		if (expectedThread != current_thread()) {
			IOSimpleLockUnlock(runLock);
			status = AMDVM_VMRUN_FPU_NOT_READY;
			break;
		}
		*cpuBefore = cpu_number();
		uint64_t pinnedCr0;
		__asm__ volatile("mov %%cr0, %0" : "=r"(pinnedCr0));
		*cr0Before = pinnedCr0;
		*cr0After = pinnedCr0;
		amdvm_xstate_capture_action_t pinnedAction =
		    amdvm_xstate_capture_pinned_action(pinnedCr0,
		    expectedThread == current_thread());
		if (pinnedAction == AMDVM_XSTATE_CAPTURE_REJECT ||
		    pinnedAction == AMDVM_XSTATE_CAPTURE_RETRY) {
			IOSimpleLockUnlock(runLock);
			status = AMDVM_VMRUN_FPU_NOT_READY;
			break;
		}
		if (pinnedAction == AMDVM_XSTATE_CAPTURE_LAZY_RESTORE) {
			IOSimpleLockUnlock(runLock);
			continue;
		}

		*interruptState = ml_set_interrupts_enabled(FALSE);
		if (interruptOffStartTicks != nullptr)
			*interruptOffStartTicks = mach_absolute_time();
		if (expectedThread != current_thread() ||
		    *cpuBefore != cpu_number()) {
			(void)ml_set_interrupts_enabled(*interruptState);
			IOSimpleLockUnlock(runLock);
			status = AMDVM_VMRUN_FPU_NOT_READY;
			break;
		}
		*firstStatus = AMDVM_VMRUN_SUCCESS;
		status = amdvm_vmrun_xstate_capture_current_thread(area, xcr0,
		    cr0Before, cr0After);
		*cpuAfter = cpu_number();
		*secondStatus = status;
		if (status == AMDVM_VMRUN_SUCCESS &&
		    expectedThread == current_thread() &&
		    *cpuBefore == *cpuAfter) {
			*firstStatus = AMDVM_VMRUN_SUCCESS;
			return status;
		}

		(void)ml_set_interrupts_enabled(*interruptState);
		IOSimpleLockUnlock(runLock);
		if (status == AMDVM_VMRUN_FPU_NOT_READY &&
		    (*cr0After & (UINT64_C(1) << 3)) != 0 &&
		    (*cr0After & (UINT64_C(1) << 2)) == 0) {
			continue;
		}
		*firstStatus = AMDVM_VMRUN_SUCCESS;
		return status;
	}

	*firstStatus = status;
	*secondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	return status;
}

/* VM-scoped RUN has a second, mandatory XSAVE at the last C boundary in
 * amdvm_vmrun_once(). On the ordinary TS=0 path, only pin the caller here and
 * leave the host-state save to that last-boundary capture. XSAVE here is
 * needed only to trigger XNU's lazy #NM restore if TS is observed set. */
static int
amdvm_pin_run_context(void *lazyRestoreArea, thread_t expectedThread,
	IOSimpleLock *runLock, int *cpuBefore, int *cpuAfter,
	uint64_t *cr0Before, uint64_t *cr0After,
	IOInterruptState *interruptState, int *firstStatus, int *secondStatus,
	uint64_t *interruptOffStartTicks)
{
	const unsigned int retryLimit = 4;
	int status = AMDVM_VMRUN_FPU_NOT_READY;

	for (unsigned int attempt = 0; attempt < retryLimit; ++attempt) {
		uint64_t observedCr0;
		__asm__ volatile("mov %%cr0, %0" : "=r"(observedCr0));
		*cr0Before = observedCr0;
		*cr0After = observedCr0;
		amdvm_xstate_capture_action_t action =
		    amdvm_xstate_capture_preflight_action(observedCr0);
		if (action == AMDVM_XSTATE_CAPTURE_REJECT) {
			status = AMDVM_VMRUN_FPU_NOT_READY;
			*firstStatus = status;
			*secondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
			return status;
		}
		if (action == AMDVM_XSTATE_CAPTURE_LAZY_RESTORE) {
			uint64_t lazyXcr0 = 0;
			status = amdvm_vmrun_xstate_capture_current_thread(lazyRestoreArea,
			    &lazyXcr0, cr0Before, cr0After);
			if (status == AMDVM_VMRUN_SUCCESS) {
				*firstStatus = AMDVM_VMRUN_SUCCESS;
				continue;
			}
			if (status == AMDVM_VMRUN_FPU_NOT_READY &&
			    (*cr0After & AMDVM_XSTATE_POLICY_CR0_TS) != 0 &&
			    (*cr0After & AMDVM_XSTATE_POLICY_CR0_EM) == 0) {
				continue;
			}
			*firstStatus = status;
			*secondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
			return status;
		}

		IOSimpleLockLock(runLock);
		if (expectedThread != current_thread()) {
			IOSimpleLockUnlock(runLock);
			status = AMDVM_VMRUN_FPU_NOT_READY;
			break;
		}
		*cpuBefore = cpu_number();
		uint64_t pinnedCr0;
		__asm__ volatile("mov %%cr0, %0" : "=r"(pinnedCr0));
		*cr0Before = pinnedCr0;
		*cr0After = pinnedCr0;
		action = amdvm_xstate_capture_pinned_action(pinnedCr0,
		    expectedThread == current_thread());
		if (action == AMDVM_XSTATE_CAPTURE_REJECT ||
		    action == AMDVM_XSTATE_CAPTURE_RETRY) {
			IOSimpleLockUnlock(runLock);
			status = AMDVM_VMRUN_FPU_NOT_READY;
			break;
		}
		if (action == AMDVM_XSTATE_CAPTURE_LAZY_RESTORE) {
			IOSimpleLockUnlock(runLock);
			continue;
		}

		*interruptState = ml_set_interrupts_enabled(FALSE);
		if (interruptOffStartTicks != nullptr)
			*interruptOffStartTicks = mach_absolute_time();
		uint64_t finalCr0;
		__asm__ volatile("mov %%cr0, %0" : "=r"(finalCr0));
		*cr0After = finalCr0;
		if (expectedThread != current_thread() ||
		    *cpuBefore != cpu_number() ||
		    amdvm_xstate_capture_pinned_action(finalCr0, 1) !=
		    AMDVM_XSTATE_CAPTURE_PINNED) {
			(void)ml_set_interrupts_enabled(*interruptState);
			IOSimpleLockUnlock(runLock);
			status = AMDVM_VMRUN_FPU_NOT_READY;
			if ((finalCr0 & AMDVM_XSTATE_POLICY_CR0_TS) != 0 &&
			    (finalCr0 & AMDVM_XSTATE_POLICY_CR0_EM) == 0)
				continue;
			break;
		}
		*cpuAfter = cpu_number();
		*firstStatus = AMDVM_VMRUN_SUCCESS;
		*secondStatus = AMDVM_VMRUN_SUCCESS;
		return AMDVM_VMRUN_SUCCESS; /* lock held; interrupts disabled */
	}

	*firstStatus = status;
	*secondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	return status;
}

/* IOUserClient places structures larger than io_struct_inband_t in an
 * IOMemoryDescriptor. Never dereference structureInput for those requests. */
class AMDVMStructureInput {
public:
	AMDVMStructureInput() : fBytes(nullptr), fOwned(nullptr), fSize(0) {}
	~AMDVMStructureInput()
	{
		if (fOwned != nullptr) {
			IOFree(fOwned, fSize);
		}
	}
	bool init(IOExternalMethodArguments *arguments, size_t expectedSize)
	{
		if (arguments == nullptr) return false;
		if (arguments->structureInputDescriptor != nullptr) {
			IOMemoryDescriptor *descriptor = arguments->structureInputDescriptor;
			if (arguments->structureInputSize != 0 ||
			    descriptor->getLength() != expectedSize) return false;
			fOwned = IOMalloc(expectedSize);
			if (fOwned == nullptr) return false;
			fSize = expectedSize;
			IOReturn prepareStatus = descriptor->prepare();
			if (prepareStatus != kIOReturnSuccess) {
				IOFree(fOwned, fSize);
				fOwned = nullptr;
				fSize = 0;
				return false;
			}
			IOByteCount copied = descriptor->readBytes(0, fOwned, expectedSize);
			IOReturn completeStatus = descriptor->complete();
			if (copied != expectedSize || completeStatus != kIOReturnSuccess) {
				IOFree(fOwned, fSize);
				fOwned = nullptr;
				fSize = 0;
				return false;
			}
			fBytes = fOwned;
			return true;
		}
		if (arguments->structureInput == nullptr ||
		    arguments->structureInputSize != expectedSize) return false;
		fBytes = arguments->structureInput;
		return true;
	}
	bool initEither(IOExternalMethodArguments *arguments, size_t firstSize,
	    size_t secondSize, size_t *actualSize)
	{
		if (arguments == nullptr || actualSize == nullptr) return false;
		size_t providedSize = arguments->structureInputDescriptor != nullptr ?
		    (size_t)arguments->structureInputDescriptor->getLength() :
		    (size_t)arguments->structureInputSize;
		if (providedSize != firstSize && providedSize != secondSize) return false;
		if (!init(arguments, providedSize)) return false;
		*actualSize = providedSize;
		return true;
	}
	const void *bytes() const { return fBytes; }
private:
	const void *fBytes;
	void *fOwned;
	size_t fSize;
};

static void amdvm_run_slot_wake_if_quiescent(IOLock *lock,
	volatile uint64_t *state, volatile uint32_t *buffer_busy);

class AMDVMRunBufferGuard {
public:
	AMDVMRunBufferGuard() : fBusy(nullptr), fRunState(nullptr), fLock(nullptr),
	    fActiveReference(false) {}
	~AMDVMRunBufferGuard()
	{
		if (fBusy != nullptr) {
			if (fActiveReference) (void)amdvm_run_slot_release(fRunState);
			amdvm_run_buffer_release(fBusy);
			amdvm_run_slot_wake_if_quiescent(fLock, fRunState, fBusy);
		}
	}
	void acquire(IOLock *lock, volatile uint64_t *runState,
	    volatile uint32_t *busy, bool activeReference)
	{
		fLock = lock;
		fRunState = runState;
		fBusy = busy;
		fActiveReference = activeReference;
	}
private:
	volatile uint32_t *fBusy;
	volatile uint64_t *fRunState;
	IOLock *fLock;
	bool fActiveReference;
};

static void
amdvm_run_slot_wake_if_quiescent(IOLock *lock, volatile uint64_t *state,
	volatile uint32_t *buffer_busy)
{
	if (lock == nullptr || state == nullptr || buffer_busy == nullptr ||
	    !amdvm_run_slot_is_closing(state)) return;
	IOLockLock(lock);
	if (amdvm_run_slot_quiescent(state, buffer_busy))
		IOLockWakeup(lock, (void *)state, false);
	IOLockUnlock(lock);
}

static bool
amdvm_run_slot_wait_quiescent(IOLock *lock, volatile uint64_t *state,
	volatile uint32_t *buffer_busy)
{
	AbsoluteTime deadline;
	clock_interval_to_deadline(1000, 1000000u, &deadline);
	while (!amdvm_run_slot_quiescent(state, buffer_busy)) {
		int waitResult = IOLockSleepDeadline(lock, (void *)state,
		    deadline, THREAD_UNINT);
		if (waitResult == THREAD_TIMED_OUT) return false;
	}
	return true;
}

/* Copy large output structures through the prepared OOL descriptor supplied
 * by IOUserClient. Small results continue to use the inline structure buffer. */
class AMDVMStructureOutput {
public:
	AMDVMStructureOutput() : fBytes(nullptr), fSize(0), fOwned(false) {}
	~AMDVMStructureOutput()
	{
		if (fOwned && fBytes != nullptr) IOFree(fBytes, fSize);
	}
	bool init(size_t size)
	{
		fBytes = IOMalloc(size);
		if (fBytes == nullptr) return false;
		fSize = size;
		fOwned = true;
		bzero(fBytes, size);
		return true;
	}
	bool initWithBuffer(void *buffer, size_t size)
	{
		if (buffer == nullptr || size == 0) return false;
		fBytes = buffer;
		fSize = size;
		fOwned = false;
		bzero(fBytes, size);
		return true;
	}
	void *bytes() const { return fBytes; }
	IOReturn copyTo(IOExternalMethodArguments *arguments)
	{
		if (arguments == nullptr) return kIOReturnBadArgument;
		if (arguments->structureOutputDescriptor != nullptr) {
			IOMemoryDescriptor *descriptor = arguments->structureOutputDescriptor;
			if (descriptor->getLength() < fSize) return kIOReturnBadArgument;
			IOReturn prepareStatus = descriptor->prepare();
			if (prepareStatus != kIOReturnSuccess) return prepareStatus;
			IOByteCount copied = descriptor->writeBytes(0, fBytes, fSize);
			IOReturn completeStatus = descriptor->complete();
			if (copied != fSize || completeStatus != kIOReturnSuccess) {
				return kIOReturnError;
			}
			arguments->structureOutputDescriptorSize = (uint32_t)fSize;
			return kIOReturnSuccess;
		}
		if (arguments->structureOutput == nullptr ||
		    arguments->structureOutputSize < fSize) return kIOReturnBadArgument;
		if (arguments->structureOutput == fBytes) {
			arguments->structureOutputSize = (uint32_t)fSize;
			return kIOReturnSuccess;
		}
		bcopy(fBytes, arguments->structureOutput, fSize);
		arguments->structureOutputSize = (uint32_t)fSize;
		return kIOReturnSuccess;
	}
private:
	void *fBytes;
	size_t fSize;
	bool fOwned;
};

/* Kernel collections still require kmod_info for an IOKit service kext.
 * These hooks acknowledge module lifecycle; service startup stays with IOKit. */
extern "C" {
kern_return_t _start(kmod_info_t *info, void *data);
kern_return_t _stop(kmod_info_t *info, void *data);
KMOD_EXPLICIT_DECL(AMDVMProbe, "0.1.0", _start, _stop)

kern_return_t
_start(kmod_info_t *info, void *data)
{
	(void)info;
	(void)data;
	return KERN_SUCCESS;
}

kern_return_t
_stop(kmod_info_t *info, void *data)
{
	(void)info;
	(void)data;
	/* Never unload code while SVM ownership or backing pages may be retained. */
	if (__sync_val_compare_and_swap(&gHltContextBusy, 0u, 0u) != 0 ||
	    gHltContext.lease.active != 0 ||
	    gHltContext.backing.memory_descriptor != nullptr ||
	    gHltContext.npt_backing.memory_descriptor != nullptr ||
	    gHltContext.hsave.memory_descriptor != nullptr ||
	    amdvm_hlt_context_quarantine_count() != 0 ||
	    amdvm_vm_memory_owner_count() != 0 ||
	    __sync_val_compare_and_swap(&gVcpuContextCount, 0u, 0u) != 0 ||
	    __sync_val_compare_and_swap(&gVcpuLeaseCount, 0u, 0u) != 0) {
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}
}

static const uint32_t kMsrVmCr = 0xc0010114u;
static const uint32_t kMsrEfer = 0xc0000080u;
static const uint32_t kMsrVmHsavePa = 0xc0010117u;
static const uint64_t kVmCrLock = 1ull << 3;
static const uint64_t kVmCrSvmdis = 1ull << 4;
static const uint64_t kEferSvme = 1ull << 12;

extern "C" void mp_rendezvous(void (*setup)(void *),
	void (*action)(void *), void (*teardown)(void *), void *arg);
#if defined(AMDVM_PATCHED_XNU)
/* Experimental API available only in the matching patched XNU profile. */
typedef volatile uint64_t amdvm_xnu_cpumask_t;
extern "C" kern_return_t mp_cpus_kick_bounded(
	amdvm_xnu_cpumask_t cpus, uint64_t timeout_ns);
static const uint64_t kAmdvmKickTimeoutNs = 10000000ull;
#elif !defined(AMDVM_STOCK_XNU)
#error Select exactly one AMDVM_XNU_PROFILE for this kernel provider
#endif

static inline uint64_t
amdvm_rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void
amdvm_cpuid(uint32_t leaf, uint32_t subleaf,
    uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid"
	    : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
	    : "a"(leaf), "c"(subleaf));
}

static void
amdvm_query_cpu(void *opaque)
{
	amdvm_probe_result_t *result = (amdvm_probe_result_t *)opaque;
	uint32_t a, b, c, d;
	uint32_t maxExtended;
	uint64_t vmCr, efer, hsave;
	int cpuIndex = cpu_number();
	amdvm_probe_cpu_state_t *cpuState;

	__sync_fetch_and_add(&result->logical_cpus_seen, 1u);
	if (cpuIndex < 0 || cpuIndex >= (int)AMDVM_PROBE_MAX_CPUS) {
		__sync_fetch_and_add(&result->cpu_index_overflow, 1u);
		return;
	}
	cpuState = &result->cpus[cpuIndex];
	cpuState->cpu_index = (uint32_t)cpuIndex;
	cpuState->flags = AMDVM_CPU_PRESENT;
	amdvm_cpuid(0x80000000u, 0, &maxExtended, &b, &c, &d);
	if (maxExtended < 0x80000001u) {
		return;
	}
	amdvm_cpuid(0x80000001u, 0, &a, &b, &c, &d);
	if ((c & (1u << 2)) == 0) {
		return;
	}
	__sync_fetch_and_add(&result->svm_cpuid_cpus, 1u);
	cpuState->flags |= AMDVM_CPU_SVM;
	if (maxExtended < 0x8000000au) {
		goto read_svm_msrs;
	}
	amdvm_cpuid(0x8000000au, 0, &a, &b, &c, &d);
	if (d & (1u << 0)) {
		__sync_fetch_and_add(&result->npt_cpuid_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_NPT;
	}
	if (d & (1u << 3)) {
		__sync_fetch_and_add(&result->nrip_save_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_NRIP_SAVE;
	}

read_svm_msrs:
	vmCr = amdvm_rdmsr(kMsrVmCr);
	efer = amdvm_rdmsr(kMsrEfer);
	hsave = amdvm_rdmsr(kMsrVmHsavePa);
	cpuState->vm_cr = vmCr;
	cpuState->efer = efer;
	cpuState->hsave_pa = hsave;
	if (vmCr & kVmCrSvmdis) {
		__sync_fetch_and_add(&result->vm_cr_svmdis_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_VMCR_SVMDIS;
	}
	if (vmCr & kVmCrLock) {
		__sync_fetch_and_add(&result->vm_cr_lock_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_VMCR_LOCK;
	}
	if (efer & kEferSvme) {
		__sync_fetch_and_add(&result->efer_svme_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_EFER_SVME;
	}
	if (hsave != 0) {
		__sync_fetch_and_add(&result->hsave_nonzero_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_HSAVE_NONZERO;
		if ((hsave & 0xfffu) == 0) {
			__sync_fetch_and_add(&result->hsave_valid_cpus, 1u);
			cpuState->flags |= AMDVM_CPU_HSAVE_ALIGNED;
		}
	}
}

class AMDVMProbeUserClient : public IOUserClient {
	using super = IOUserClient;
	OSDeclareDefaultStructors(AMDVMProbeUserClient)

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties = nullptr) APPLE_KEXT_OVERRIDE;
	IOReturn query(IOExternalMethodArguments *arguments);
	IOReturn hltPreflight(IOExternalMethodArguments *arguments);
	IOReturn hltFpuPreflight(IOExternalMethodArguments *arguments);
	IOReturn hltRun(IOExternalMethodArguments *arguments);
	IOReturn vcpuCreate(IOExternalMethodArguments *arguments);
	IOReturn vcpuDiagnostic(IOExternalMethodArguments *arguments);
	IOReturn vcpuControlDiagnostic(IOExternalMethodArguments *arguments);
	IOReturn vcpuSetResetRom(IOExternalMethodArguments *arguments);
	IOReturn vcpuMapMemory(IOExternalMethodArguments *arguments);
	IOReturn vcpuSetMemoryTypes(IOExternalMethodArguments *arguments);
	IOReturn vcpuRun(IOExternalMethodArguments *arguments);
	IOReturn vcpuKick(IOExternalMethodArguments *arguments);
	IOReturn vcpuDestroy(IOExternalMethodArguments *arguments);
	IOReturn vmApiNegotiate(IOExternalMethodArguments *arguments);
	IOReturn vmSetMemoryMap(IOExternalMethodArguments *arguments);
	IOReturn vmDestroy(IOExternalMethodArguments *arguments);
	IOReturn vmSetResetRom(IOExternalMethodArguments *arguments);
	IOReturn vmVcpuCreate(IOExternalMethodArguments *arguments);
	IOReturn vmVcpuDestroy(IOExternalMethodArguments *arguments);
	IOReturn vmVcpuRun(IOExternalMethodArguments *arguments);
	IOReturn vmVcpuRunInternal(IOExternalMethodArguments *arguments,
	    int32_t expectedVcpuId, bool borrowSharedInput = false);
	IOReturn vmVcpuRunShared(IOExternalMethodArguments *arguments);
	IOReturn vmVcpuKick(IOExternalMethodArguments *arguments);
	IOReturn vmVcpuState(IOExternalMethodArguments *arguments, bool set);
	IOReturn vmVcpuDiagnostic(IOExternalMethodArguments *arguments);
	IOReturn vmVcpuSetMemoryTypes(IOExternalMethodArguments *arguments);
	IOReturn externalMethod(uint32_t selector,
	    IOExternalMethodArguments *arguments,
	    IOExternalMethodDispatch *dispatch = nullptr,
	    OSObject *target = nullptr,
	    void *reference = nullptr) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;

private:
	int closeVmMemory(bool orphan);
	int closeVmCpusLocked(void);
	int ensureVmSharedContextLocked(void);
	int destroyVmSharedContextLocked(void);
	struct __attribute__((aligned(64))) VmCpuSlot {
		amdvm_hlt_context_t *context;
		amdvm_vm_cpu_state_t *state;
		void *runInput;
		void *runOutput;
		IOSimpleLock *runLock;
		uint32_t apicId;
		uint32_t resetProfile;
		/* Diagnostic-only: keep #PF capture logging bounded to the first fault. */
		uint32_t diagnosticPfLogged;
		struct __attribute__((aligned(64))) HotRunState {
			volatile uint64_t runState;
			volatile uint32_t runBufferBusy;
			volatile uint32_t runCpu;
			volatile uint64_t runGeneration;
			volatile uint64_t kickGeneration;
		} hot;
		struct __attribute__((aligned(64))) PauseCounters {
			volatile uint64_t fastPathEntries;
			volatile uint64_t handled;
			volatile uint64_t returnsBudget;
			volatile uint64_t returnsEvent;
			volatile uint64_t returnsUnhandled;
		} pause;
		struct __attribute__((aligned(64))) ProviderTiming {
			volatile uint64_t runs;
			volatile uint64_t stateSetTicks;
			volatile uint64_t xstatePreflightTicks;
			volatile uint64_t interruptOffTicks;
			volatile uint64_t vmrunTicks;
			volatile uint64_t stateGetTicks;
			volatile uint64_t memoryTypeCheckTicks;
			volatile uint64_t ownerAcquireTicks;
			volatile uint64_t vmcbPrepareTicks;
			volatile uint64_t vmrunXstateTicks;
			volatile uint64_t ownerReleaseTicks;
			volatile uint64_t vmrunSamples;
		} timing;
		uint32_t leaseTracked;
	} fVmCpus[AMDVM_VM_ABI_DRAFT_MAX_VCPUS];
	static_assert(alignof(VmCpuSlot) >= 64 && sizeof(VmCpuSlot) % 64 == 0,
	    "each VM vCPU slot must occupy whole cache lines");
	static_assert(offsetof(VmCpuSlot, hot) % 64 == 0 &&
	    offsetof(VmCpuSlot, pause) % 64 == 0 &&
	    offsetof(VmCpuSlot, timing) % 64 == 0,
	    "RUN coordination and telemetry must not share cache lines");
	uint8_t *fVmResetRom;
	IOLock *fVmLock;
	amdvm_vm_memory_t *fVmMemory;
	amdvm_vm_memory_t *fVmPending;
	amdvm_hlt_context_t *fVmSharedContext;
	IOBufferMemoryDescriptor *fVmRunSharedMemory;
	amdvm_vm_shared_run_area_t *fVmRunSharedArea;
	uint64_t fVmGeneration;
	bool fVmClosed;
	void *fSecurityID;
	volatile uint32_t fApiMode;
	amdvm_hlt_context_t fVcpuContext;
	volatile uint32_t fVcpuBusy;
	volatile uint32_t fVcpuActiveRuns;
	volatile uint32_t fVcpuRunCpu;
	volatile uint64_t fVcpuRunGeneration;
	volatile uint64_t fVcpuKickGeneration;
	uint32_t fVcpuState;
	uint64_t fVcpuExpectedRip;
	uint32_t fVcpuGuestProfile;
	uint32_t fVcpuResetRomInstalled;
	uint32_t fVcpuContextTracked;
	uint32_t fVcpuLeaseTracked;
};

class AMDVMProbeService : public IOService {
	using super = IOService;
	OSDeclareDefaultStructors(AMDVMProbeService)

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AMDVMProbeService, IOService)
OSDefineMetaClassAndStructors(AMDVMProbeUserClient, IOUserClient)

static IOReturn
amdvm_query_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->query(arguments);
}

static IOReturn
amdvm_hlt_preflight_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->hltPreflight(arguments);
}

static IOReturn
amdvm_hlt_fpu_preflight_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->hltFpuPreflight(arguments);
}

static IOReturn
amdvm_hlt_run_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->hltRun(arguments);
}

static IOReturn
amdvm_vcpu_create_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuCreate(arguments);
}

static IOReturn
amdvm_vcpu_diagnostic_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuDiagnostic(arguments);
}

static IOReturn
amdvm_vcpu_control_diagnostic_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuControlDiagnostic(arguments);
}

static IOReturn
amdvm_vcpu_set_reset_rom_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuSetResetRom(arguments);
}

static IOReturn
amdvm_vcpu_run_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuRun(arguments);
}

static IOReturn
amdvm_vcpu_kick_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuKick(arguments);
}

static IOReturn
amdvm_vcpu_map_memory_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuMapMemory(arguments);
}

static IOReturn
amdvm_vcpu_set_memory_types_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuSetMemoryTypes(arguments);
}

static IOReturn
amdvm_vcpu_destroy_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuDestroy(arguments);
}

IOReturn
AMDVMProbeUserClient::query(IOExternalMethodArguments *arguments)
{
	amdvm_probe_result_t result = {};
	IOReturn privilege;
	if (arguments == nullptr || arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	result.abi_version = AMDVM_PROBE_ABI_VERSION;
	mp_rendezvous(nullptr, amdvm_query_cpu, nullptr, &result);
	(void)amdvm_hlt_context_retry_quarantined();
	{
		size_t contexts, ranges;
		amdvm_hlt_context_quarantine_stats(&contexts, &ranges,
		    &result.quarantined_guest_bytes);
		result.quarantined_memory_contexts = (uint32_t)contexts;
		result.quarantined_memory_ranges = (uint32_t)ranges;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::hltPreflight(IOExternalMethodArguments *arguments)
{
	amdvm_hlt_preflight_result_t result = {};
	IOReturn privilege;
	int status;
	uint64_t backing_base, backing_end;

	if (arguments == nullptr || arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&gHltContextBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_HLT_PREFLIGHT_ABI_VERSION;
	result.init_status = AMDVM_HLT_CONTEXT_INVALID;
	result.destroy_status = AMDVM_HLT_CONTEXT_INVALID;
	if (gHltContext.initialized != 0 ||
	    gHltContext.backing.memory_descriptor != nullptr ||
	    gHltContext.hsave.memory_descriptor != nullptr) {
		__sync_lock_release(&gHltContextBusy);
		return kIOReturnBusy;
	}

	status = amdvm_hlt_context_init(&gHltContext);
	result.init_status = status;
	result.flags |= AMDVM_HLT_PREFLIGHT_SVM_UNTOUCHED |
	    AMDVM_HLT_PREFLIGHT_VMRUN_NOT_EXECUTED;
	if (status == AMDVM_HLT_CONTEXT_OK) {
		backing_base = gHltContext.backing.physical_address;
		backing_end = backing_base + gHltContext.backing.size;
		if (amdvm_physmem_validate(&gHltContext.backing) == 0 &&
		    amdvm_physmem_validate(&gHltContext.hsave) == 0) {
			result.flags |= AMDVM_HLT_PREFLIGHT_ALLOCATIONS_VALID;
		}
		if (gHltContext.npt.pool != nullptr &&
		    amdvm_physmem_validate(&gHltContext.npt_backing) == 0 &&
		    gHltContext.npt.pool_pa ==
		    gHltContext.npt_backing.physical_address &&
		    amdvm_npt_validate_single_4k(&gHltContext.npt, 0,
		    gHltContext.guest_payload_pa,
		    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK &&
		    (gHltContext.npt.pool_pa & (AMDVM_PAGE_SIZE - 1u)) == 0) {
			result.flags |= AMDVM_HLT_PREFLIGHT_NPT_READY;
		}
		if (gHltContext.guest_payload != nullptr &&
		    gHltContext.guest_payload[0] == 0xf4u &&
		    gHltContext.guest_payload_pa >= backing_base &&
		    gHltContext.guest_payload_pa < backing_end) {
			result.flags |= AMDVM_HLT_PREFLIGHT_HLT_PAYLOAD;
		}
		if (gHltContext.host_vmcb != nullptr &&
		    gHltContext.guest_vmcb != nullptr &&
		    gHltContext.host_vmcb_pa >= backing_base &&
		    gHltContext.host_vmcb_pa < backing_end &&
		    gHltContext.guest_vmcb_pa >= backing_base &&
		    gHltContext.guest_vmcb_pa < backing_end &&
		    (gHltContext.host_vmcb_pa & (AMDVM_PAGE_SIZE - 1u)) == 0 &&
		    (gHltContext.guest_vmcb_pa & (AMDVM_PAGE_SIZE - 1u)) == 0) {
			result.flags |= AMDVM_HLT_PREFLIGHT_VMCB_PAGES_VALID;
		}
		result.destroy_status = amdvm_hlt_context_destroy(
		    &gHltContext);
		if (result.destroy_status == AMDVM_HLT_CONTEXT_OK) {
			result.flags |= AMDVM_HLT_PREFLIGHT_DESTROYED;
		}
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&gHltContextBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::hltFpuPreflight(IOExternalMethodArguments *arguments)
{
	amdvm_hlt_fpu_preflight_result_t result = {};
	IOSimpleLock *runLock = nullptr;
	thread_t initialThread = THREAD_NULL;
	IOInterruptState interruptState = FALSE;
	uint64_t firstCr0Before = 0, firstCr0After = 0;
	IOReturn privilege;
	int firstStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int secondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int initialCpu = -1;
	int finalCpu = -1;
	boolean_t interruptsWereEnabled;

	if (arguments == nullptr || arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&gHltContextBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_HLT_FPU_PREFLIGHT_ABI_VERSION;
	result.init_status = AMDVM_HLT_CONTEXT_INVALID;
	result.first_xsave_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	result.second_xsave_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	result.destroy_status = AMDVM_HLT_CONTEXT_INVALID;
	result.flags = AMDVM_HLT_FPU_PREFLIGHT_VMRUN_NOT_EXECUTED;
	if (gHltContext.initialized != 0 ||
	    gHltContext.backing.memory_descriptor != nullptr ||
	    gHltContext.hsave.memory_descriptor != nullptr) {
		__sync_lock_release(&gHltContextBusy);
		return kIOReturnBusy;
	}

	result.init_status = amdvm_hlt_context_init(&gHltContext);
	if (result.init_status == AMDVM_HLT_CONTEXT_OK) {
		runLock = IOSimpleLockAlloc();
		if (runLock != nullptr) {
			initialThread = current_thread();
			interruptsWereEnabled = ml_get_interrupts_enabled();
			if (interruptsWereEnabled) {
				int stableFirstStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
				int stableSecondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
				firstStatus = amdvm_capture_xstate_pinned(
				    gHltContext.host_xstate, &result.xcr0, initialThread,
				    runLock, &initialCpu, &finalCpu, &firstCr0Before,
				    &firstCr0After, &interruptState, &stableFirstStatus,
				    &stableSecondStatus, nullptr);
				firstStatus = stableFirstStatus;
				secondStatus = stableSecondStatus;
				result.ts_before = (uint32_t)((firstCr0Before >> 3) & 1u);
				result.em_before = (uint32_t)((firstCr0Before >> 2) & 1u);
				result.ts_after = (uint32_t)((firstCr0After >> 3) & 1u);
				result.em_after = (uint32_t)((firstCr0After >> 2) & 1u);
				if (firstStatus == AMDVM_VMRUN_SUCCESS &&
				    secondStatus == AMDVM_VMRUN_SUCCESS) {
					result.flags |= AMDVM_HLT_FPU_PREFLIGHT_LOCKED_CPU |
					    AMDVM_HLT_FPU_PREFLIGHT_XSAVE_OK |
					    AMDVM_HLT_FPU_PREFLIGHT_TS_CLEARED;
					result.cpu_before = (uint32_t)initialCpu;
					result.cpu_after = (uint32_t)finalCpu;
					if (initialThread == current_thread()) {
						result.flags |= AMDVM_HLT_FPU_PREFLIGHT_THREAD_STABLE;
					}
					if (initialCpu == finalCpu) {
						result.flags |= AMDVM_HLT_FPU_PREFLIGHT_CPU_STABLE;
					}
					(void)ml_set_interrupts_enabled(interruptState);
					result.flags |= AMDVM_HLT_FPU_PREFLIGHT_INTERRUPTS_RESTORED;
					IOSimpleLockUnlock(runLock);
				}
			}
			IOSimpleLockFree(runLock);
		}
		result.first_xsave_status = firstStatus;
		result.second_xsave_status = secondStatus;
		result.destroy_status = amdvm_hlt_context_destroy(&gHltContext);
		if (result.destroy_status == AMDVM_HLT_CONTEXT_OK) {
			result.flags |= AMDVM_HLT_FPU_PREFLIGHT_CONTEXT_DESTROYED;
		}
		const uint32_t expectedFlags =
		    AMDVM_HLT_FPU_PREFLIGHT_LOCKED_CPU |
		    AMDVM_HLT_FPU_PREFLIGHT_XSAVE_OK |
		    AMDVM_HLT_FPU_PREFLIGHT_TS_CLEARED |
		    AMDVM_HLT_FPU_PREFLIGHT_CPU_STABLE |
		    AMDVM_HLT_FPU_PREFLIGHT_THREAD_STABLE |
		    AMDVM_HLT_FPU_PREFLIGHT_INTERRUPTS_RESTORED |
		    AMDVM_HLT_FPU_PREFLIGHT_VMRUN_NOT_EXECUTED |
		    AMDVM_HLT_FPU_PREFLIGHT_CONTEXT_DESTROYED;
		if (result.first_xsave_status == AMDVM_VMRUN_SUCCESS &&
		    result.second_xsave_status == AMDVM_VMRUN_SUCCESS &&
		    result.destroy_status == AMDVM_HLT_CONTEXT_OK &&
		    result.flags == expectedFlags) {
			__sync_lock_test_and_set(&gHltFpuPreflightPassed, 1u);
		}
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&gHltContextBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::hltRun(IOExternalMethodArguments *arguments)
{
	amdvm_hlt_run_result_t result = {};
	const amdvm_hlt_run_request_t *request;
	IOSimpleLock *runLock = nullptr;
	thread_t initialThread = THREAD_NULL;
	IOInterruptState interruptState = FALSE;
	boolean_t interruptsWereEnabled;
	uint64_t xcr0 = 0, cr0Before = 0, cr0After = 0;
	amdvm_vmrun_status_t vmrunStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	amdvm_vmexit_snapshot_t exitSnapshot = {};
	IOReturn privilege;
	int status, xstateStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int cpuBefore = -1;

	if (arguments == nullptr || arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(amdvm_hlt_run_request_t) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_hlt_run_request_t *)arguments->structureInput;
	if (request->abi_version != AMDVM_HLT_RUN_REQUEST_ABI_VERSION ||
	    request->flags != AMDVM_HLT_RUN_CONFIRM_NO_KDP_WATCHPOINTS) {
		return kIOReturnNotPermitted;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (__sync_val_compare_and_swap(&gHltFpuPreflightPassed, 0u, 0u) == 0) {
		result.abi_version = AMDVM_HLT_RUN_ABI_VERSION;
		result.context_status = AMDVM_HLT_CONTEXT_FPU_PREFLIGHT_REQUIRED;
		result.owner_status = AMDVM_SVM_OWNER_INVALID;
		result.vmrun_status = AMDVM_VMRUN_FPU_NOT_READY;
		result.destroy_status = AMDVM_HLT_CONTEXT_INVALID;
		bcopy(&result, arguments->structureOutput, sizeof(result));
		arguments->structureOutputSize = sizeof(result);
		return kIOReturnSuccess;
	}
	if (!__sync_bool_compare_and_swap(&gHltContextBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_HLT_RUN_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	result.owner_status = AMDVM_SVM_OWNER_INVALID;
	result.vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	result.destroy_status = AMDVM_HLT_CONTEXT_INVALID;
	if (gHltContext.initialized != 0 ||
	    gHltContext.backing.memory_descriptor != nullptr ||
	    gHltContext.hsave.memory_descriptor != nullptr) {
		__sync_lock_release(&gHltContextBusy);
		return kIOReturnBusy;
	}

	status = amdvm_hlt_context_init(&gHltContext);
	result.context_status = status;
	if (status == AMDVM_HLT_CONTEXT_OK) {
		result.flags |= AMDVM_HLT_RUN_CONTEXT_INITIALIZED |
		    AMDVM_HLT_RUN_CALLED;
		result.owner_status = AMDVM_SVM_OWNER_INVALID;
		result.vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
#if defined(AMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN)
		runLock = IOSimpleLockAlloc();
		if (runLock == nullptr) {
			result.vmrun_status = AMDVM_VMRUN_XSTATE_UNSUPPORTED;
			result.context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
		} else {
			initialThread = current_thread();
			interruptsWereEnabled = ml_get_interrupts_enabled();
			if (interruptsWereEnabled) {
				/*
				 * The initial XSAVE can take XNU's lazy #NM restore in this
				 * thread context. Replace that snapshot after IOSimpleLock has
				 * disabled preemption. Keep that lock through VM entry, VMEXIT,
				 * and the wrapper's host-XSTATE restore; local interrupts are
				 * disabled over the same pinned-CPU interval.
				 */
				xstateStatus = amdvm_vmrun_xstate_capture_current_thread(
				    gHltContext.host_xstate, &xcr0,
				    &cr0Before, &cr0After);
				if (xstateStatus == AMDVM_VMRUN_SUCCESS) {
					IOSimpleLockLock(runLock);
					cpuBefore = cpu_number();
					interruptState = ml_set_interrupts_enabled(FALSE);
					if (initialThread == current_thread()) {
						xstateStatus = amdvm_vmrun_xstate_capture_current_thread(
						    gHltContext.host_xstate, &xcr0,
						    &cr0Before, &cr0After);
					} else {
						xstateStatus = AMDVM_VMRUN_FPU_NOT_READY;
					}
					if (xstateStatus == AMDVM_VMRUN_SUCCESS &&
					    cpuBefore == cpu_number() &&
					    initialThread == current_thread()) {
						result.context_status = amdvm_hlt_context_run_once(
						    &gHltContext, &result.owner_status,
						    &vmrunStatus, &exitSnapshot);
						result.vmrun_status = (int32_t)vmrunStatus;
						result.exit_code = exitSnapshot.code;
						result.exit_info1 = exitSnapshot.info1;
						result.exit_info2 = exitSnapshot.info2;
						result.next_rip = exitSnapshot.next_rip;
						result.guest_rip = exitSnapshot.guest_rip;
						result.guest_rflags = exitSnapshot.guest_rflags;
					} else {
						result.vmrun_status = xstateStatus;
						result.context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
					}
					(void)ml_set_interrupts_enabled(interruptState);
					IOSimpleLockUnlock(runLock);
				}
			} else {
				result.vmrun_status = AMDVM_VMRUN_FPU_NOT_READY;
				result.context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
			}
			IOSimpleLockFree(runLock);
		}
#else
		result.vmrun_status = AMDVM_VMRUN_ENTRY_GATED;
		result.context_status = AMDVM_HLT_CONTEXT_RUN_GATED;
#endif
		if (gHltContext.lease.active != 0) {
			result.flags |= AMDVM_HLT_RUN_LEASE_RETAINED;
		} else {
			result.destroy_status = amdvm_hlt_context_destroy(
			    &gHltContext);
			if (result.destroy_status == AMDVM_HLT_CONTEXT_OK) {
				result.flags |= AMDVM_HLT_RUN_CONTEXT_DESTROYED;
			}
		}
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&gHltContextBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuCreate(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_lifecycle_result_t result = {};
	const amdvm_vcpu_create_request_t *request;
	IOReturn privilege;
	int status;
	static const uint8_t expectedCode[AMDVM_VCPU_TEST_ROM_SIZE] = {
		AMDVM_VCPU_TEST_ROM_CPUID_0,
		AMDVM_VCPU_TEST_ROM_CPUID_1,
		AMDVM_VCPU_TEST_ROM_MOV_AL_IMM8,
		AMDVM_VCPU_TEST_ROM_DEBUG_BYTE,
		AMDVM_VCPU_TEST_ROM_OUT_IMM8_AL,
		AMDVM_VCPU_TEST_ROM_DEBUG_PORT,
		AMDVM_VCPU_TEST_ROM_HLT
	};
	bool fixedCodeProfile;
	bool firmwareProfile;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(*request) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_create_request_t *)arguments->structureInput;
	fixedCodeProfile = request->guest_profile ==
	    AMDVM_VCPU_GUEST_PROFILE_REALMODE_TEST_V1 ||
	    request->guest_profile ==
	    AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1;
	firmwareProfile = request->guest_profile ==
	    AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1;
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    (!fixedCodeProfile && !firmwareProfile) || request->reserved != 0 ||
	    (fixedCodeProfile && (request->code_size != sizeof(expectedCode) ||
	    bcmp(request->code, expectedCode, sizeof(expectedCode)) != 0)) ||
	    (firmwareProfile && request->code_size != 0)) {
		return kIOReturnNotPermitted;
	}
	if (firmwareProfile) {
		for (size_t i = 0; i < sizeof(request->code); ++i) {
			if (request->code[i] != 0) {
				return kIOReturnNotPermitted;
			}
		}
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (fVcpuContext.initialized != 0 ||
	    fVcpuContext.backing.memory_descriptor != nullptr ||
	    fVcpuContext.npt_backing.memory_descriptor != nullptr ||
	    fVcpuContext.hsave.memory_descriptor != nullptr) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnBusy;
	}
	status = request->guest_profile ==
	    AMDVM_VCPU_GUEST_PROFILE_REALMODE_TEST_V1 ?
	    amdvm_hlt_context_init(&fVcpuContext) :
	    amdvm_hlt_context_init_qemu_reset_vector_v1(&fVcpuContext);
	result.context_status = status;
	if (status == AMDVM_HLT_CONTEXT_OK) {
		/* Fixed smoke payload, or a placeholder until QEMU supplies firmware. */
		if (fixedCodeProfile &&
		    (fVcpuContext.guest_payload_offset > AMDVM_PAGE_SIZE ||
		    request->code_size > AMDVM_PAGE_SIZE -
		    fVcpuContext.guest_payload_offset)) {
			result.context_status = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			(void)amdvm_hlt_context_destroy(&fVcpuContext);
			bcopy(&result, arguments->structureOutput, sizeof(result));
			arguments->structureOutputSize = sizeof(result);
			__sync_lock_release(&fVcpuBusy);
			return kIOReturnSuccess;
		}
		if (fixedCodeProfile) {
			bcopy(request->code,
			    (uint8_t *)fVcpuContext.guest_payload +
			    fVcpuContext.guest_payload_offset,
			    request->code_size);
		}
		__sync_fetch_and_add(&gVcpuContextCount, 1u);
		fVcpuContextTracked = 1;
		fVcpuExpectedRip = fVcpuContext.initial_rip;
		fVcpuGuestProfile = request->guest_profile;
		fVcpuResetRomInstalled = fixedCodeProfile ? 1u : 0u;
		fVcpuState = AMDVM_VCPU_STATE_READY;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuDiagnostic(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_diagnostic_result_t result = {};
	IOReturn privilege;
	uint32_t csOffset = AMDVM_VMCB_GUEST_CS;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInputSize != 0 ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (fVcpuContext.initialized == 0 || fVcpuContext.guest_vmcb == nullptr ||
	    fVcpuContext.guest_payload == nullptr || fVcpuResetRomInstalled == 0 ||
	    (fVcpuState != AMDVM_VCPU_STATE_READY &&
	     fVcpuState != AMDVM_VCPU_STATE_MEMORY_MAPPED) ||
	    fVcpuActiveRuns != 0 || fVcpuContext.lease.active != 0 ||
	    fVcpuContext.guest_payload_offset != AMDVM_VCPU_RESET_ROM_OFFSET) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}

	result.context_status = AMDVM_HLT_CONTEXT_OK;
	result.guest_profile = fVcpuGuestProfile;
	result.guest_physical_address = AMDVM_VCPU_RESET_ROM_GPA;
	result.rom_offset = AMDVM_VCPU_RESET_ROM_OFFSET;
	bcopy(fVcpuContext.guest_payload + AMDVM_VCPU_RESET_ROM_OFFSET,
	    result.entry_bytes, sizeof(result.entry_bytes));
	bcopy(&fVcpuContext.guest_vmcb->bytes[csOffset],
	    &result.cs_selector, sizeof(result.cs_selector));
	bcopy(&fVcpuContext.guest_vmcb->bytes[csOffset + 2],
	    &result.cs_attributes, sizeof(result.cs_attributes));
	bcopy(&fVcpuContext.guest_vmcb->bytes[csOffset + 4],
	    &result.cs_limit, sizeof(result.cs_limit));
	bcopy(&fVcpuContext.guest_vmcb->bytes[csOffset + 8],
	    &result.cs_base, sizeof(result.cs_base));
	result.rip = amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
	    AMDVM_VMCB_GUEST_RIP);
	result.rflags = amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
	    AMDVM_VMCB_GUEST_RFLAGS);
	result.cr0 = amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
	    AMDVM_VMCB_GUEST_CR0);
	result.efer = amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
	    AMDVM_VMCB_GUEST_EFER);
	result.flags |= AMDVM_VCPU_DIAGNOSTIC_ROM_INSTALLED;
	if (amdvm_npt_validate_4k_mapping(&fVcpuContext.npt,
	    fVcpuContext.guest_payload_gpa, fVcpuContext.guest_payload_pa,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK) {
		result.flags |= AMDVM_VCPU_DIAGNOSTIC_NPT_RX_VALID;
	}
	if (result.cs_selector == 0xf000u &&
	    result.cs_attributes == 0x009bu && result.cs_limit == 0xffffu &&
	    result.cs_base == AMDVM_VCPU_RESET_CS_BASE &&
	    result.rip == AMDVM_VCPU_RESET_VECTOR_RIP &&
	    result.rflags == 2u && result.cr0 == 0x60000010ull &&
	    result.efer == AMDVM_EFER_SVME) {
		result.flags |= AMDVM_VCPU_DIAGNOSTIC_RESET_STATE;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuControlDiagnostic(
	IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_control_diagnostic_result_t result = {};
	const amdvm_vmcb_t *vmcb;
	uint32_t nested_paging;
	uint32_t misc1;
	uint32_t misc2;
	uint32_t exceptions;
	uint32_t interrupt_control;
	uint64_t npt_root;
	uint64_t iopm;
	uint64_t msrpm;
	uint64_t asid_tlb;
	IOReturn privilege;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInputSize != 0 ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	if (fVcpuContext.initialized == 0 || fVcpuContext.guest_vmcb == nullptr ||
	    fVcpuResetRomInstalled == 0 ||
	    (fVcpuState != AMDVM_VCPU_STATE_READY &&
	     fVcpuState != AMDVM_VCPU_STATE_MEMORY_MAPPED) ||
	    fVcpuActiveRuns != 0 || fVcpuContext.lease.active != 0) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}

	vmcb = fVcpuContext.guest_vmcb;
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
	    &result.intercept_cr_read, sizeof(result.intercept_cr_read));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_WRITE],
	    &result.intercept_cr_write, sizeof(result.intercept_cr_write));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_DR_READ],
	    &result.intercept_dr_read, sizeof(result.intercept_dr_read));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_DR_WRITE],
	    &result.intercept_dr_write, sizeof(result.intercept_dr_write));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_EXCEPTIONS], &exceptions,
	    sizeof(exceptions));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC1], &misc1,
	    sizeof(misc1));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC2], &misc2,
	    sizeof(misc2));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERRUPT_CONTROL], &interrupt_control,
	    sizeof(interrupt_control));
	nested_paging = (uint32_t)amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_NESTED_PAGING);
	npt_root = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_NPT_ROOT);
	iopm = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_IOPM_PA);
	msrpm = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_MSRPM_PA);
	asid_tlb = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_TLB_CONTROL);

	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_OK;
	result.intercept_exceptions = exceptions;
	result.intercept_misc1 = misc1;
	result.intercept_misc2 = misc2;
	result.interrupt_control = interrupt_control;
	result.nested_paging_control = nested_paging;
	result.asid = (uint32_t)asid_tlb;
	result.tlb_control = (uint32_t)(asid_tlb >> 32);
	if (misc1 & AMDVM_SVM_INTERCEPT_CPUID) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_CPUID_INTERCEPT;
	}
	if (exceptions & (1u << 6)) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_UD_INTERCEPT;
	}
	if (misc1 & AMDVM_SVM_INTERCEPT_HLT) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_HLT_INTERCEPT;
	}
	if (misc1 & AMDVM_SVM_INTERCEPT_IOIO) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_IOIO_INTERCEPT;
	}
	if (misc1 & AMDVM_SVM_INTERCEPT_INTR) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_INTR_INTERCEPT;
	}
	if (interrupt_control & AMDVM_VMCB_V_INTR_MASKING) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_V_INTR_MASKING;
	}
	if (nested_paging & AMDVM_VMCB_NPT_ENABLE) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_NPT_ENABLED;
	}
	if (npt_root != 0 &&
	    (npt_root & (AMDVM_PAGE_SIZE - 1u)) == 0) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_NPT_ROOT_VALID;
	}
	if (iopm != 0 && (iopm & (AMDVM_PAGE_SIZE - 1u)) == 0) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_IOPM_VALID;
	}
	if (msrpm != 0 && (msrpm & (AMDVM_PAGE_SIZE - 1u)) == 0) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_MSRPM_VALID;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuSetResetRom(IOExternalMethodArguments *arguments)
{
	const amdvm_vcpu_set_reset_rom_request_t *request;
	AMDVMStructureInput input;
	amdvm_vcpu_lifecycle_result_t result = {};
	IOReturn privilege;
	if (arguments == nullptr || !input.init(arguments, sizeof(*request)) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_set_reset_rom_request_t *)
	    input.bytes();
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->reserved != 0) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (fVcpuState != AMDVM_VCPU_STATE_READY ||
	    fVcpuGuestProfile != AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1 ||
	    fVcpuResetRomInstalled != 0 || fVcpuContext.guest_payload == nullptr ||
	    fVcpuContext.guest_payload_offset != AMDVM_VCPU_RESET_ROM_OFFSET) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}
	bcopy(request->page, fVcpuContext.guest_payload,
	    AMDVM_VCPU_RESET_ROM_SIZE);
	fVcpuResetRomInstalled = 1;
	result.context_status = AMDVM_HLT_CONTEXT_OK;
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuKick(IOExternalMethodArguments *arguments)
{
	const amdvm_vcpu_kick_request_t *request;
	IOReturn privilege;
	uint64_t current;
	uint64_t previous;
#if defined(AMDVM_PATCHED_XNU)
	uint32_t runCpu;
	uint64_t runGeneration;
	bool waitForRunCompletion = false;
#endif
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(*request)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_kick_request_t *)arguments->structureInput;
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->reserved != 0 || request->generation == 0) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}

	current = __sync_val_compare_and_swap(&fVcpuKickGeneration, 0u, 0u);
	while (amdvm_generation_newer(request->generation, current)) {
		previous = __sync_val_compare_and_swap(&fVcpuKickGeneration, current,
		    request->generation);
		if (previous == current) {
			current = request->generation;
			break;
		}
		current = previous;
	}
	if (!amdvm_kick_generation_should_dispatch(request->generation, current)) {
		return kIOReturnSuccess;
	}

	/*
	 * RUN compares this generation around entry and after every VMEXIT. The
	 * stock-XNU profile has no cross-CPU private-kick import: it relies on
	 * AMD SVM's mandatory external-interrupt intercept returning to QEMU on
	 * the next host interrupt. This is not a hard wall-clock latency guarantee;
	 * firmware execution remains gated until target behavior is measured.
	 */
#if defined(AMDVM_PATCHED_XNU)
	/* Retry an equal generation after timeout; a prior send may not have run. */
	runCpu = __sync_val_compare_and_swap(&fVcpuRunCpu, UINT32_MAX, UINT32_MAX);
	runGeneration = __sync_val_compare_and_swap(&fVcpuRunGeneration, 0u, 0u);
	if (runCpu < AMDVM_PROBE_MAX_CPUS &&
	    amdvm_generation_newer(request->generation, runGeneration)) {
		amdvm_xnu_cpumask_t target = 1ull << runCpu;
		kern_return_t kickStatus = mp_cpus_kick_bounded(target,
			kAmdvmKickTimeoutNs);
		if (kickStatus == KERN_OPERATION_TIMED_OUT) {
			return kIOReturnTimeout;
		}
		if (kickStatus != KERN_SUCCESS) {
			return kIOReturnError;
		}
		waitForRunCompletion = true;
	} else if (runCpu >= AMDVM_PROBE_MAX_CPUS &&
	    __sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) != 0) {
		/* KICK raced before CPU publication; RUN will observe the new generation. */
		waitForRunCompletion = true;
	}
	if (waitForRunCompletion) {
		/*
		 * A successful IPI send is not proof that RUN left VMRUN. Wait for
		 * vcpuRun's active reference to drain, under a second finite deadline.
		 * On timeout QEMU must retain the context; destroy rejects active runs.
		 */
		uint64_t runDeadline;
		clock_interval_to_deadline(10, 1000000u, &runDeadline);
		while (__sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) != 0) {
			if (mach_absolute_time() >= runDeadline) {
				return kIOReturnTimeout;
			}
			IOSleep(1);
		}
	}
#endif
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuRun(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_run_result_t *result;
	const amdvm_vcpu_run_request_t *request;
	AMDVMStructureInput input;
	AMDVMStructureOutput output;
	IOSimpleLock *runLock = nullptr;
	thread_t initialThread = THREAD_NULL;
	IOInterruptState interruptState = FALSE;
	boolean_t interruptsWereEnabled;
	boolean_t interruptsAtEntry = FALSE;
	boolean_t interruptsAfterLock = FALSE;
	uint64_t xcr0 = 0, cr0Before = 0, cr0After = 0;
	amdvm_vmrun_status_t vmrunStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	amdvm_vmexit_snapshot_t exitSnapshot = {};
	IOReturn privilege;
	int xstateStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int firstCaptureStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int secondCaptureStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int cpuBefore = -1;
	int cpuAfter = -1;
	uint32_t xstateStage = 0;
	uint32_t i;
	uint32_t priorState;
	bool fullState = false;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || !input.init(arguments, sizeof(*request)) ||
	    !output.init(sizeof(*result))) {
		return kIOReturnBadArgument;
	}
	result = (amdvm_vcpu_run_result_t *)output.bytes();
	request = (const amdvm_vcpu_run_request_t *)input.bytes();
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->flags != AMDVM_VCPU_RUN_CONFIRM_NO_KDP_WATCHPOINTS ||
	    amdvm_vmcb_validate_event_injection(request->event_injection,
	    request->event_injection_error_code) != 0) {
		return kIOReturnNotPermitted;
	}
	fullState = amdvm_x86_state_abi_supported(request->state.abi_version);
	if (fullState) {
		if (request->rip != 0 || request->rflags != 0 ||
		    amdvm_vmcb_validate_x86_state_v1(&request->state) != 0) {
			return kIOReturnBadArgument;
		}
		for (i = 0; i < sizeof(request->gpr) / sizeof(request->gpr[0]); ++i) {
			if (request->gpr[i] != 0) return kIOReturnBadArgument;
		}
	} else {
		if (request->event_injection != 0 ||
		    request->state.abi_version != 0 ||
		    request->rflags != 2u) {
			return kIOReturnBadArgument;
		}
		for (i = 0; i < sizeof(request->state); ++i) {
			if (((const uint8_t *)&request->state)[i] != 0) {
				return kIOReturnBadArgument;
			}
		}
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (__sync_val_compare_and_swap(&gHltFpuPreflightPassed, 0u, 0u) == 0) {
		result->abi_version = AMDVM_VCPU_ABI_VERSION;
		result->context_status = AMDVM_HLT_CONTEXT_FPU_PREFLIGHT_REQUIRED;
		result->owner_status = AMDVM_SVM_OWNER_INVALID;
		result->vmrun_status = AMDVM_VMRUN_FPU_NOT_READY;
		return output.copyTo(arguments);
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	if (!fullState && request->rip != fVcpuExpectedRip) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotPermitted;
	}
	priorState = fVcpuState;
	result->abi_version = AMDVM_VCPU_ABI_VERSION;
	result->context_status = AMDVM_HLT_CONTEXT_INVALID;
	result->owner_status = AMDVM_SVM_OWNER_INVALID;
	result->vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	bool contextInitialized = fVcpuContext.initialized != 0;
	bool gprsReady = fVcpuContext.guest_gprs != nullptr;
	bool vmcbReady = fVcpuContext.guest_vmcb != nullptr;
	bool eventPending = request->event_injection != 0;
	bool lifecycleAllowsRun = amdvm_vcpu_state_can_run(fVcpuState, fullState,
	    eventPending);
	if (!contextInitialized || !gprsReady || !vmcbReady || !lifecycleAllowsRun) {
		if (__sync_fetch_and_add(&gVcpuRunNotReadyLogCount, 1u) == 0) {
			IOLog("AMDVM RUN rejected: state=%u full_state=%u event_pending=%u "
			    "context_initialized=%u guest_memory_mapped=%u gprs_ready=%u "
			    "vmcb_ready=%u lifecycle_allows_run=%u lease_active=%u "
			    "active_runs=%u\n",
			    fVcpuState, fullState, eventPending, contextInitialized,
			    fVcpuContext.guest_memory_mapped != 0, gprsReady, vmcbReady,
			    lifecycleAllowsRun, fVcpuContext.lease.active,
			    __sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u));
		}
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}
	if (request->kick_generation !=
	    __sync_val_compare_and_swap(&fVcpuKickGeneration, 0u, 0u)) {
		/* QEMU processes the pending event before retrying this same state. */
		result->abi_version = AMDVM_VCPU_ABI_VERSION;
		result->flags = AMDVM_VCPU_RUN_KICKED;
		result->context_status = AMDVM_HLT_CONTEXT_OK;
		result->owner_status = AMDVM_SVM_OWNER_OK;
		result->vmrun_status = AMDVM_VMRUN_SUCCESS;
		result->exit_code = UINT64_MAX;
		if (fullState) {
			result->state = request->state;
		}
		IOReturn outputStatus = output.copyTo(arguments);
		__sync_lock_release(&fVcpuBusy);
		return outputStatus;
	}
	__sync_fetch_and_add(&fVcpuActiveRuns, 1u);
	fVcpuState = AMDVM_VCPU_STATE_RUNNING;
	if (amdvm_vmcb_stage_run_request(fVcpuContext.guest_vmcb,
	    (uint64_t *)fVcpuContext.guest_gprs, fVcpuContext.guest_xstate,
	    request, fullState) != 0) {
		fVcpuState = priorState;
		__sync_fetch_and_sub(&fVcpuActiveRuns, 1u);
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnBadArgument;
	}
	if (fullState) result->state = request->state;
	fVcpuContext.guest_execution_xcr0 = fullState ?
	    amdvm_xstate_execution_mask(request->state.xcr0,
	    request->state.cr[4]) : 3;
	if (amdvm_hlt_context_prepare_guest_pat(&fVcpuContext) != 0) {
		result->context_status = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		result->exit_code = UINT64_MAX;
		fVcpuState = priorState;
		__sync_fetch_and_sub(&fVcpuActiveRuns, 1u);
		IOReturn outputStatus = output.copyTo(arguments);
		__sync_lock_release(&fVcpuBusy);
		return outputStatus;
	}

	interruptsAtEntry = ml_get_interrupts_enabled();
	if (interruptsAtEntry) {
		runLock = IOSimpleLockAlloc();
		if (runLock != nullptr) {
			initialThread = current_thread();
			interruptsWereEnabled = ml_get_interrupts_enabled();
			interruptsAfterLock = FALSE;
			if (interruptsWereEnabled) {
				xstateStage = 1;
				xstateStatus = amdvm_capture_xstate_pinned(
				    fVcpuContext.host_xstate, &xcr0, initialThread, runLock,
				    &cpuBefore, &cpuAfter, &cr0Before, &cr0After,
				    &interruptState, &firstCaptureStatus,
				    &secondCaptureStatus, nullptr);
				if (xstateStatus == AMDVM_VMRUN_SUCCESS) {
					xstateStage = 2;
					interruptsAfterLock = TRUE;
					if (xstateStatus == AMDVM_VMRUN_SUCCESS &&
					    cpuBefore == cpu_number() &&
					    initialThread == current_thread()) {
						/* Publish generation before CPU so KICK sees a coherent pair. */
						__sync_lock_test_and_set(&fVcpuRunGeneration,
						    request->kick_generation);
						__sync_lock_test_and_set(&fVcpuRunCpu,
						    (uint32_t)cpuBefore);
						if (request->kick_generation !=
						    __sync_val_compare_and_swap(&fVcpuKickGeneration,
						    0u, 0u)) {
							/* A kick before CPU publication needs no VMRUN. */
							result->flags |= AMDVM_VCPU_RUN_KICKED;
							result->context_status = AMDVM_HLT_CONTEXT_OK;
							result->owner_status = AMDVM_SVM_OWNER_OK;
							result->vmrun_status = AMDVM_VMRUN_SUCCESS;
							result->exit_code = UINT64_MAX;
							if (fullState) {
								result->state = request->state;
							}
						} else {
							uint64_t entryGuestXcr0 = fullState ?
							    request->state.xcr0 : 0;
							uint32_t entryGuestXstateSize = fullState ?
							    request->state.xstate_size : 0;
							if (amdvm_vmcb_capture_entry_snapshot(
							    fVcpuContext.guest_vmcb, &fVcpuContext.npt,
							    fVcpuContext.guest_payload_gpa,
							    fVcpuContext.guest_payload_offset,
							    fVcpuContext.backing.virtual_address,
							    fVcpuContext.backing.physical_address,
							    fVcpuContext.backing.size,
							    (const uint64_t *)fVcpuContext.guest_gprs,
							    xcr0, entryGuestXcr0,
							    entryGuestXstateSize, &result->entry) == 0) {
								result->flags |= AMDVM_VCPU_RUN_CALLED;
								result->context_status = amdvm_hlt_context_run(
								    &fVcpuContext, &result->owner_status,
								    &vmrunStatus, &exitSnapshot);
							} else {
								result->context_status =
								    AMDVM_HLT_CONTEXT_INVALID_GUEST;
								vmrunStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
							}
							result->vmrun_status = (int32_t)vmrunStatus;
							result->exit_code = exitSnapshot.code;
							result->exit_info1 = exitSnapshot.info1;
							result->exit_info2 = exitSnapshot.info2;
							result->next_rip = exitSnapshot.next_rip;
							result->instruction_length =
							    exitSnapshot.instruction_length;
							bcopy(exitSnapshot.instruction_bytes,
							    result->instruction_bytes,
							    sizeof(result->instruction_bytes));
							result->rip = exitSnapshot.guest_rip;
							result->rflags = exitSnapshot.guest_rflags;
							result->exit_int_info = (uint32_t)
							    amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
							    AMDVM_VMCB_EXIT_INT_INFO);
							bcopy(fVcpuContext.guest_gprs, result->gpr,
							    sizeof(result->gpr));
							if (request->kick_generation !=
							    __sync_val_compare_and_swap(
							    &fVcpuKickGeneration, 0u, 0u)) {
								result->flags |= AMDVM_VCPU_RUN_KICKED;
							}
						}
						/* No new IPI should target this entry after this point. */
						__sync_lock_test_and_set(&fVcpuRunCpu, UINT32_MAX);
						__sync_lock_test_and_set(&fVcpuRunGeneration, 0u);
						if ((result->flags & AMDVM_VCPU_RUN_CALLED) != 0 &&
						    vmrunStatus == AMDVM_VMRUN_SUCCESS) {
							if (fullState) {
								if (amdvm_vmcb_store_x86_state_v1(
								    fVcpuContext.guest_vmcb, &result->state,
								    (const uint64_t *)fVcpuContext.guest_gprs,
								    fVcpuContext.guest_xstate,
								    AMDVM_X86_XSTATE_MAX_SIZE) != 0) {
										uint64_t xstateBv = 0;
										uint64_t xcompBv = 0;
										uint32_t reservedNonzero = 0;
										const uint8_t *guestXstate =
										    (const uint8_t *)fVcpuContext.guest_xstate;
										if (guestXstate != nullptr) {
											bcopy(guestXstate + 512u, &xstateBv,
											    sizeof(xstateBv));
											bcopy(guestXstate + 520u, &xcompBv,
											    sizeof(xcompBv));
											for (uint32_t byte = 528u; byte < 576u; ++byte) {
												reservedNonzero |= guestXstate[byte] != 0;
											}
										}
										IOLog("AMDVM_STATE_STORE_REJECT exit=0x%llx rip=0x%llx "
										    "state_abi=%u interruptibility=0x%x xcr0=0x%llx "
										    "xstate_size=%u xstate_bv=0x%llx xcomp_bv=0x%llx "
										    "xstate_reserved_nonzero=%u guest_xstate=%u\n",
										    (unsigned long long)exitSnapshot.code,
										    (unsigned long long)exitSnapshot.guest_rip,
										    result->state.abi_version,
										    result->state.interruptibility,
										    (unsigned long long)result->state.xcr0,
										    result->state.xstate_size,
										    (unsigned long long)xstateBv,
										    (unsigned long long)xcompBv,
										    reservedNonzero,
										    guestXstate != nullptr);
									result->context_status = AMDVM_HLT_CONTEXT_INVALID_GUEST;
									fVcpuState = AMDVM_VCPU_STATE_FAILED;
								} else {
									result->rip = result->state.rip;
									result->rflags = result->state.rflags;
								if (exitSnapshot.code == AMDVM_HLT_EXPECTED_EXIT) {
									fVcpuState = AMDVM_VCPU_STATE_HALTED;
								} else {
									fVcpuState = priorState == AMDVM_VCPU_STATE_HALTED ?
									    AMDVM_VCPU_STATE_MEMORY_MAPPED : priorState;
									}
								}
							} else if (request->rip == AMDVM_VCPU_TEST_ROM_CPUID_RIP &&
							    exitSnapshot.code == AMDVM_VCPU_EXPECTED_CPUID_EXIT) {
								fVcpuExpectedRip = AMDVM_VCPU_TEST_ROM_CPUID_NEXT_RIP;
							} else if (request->rip == AMDVM_VCPU_TEST_ROM_CPUID_NEXT_RIP &&
							    exitSnapshot.code == AMDVM_VCPU_EXPECTED_IOIO_EXIT) {
								fVcpuExpectedRip = AMDVM_VCPU_TEST_ROM_HLT_RIP;
							} else if (request->rip == AMDVM_VCPU_TEST_ROM_HLT_RIP &&
							    exitSnapshot.code == AMDVM_HLT_EXPECTED_EXIT) {
								fVcpuState = AMDVM_VCPU_STATE_HALTED;
							} else {
								fVcpuState = AMDVM_VCPU_STATE_FAILED;
							}
						}
					} else {
						result->vmrun_status = xstateStatus;
						result->context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
					}
					(void)ml_set_interrupts_enabled(interruptState);
					IOSimpleLockUnlock(runLock);
				}
			} else {
				/* Do not return the untouched INVALID sentinels as a success. */
				xstateStatus = AMDVM_VMRUN_FPU_NOT_READY;
			}
			if (xstateStatus != AMDVM_VMRUN_SUCCESS) {
				result->vmrun_status = xstateStatus;
				result->context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
			}
			IOSimpleLockFree(runLock);
		} else {
			result->vmrun_status = AMDVM_VMRUN_XSTATE_UNSUPPORTED;
			result->context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
		}
	} else {
		xstateStage = 4;
		result->vmrun_status = AMDVM_VMRUN_FPU_NOT_READY;
		result->context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
	}
	if (result->vmrun_status == AMDVM_VMRUN_FPU_NOT_READY &&
	    __sync_fetch_and_add(&gVcpuFpuRejectLogged, 1u) == 0) {
		IOLog("AMDVM RUN FPU_NOT_READY stage=%u if_entry=%u if_after_lock=%u "
		    "cpu=%d/%d thread_same=%u xstate_status=%d capture_status=%d/%d xcr0=0x%llx "
		    "cr0=0x%llx/0x%llx lease=%u\n",
		    xstateStage, interruptsAtEntry, interruptsAfterLock, cpuBefore,
		    cpuAfter, initialThread == current_thread(), xstateStatus,
		    firstCaptureStatus, secondCaptureStatus,
		    (unsigned long long)xcr0, (unsigned long long)cr0Before,
		    (unsigned long long)cr0After, fVcpuContext.lease.active);
	}
	amdvm_hlt_context_restore_guest_pat(&fVcpuContext);
	if (fullState) result->state.pat = request->state.pat;
	if (fVcpuContext.lease.active != 0) {
		result->flags |= AMDVM_VCPU_RUN_LEASE_RETAINED;
		if (fVcpuLeaseTracked == 0) {
			fVcpuLeaseTracked = 1;
			__sync_fetch_and_add(&gVcpuLeaseCount, 1u);
		}
	}
	if (fVcpuContext.lease.active != 0) {
		fVcpuState = AMDVM_VCPU_STATE_LEASED;
	} else if (fVcpuState == AMDVM_VCPU_STATE_RUNNING) {
		/* Pre-entry failures leave the same resume point available. */
		fVcpuState = priorState;
	}
	__sync_fetch_and_sub(&fVcpuActiveRuns, 1u);
	IOReturn outputStatus = output.copyTo(arguments);
	__sync_lock_release(&fVcpuBusy);
	return outputStatus;
}

IOReturn
AMDVMProbeUserClient::vcpuMapMemory(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_memory_map_result_t result = {};
	const amdvm_vcpu_memory_map_request_t *request;
	IOReturn privilege;
	size_t nptPageCount = 0;
	size_t i;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(*request) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_memory_map_request_t *)arguments->structureInput;
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->range_count == 0 ||
	    request->range_count > AMDVM_VCPU_MEMORY_MAX_RANGES ||
	    request->reserved[0] != 0 || request->reserved[1] != 0) {
		return kIOReturnBadArgument;
	}
	for (i = request->range_count; i < AMDVM_VCPU_MEMORY_MAX_RANGES; ++i) {
		const uint8_t *bytes = (const uint8_t *)&request->ranges[i];
		size_t j;
		for (j = 0; j < sizeof(request->ranges[i]); ++j) {
			if (bytes[j] != 0) {
				return kIOReturnBadArgument;
			}
		}
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	if (fVcpuState != AMDVM_VCPU_STATE_READY ||
	    fVcpuResetRomInstalled == 0 ||
	    fVcpuContext.guest_memory_mapped != 0) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}
	result.context_status = amdvm_hlt_context_map_user_ranges(
	    &fVcpuContext, request->ranges, request->range_count,
	    current_task(), &nptPageCount);
	if (result.context_status == AMDVM_HLT_CONTEXT_OK) {
		result.range_count = request->range_count;
		result.guest_page_count = fVcpuContext.guest_page_count;
		result.npt_page_count = nptPageCount;
		fVcpuState = AMDVM_VCPU_STATE_MEMORY_MAPPED;
	} else if (result.context_status == AMDVM_HLT_CONTEXT_UNPIN_FAILED) {
		fVcpuState = AMDVM_VCPU_STATE_FAILED;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuSetMemoryTypes(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_lifecycle_result_t result = {};
	const amdvm_vcpu_set_memory_types_request_t *request;
	AMDVMStructureInput input;
	IOReturn privilege;
	size_t pageCount = 0;
	size_t i;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || !input.init(arguments, sizeof(*request)) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_set_memory_types_request_t *)
	    input.bytes();
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->range_count == 0 ||
	    request->range_count > AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES ||
	    request->reserved[0] != 0 || request->reserved[1] != 0) {
		return kIOReturnBadArgument;
	}
	for (i = request->range_count;
	    i < AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES; ++i) {
		const uint8_t *bytes = (const uint8_t *)&request->ranges[i];
		size_t j;
		for (j = 0; j < sizeof(request->ranges[i]); ++j) {
			if (bytes[j] != 0) return kIOReturnBadArgument;
		}
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) return kIOReturnNotPrivileged;
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	if (fVcpuState != AMDVM_VCPU_STATE_MEMORY_MAPPED ||
	    fVcpuContext.guest_memory_mapped == 0 ||
	    fVcpuActiveRuns != 0 || fVcpuContext.lease.active != 0) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}
	result.context_status = amdvm_hlt_context_set_memory_types(
	    &fVcpuContext, request, &pageCount);
	if (result.context_status == AMDVM_HLT_CONTEXT_OK) {
		if (pageCount > UINT32_MAX) {
			result.context_status = AMDVM_HLT_CONTEXT_INVALID_GUEST;
		} else {
			result.reserved[0] = (uint32_t)pageCount;
		}
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuDestroy(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_lifecycle_result_t result = {};
	uint32_t priorState;
	IOReturn privilege;
	AMDVMObjectRetainGuard lifetime(this);
	if (arguments == nullptr || arguments->structureInputSize != 0 ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	if (__sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) != 0 ||
	    fVcpuState == AMDVM_VCPU_STATE_RUNNING ||
	    fVcpuState == AMDVM_VCPU_STATE_DESTROYING) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnBusy;
	}
	priorState = fVcpuState;
	fVcpuState = AMDVM_VCPU_STATE_DESTROYING;
	result.context_status = amdvm_hlt_context_destroy(&fVcpuContext);
	if (result.context_status == AMDVM_HLT_CONTEXT_OK) {
		fVcpuState = AMDVM_VCPU_STATE_EMPTY;
		fVcpuExpectedRip = 0;
		fVcpuGuestProfile = 0;
		fVcpuResetRomInstalled = 0;
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
	} else if (fVcpuContext.quarantined != 0) {
		fVcpuState = AMDVM_VCPU_STATE_FAILED;
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
	} else {
		fVcpuState = fVcpuContext.lease.active != 0 ?
		    AMDVM_VCPU_STATE_LEASED : priorState;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return result.context_status == AMDVM_HLT_CONTEXT_OK ?
	    kIOReturnSuccess : kIOReturnError;
}

bool
AMDVMProbeUserClient::initWithTask(task_t owningTask, void *securityID,
	UInt32 type, OSDictionary *properties)
{
	fVmLock = nullptr;
	fVmMemory = nullptr;
	fVmPending = nullptr;
	fVmSharedContext = nullptr;
	fVmRunSharedMemory = nullptr;
	fVmRunSharedArea = nullptr;
	fVmGeneration = 0;
	fVmClosed = false;
	bzero(fVmCpus, sizeof(fVmCpus));
	fVmResetRom = nullptr;
	if (type != 0 || !super::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fSecurityID = securityID;
	fApiMode = AMDVM_CLIENT_MODE_UNSELECTED;
	bzero(&fVcpuContext, sizeof(fVcpuContext));
	fVcpuBusy = 0;
	fVcpuActiveRuns = 0;
	fVcpuRunCpu = UINT32_MAX;
	fVcpuRunGeneration = 0;
	fVcpuKickGeneration = 0;
	fVcpuState = AMDVM_VCPU_STATE_EMPTY;
	fVcpuExpectedRip = 0;
	fVcpuGuestProfile = 0;
	fVcpuResetRomInstalled = 0;
	fVcpuContextTracked = 0;
	fVcpuLeaseTracked = 0;
	fVmLock = IOLockAlloc();
	return fVmLock != nullptr;
}

int
AMDVMProbeUserClient::closeVmCpusLocked(void)
{
	int status = AMDVM_HLT_CONTEXT_OK;
	for (size_t i = 0; i < AMDVM_VM_ABI_DRAFT_MAX_VCPUS; ++i) {
		auto *context = fVmCpus[i].context;
		if (!context) continue;
		amdvm_run_slot_begin_destroy(&fVmCpus[i].hot.runState);
		if (!amdvm_run_slot_wait_quiescent(fVmLock, &fVmCpus[i].hot.runState,
		    &fVmCpus[i].hot.runBufferBusy)) {
			/*
			 * Do not free the remaining contexts while one vCPU still
			 * owns a run reference. Return busy and let the caller retry
			 * the whole teardown after the run drains.
			 */
			status = kIOReturnBusy;
			break;
		}
		int rc = amdvm_hlt_context_destroy(context);
		if (rc) { status = rc; continue; }
		IOFree(context, sizeof(*context));
		if (fVmCpus[i].state) IOFree(fVmCpus[i].state, sizeof(*fVmCpus[i].state));
		if (fVmCpus[i].runInput)
			IOFree(fVmCpus[i].runInput, AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE);
		if (fVmCpus[i].runOutput)
			IOFree(fVmCpus[i].runOutput, sizeof(amdvm_vm_abi_draft_vcpu_run_result_t));
		if (fVmCpus[i].runLock) IOSimpleLockFree(fVmCpus[i].runLock);
		if (fVmCpus[i].leaseTracked)
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		fVmCpus[i] = {};
		__sync_fetch_and_sub(&gVcpuContextCount, 1u);
	}
	return status;
}

int
AMDVMProbeUserClient::destroyVmSharedContextLocked(void)
{
	if (!fVmSharedContext) return AMDVM_HLT_CONTEXT_OK;
	int status = amdvm_hlt_context_destroy(fVmSharedContext);
	if (status == AMDVM_HLT_CONTEXT_OK) {
		IOFree(fVmSharedContext, sizeof(*fVmSharedContext));
		fVmSharedContext = nullptr;
	}
	return status;
}

int
AMDVMProbeUserClient::ensureVmSharedContextLocked(void)
{
	if (fVmSharedContext) return AMDVM_HLT_CONTEXT_OK;
	if (!fVmMemory || !fVmResetRom) return AMDVM_HLT_CONTEXT_INVALID;
	auto *context = static_cast<amdvm_hlt_context_t *>(
	    IOMalloc(sizeof(amdvm_hlt_context_t)));
	if (!context) return AMDVM_HLT_CONTEXT_ALLOC_FAILED;
	bzero(context, sizeof(*context));
	int status = amdvm_hlt_context_init_qemu_reset_vector_v1(context);
	if (!status) {
		bcopy(fVmResetRom, context->guest_payload,
		    AMDVM_VCPU_RESET_ROM_SIZE);
		size_t pages = 0;
		status = amdvm_hlt_context_attach_vm_memory(context, fVmMemory,
		    fVmGeneration, &pages);
	}
	if (status) {
		if (context->initialized) {
			(void)amdvm_hlt_context_destroy(context);
		}
		IOFree(context, sizeof(*context));
		return status;
	}
	fVmSharedContext = context;
	return AMDVM_HLT_CONTEXT_OK;
}

int
AMDVMProbeUserClient::closeVmMemory(bool orphan)
{
	if (fVmLock == nullptr) return AMDVM_VM_MEMORY_OK;
	IOLockLock(fVmLock);
	fVmClosed = true;
	int cpus = closeVmCpusLocked();
	if (!cpus) cpus = destroyVmSharedContextLocked();
	/* An admitted RUN still owns its VMCB/NPT and shared pinned RAM. */
	if (cpus) {
		IOLockUnlock(fVmLock);
		return cpus;
	}
	int first = amdvm_vm_memory_close(&fVmPending, orphan);
	int second = amdvm_vm_memory_close(&fVmMemory, orphan);
	if (fVmResetRom) {
		IOFree(fVmResetRom, AMDVM_VCPU_RESET_ROM_SIZE);
		fVmResetRom = nullptr;
	}
	IOLockUnlock(fVmLock);
	return cpus ? cpus : first != AMDVM_VM_MEMORY_OK ? first : second;
}

IOReturn
AMDVMProbeUserClient::vmSetResetRom(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	amdvm_vm_abi_draft_lifecycle_result_t result = {};
	if (!arguments || !arguments->structureOutput ||
	    arguments->structureOutputSize < sizeof(result) ||
	    !input.init(arguments, sizeof(amdvm_vm_abi_draft_reset_rom_request_t)))
		return kIOReturnBadArgument;
	const auto *request = static_cast<const amdvm_vm_abi_draft_reset_rom_request_t *>(input.bytes());
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != sizeof(*request) ||
	    request->reserved[0] || request->reserved[1]) return kIOReturnBadArgument;
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK) return kIOReturnNotPermitted;
	IOLockLock(fVmLock);
	if (fVmClosed) { IOLockUnlock(fVmLock); return kIOReturnNotOpen; }
	if (fVmMemory || fVmResetRom) { IOLockUnlock(fVmLock); return kIOReturnBusy; }
	fVmResetRom = static_cast<uint8_t *>(IOMalloc(AMDVM_VCPU_RESET_ROM_SIZE));
	result.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result.structure_size = sizeof(result);
	result.context_status = fVmResetRom ? 0 : AMDVM_HLT_CONTEXT_ALLOC_FAILED;
	if (fVmResetRom) bcopy(request->page, fVmResetRom, AMDVM_VCPU_RESET_ROM_SIZE);
	IOLockUnlock(fVmLock);
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vmVcpuCreate(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	amdvm_vm_abi_draft_vcpu_create_result_t result = {};
	if (!arguments || !arguments->structureOutput ||
	    arguments->structureOutputSize < sizeof(result) ||
	    !input.init(arguments, sizeof(amdvm_vm_abi_draft_vcpu_create_request_t)))
		return kIOReturnBadArgument;
	const auto *request = static_cast<const amdvm_vm_abi_draft_vcpu_create_request_t *>(input.bytes());
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != sizeof(*request) ||
	    request->flags ||
	    request->reserved[0] || request->reserved[1] ||
	    request->vcpu_id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS || request->apic_id >= 256 ||
	    request->reset_profile != AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1)
		return kIOReturnBadArgument;
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK) return kIOReturnNotPermitted;
	IOLockLock(fVmLock);
	if (fVmClosed) { IOLockUnlock(fVmLock); return kIOReturnNotOpen; }
	if (!fVmMemory || fVmPending || !fVmResetRom ||
	    request->map_generation != fVmGeneration) {
		IOLockUnlock(fVmLock); return kIOReturnNotReady;
	}
	for (size_t i = 0; i < AMDVM_VM_ABI_DRAFT_MAX_VCPUS; ++i) {
		if (fVmCpus[i].context && (i == request->vcpu_id ||
		    fVmCpus[i].apicId == request->apic_id)) {
			IOLockUnlock(fVmLock); return kIOReturnBusy;
		}
	}
	void *runInput = IOMalloc(AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE);
	void *runOutput = IOMalloc(sizeof(amdvm_vm_abi_draft_vcpu_run_result_t));
	IOSimpleLock *runLock = IOSimpleLockAlloc();
	if (!runInput || !runOutput || !runLock) {
		if (runInput) IOFree(runInput, AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE);
		if (runOutput) IOFree(runOutput, sizeof(amdvm_vm_abi_draft_vcpu_run_result_t));
		if (runLock) IOSimpleLockFree(runLock);
		result.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
		result.structure_size = sizeof(result);
		result.vcpu_id = request->vcpu_id;
		result.map_generation = fVmGeneration;
		result.context_status = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		IOLockUnlock(fVmLock);
		bcopy(&result, arguments->structureOutput, sizeof(result));
		arguments->structureOutputSize = sizeof(result);
		return kIOReturnSuccess;
	}
	auto *context = static_cast<amdvm_hlt_context_t *>(IOMalloc(sizeof(amdvm_hlt_context_t)));
	result.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result.structure_size = sizeof(result);
	result.vcpu_id = request->vcpu_id;
	result.map_generation = fVmGeneration;
	result.context_status = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
	if (context) {
		bzero(context, sizeof(*context));
		__sync_fetch_and_add(&gVcpuContextCount, 1u);
		result.context_status = ensureVmSharedContextLocked();
		if (!result.context_status) {
			result.context_status =
			    amdvm_hlt_context_init_qemu_reset_vector_v1(context);
		}
		if (!result.context_status) {
			bcopy(fVmResetRom, context->guest_payload, AMDVM_VCPU_RESET_ROM_SIZE);
			size_t pages = fVmSharedContext->npt.pages_used;
			result.context_status = amdvm_hlt_context_attach_shared_npt(context,
			    fVmMemory, fVmGeneration, &fVmSharedContext->npt, pages,
			    &fVmSharedContext->host_mtrr);
			if (!result.context_status) {
				/* This vCPU executes the template's mapped reset page. */
				context->guest_payload_pa =
				    fVmSharedContext->guest_payload_pa;
				result.npt_page_count = pages;
			}
		}
		amdvm_vm_cpu_state_t *state = nullptr;
		if (!result.context_status) {
			state = static_cast<amdvm_vm_cpu_state_t *>(IOMalloc(sizeof(*state)));
			if (!state) result.context_status = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
			else if (amdvm_vm_cpu_state_init(state, context->guest_vmcb,
			    reinterpret_cast<const uint64_t *>(context->guest_gprs), context->guest_xstate))
				result.context_status = AMDVM_HLT_CONTEXT_INVALID_GUEST;
		}
		if (!result.context_status || (context->initialized &&
		    amdvm_hlt_context_destroy(context) != AMDVM_HLT_CONTEXT_OK)) {
			/* Failed destruction remains accounted and retryable. */
			fVmCpus[request->vcpu_id] = {};
			fVmCpus[request->vcpu_id].context = context;
			fVmCpus[request->vcpu_id].state = state;
			fVmCpus[request->vcpu_id].runInput = runInput;
			fVmCpus[request->vcpu_id].runOutput = runOutput;
			fVmCpus[request->vcpu_id].runLock = runLock;
			fVmCpus[request->vcpu_id].apicId = request->apic_id;
			fVmCpus[request->vcpu_id].resetProfile = request->reset_profile;
			fVmCpus[request->vcpu_id].hot.runCpu = UINT32_MAX;
		} else {
			IOFree(runInput, AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE);
			IOFree(runOutput, sizeof(amdvm_vm_abi_draft_vcpu_run_result_t));
			IOSimpleLockFree(runLock);
			if (state) IOFree(state, sizeof(*state));
			IOFree(context, sizeof(*context));
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
	} else {
		IOFree(runInput, AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE);
		IOFree(runOutput, sizeof(amdvm_vm_abi_draft_vcpu_run_result_t));
		IOSimpleLockFree(runLock);
	}
	IOLockUnlock(fVmLock);
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vmVcpuDestroy(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	amdvm_vm_abi_draft_lifecycle_result_t result = {};
	if (!arguments || !arguments->structureOutput ||
	    arguments->structureOutputSize < sizeof(result) ||
	    !input.init(arguments, sizeof(amdvm_vm_abi_draft_vcpu_lifecycle_request_t)))
		return kIOReturnBadArgument;
	const auto *request = static_cast<const amdvm_vm_abi_draft_vcpu_lifecycle_request_t *>(input.bytes());
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != sizeof(*request) || request->reserved ||
	    request->vcpu_id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS) return kIOReturnBadArgument;
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK) return kIOReturnNotPermitted;
	IOLockLock(fVmLock);
	if (request->map_generation != fVmGeneration || !fVmMemory) {
		IOLockUnlock(fVmLock); return kIOReturnNotReady;
	}
	auto &slot = fVmCpus[request->vcpu_id];
	if (!slot.context) {
		IOLockUnlock(fVmLock);
		return kIOReturnNotReady;
	}
	amdvm_run_slot_begin_destroy(&slot.hot.runState);
	if (!amdvm_run_slot_wait_quiescent(fVmLock, &slot.hot.runState,
	    &slot.hot.runBufferBusy)) {
		IOLockUnlock(fVmLock); return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result.structure_size = sizeof(result);
	result.map_generation = fVmGeneration;
	if (slot.context) {
		result.context_status = amdvm_hlt_context_destroy(slot.context);
		if (!result.context_status) {
			IOFree(slot.context, sizeof(*slot.context));
			if (slot.state) IOFree(slot.state, sizeof(*slot.state));
			if (slot.runInput)
				IOFree(slot.runInput, AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE);
			if (slot.runOutput)
				IOFree(slot.runOutput, sizeof(amdvm_vm_abi_draft_vcpu_run_result_t));
			if (slot.runLock) IOSimpleLockFree(slot.runLock);
			slot = {};
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
	}
	IOLockUnlock(fVmLock);
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vmVcpuState(IOExternalMethodArguments *arguments, bool set)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	AMDVMStructureOutput output;
	size_t resultSize = set ? sizeof(amdvm_vm_abi_draft_lifecycle_result_t) :
	    sizeof(amdvm_vm_abi_draft_vcpu_state_result_t);
	if (!arguments || !input.init(arguments, set ?
	    sizeof(amdvm_vm_abi_draft_vcpu_state_request_t) :
	    sizeof(amdvm_vm_abi_draft_vcpu_lifecycle_request_t)) || !output.init(resultSize))
		return kIOReturnBadArgument;
	if ((arguments->structureOutputDescriptor &&
	    arguments->structureOutputDescriptor->getLength() < resultSize) ||
	    (!arguments->structureOutputDescriptor && (!arguments->structureOutput ||
	    arguments->structureOutputSize < resultSize))) return kIOReturnBadArgument;
	/* Both request layouts share version, size, id, zero flags, generation. */
	amdvm_vm_abi_draft_vcpu_lifecycle_request_t header;
	bcopy(input.bytes(), &header, sizeof(header));
	const auto *request = &header;
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != (set ? sizeof(amdvm_vm_abi_draft_vcpu_state_request_t) :
	    sizeof(amdvm_vm_abi_draft_vcpu_lifecycle_request_t)) || request->reserved ||
	    request->vcpu_id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS) return kIOReturnBadArgument;
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK) return kIOReturnNotPermitted;
	IOLockLock(fVmLock);
	if (fVmClosed) { IOLockUnlock(fVmLock); return kIOReturnNotOpen; }
	auto &slot = fVmCpus[request->vcpu_id];
	if (!fVmMemory || fVmPending || request->map_generation != fVmGeneration || !slot.context ||
	    !slot.state || !slot.state->valid || !slot.context->initialized || slot.context->failed) {
		IOLockUnlock(fVmLock); return kIOReturnNotReady;
	}
	if (amdvm_run_slot_try_acquire(&slot.hot.runState) != 1) {
		IOLockUnlock(fVmLock); return kIOReturnBusy;
	}
	auto *context = slot.context;
	if (context->lease.active || !__sync_bool_compare_and_swap(&context->busy, 0u, 1u)) {
		(void)amdvm_run_slot_release(&slot.hot.runState);
		IOLockUnlock(fVmLock); return kIOReturnBusy;
	}
	auto *state = slot.state;
	uint64_t mapGeneration = fVmGeneration;
	IOLockUnlock(fVmLock);
	if (set) {
		const auto *stateRequest = static_cast<const amdvm_vm_abi_draft_vcpu_state_request_t *>(input.bytes());
		auto *result = static_cast<amdvm_vm_abi_draft_lifecycle_result_t *>(output.bytes());
		result->abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
		result->structure_size = static_cast<uint32_t>(resultSize);
		result->map_generation = mapGeneration;
		result->context_status = amdvm_vm_cpu_state_set(state, context->guest_vmcb,
		    reinterpret_cast<uint64_t *>(context->guest_gprs), context->guest_xstate,
		    &stateRequest->state) ? AMDVM_HLT_CONTEXT_INVALID_GUEST : 0;
		if (!result->context_status) context->guest_execution_xcr0 =
		    amdvm_xstate_execution_mask(stateRequest->state.xcr0, stateRequest->state.cr[4]);
	} else {
		auto *result = static_cast<amdvm_vm_abi_draft_vcpu_state_result_t *>(output.bytes());
		result->abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
		result->structure_size = static_cast<uint32_t>(resultSize);
		result->vcpu_id = request->vcpu_id;
		result->map_generation = mapGeneration;
		result->context_status = amdvm_vm_cpu_state_get(state, context->guest_vmcb,
		    reinterpret_cast<const uint64_t *>(context->guest_gprs), context->guest_xstate,
		    &result->state) ? AMDVM_HLT_CONTEXT_INVALID_GUEST : 0;
	}
	__sync_lock_release(&context->busy);
	(void)amdvm_run_slot_release(&slot.hot.runState);
	amdvm_run_slot_wake_if_quiescent(fVmLock, &slot.hot.runState,
	    &slot.hot.runBufferBusy);
	return output.copyTo(arguments);
}

IOReturn
AMDVMProbeUserClient::vmVcpuDiagnostic(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	AMDVMStructureOutput output;
	if (!arguments || !input.init(arguments,
	    sizeof(amdvm_vm_abi_draft_vcpu_diagnostic_request_t)) ||
	    !output.init(sizeof(amdvm_vm_abi_draft_vcpu_diagnostic_result_t))) {
		return kIOReturnBadArgument;
	}
	if ((arguments->structureOutputDescriptor &&
	    arguments->structureOutputDescriptor->getLength() < sizeof(amdvm_vm_abi_draft_vcpu_diagnostic_result_t)) ||
	    (!arguments->structureOutput &&
	    arguments->structureOutputSize < sizeof(amdvm_vm_abi_draft_vcpu_diagnostic_result_t))) {
		return kIOReturnBadArgument;
	}
	const auto *request = static_cast<const amdvm_vm_abi_draft_vcpu_diagnostic_request_t *>(input.bytes());
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != sizeof(*request) || request->reserved ||
	    request->vcpu_id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS) {
		return kIOReturnBadArgument;
	}
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK) {
		return kIOReturnNotPermitted;
	}
	IOLockLock(fVmLock);
	if (fVmClosed) {
		IOLockUnlock(fVmLock);
		return kIOReturnNotOpen;
	}
	auto &slot = fVmCpus[request->vcpu_id];
	if (!fVmMemory || fVmPending || request->map_generation != fVmGeneration ||
	    !slot.context || !slot.state || !slot.context->initialized ||
	    slot.context->failed || !slot.context->guest_memory_mapped) {
		IOLockUnlock(fVmLock);
		return kIOReturnNotReady;
	}
	if (amdvm_run_slot_try_acquire(&slot.hot.runState) != 1) {
		IOLockUnlock(fVmLock);
		return kIOReturnBusy;
	}
	auto *context = slot.context;
	if (context->lease.active || !__sync_bool_compare_and_swap(&context->busy, 0u, 1u)) {
		(void)amdvm_run_slot_release(&slot.hot.runState);
		IOLockUnlock(fVmLock);
		return kIOReturnBusy;
	}
	uint32_t resetProfile = slot.resetProfile;
	uint64_t mapGeneration = fVmGeneration;
	IOLockUnlock(fVmLock);
	auto *result = static_cast<amdvm_vm_abi_draft_vcpu_diagnostic_result_t *>(output.bytes());
	result->abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result->structure_size = sizeof(*result);
	result->vcpu_id = request->vcpu_id;
	result->map_generation = mapGeneration;
	result->context_status = AMDVM_HLT_CONTEXT_OK;
	result->snapshot.abi_version = AMDVM_VCPU_ABI_VERSION;
	result->snapshot.context_status = AMDVM_HLT_CONTEXT_OK;
	result->snapshot.guest_profile = resetProfile;
	result->snapshot.guest_physical_address = AMDVM_VCPU_RESET_ROM_GPA;
	result->snapshot.rom_offset = AMDVM_VCPU_RESET_ROM_OFFSET;
	result->snapshot.flags |= AMDVM_VCPU_DIAGNOSTIC_ROM_INSTALLED;
	bcopy((const uint8_t *)context->guest_payload + AMDVM_VCPU_RESET_ROM_OFFSET,
	    result->snapshot.entry_bytes, sizeof(result->snapshot.entry_bytes));
	const uint32_t csOffset = AMDVM_VMCB_GUEST_CS;
	bcopy(&context->guest_vmcb->bytes[csOffset], &result->snapshot.cs_selector,
	    sizeof(result->snapshot.cs_selector));
	bcopy(&context->guest_vmcb->bytes[csOffset + 2],
	    &result->snapshot.cs_attributes, sizeof(result->snapshot.cs_attributes));
	bcopy(&context->guest_vmcb->bytes[csOffset + 4],
	    &result->snapshot.cs_limit, sizeof(result->snapshot.cs_limit));
	result->snapshot.cs_base = amdvm_vmcb_get_u64(context->guest_vmcb, csOffset + 8);
	result->snapshot.rip = amdvm_vmcb_get_u64(context->guest_vmcb, AMDVM_VMCB_GUEST_RIP);
	result->snapshot.rflags = amdvm_vmcb_get_u64(context->guest_vmcb, AMDVM_VMCB_GUEST_RFLAGS);
	result->snapshot.cr0 = amdvm_vmcb_get_u64(context->guest_vmcb, AMDVM_VMCB_GUEST_CR0);
	result->snapshot.efer = amdvm_vmcb_get_u64(context->guest_vmcb, AMDVM_VMCB_GUEST_EFER);
	result->nested_paging_control = (uint32_t)amdvm_vmcb_get_u64(
	    context->guest_vmcb, AMDVM_VMCB_NESTED_PAGING);
	result->npt_root = amdvm_vmcb_get_u64(context->guest_vmcb, AMDVM_VMCB_NPT_ROOT);
	result->npt_root_matches = result->npt_root != 0 &&
	    result->npt_root == amdvm_npt_root_pa(&context->npt);
	if (result->nested_paging_control & AMDVM_VMCB_NPT_ENABLE) {
		result->flags |= AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ENABLED;
	}
	if (result->npt_root_matches) {
		result->flags |= AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ROOT_MATCH;
	}
	amdvm_npt_translation_t translation = {};
	int resetWalk = amdvm_npt_translate_4k_diagnostic(&context->npt,
	    AMDVM_VCPU_RESET_ROM_GPA, &translation);
	result->reset_translation_status = resetWalk == AMDVM_NPT_OK ?
	    AMDVM_VM_ABI_DRAFT_NPT_STATUS_MAPPED :
	    AMDVM_VM_ABI_DRAFT_NPT_STATUS_INVALID;
	if (result->reset_translation_status == AMDVM_NPT_OK &&
	    (translation.permissions & (AMDVM_NPT_READ | AMDVM_NPT_WRITE |
	    AMDVM_NPT_EXECUTE)) == (AMDVM_NPT_READ | AMDVM_NPT_EXECUTE)) {
		result->reset_translation_hpa = translation.host_physical_address &
		    ~(uint64_t)(AMDVM_NPT_PAGE_SIZE - 1u);
		result->flags |= AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_TRANSLATES;
		result->snapshot.flags |= AMDVM_VCPU_DIAGNOSTIC_NPT_RX_VALID;
		if (result->reset_translation_hpa == context->guest_payload_pa) {
			result->flags |= AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_BACKING_MATCH;
		}
	}
	int apicWalk = amdvm_npt_translate_4k_diagnostic(&context->npt,
	    UINT64_C(0xfee00000), &translation);
	result->apic_translation_status = apicWalk == AMDVM_NPT_UNMAPPED ?
	    AMDVM_VM_ABI_DRAFT_NPT_STATUS_UNMAPPED :
	    (apicWalk == AMDVM_NPT_OK ? AMDVM_VM_ABI_DRAFT_NPT_STATUS_MAPPED :
	    AMDVM_VM_ABI_DRAFT_NPT_STATUS_INVALID);
	if (result->apic_translation_status ==
	    AMDVM_VM_ABI_DRAFT_NPT_STATUS_UNMAPPED) {
		result->flags |= AMDVM_VM_ABI_DRAFT_VCPU_DIAG_APIC_UNMAPPED;
	}
	if (result->snapshot.cs_selector == 0xf000u &&
	    result->snapshot.cs_base == AMDVM_VCPU_RESET_CS_BASE &&
	    result->snapshot.rip == AMDVM_VCPU_RESET_VECTOR_RIP &&
	    result->snapshot.rflags == 2u && result->snapshot.cr0 == 0x60000010ull) {
		result->snapshot.flags |= AMDVM_VCPU_DIAGNOSTIC_RESET_STATE;
	}
	__sync_lock_release(&context->busy);
	(void)amdvm_run_slot_release(&slot.hot.runState);
	amdvm_run_slot_wake_if_quiescent(fVmLock, &slot.hot.runState,
	    &slot.hot.runBufferBusy);
	return output.copyTo(arguments);
}

IOReturn
AMDVMProbeUserClient::vmVcpuSetMemoryTypes(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	amdvm_vm_abi_draft_vcpu_memory_types_result_t result = {};
	amdvm_vcpu_set_memory_types_request_t *legacy = nullptr;
	const amdvm_vm_abi_draft_vcpu_memory_types_request_t *request;
	size_t updatedPageCount = 0;
	IOReturn status = kIOReturnSuccess;

	if (!arguments || !input.init(arguments, sizeof(*request)) ||
	    !arguments->structureOutput ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = static_cast<const amdvm_vm_abi_draft_vcpu_memory_types_request_t *>(input.bytes());
	result.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result.structure_size = sizeof(result);
	result.vcpu_id = request->vcpu_id;
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != sizeof(*request) || request->range_count == 0 ||
	    request->range_count > AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES ||
	    request->vcpu_id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
	    request->reserved[0] || request->reserved[1]) {
		return kIOReturnBadArgument;
	}
	for (size_t i = request->range_count;
	    i < AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES; ++i) {
		amdvm_vm_abi_draft_memory_type_range_t zero = {};
		if (bcmp(&request->ranges[i], &zero, sizeof(zero)))
			return kIOReturnBadArgument;
	}
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK)
		return kIOReturnNotPermitted;

	legacy = static_cast<amdvm_vcpu_set_memory_types_request_t *>(
	    IOMalloc(sizeof(*legacy)));
	if (!legacy) return kIOReturnNoMemory;
	bzero(legacy, sizeof(*legacy));
	legacy->abi_version = AMDVM_VCPU_ABI_VERSION;
	legacy->range_count = request->range_count;
	for (size_t i = 0; i < request->range_count; ++i) {
		legacy->ranges[i].guest_physical_address =
		    request->ranges[i].guest_physical_address;
		legacy->ranges[i].size = request->ranges[i].size;
		legacy->ranges[i].memory_type = request->ranges[i].memory_type;
		legacy->ranges[i].reserved = request->ranges[i].reserved;
	}

	IOLockLock(fVmLock);
	result.map_generation = fVmGeneration;
	if (fVmClosed) {
		status = kIOReturnNotOpen;
	} else if (!fVmMemory || fVmPending || request->map_generation != fVmGeneration ||
	    !fVmCpus[request->vcpu_id].context ||
	    !fVmCpus[request->vcpu_id].state ||
	    !fVmCpus[request->vcpu_id].state->valid) {
		status = kIOReturnNotReady;
	} else {
		auto &slot = fVmCpus[request->vcpu_id];
		auto *context = slot.context;
		bool anyActiveRun = false;
		for (uint32_t i = 0; i < AMDVM_VM_ABI_DRAFT_MAX_VCPUS; ++i) {
			if (amdvm_run_slot_refcount(&fVmCpus[i].hot.runState) != 0) {
				anyActiveRun = true;
				break;
			}
		}
		if (amdvm_run_slot_is_closing(&slot.hot.runState) || anyActiveRun) {
			status = kIOReturnBusy;
		}
		else if (!context->initialized || context->failed || context->lease.active) {
			result.context_status = AMDVM_HLT_CONTEXT_INVALID;
		} else {
			result.context_status = amdvm_hlt_context_set_memory_types(
			    context, legacy, &updatedPageCount);
			if (result.context_status == AMDVM_HLT_CONTEXT_OK) {
				result.range_count = request->range_count;
				result.updated_page_count = updatedPageCount;
			}
		}
	}
	IOLockUnlock(fVmLock);
	IOFree(legacy, sizeof(*legacy));
	if (status != kIOReturnSuccess) return status;
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vmVcpuKick(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	amdvm_vm_abi_draft_vcpu_kick_result_t result = {};
	const amdvm_vm_abi_draft_vcpu_kick_request_t *request;
	uint64_t current, previous;

	if (!arguments || !input.init(arguments, sizeof(*request)) ||
	    !arguments->structureOutput ||
	    arguments->structureOutputSize < sizeof(result))
		return kIOReturnBadArgument;
	request = static_cast<const amdvm_vm_abi_draft_vcpu_kick_request_t *>(input.bytes());
	bool reportPauseStats = request->flags ==
	    AMDVM_VM_ABI_DRAFT_VCPU_KICK_REPORT_PAUSE_STATS;
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != sizeof(*request) ||
	    (request->flags != AMDVM_VM_ABI_DRAFT_VCPU_KICK_HOST_SIGNAL_SENT &&
	     !reportPauseStats) ||
	    request->vcpu_id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
	    request->map_generation == 0 ||
	    (!reportPauseStats && request->kick_generation == 0))
		return kIOReturnBadArgument;
#if !defined(AMDVM_EXPERIMENTAL_PROVIDER_PAUSE)
	if (reportPauseStats) return kIOReturnUnsupported;
#endif
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK)
		return kIOReturnNotPermitted;

	IOLockLock(fVmLock);
	auto &slot = fVmCpus[request->vcpu_id];
	if (!fVmMemory || fVmPending || request->map_generation != fVmGeneration || !slot.context) {
		IOLockUnlock(fVmLock);
		return kIOReturnNotReady;
	}
	current = __sync_val_compare_and_swap(&slot.hot.kickGeneration, 0u, 0u);
	while (amdvm_kick_generation_should_update(request->kick_generation,
	    current, reportPauseStats)) {
		previous = __sync_val_compare_and_swap(&slot.hot.kickGeneration, current,
		    request->kick_generation);
		if (previous == current) {
			current = request->kick_generation;
			break;
		}
		current = previous;
	}
	result.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result.structure_size = sizeof(result);
	result.vcpu_id = request->vcpu_id;
	result.map_generation = fVmGeneration;
	result.kick_generation = current;
	uint64_t pauseEntries = __sync_val_compare_and_swap(
	    &slot.pause.fastPathEntries, 0u, 0u);
	uint64_t pauseHandled = __sync_val_compare_and_swap(
	    &slot.pause.handled, 0u, 0u);
	uint64_t pauseBudget = __sync_val_compare_and_swap(
	    &slot.pause.returnsBudget, 0u, 0u);
	uint64_t pauseEvents = __sync_val_compare_and_swap(
	    &slot.pause.returnsEvent, 0u, 0u);
	uint64_t pauseUnhandled = __sync_val_compare_and_swap(
	    &slot.pause.returnsUnhandled, 0u, 0u);
	IOLockUnlock(fVmLock);
	if (reportPauseStats) {
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_PAUSE)
		amdvm_pause_stats_report(request->vcpu_id, current, pauseEntries,
		    pauseHandled, pauseBudget, pauseEvents, pauseUnhandled);
#endif
	#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
		amdvm_provider_timing_report(request->vcpu_id,
		    __sync_val_compare_and_swap(&slot.timing.runs, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.stateSetTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.xstatePreflightTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.interruptOffTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.vmrunTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.stateGetTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.memoryTypeCheckTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.ownerAcquireTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.vmcbPrepareTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.vmrunXstateTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.ownerReleaseTicks, 0u, 0u),
		    __sync_val_compare_and_swap(&slot.timing.vmrunSamples, 0u, 0u));
	#endif
	}
	/*
	 * The caller sends QEMU's host-thread signal before this latch update. This
	 * selector only records the generation and never waits on a running vCPU or
	 * calls a private CPU-kick KPI. The signal does not by itself prove bounded
	 * physical interrupt delivery to a processor executing VMRUN; host IPI/TLB
	 * progress remains a separate hardware safety gate.
	 */
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vmVcpuRun(IOExternalMethodArguments *arguments)
{
	return vmVcpuRunInternal(arguments, -1);
}

IOReturn
AMDVMProbeUserClient::vmVcpuRunInternal(IOExternalMethodArguments *arguments,
	int32_t expectedVcpuId, bool borrowSharedInput)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMRunBufferGuard bufferGuard;
	AMDVMStructureOutput output;
	IOSimpleLock *runLock = nullptr;
	thread_t initialThread = THREAD_NULL;
	IOInterruptState interruptState = FALSE;
	uint64_t cr0Before = 0, cr0After = 0;
	int cpuBefore = -1, cpuAfter = -1;
	int firstCaptureStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int secondCaptureStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int xstateStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int ownerStatus = AMDVM_SVM_OWNER_INVALID;
	int contextStatus = AMDVM_HLT_CONTEXT_INVALID;
	amdvm_vmrun_status_t vmrunStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	amdvm_vmexit_snapshot_t exitSnapshot = {};
	const amdvm_vm_abi_draft_vcpu_run_request_t *request;
	amdvm_vm_abi_draft_vcpu_run_result_t *result;
	amdvm_hlt_context_t *context = nullptr;
	amdvm_vm_cpu_state_t *state = nullptr;
	void *runInputBuffer = nullptr;
	void *runOutputBuffer = nullptr;
	IOMemoryDescriptor *inputDescriptor = nullptr;
	bool descriptorPrepared = false;
	amdvm_run_request_header_t requestHeader = {};
	uint32_t vcpuId = UINT32_MAX;
	uint64_t generation = 0;
	uint64_t pinnedGeneration = 0;
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	uint64_t timingPhaseStarted = 0;
	uint64_t interruptOffElapsedTicks = 0;
	uint64_t interruptOffStartTicks = 0;
#endif
	bool interruptsDisabled = false;
	bool patPrepared = false;
	bool nmiIretStepArmed = false;
	bool diagnosticGpIntercept = false;
	bool diagnosticPfIntercept = false;
	bool nativeRdtsc = false;
	bool nativeCr3 = false;
	bool pauseRequested = false;
	uint32_t savedMisc1 = 0;
	bool nativeRdtscPrepared = false;
	bool exceptionInterceptsSaved = false;
	uint32_t savedExceptionIntercepts = 0;
	uint32_t diagnosticSavedExceptionIntercepts = 0;
	uint32_t diagnosticExceptionMask = 0;
	uint64_t originalGuestRflags = 0;
	uint64_t pauseDeadline = 0;
	uint32_t pauseHandled = 0;
	bool pauseLoopStarted = false;
	bool pauseDeadlineInitialized = false;
	bool pauseAdvancePending = false;
	bool maskedQemuEventPossible = false;
	int contextRunStatus = AMDVM_HLT_CONTEXT_INVALID;
	int stateSetStatus = 0;
	size_t outputSize = sizeof(amdvm_vm_abi_draft_vcpu_run_result_t);
	size_t inputSize = 0;
	IOReturn outputStatus = kIOReturnSuccess;

	if (!arguments || expectedVcpuId < -1 ||
	    expectedVcpuId >= (int32_t)AMDVM_VM_ABI_DRAFT_MAX_VCPUS)
		return kIOReturnBadArgument;
	inputDescriptor = arguments->structureInputDescriptor;
	inputSize = inputDescriptor != nullptr ?
	    (size_t)inputDescriptor->getLength() : (size_t)arguments->structureInputSize;
	if ((inputSize != AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V2_SIZE &&
	    inputSize != AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE) ||
	    (borrowSharedInput && (expectedVcpuId < 0 || inputDescriptor != nullptr)) ||
	    (inputDescriptor != nullptr && arguments->structureInputSize != 0) ||
	    (inputDescriptor == nullptr && arguments->structureInput == nullptr) ||
	    (arguments->structureOutputDescriptor &&
	    arguments->structureOutputDescriptor->getLength() < outputSize) ||
	    (!arguments->structureOutputDescriptor && (!arguments->structureOutput ||
	    arguments->structureOutputSize < outputSize)))
		return kIOReturnBadArgument;
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK)
		return kIOReturnNotPermitted;
	if (inputDescriptor != nullptr) {
		if (inputDescriptor->prepare() != kIOReturnSuccess)
			return kIOReturnBadArgument;
		descriptorPrepared = true;
		if (inputDescriptor->readBytes(0, &requestHeader,
		    sizeof(requestHeader)) != sizeof(requestHeader)) {
			(void)inputDescriptor->complete();
			return kIOReturnBadArgument;
		}
	} else {
		bcopy(arguments->structureInput, &requestHeader, sizeof(requestHeader));
	}
	if (amdvm_run_buffer_header_validate(&requestHeader, inputSize,
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V2_SIZE,
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE,
	    AMDVM_VM_ABI_DRAFT_MAX_VCPUS, &vcpuId) != 0) {
		if (descriptorPrepared) (void)inputDescriptor->complete();
		return kIOReturnBadArgument;
	}
	if (amdvm_run_buffer_vcpu_matches(vcpuId, expectedVcpuId,
	    AMDVM_VM_ABI_DRAFT_MAX_VCPUS) != 0) {
		if (descriptorPrepared) (void)inputDescriptor->complete();
		return kIOReturnBadArgument;
	}
	IOLockLock(fVmLock);
	auto &reservedSlot = fVmCpus[vcpuId];
	if (fVmClosed || !fVmMemory || fVmPending || !reservedSlot.context ||
	    !reservedSlot.state || !reservedSlot.runInput || !reservedSlot.runOutput ||
	    !reservedSlot.runLock || !reservedSlot.state->valid ||
	    !reservedSlot.context->initialized || reservedSlot.context->failed ||
	    !reservedSlot.context->guest_memory_mapped ||
	    !reservedSlot.context->memory_type_count ||
	    reservedSlot.context->lease.active) {
		IOLockUnlock(fVmLock);
		if (descriptorPrepared) (void)inputDescriptor->complete();
		return kIOReturnNotReady;
	}
	if (amdvm_run_slot_try_acquire(&reservedSlot.hot.runState) != 1) {
		IOLockUnlock(fVmLock);
		if (descriptorPrepared) (void)inputDescriptor->complete();
		return kIOReturnBusy;
	}
	if (!amdvm_run_buffer_try_reserve(&reservedSlot.hot.runBufferBusy)) {
		(void)amdvm_run_slot_release(&reservedSlot.hot.runState);
		IOLockUnlock(fVmLock);
		if (descriptorPrepared) (void)inputDescriptor->complete();
		return kIOReturnBusy;
	}
	bufferGuard.acquire(fVmLock, &reservedSlot.hot.runState,
	    &reservedSlot.hot.runBufferBusy, true);
	context = reservedSlot.context;
	state = reservedSlot.state;
	pinnedGeneration = fVmGeneration;
	runInputBuffer = reservedSlot.runInput;
	runOutputBuffer = reservedSlot.runOutput;
	runLock = reservedSlot.runLock;
	IOLockUnlock(fVmLock);
	if (!amdvm_run_structure_requires_staging_copy(inputDescriptor != nullptr)) {
		/* In-band structureInput belongs to this synchronous IOUserClient call.
		 * Shared RUN additionally owns the slot through COMPLETE and retains its
		 * memory descriptor. Both sources stay stable until this method returns. */
		request = static_cast<const amdvm_vm_abi_draft_vcpu_run_request_t *>(
		    arguments->structureInput);
	} else {
		IOByteCount inputCopied = inputDescriptor != nullptr ?
		    inputDescriptor->readBytes(0, runInputBuffer, inputSize) :
		    (bcopy(arguments->structureInput, runInputBuffer, inputSize), inputSize);
		IOReturn inputComplete = descriptorPrepared ? inputDescriptor->complete() :
		    kIOReturnSuccess;
		descriptorPrepared = false;
		if (inputCopied != inputSize || inputComplete != kIOReturnSuccess)
			return kIOReturnBadArgument;
		request = static_cast<const amdvm_vm_abi_draft_vcpu_run_request_t *>(
		    runInputBuffer);
	}
	/* Inline output is an in-band IPC buffer owned by this synchronous
	 * IOUserClient call. Write there directly; OOL descriptors still stage. */
	void *outputBuffer = !amdvm_run_structure_requires_staging_copy(
	    arguments->structureOutputDescriptor != nullptr) ?
	    arguments->structureOutput : runOutputBuffer;
	if (!output.initWithBuffer(outputBuffer, outputSize))
		return kIOReturnBadArgument;
	result = static_cast<amdvm_vm_abi_draft_vcpu_run_result_t *>(output.bytes());
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != inputSize ||
	    (request->flags &
	      ~(AMDVM_VM_ABI_DRAFT_VCPU_RUN_DIAGNOSTIC_INTERCEPT_GP |
	        AMDVM_VM_ABI_DRAFT_VCPU_RUN_DIAGNOSTIC_INTERCEPT_PF |
	        AMDVM_VM_ABI_DRAFT_VCPU_RUN_NATIVE_RDTSC |
	        AMDVM_VM_ABI_DRAFT_VCPU_RUN_MTRR_CACHE |
	        AMDVM_VM_ABI_DRAFT_VCPU_RUN_PROVIDER_PAUSE |
	        AMDVM_VM_ABI_DRAFT_VCPU_RUN_NATIVE_CR3)) != 0 ||
	    request->vcpu_id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
	    request->vcpu_id != vcpuId ||
	    amdvm_run_buffer_vcpu_matches(request->vcpu_id, expectedVcpuId,
	    AMDVM_VM_ABI_DRAFT_MAX_VCPUS) != 0 ||
	    request->map_generation == 0 ||
	    amdvm_vmcb_validate_x86_state_v1(&request->state) != 0 ||
	    (((request->flags &
	       AMDVM_VM_ABI_DRAFT_VCPU_RUN_DIAGNOSTIC_INTERCEPT_PF) != 0) ?
	     amdvm_vmcb_validate_event_injection_diagnostic_pf(
	        request->event_injection,
	        request->event_injection_error_code) :
	     amdvm_vmcb_validate_event_injection(request->event_injection,
	        request->event_injection_error_code)) != 0)
		return kIOReturnBadArgument;
	diagnosticGpIntercept = (request->flags &
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_DIAGNOSTIC_INTERCEPT_GP) != 0;
	diagnosticPfIntercept = (request->flags &
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_DIAGNOSTIC_INTERCEPT_PF) != 0;
	nativeRdtsc = (request->flags &
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_NATIVE_RDTSC) != 0;
	nativeCr3 = (request->flags &
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_NATIVE_CR3) != 0;
	pauseRequested = (request->flags &
	    AMDVM_VM_ABI_DRAFT_VCPU_RUN_PROVIDER_PAUSE) != 0;
#if !defined(AMDVM_EXPERIMENTAL_NATIVE_CR3)
	if (nativeCr3) return kIOReturnUnsupported;
#endif
	if (nativeCr3 && (request->state.cr[4] & (UINT64_C(1) << 17)))
		return kIOReturnUnsupported;
	if (nativeRdtsc && inputSize != AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE)
		return kIOReturnBadArgument;
	result->abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result->structure_size = sizeof(*result);
	result->vcpu_id = request->vcpu_id;
	result->context_status = AMDVM_HLT_CONTEXT_INVALID;
	result->owner_status = AMDVM_SVM_OWNER_INVALID;
	result->run_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	result->exit_code = UINT64_MAX;
	result->map_generation = request->map_generation;
	result->state = request->state;
	vcpuId = request->vcpu_id;
	generation = request->map_generation;
	if (generation != pinnedGeneration ||
	    amdvm_run_slot_is_closing(&fVmCpus[vcpuId].hot.runState)) {
		return kIOReturnNotReady;
	}
	auto &slot = fVmCpus[vcpuId];
	if (request->kick_generation != __sync_val_compare_and_swap(
	    &slot.hot.kickGeneration, 0u, 0u)) {
		result->flags = AMDVM_VCPU_RUN_KICKED;
		result->context_status = AMDVM_HLT_CONTEXT_OK;
		result->owner_status = AMDVM_SVM_OWNER_OK;
		result->run_status = AMDVM_VMRUN_SUCCESS;
		result->state = request->state;
		return output.copyTo(arguments);
	}
	__sync_lock_test_and_set(&slot.hot.runGeneration, request->kick_generation);
	__sync_lock_test_and_set(&slot.hot.runCpu, UINT32_MAX);

	/* The active reference pins context/state against destroy and state APIs.
	 * Stage the large architectural state and VMCB fields outside fVmLock so
	 * another vCPU does not queue behind this copy. */
	context->vmcb_clean_candidates = amdvm_vmcb_state_clean_candidates(
	    context->guest_vmcb, &request->state);
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	timingPhaseStarted = mach_absolute_time();
#endif
	stateSetStatus = amdvm_vm_cpu_state_set_validated(state, context->guest_vmcb,
	    reinterpret_cast<uint64_t *>(context->guest_gprs), context->guest_xstate,
	    &request->state);
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	__sync_fetch_and_add(&slot.timing.stateSetTicks,
	    mach_absolute_time() - timingPhaseStarted);
#endif
	if (stateSetStatus != 0) {
		contextStatus = AMDVM_HLT_CONTEXT_INVALID_GUEST;
		goto run_done;
	}
	if (request->state.interruptibility &
	    AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP) {
		originalGuestRflags = request->state.rflags;
		if (amdvm_vmcb_arm_nmi_iret_step(context->guest_vmcb,
		    &request->state, &savedExceptionIntercepts) != 0) {
			contextStatus = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			goto run_done;
		}
		nmiIretStepArmed = true;
	}
	context->guest_execution_xcr0 = amdvm_xstate_execution_mask(
	    request->state.xcr0, request->state.cr[4]);
	if (pauseRequested && !amdvm_provider_pause_cycle_enabled()) {
		contextStatus = AMDVM_HLT_CONTEXT_INVALID_GUEST;
		goto run_done;
	}
	context->host_mtrr_cache_enabled =
	    (request->flags & AMDVM_VM_ABI_DRAFT_VCPU_RUN_MTRR_CACHE) != 0;
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_EVENT_INJECTION,
	    ((uint64_t)request->event_injection_error_code << 32) |
	    request->event_injection);
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_EXIT_INT_INFO, 0);
	amdvm_vmcb_set_u32(context->guest_vmcb, AMDVM_VMCB_EXIT_INT_INFO_ERROR, 0);
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_EXIT_CODE,
	    AMDVM_SVM_EXIT_NOT_RUN);
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_EXIT_INFO1, 0);
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_EXIT_INFO2, 0);
	context->guest_vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_LENGTH] = 0;
	memset(&context->guest_vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_BYTES], 0xcc,
	    AMDVM_VMCB_DECODE_ASSIST_MAX);

	if (__sync_val_compare_and_swap(&gHltFpuPreflightPassed, 0u, 0u) == 0) {
		contextStatus = AMDVM_HLT_CONTEXT_FPU_PREFLIGHT_REQUIRED;
		vmrunStatus = AMDVM_VMRUN_FPU_NOT_READY;
		goto run_done;
	}
	if (amdvm_hlt_context_prepare_guest_pat(context) != 0) {
		contextStatus = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		goto run_done;
	}
	patPrepared = true;
	/* These are VMCB control-area edits only. Finish them while interrupts are
	 * enabled; the pinned XSAVE helper below returns with the CPU pinned and
	 * local interrupts disabled for the SVM lease/VMRUN window. */
	if (diagnosticGpIntercept || diagnosticPfIntercept) {
		if (diagnosticGpIntercept)
			diagnosticExceptionMask |= UINT32_C(1) << AMDVM_VCPU_EXCEPTION_GP;
		if (diagnosticPfIntercept)
			diagnosticExceptionMask |= UINT32_C(1) << AMDVM_VCPU_EXCEPTION_PF;
		if (amdvm_vmcb_add_exception_intercepts(context->guest_vmcb,
		    diagnosticExceptionMask, &diagnosticSavedExceptionIntercepts) != 0) {
			contextStatus = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			goto run_done;
		}
		exceptionInterceptsSaved = true;
	}
	if (nativeRdtsc) {
		if (amdvm_vmcb_prepare_native_rdtsc(context->guest_vmcb,
		    request->tsc_offset, &savedMisc1) != 0) {
			contextStatus = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			goto run_done;
		}
		nativeRdtscPrepared = true;
	}
	initialThread = current_thread();
	#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	timingPhaseStarted = mach_absolute_time();
	#endif
	xstateStatus = amdvm_pin_run_context(context->host_xstate, initialThread,
	    runLock, &cpuBefore, &cpuAfter, &cr0Before, &cr0After,
	    &interruptState, &firstCaptureStatus, &secondCaptureStatus,
	#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	    &interruptOffStartTicks);
	#else
	    nullptr);
	#endif
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	__sync_fetch_and_add(&slot.timing.xstatePreflightTicks,
	    mach_absolute_time() - timingPhaseStarted);
	#endif
	if (xstateStatus != AMDVM_VMRUN_SUCCESS) {
		contextStatus = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
		vmrunStatus = (amdvm_vmrun_status_t)xstateStatus;
		goto run_done;
	}
	interruptsDisabled = true;
	if (cpuBefore < 0 || cpuBefore >= 64 || cpuBefore != cpu_number() ||
	    initialThread != current_thread()) {
		contextStatus = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
		vmrunStatus = AMDVM_VMRUN_FPU_NOT_READY;
		goto run_done;
	}
	__sync_lock_test_and_set(&slot.hot.runGeneration, request->kick_generation);
	__sync_lock_test_and_set(&slot.hot.runCpu, (uint32_t)cpuBefore);
	if (request->kick_generation != __sync_val_compare_and_swap(
	    &slot.hot.kickGeneration, 0u, 0u)) {
		result->flags = AMDVM_VCPU_RUN_KICKED;
		contextStatus = AMDVM_HLT_CONTEXT_OK;
		ownerStatus = AMDVM_SVM_OWNER_OK;
		vmrunStatus = AMDVM_VMRUN_SUCCESS;
		result->state = request->state;
		goto run_done;
	}
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	timingPhaseStarted = mach_absolute_time();
#endif
	contextRunStatus = amdvm_vmapi_run_context(context, &ownerStatus,
	    &vmrunStatus, &exitSnapshot, nativeCr3);
	if (diagnosticPfIntercept && vmrunStatus == AMDVM_VMRUN_SUCCESS &&
	    exitSnapshot.code == 0x04eULL && !slot.diagnosticPfLogged) {
		/* Capture the first intercepted #PF only. QEMU may reinject ordinary
		 * page faults while searching for the mapping mismatch; per-fault IOLog
		 * would perturb the run and flood the kernel log. */
		slot.diagnosticPfLogged = 1;
		IOLog("AMDVM_FIRST_PF_VMCB_EVENT vcpu=%u code=0x%llx info1=0x%llx info2=0x%llx eventinj=0x%llx exit_int_info=0x%llx tlb_control=0x%llx clean=0x%llx\n",
		    vcpuId, (unsigned long long)exitSnapshot.code,
		    (unsigned long long)exitSnapshot.info1,
		    (unsigned long long)exitSnapshot.info2,
		    (unsigned long long)exitSnapshot.event_injection,
		    (unsigned long long)exitSnapshot.exit_int_info,
		    (unsigned long long)exitSnapshot.tlb_control,
		    (unsigned long long)exitSnapshot.clean_bits);
	}
	if (context->host_mtrr_cache_refresh_needed) {
		clock_interval_to_deadline(100u, kMillisecondScale,
		    &context->host_mtrr_cache_deadline);
		context->host_mtrr_cache_refresh_needed = 0;
	}
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	__sync_fetch_and_add(&slot.timing.vmrunTicks,
	    mach_absolute_time() - timingPhaseStarted);
	if (exitSnapshot.vmrun_samples != 0) {
		__sync_fetch_and_add(&slot.timing.memoryTypeCheckTicks,
		    exitSnapshot.memory_type_check_ticks);
		__sync_fetch_and_add(&slot.timing.ownerAcquireTicks,
		    exitSnapshot.owner_acquire_ticks);
		__sync_fetch_and_add(&slot.timing.vmcbPrepareTicks,
		    exitSnapshot.vmcb_prepare_ticks);
		__sync_fetch_and_add(&slot.timing.vmrunXstateTicks,
		    exitSnapshot.vmrun_xstate_ticks);
		__sync_fetch_and_add(&slot.timing.ownerReleaseTicks,
		    exitSnapshot.owner_release_ticks);
		__sync_fetch_and_add(&slot.timing.vmrunSamples,
		    exitSnapshot.vmrun_samples);
	}
#endif
	if (contextRunStatus == AMDVM_HLT_CONTEXT_INVALID_GUEST &&
	    vmrunStatus == AMDVM_VMRUN_SUCCESS &&
	    __sync_bool_compare_and_swap(&gInvalidVmcbFailureReported, 0u, 1u)) {
		/* One failure-only VMCB snapshot; never log on the VMEXIT hot path. */
		uint64_t code = amdvm_vmcb_get_u64(context->guest_vmcb,
		    AMDVM_VMCB_EXIT_CODE);
		if (code == AMDVM_SVM_EXIT_INVALID) {
			uint64_t cs = amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_CS);
			uint64_t ss = amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_SS);
			uint64_t ds = amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_DS);
			uint64_t es = amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_ES);
			uint64_t gdtr = amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_GDTR);
			uint64_t idtr = amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_IDTR);
			uint64_t ldtr = amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_LDTR);
			uint64_t tr = amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_TR);
			IOLog("AMDVM_VMEXIT_INVALID vcpu=%u vmcb_pa=0x%llx code=0x%llx info1=0x%llx info2=0x%llx next_rip=0x%llx rip=0x%llx rsp=0x%llx rflags=0x%llx eventinj=0x%llx exit_int_info=0x%llx clean=0x%llx run=%d owner=%d\n",
			    vcpuId, (unsigned long long)context->guest_vmcb_pa,
			    (unsigned long long)code,
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_EXIT_INFO1),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_EXIT_INFO2),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_NEXT_RIP),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_RIP),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_RSP),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_RFLAGS),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_EVENT_INJECTION),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_EXIT_INT_INFO),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_CLEAN_BITS), vmrunStatus, ownerStatus);
			IOLog("AMDVM_VMEXIT_INVALID_CR cr0=0x%llx cr2=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx dr6=0x%llx dr7=0x%llx rax=0x%llx pat=0x%llx cpl=0x%x\n",
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_CR0),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_CR2),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_CR3),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_CR4),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_EFER),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_DR6),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_DR7),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_RAX),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_PAT),
			    context->guest_vmcb->bytes[AMDVM_VMCB_GUEST_CPL]);
			IOLog("AMDVM_VMEXIT_INVALID_SEG cs=%04x/%03x/%08x/%016llx ss=%04x/%03x/%08x/%016llx ds=%04x/%03x/%08x/%016llx es=%04x/%03x/%08x/%016llx\n",
			    (unsigned)(uint16_t)cs, (unsigned)(uint16_t)(cs >> 16),
			    (unsigned)(uint32_t)(cs >> 32),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_CS + 8u),
			    (unsigned)(uint16_t)ss, (unsigned)(uint16_t)(ss >> 16),
			    (unsigned)(uint32_t)(ss >> 32),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_SS + 8u),
			    (unsigned)(uint16_t)ds, (unsigned)(uint16_t)(ds >> 16),
			    (unsigned)(uint32_t)(ds >> 32),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_DS + 8u),
			    (unsigned)(uint16_t)es, (unsigned)(uint16_t)(es >> 16),
			    (unsigned)(uint32_t)(es >> 32),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_ES + 8u));
			IOLog("AMDVM_VMEXIT_INVALID_TABLE gdtr=%04x/%03x/%08x/%016llx idtr=%04x/%03x/%08x/%016llx ldtr=%04x/%03x/%08x/%016llx tr=%04x/%03x/%08x/%016llx\n",
			    (unsigned)(uint16_t)gdtr, (unsigned)(uint16_t)(gdtr >> 16),
			    (unsigned)(uint32_t)(gdtr >> 32),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_GDTR + 8u),
			    (unsigned)(uint16_t)idtr, (unsigned)(uint16_t)(idtr >> 16),
			    (unsigned)(uint32_t)(idtr >> 32),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_IDTR + 8u),
			    (unsigned)(uint16_t)ldtr, (unsigned)(uint16_t)(ldtr >> 16),
			    (unsigned)(uint32_t)(ldtr >> 32),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_LDTR + 8u),
			    (unsigned)(uint16_t)tr, (unsigned)(uint16_t)(tr >> 16),
			    (unsigned)(uint32_t)(tr >> 32),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_TR + 8u));
			IOLog("AMDVM_VMEXIT_INVALID_CTRL intercepts=0x%llx cr_read_write=0x%llx dr_read_write=0x%llx exceptions=0x%llx iopm=0x%llx msrpm=0x%llx asid_tlb=0x%llx nested=0x%llx npt_root=0x%llx int_ctl_state=0x%llx\n",
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb, 0x00cu),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb, 0x000u),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb, 0x004u),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_INTERCEPT_EXCEPTIONS),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_IOPM_PA),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_MSRPM_PA),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_TLB_CONTROL),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_NESTED_PAGING),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_NPT_ROOT),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_INTERRUPT_CONTROL));
		} else {
			IOLog("AMDVM_NOT_RUN_FAILURE vcpu=%u vmcb_pa=0x%llx snapshot_code=0x%llx vmcb_code=0x%llx vmcb_info1=0x%llx vmcb_info2=0x%llx vmcb_next_rip=0x%llx vmcb_rip=0x%llx vmcb_clean=0x%llx eventinj=0x%llx exit_int_info=0x%llx run_status=%d owner_status=%d\n",
			    vcpuId, (unsigned long long)context->guest_vmcb_pa,
			    (unsigned long long)exitSnapshot.code,
			    (unsigned long long)code,
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_EXIT_INFO1),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_EXIT_INFO2),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_NEXT_RIP),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_GUEST_RIP),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_CLEAN_BITS),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_EVENT_INJECTION),
			    (unsigned long long)amdvm_vmcb_get_u64(context->guest_vmcb,
			    AMDVM_VMCB_EXIT_INT_INFO), vmrunStatus, ownerStatus);
		}
	}
	/*
	 * PAUSE re-entry is compatible with native RDTSC: TSC_OFFSET and the
	 * disabled RDTSC intercept are installed once before the first VMRUN
	 * and remain in effect for every internal re-entry until run_done
	 * restores them. The per-RUN request flag, not the compile-time
	 * capability, decides whether this loop may run.
	 */
	if (contextRunStatus == AMDVM_HLT_CONTEXT_OK &&
	    vmrunStatus == AMDVM_VMRUN_SUCCESS &&
	    pauseRequested && !nmiIretStepArmed) {
		for (;;) {
			uint64_t eventInjection, exitIntInfo;
			uint64_t currentKick = __sync_val_compare_and_swap(
			    &slot.hot.kickGeneration, 0u, 0u);
			int kickChanged = request->kick_generation != currentKick;
			int eventPending;
			amdvm_pause_decision_t decision;

			/* Keep ordinary exits entirely on the existing QEMU dispatcher path. */
			if (exitSnapshot.code != AMDVM_PAUSE_EXIT_CODE &&
			    !pauseLoopStarted)
				break;
			if (exitSnapshot.code == AMDVM_PAUSE_EXIT_CODE &&
			    !pauseLoopStarted) {
				pauseLoopStarted = true;
				clock_interval_to_deadline(150, 1000u, &pauseDeadline);
				pauseDeadlineInitialized = true;
				__sync_fetch_and_add(&slot.pause.fastPathEntries, 1u);
			}
			eventInjection = exitSnapshot.event_injection;
			exitIntInfo = exitSnapshot.exit_int_info;
			maskedQemuEventPossible = amdvm_pause_masked_event_possible(
			    exitSnapshot.guest_rflags,
			    exitSnapshot.guest_interruptibility);
			eventPending = amdvm_pause_event_pending(eventInjection,
			    exitIntInfo, maskedQemuEventPossible);
			if (exitSnapshot.code != AMDVM_PAUSE_EXIT_CODE) {
				if (kickChanged || eventPending ||
				    amdvm_pause_exit_is_event(exitSnapshot.code)) {
					__sync_fetch_and_add(&slot.pause.returnsEvent, 1u);
				} else {
					__sync_fetch_and_add(&slot.pause.returnsUnhandled, 1u);
				}
				break;
			}
			decision = amdvm_pause_decide(exitSnapshot.code,
			    exitSnapshot.guest_rip, exitSnapshot.next_rip, pauseHandled,
			    pauseDeadlineInitialized &&
			    mach_absolute_time() >= pauseDeadline, kickChanged,
			    eventPending);
			if (decision == AMDVM_PAUSE_CONTINUE) {
				amdvm_vmexit_snapshot_t priorPause = exitSnapshot;
				uint64_t pauseRip = exitSnapshot.guest_rip;
				amdvm_vmcb_set_u64(context->guest_vmcb,
				    AMDVM_VMCB_GUEST_RIP, exitSnapshot.next_rip);
				/* Recheck the asynchronous stop/event signal at the entry edge. */
				currentKick = __sync_val_compare_and_swap(
				    &slot.hot.kickGeneration, 0u, 0u);
				eventInjection = amdvm_vmcb_get_u64(context->guest_vmcb,
				    AMDVM_VMCB_EVENT_INJECTION);
				exitIntInfo = amdvm_vmcb_get_u64(context->guest_vmcb,
				    AMDVM_VMCB_EXIT_INT_INFO);
				if (request->kick_generation != currentKick ||
				    (eventInjection & AMDVM_PAUSE_EVENT_VALID) != 0 ||
				    (exitIntInfo & AMDVM_PAUSE_EVENT_VALID) != 0 ||
				    amdvm_pause_masked_event_possible(
				        priorPause.guest_rflags,
				        priorPause.guest_interruptibility)) {
					amdvm_vmcb_set_u64(context->guest_vmcb,
					    AMDVM_VMCB_GUEST_RIP, pauseRip);
					exitSnapshot.guest_rip = pauseRip;
					__sync_fetch_and_add(&slot.pause.returnsEvent, 1u);
					break;
				}
				if (mach_absolute_time() >= pauseDeadline) {
					amdvm_vmcb_set_u64(context->guest_vmcb,
					    AMDVM_VMCB_GUEST_RIP, pauseRip);
					exitSnapshot.guest_rip = pauseRip;
					__sync_fetch_and_add(&slot.pause.returnsBudget, 1u);
					break;
				}
				pauseAdvancePending = true;
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
				timingPhaseStarted = mach_absolute_time();
#endif
				contextRunStatus = amdvm_vmapi_run_context(context,
				    &ownerStatus, &vmrunStatus, &exitSnapshot,
				    nativeCr3);
				if (context->host_mtrr_cache_refresh_needed) {
					clock_interval_to_deadline(100u, kMillisecondScale,
					    &context->host_mtrr_cache_deadline);
					context->host_mtrr_cache_refresh_needed = 0;
				}
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
				__sync_fetch_and_add(&slot.timing.vmrunTicks,
				    mach_absolute_time() - timingPhaseStarted);
				if (exitSnapshot.vmrun_samples != 0) {
					__sync_fetch_and_add(&slot.timing.memoryTypeCheckTicks,
					    exitSnapshot.memory_type_check_ticks);
					__sync_fetch_and_add(&slot.timing.ownerAcquireTicks,
					    exitSnapshot.owner_acquire_ticks);
					__sync_fetch_and_add(&slot.timing.vmcbPrepareTicks,
					    exitSnapshot.vmcb_prepare_ticks);
					__sync_fetch_and_add(&slot.timing.vmrunXstateTicks,
					    exitSnapshot.vmrun_xstate_ticks);
					__sync_fetch_and_add(&slot.timing.ownerReleaseTicks,
					    exitSnapshot.owner_release_ticks);
					__sync_fetch_and_add(&slot.timing.vmrunSamples,
					    exitSnapshot.vmrun_samples);
				}
#endif
				if (contextRunStatus != AMDVM_HLT_CONTEXT_OK ||
				    vmrunStatus != AMDVM_VMRUN_SUCCESS) {
					/* A failed re-entry did not supersede the already-observed
					 * PAUSE exit. Keep its real snapshot and original RIP. */
					amdvm_vmcb_set_u64(context->guest_vmcb,
					    AMDVM_VMCB_GUEST_RIP, priorPause.guest_rip);
					exitSnapshot = priorPause;
					__sync_fetch_and_add(&slot.pause.returnsUnhandled, 1u);
					break;
				}
				if (pauseAdvancePending) {
					pauseHandled++;
					__sync_fetch_and_add(&slot.pause.handled, 1u);
					pauseAdvancePending = false;
				}
				continue;
			}
			if (decision == AMDVM_PAUSE_RETURN_BUDGET) {
				/* Leave the final PAUSE at its original RIP for QEMU's dispatcher. */
				amdvm_vmcb_set_u64(context->guest_vmcb,
				    AMDVM_VMCB_GUEST_RIP, exitSnapshot.guest_rip);
				__sync_fetch_and_add(&slot.pause.returnsBudget, 1u);
			} else if (decision == AMDVM_PAUSE_RETURN_EVENT) {
				__sync_fetch_and_add(&slot.pause.returnsEvent, 1u);
			} else {
				__sync_fetch_and_add(&slot.pause.returnsUnhandled, 1u);
			}
			break;
		}
	}
	if (nativeRdtscPrepared) {
		amdvm_vmcb_restore_rdtsc_intercept(context->guest_vmcb, savedMisc1);
		nativeRdtscPrepared = false;
	}
	if (exceptionInterceptsSaved) {
		amdvm_vmcb_restore_exception_intercepts(context->guest_vmcb,
		    diagnosticSavedExceptionIntercepts);
		exceptionInterceptsSaved = false;
	}
	if (contextRunStatus == AMDVM_HLT_CONTEXT_RUN_GATED) {
		/* Preserve the fail-closed result; this path did not acquire SVM. */
		contextStatus = AMDVM_HLT_CONTEXT_RUN_GATED;
		result->state = request->state;
		goto run_done;
	}
	result->flags |= AMDVM_VCPU_RUN_CALLED;
	if (contextRunStatus == AMDVM_HLT_CONTEXT_OK) {
		contextStatus = AMDVM_HLT_CONTEXT_OK;
	} else if (contextRunStatus == AMDVM_HLT_CONTEXT_INVALID_GUEST) {
		contextStatus = AMDVM_HLT_CONTEXT_INVALID_GUEST;
	} else {
		contextStatus = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
	}
	result->exit_code = exitSnapshot.code;
	result->exit_info1 = exitSnapshot.info1;
	result->exit_info2 = exitSnapshot.info2;
	result->next_rip = exitSnapshot.next_rip;
	result->instruction_length = exitSnapshot.instruction_length;
	result->exit_int_info = (uint32_t)exitSnapshot.exit_int_info;
	bcopy(exitSnapshot.instruction_bytes, result->instruction_bytes,
	    sizeof(result->instruction_bytes));
	if (patPrepared) {
		amdvm_hlt_context_restore_guest_pat(context);
		patPrepared = false;
	}
	if (request->kick_generation != __sync_val_compare_and_swap(
	    &slot.hot.kickGeneration, 0u, 0u))
		result->flags |= AMDVM_VCPU_RUN_KICKED;
	if (contextStatus == AMDVM_HLT_CONTEXT_OK) {
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
		timingPhaseStarted = mach_absolute_time();
#endif
		int stateGetStatus = amdvm_vm_cpu_state_get(state, context->guest_vmcb,
	    reinterpret_cast<const uint64_t *>(context->guest_gprs), context->guest_xstate,
	    &result->state);
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
		__sync_fetch_and_add(&slot.timing.stateGetTicks,
		    mach_absolute_time() - timingPhaseStarted);
#endif
		if (stateGetStatus != 0)
			contextStatus = AMDVM_HLT_CONTEXT_INVALID_GUEST;
	}
	if (contextStatus == AMDVM_HLT_CONTEXT_OK && nmiIretStepArmed) {
		amdvm_vmcb_finish_nmi_iret_step(context->guest_vmcb,
		    savedExceptionIntercepts, originalGuestRflags, &result->state);
		nmiIretStepArmed = false;
	}

run_done:
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	__sync_fetch_and_add(&slot.timing.runs, 1u);
#endif
	context->vmcb_clean_candidates = 0;
	if (nativeRdtscPrepared) {
		amdvm_vmcb_restore_rdtsc_intercept(context->guest_vmcb, savedMisc1);
	}
	if (nmiIretStepArmed) {
		amdvm_vmcb_finish_nmi_iret_step(context->guest_vmcb,
		    savedExceptionIntercepts, originalGuestRflags, nullptr);
	}
	if (patPrepared) amdvm_hlt_context_restore_guest_pat(context);
	if (interruptsDisabled) {
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
		interruptOffElapsedTicks = mach_absolute_time() - interruptOffStartTicks;
#endif
		(void)ml_set_interrupts_enabled(interruptState);
		IOSimpleLockUnlock(runLock);
	}
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_TIMING)
	__sync_fetch_and_add(&slot.timing.interruptOffTicks, interruptOffElapsedTicks);
#endif
	__sync_lock_test_and_set(&fVmCpus[vcpuId].hot.runCpu, UINT32_MAX);
	__sync_lock_test_and_set(&fVmCpus[vcpuId].hot.runGeneration, 0u);
	result->context_status = contextStatus;
	result->owner_status = ownerStatus;
	result->run_status = (int32_t)vmrunStatus;
	if (context->lease.active) {
		result->flags |= AMDVM_VCPU_RUN_LEASE_RETAINED;
		IOLockLock(fVmLock);
		if (!fVmCpus[vcpuId].leaseTracked) {
			fVmCpus[vcpuId].leaseTracked = 1;
			__sync_fetch_and_add(&gVcpuLeaseCount, 1u);
		}
		IOLockUnlock(fVmLock);
	}
	outputStatus = output.copyTo(arguments);
	(void)firstCaptureStatus;
	(void)secondCaptureStatus;
	(void)cpuAfter;
	(void)cr0Before;
	(void)cr0After;
	return outputStatus;
}

IOReturn
AMDVMProbeUserClient::vmVcpuRunShared(IOExternalMethodArguments *arguments)
{
#if !defined(AMDVM_EXPERIMENTAL_SHARED_RUN) || \
	!defined(AMDVM_VM_API_RUN_EXPERIMENTAL)
	(void)arguments;
	return kIOReturnUnsupported;
#else
	AMDVMObjectRetainGuard lifetime(this);
	amdvm_vm_shared_run_call_t call = {};
	amdvm_vm_shared_run_ack_t ack = {};
	amdvm_vm_shared_run_slot_t *sharedSlot;
	IOBufferMemoryDescriptor *sharedMemory = nullptr;
	IOExternalMethodArguments runArguments = {};
	IOReturn runStatus = kIOReturnBadArgument;

	if (arguments == nullptr || arguments->structureInputDescriptor != nullptr ||
	    arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(call) ||
	    arguments->structureOutputDescriptor != nullptr ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(ack)) {
		return kIOReturnBadArgument;
	}
	bcopy(arguments->structureInput, &call, sizeof(call));
	ack.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	ack.structure_size = sizeof(ack);
	ack.vcpu_id = call.vcpu_id;
	ack.sequence = call.sequence;
	ack.map_generation = call.map_generation;
	if (call.abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    call.structure_size != sizeof(call) ||
	    call.vcpu_id >= AMDVM_VM_ABI_DRAFT_MAX_VCPUS ||
	    call.sequence == 0 ||
	    !amdvm_vm_shared_run_request_size_valid(call.request_size) ||
	    amdvm_client_mode_accepts(&fApiMode,
	    AMDVM_CLIENT_MODE_VM_SCOPED) != AMDVM_CLIENT_MODE_OK) {
		goto shared_run_done;
	}
	IOLockLock(fVmLock);
	if (fVmClosed || fVmRunSharedArea == nullptr ||
	    fVmRunSharedMemory == nullptr || call.map_generation != fVmGeneration) {
		IOLockUnlock(fVmLock);
		runStatus = kIOReturnNotReady;
		goto shared_run_done;
	}
	sharedSlot = &fVmRunSharedArea->slots[call.vcpu_id];
	if (!amdvm_vm_shared_run_provider_claim(sharedSlot, call.sequence,
	    call.request_size)) {
		IOLockUnlock(fVmLock);
		runStatus = kIOReturnNotReady;
		goto shared_run_done;
	}
	/* Pin the shared request/result memory across the internal RUN and response
	 * publication. VM destroy may drop its own reference after releasing the
	 * lock, but cannot invalidate this claimed slot until we complete it. */
	sharedMemory = fVmRunSharedMemory;
	sharedMemory->retain();
	IOLockUnlock(fVmLock);

	bzero(sharedSlot->result, sizeof(sharedSlot->result));
	runArguments.structureInput = sharedSlot->request;
	runArguments.structureInputSize = call.request_size;
	runArguments.structureOutput = sharedSlot->result;
	runArguments.structureOutputSize = sizeof(sharedSlot->result);
	runStatus = vmVcpuRunInternal(&runArguments, (int32_t)call.vcpu_id, true);
	/* Publish the response before COMPLETE makes the slot visible to QEMU. */
	if (!amdvm_vm_shared_run_provider_complete(sharedSlot, call.sequence,
	    (int32_t)runStatus)) {
		runStatus = kIOReturnError;
	}
	sharedMemory->release();

shared_run_done:
	ack.status = runStatus;
	bcopy(&ack, arguments->structureOutput, sizeof(ack));
	arguments->structureOutputSize = sizeof(ack);
	return kIOReturnSuccess;
#endif
}

IOReturn
AMDVMProbeUserClient::vmSetMemoryMap(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	amdvm_vm_abi_draft_memory_map_result_t result = {};
	if (!arguments || !arguments->structureOutput ||
	    arguments->structureOutputSize < sizeof(result) ||
	    !input.init(arguments, sizeof(amdvm_vm_abi_draft_memory_map_request_t)))
		return kIOReturnBadArgument;
	const auto *request = static_cast<const amdvm_vm_abi_draft_memory_map_request_t *>(input.bytes());
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != sizeof(*request) ||
	    !amdvm_vm_abi_draft_memory_map_flags_valid(request->flags) ||
	    request->reserved[0] || request->reserved[1] || !request->range_count ||
	    request->range_count > AMDVM_VCPU_MEMORY_MAX_RANGES)
		return kIOReturnBadArgument;
	for (size_t i = request->range_count; i < AMDVM_VCPU_MEMORY_MAX_RANGES; ++i) {
		amdvm_vcpu_memory_range_t zero = {};
		if (bcmp(&request->ranges[i], &zero, sizeof(zero))) return kIOReturnBadArgument;
	}
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK) return kIOReturnNotPermitted;
	amdvm_vm_memory_limits_t limits = {};
	amdvm_vm_memory_ops_t ops = {};
	if (amdvm_vm_memory_kernel_limits(&limits)) return kIOReturnUnsupported;
	amdvm_vm_memory_kernel_ops(&ops);
	IOLockLock(fVmLock);
	if (fVmClosed) { IOLockUnlock(fVmLock); return kIOReturnNotOpen; }
	if (request->expected_generation != fVmGeneration) {
		IOLockUnlock(fVmLock); return kIOReturnNotReady;
	}
	const bool replacing =
	    (request->flags & AMDVM_VM_ABI_DRAFT_MEMORY_MAP_REPLACE) != 0;
	if ((replacing && !fVmMemory) || (!replacing && fVmMemory) ||
	    (fVmPending && amdvm_vm_memory_close(&fVmPending, 0))) {
		IOLockUnlock(fVmLock); return kIOReturnBusy;
	}
	if (replacing) {
		for (size_t i = 0; i < AMDVM_VM_ABI_DRAFT_MAX_VCPUS; ++i) {
			if (fVmCpus[i].context ||
			    amdvm_run_slot_refcount(&fVmCpus[i].hot.runState) != 0 ||
			    amdvm_run_slot_is_closing(&fVmCpus[i].hot.runState)) {
				IOLockUnlock(fVmLock); return kIOReturnBusy;
			}
		}
	}
	amdvm_vm_memory_t *candidate = nullptr;
	result.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result.structure_size = sizeof(result);
	result.context_status = amdvm_vm_memory_prepare(&candidate, request->ranges,
	    request->range_count, current_task(), &limits, &ops);
	if (result.context_status == AMDVM_VM_MEMORY_OK) {
		result.context_status = amdvm_vm_memory_commit(candidate, fVmGeneration + 1u);
		if (result.context_status == AMDVM_VM_MEMORY_OK) {
			if (replacing) {
				int sharedStatus = destroyVmSharedContextLocked();
				int closeStatus = sharedStatus == AMDVM_HLT_CONTEXT_OK ?
				    amdvm_vm_memory_close(&fVmMemory, 0) : sharedStatus;
				if (closeStatus != AMDVM_VM_MEMORY_OK) {
					/* Fail closed if old pins cannot be retired after vCPU teardown. */
					fVmPending = candidate;
					fVmClosed = true;
					result.context_status = closeStatus;
				} else {
					fVmMemory = candidate;
				}
			} else {
				fVmMemory = candidate;
			}
			if (result.context_status == AMDVM_VM_MEMORY_OK) {
				fVmGeneration = candidate->generation;
				result.range_count = (uint32_t)candidate->range_count;
				result.guest_page_count = result.pinned_page_count = candidate->page_count;
				IOLog("AMDVM VM RAM committed generation=%llu ranges=%u pages=%llu replacing=%u\n",
				    (unsigned long long)fVmGeneration, result.range_count,
				    (unsigned long long)result.guest_page_count, replacing ? 1u : 0u);
			}
		} else {
			(void)amdvm_vm_memory_close(&candidate, 0);
			fVmPending = candidate;
		}
	} else {
		/* Failed cleanup is durable and never publishes a partial map. */
		fVmPending = candidate;
	}
	result.map_generation = fVmGeneration;
	IOLockUnlock(fVmLock);
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vmDestroy(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	amdvm_vm_abi_draft_lifecycle_result_t result = {};
	if (!arguments || !arguments->structureOutput ||
	    arguments->structureOutputSize < sizeof(result) ||
	    !input.init(arguments, sizeof(amdvm_vm_abi_draft_vm_destroy_request_t)))
		return kIOReturnBadArgument;
	const auto *request = static_cast<const amdvm_vm_abi_draft_vm_destroy_request_t *>(input.bytes());
	if (request->abi_version != AMDVM_VM_ABI_DRAFT_VERSION ||
	    request->structure_size != sizeof(*request) || request->flags || request->reserved)
		return kIOReturnBadArgument;
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	if (amdvm_client_mode_accepts(&fApiMode, AMDVM_CLIENT_MODE_VM_SCOPED) !=
	    AMDVM_CLIENT_MODE_OK) return kIOReturnNotPermitted;
	IOLockLock(fVmLock);
	if (request->expected_generation != fVmGeneration) {
		IOLockUnlock(fVmLock); return kIOReturnNotReady;
	}
	fVmClosed = true;
	result.abi_version = AMDVM_VM_ABI_DRAFT_VERSION;
	result.structure_size = sizeof(result);
	int cpus = closeVmCpusLocked();
	if (!cpus) cpus = destroyVmSharedContextLocked();
	if (cpus) {
		/* Keep shared RAM/NPT backing pinned until active RUN calls drain. */
		result.context_status = cpus;
	} else {
		int pending = amdvm_vm_memory_close(&fVmPending, 0);
		int committed = amdvm_vm_memory_close(&fVmMemory, 0);
		result.context_status = pending ? pending : committed;
	}
	if (!cpus && fVmResetRom) {
		IOFree(fVmResetRom, AMDVM_VCPU_RESET_ROM_SIZE);
		fVmResetRom = nullptr;
	}
	result.map_generation = fVmGeneration;
	IOLockUnlock(fVmLock);
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vmApiNegotiate(IOExternalMethodArguments *arguments)
{
	AMDVMObjectRetainGuard lifetime(this);
	AMDVMStructureInput input;
	AMDVMStructureOutput output;
	amdvm_vm_abi_draft_negotiate_result_t result = {};
	const amdvm_vm_abi_draft_negotiate_request_t *request;
	int status;

	if (arguments == nullptr ||
	    !input.init(arguments,
	    sizeof(amdvm_vm_abi_draft_negotiate_request_t)) ||
	    !output.init(sizeof(result))) {
		return kIOReturnBadArgument;
	}
	if ((arguments->structureOutputDescriptor != nullptr &&
	    arguments->structureOutputDescriptor->getLength() < sizeof(result)) ||
	    (arguments->structureOutputDescriptor == nullptr &&
	    (arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)))) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vm_abi_draft_negotiate_request_t *)input.bytes();
	if (IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	amdvm_vm_memory_limits_t limits = {};
	if (amdvm_vm_memory_kernel_limits(&limits) != 0) return kIOReturnUnsupported;
	IOLockLock(fVmLock);
	if (fVmClosed) { IOLockUnlock(fVmLock); return kIOReturnNotOpen; }
	status = amdvm_vm_negotiate(&fApiMode, request,
	    sizeof(*request), &result, sizeof(result));
	IOLockUnlock(fVmLock);
	if (status == AMDVM_VM_NEGOTIATE_UNSUPPORTED) {
		return kIOReturnUnsupported;
	}
	if (status == AMDVM_VM_NEGOTIATE_MODE_CONFLICT) {
		return kIOReturnNotPermitted;
	}
	if (status != AMDVM_VM_NEGOTIATE_OK) {
		return kIOReturnBadArgument;
	}
	#if defined(AMDVM_EXPERIMENTAL_SHARED_RUN)
	IOLockLock(fVmLock);
	if (fVmRunSharedMemory == nullptr) {
		auto *descriptor = IOBufferMemoryDescriptor::inTaskWithOptions(
		    kernel_task, kIOMemoryKernelUserShared | kIODirectionInOut,
		    sizeof(amdvm_vm_shared_run_area_t), 64);
		if (descriptor != nullptr &&
		    descriptor->getLength() >= sizeof(amdvm_vm_shared_run_area_t) &&
		    descriptor->getBytesNoCopy() != nullptr) {
			fVmRunSharedMemory = descriptor;
			fVmRunSharedArea = static_cast<amdvm_vm_shared_run_area_t *>(
			    descriptor->getBytesNoCopy());
			bzero(fVmRunSharedArea, sizeof(*fVmRunSharedArea));
		} else if (descriptor != nullptr) {
			descriptor->release();
		}
	}
	bool sharedRunReady = fVmRunSharedMemory != nullptr &&
	    fVmRunSharedArea != nullptr;
	IOLockUnlock(fVmLock);
	#endif
	result.max_memory_ranges = AMDVM_VCPU_MEMORY_MAX_RANGES;
	result.max_guest_bytes = limits.max_guest_bytes;
	result.max_mapped_bytes = limits.max_mapped_bytes;
	result.max_vcpus = AMDVM_VM_ABI_DRAFT_MAX_VCPUS;
	result.feature_flags = AMDVM_VM_ABI_DRAFT_FEATURE_TRANSACTIONAL_MAP |
	    AMDVM_VM_ABI_DRAFT_FEATURE_MAP_REPLACE_V1 |
	    AMDVM_VM_ABI_DRAFT_FEATURE_PER_VCPU_NPT | AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_PREPARE |
	    AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_STATE_V1 |
	    AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_MEMORY_TYPES |
	    AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_CR8_V1;
/* RUN/KICK are exposed only in the explicit experimental VM API build. */
#if defined(AMDVM_VM_API_RUN_EXPERIMENTAL)
	result.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_RUN_V1 |
	    AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_KICK_V1;
	result.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_ATOMIC_RUN_SLOT_V1;
	result.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_DIAGNOSTIC_GP_INTERCEPT;
	result.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_DIAGNOSTIC_PF_INTERCEPT;
	/* NMI injection and the IRET-step unblock protocol are implemented as part
	 * of RUN semantics; advertising RUN without this capability is an ABI lie. */
	result.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_NMI_EVENT_V1;
	result.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_MTRR_PAT_CACHE_V1;
#if defined(AMDVM_EXPERIMENTAL_NATIVE_CR3)
	result.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_CR3_V1;
#endif
#if defined(AMDVM_EXPERIMENTAL_NATIVE_RDTSC)
	result.feature_flags |= AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_RDTSC_V1;
#endif
#if defined(AMDVM_EXPERIMENTAL_PROVIDER_PAUSE)
	result.feature_flags |=
	    AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1;
#endif
#if defined(AMDVM_EXPERIMENTAL_SHARED_RUN)
	if (sharedRunReady) {
		result.feature_flags |=
		    AMDVM_VM_ABI_DRAFT_FEATURE_SHARED_VCPU_RUN_V1;
	}
#endif
#endif
	bcopy(&result, output.bytes(), sizeof(result));
	return output.copyTo(arguments);
}

IOReturn
AMDVMProbeUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
	IOMemoryDescriptor **memory)
{
	if (type != AMDVM_VM_ABI_DRAFT_SHARED_RUN_MEMORY_TYPE ||
	    options == nullptr || memory == nullptr ||
	    fVmRunSharedMemory == nullptr || fVmRunSharedArea == nullptr ||
	    amdvm_client_mode_accepts(&fApiMode,
	    AMDVM_CLIENT_MODE_VM_SCOPED) != AMDVM_CLIENT_MODE_OK) {
		return kIOReturnUnsupported;
	}
	fVmRunSharedMemory->retain();
	*memory = fVmRunSharedMemory;
	*options = kIOMapAnywhere;
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::externalMethod(uint32_t selector,
	IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
	OSObject *target, void *reference)
{
	static IOExternalMethodDispatch methods[] = {
		{ amdvm_query_action, 0, 0, 0, sizeof(amdvm_probe_result_t) },
		{ amdvm_hlt_preflight_action, 0, 0, 0,
		    sizeof(amdvm_hlt_preflight_result_t) },
		{ amdvm_hlt_run_action, 0, sizeof(amdvm_hlt_run_request_t), 0,
		    sizeof(amdvm_hlt_run_result_t) },
		{ amdvm_hlt_fpu_preflight_action, 0, 0, 0,
		    sizeof(amdvm_hlt_fpu_preflight_result_t) },
		{ amdvm_vcpu_create_action, 0,
		    sizeof(amdvm_vcpu_create_request_t), 0,
		    sizeof(amdvm_vcpu_lifecycle_result_t) },
		{ amdvm_vcpu_run_action, 0, sizeof(amdvm_vcpu_run_request_t), 0,
		    sizeof(amdvm_vcpu_run_result_t) },
		{ amdvm_vcpu_destroy_action, 0, 0, 0,
		    sizeof(amdvm_vcpu_lifecycle_result_t) },
		{ amdvm_vcpu_map_memory_action, 0,
		    sizeof(amdvm_vcpu_memory_map_request_t), 0,
		    sizeof(amdvm_vcpu_memory_map_result_t) },
		{ amdvm_vcpu_set_reset_rom_action, 0,
		    sizeof(amdvm_vcpu_set_reset_rom_request_t), 0,
		    sizeof(amdvm_vcpu_lifecycle_result_t) },
		{ amdvm_vcpu_kick_action, 0, sizeof(amdvm_vcpu_kick_request_t), 0, 0 },
		{ amdvm_vcpu_set_memory_types_action, 0,
		    sizeof(amdvm_vcpu_set_memory_types_request_t), 0,
		    sizeof(amdvm_vcpu_lifecycle_result_t) },
		{ amdvm_vcpu_diagnostic_action, 0, 0, 0,
		    sizeof(amdvm_vcpu_diagnostic_result_t) },
		{ amdvm_vcpu_control_diagnostic_action, 0, 0, 0,
		    sizeof(amdvm_vcpu_control_diagnostic_result_t) }
	};
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_NEGOTIATE) {
		return vmApiNegotiate(arguments);
	}
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_SET_MEMORY_MAP)
		return vmSetMemoryMap(arguments);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VM_DESTROY)
		return vmDestroy(arguments);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_SET_RESET_ROM)
		return vmSetResetRom(arguments);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_CREATE)
		return vmVcpuCreate(arguments);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DESTROY)
		return vmVcpuDestroy(arguments);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DIAGNOSTIC)
		return vmVcpuDiagnostic(arguments);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_STATE ||
	    selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_GET_STATE)
		return vmVcpuState(arguments, selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_STATE);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_MEMORY_TYPES)
		return vmVcpuSetMemoryTypes(arguments);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN_SHARED)
		return vmVcpuRunShared(arguments);
	if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_KICK ||
	    selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN) {
	#if defined(AMDVM_VM_API_RUN_EXPERIMENTAL)
		if (selector == AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_KICK)
			return vmVcpuKick(arguments);
		return vmVcpuRun(arguments);
#else
		return kIOReturnUnsupported;
#endif
	}
	if (selector >= (sizeof(methods) / sizeof(methods[0]))) {
		return kIOReturnUnsupported;
	}
	/* Select legacy mode on its first call; a future VM negotiation must be
	 * the first call on its connection so the two APIs cannot mix. */
	if (amdvm_client_mode_select(&fApiMode,
	    AMDVM_CLIENT_MODE_LEGACY) != AMDVM_CLIENT_MODE_OK) {
		return kIOReturnNotPermitted;
	}
	return super::externalMethod(selector, arguments, &methods[selector],
	    this, nullptr);
}

IOReturn
AMDVMProbeUserClient::clientClose(void)
{
	AMDVMObjectRetainGuard lifetime(this);
	if (closeVmMemory(true) != AMDVM_VM_MEMORY_OK) return kIOReturnBusy;
	if (fVcpuContext.quarantined != 0) {
		(void)amdvm_hlt_context_forget_quarantined(&fVcpuContext);
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
		fVcpuState = AMDVM_VCPU_STATE_EMPTY;
		fVcpuGuestProfile = 0;
		fVcpuResetRomInstalled = 0;
		terminate();
		return kIOReturnSuccess;
	}
	if (fVcpuContext.initialized != 0 ||
	    fVcpuContext.backing.memory_descriptor != nullptr ||
	    fVcpuContext.npt_backing.memory_descriptor != nullptr ||
	    fVcpuContext.hsave.memory_descriptor != nullptr) {
		if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
			return kIOReturnBusy;
		}
		if (__sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) != 0 ||
		    fVcpuState == AMDVM_VCPU_STATE_RUNNING ||
		    fVcpuState == AMDVM_VCPU_STATE_DESTROYING) {
			__sync_lock_release(&fVcpuBusy);
			return kIOReturnBusy;
		}
		uint32_t priorState = fVcpuState;
		fVcpuState = AMDVM_VCPU_STATE_DESTROYING;
		int destroyStatus = amdvm_hlt_context_destroy(&fVcpuContext);
		if (destroyStatus != AMDVM_HLT_CONTEXT_OK &&
		    fVcpuContext.quarantined == 0) {
			fVcpuState = fVcpuContext.lease.active != 0 ?
			    AMDVM_VCPU_STATE_LEASED : priorState;
			__sync_lock_release(&fVcpuBusy);
			return kIOReturnBusy;
		}
		fVcpuState = AMDVM_VCPU_STATE_EMPTY;
		fVcpuExpectedRip = 0;
		fVcpuGuestProfile = 0;
		fVcpuResetRomInstalled = 0;
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
		__sync_lock_release(&fVcpuBusy);
	}
	terminate();
	return kIOReturnSuccess;
}

void
AMDVMProbeUserClient::free(void)
{
	if (closeVmMemory(true) != AMDVM_VM_MEMORY_OK) {
		/* Retained owner remains counted, so kext unload is prohibited. */
		IOLog("AMDVM VM RAM owner retained after unexpected close failure\n");
	}
	if (fVcpuContext.quarantined != 0) {
		(void)amdvm_hlt_context_forget_quarantined(&fVcpuContext);
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
	}
	if (fVcpuContext.initialized != 0 ||
	    fVcpuContext.backing.memory_descriptor != nullptr ||
	    fVcpuContext.npt_backing.memory_descriptor != nullptr ||
	    fVcpuContext.hsave.memory_descriptor != nullptr) {
		if (__sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) == 0 &&
		    __sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
			uint32_t priorState = fVcpuState;
			fVcpuState = AMDVM_VCPU_STATE_DESTROYING;
			if (amdvm_hlt_context_destroy(&fVcpuContext) ==
			    AMDVM_HLT_CONTEXT_OK) {
				fVcpuState = AMDVM_VCPU_STATE_EMPTY;
				fVcpuGuestProfile = 0;
				fVcpuResetRomInstalled = 0;
				if (fVcpuContextTracked != 0) {
					fVcpuContextTracked = 0;
					__sync_fetch_and_sub(&gVcpuContextCount, 1u);
				}
				if (fVcpuLeaseTracked != 0) {
					fVcpuLeaseTracked = 0;
					__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
				}
			} else if (fVcpuContext.quarantined != 0) {
				(void)amdvm_hlt_context_forget_quarantined(&fVcpuContext);
				fVcpuState = AMDVM_VCPU_STATE_EMPTY;
				if (fVcpuContextTracked != 0) {
					fVcpuContextTracked = 0;
					__sync_fetch_and_sub(&gVcpuContextCount, 1u);
				}
				if (fVcpuLeaseTracked != 0) {
					fVcpuLeaseTracked = 0;
					__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
				}
			} else {
				fVcpuState = fVcpuContext.lease.active != 0 ?
				    AMDVM_VCPU_STATE_LEASED : priorState;
			}
			__sync_lock_release(&fVcpuBusy);
		}
	}
	if (fVmRunSharedMemory != nullptr) {
		fVmRunSharedMemory->release();
		fVmRunSharedMemory = nullptr;
		fVmRunSharedArea = nullptr;
	}
	if (fVmLock != nullptr) { IOLockFree(fVmLock); fVmLock = nullptr; }
	super::free();
}

bool
AMDVMProbeService::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	registerService();
	return true;
}

IOReturn
AMDVMProbeService::newUserClient(task_t owningTask, void *securityID,
	UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
	AMDVMProbeUserClient *client;
	if (handler == nullptr || type != 0) {
		return kIOReturnBadArgument;
	}
	*handler = nullptr;
	client = OSTypeAlloc(AMDVMProbeUserClient);
	if (client == nullptr) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type, properties) ||
	    !client->attach(this) || !client->start(this)) {
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}
