#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "../svm-kext/provider/amdvm_vm_lifecycle.h"

static void
test_atomic_map_and_independent_vcpus(void)
{
	amdvm_vm_lifecycle_t vm;
	uint32_t vcpu0, vcpu1;

	assert(amdvm_vm_lifecycle_open(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_create(&vm, 0, &vcpu0) ==
	    AMDVM_VM_LIFECYCLE_BAD_STATE);
	assert(amdvm_vm_lifecycle_map_begin(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_create(&vm, 0, &vcpu0) ==
	    AMDVM_VM_LIFECYCLE_BAD_STATE);
	assert(amdvm_vm_lifecycle_map_abort(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(vm.map_committed == 0 && vm.resources_owned == 0 && vm.vcpu_count == 0);
	assert(amdvm_vm_lifecycle_map_begin(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_map_commit(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(vm.map_generation == 1 && vm.resources_owned == 1);
	assert(amdvm_vm_lifecycle_vcpu_create(&vm, 0, &vcpu0) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_create(&vm, 1, &vcpu1) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(vcpu0 != vcpu1 && vm.vcpu_count == 2);
	assert(amdvm_vm_lifecycle_vcpu_create(&vm, 1, &vcpu1) ==
	    AMDVM_VM_LIFECYCLE_EXISTS);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu0, 2, false) ==
	    AMDVM_VM_LIFECYCLE_STALE_MAP);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu0, 1, false) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu0, 1, false) ==
	    AMDVM_VM_LIFECYCLE_BUSY);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu1, 1, false) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(vm.active_runs == 2);
	assert(amdvm_vm_lifecycle_run_end(&vm, vcpu0, true) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu0, 1, false) ==
	    AMDVM_VM_LIFECYCLE_BUSY);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu0, 1, true) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_run_end(&vm, vcpu0, false) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(vm.vcpus[vcpu0].state == AMDVM_VM_VCPU_STOPPED);
	assert(vm.vcpus[vcpu1].state == AMDVM_VM_VCPU_RUNNING);
	assert(amdvm_vm_lifecycle_close(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu0, 1, false) ==
	    AMDVM_VM_LIFECYCLE_BAD_STATE);
	assert(amdvm_vm_lifecycle_vcpu_destroy(&vm, vcpu1) ==
	    AMDVM_VM_LIFECYCLE_BUSY);
	assert(amdvm_vm_lifecycle_destroy(&vm) == AMDVM_VM_LIFECYCLE_BUSY);
	assert(amdvm_vm_lifecycle_run_end(&vm, vcpu1, false) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_destroy(&vm, vcpu0) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_destroy(&vm, vcpu1) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_destroy(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_can_free(&vm));
}

static void
test_quarantine_retains_resources(void)
{
	amdvm_vm_lifecycle_t vm;
	uint32_t vcpu;

	assert(amdvm_vm_lifecycle_open(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_map_begin(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_map_commit(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_create(&vm, 0, &vcpu) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu, 1, false) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_close(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_quarantine(&vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(vm.resources_owned == 1 && vm.map_committed == 1);
	assert(amdvm_vm_lifecycle_run_begin(&vm, vcpu, 1, false) ==
	    AMDVM_VM_LIFECYCLE_BAD_STATE);
	assert(amdvm_vm_lifecycle_finish_quarantine(&vm, true) ==
	    AMDVM_VM_LIFECYCLE_BUSY);
	assert(vm.resources_owned == 1 && vm.map_committed == 1);
	assert(amdvm_vm_lifecycle_run_end(&vm, vcpu, false) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_destroy(&vm, vcpu) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_finish_quarantine(&vm, false) ==
	    AMDVM_VM_LIFECYCLE_BUSY);
	assert(vm.resources_owned == 1 && vm.map_committed == 1);
	assert(amdvm_vm_lifecycle_finish_quarantine(&vm, true) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_can_free(&vm));
}

typedef struct concurrent_lifecycle_test {
	amdvm_vm_lifecycle_t vm;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	uint32_t slots[2];
	uint32_t entered;
	uint32_t closing;
} concurrent_lifecycle_test_t;

typedef struct concurrent_vcpu_arg {
	concurrent_lifecycle_test_t *test;
	uint32_t slot;
} concurrent_vcpu_arg_t;

static void *
concurrent_vcpu_run(void *opaque)
{
	concurrent_vcpu_arg_t *arg = (concurrent_vcpu_arg_t *)opaque;
	concurrent_lifecycle_test_t *test = arg->test;
	assert(pthread_mutex_lock(&test->lock) == 0);
	assert(amdvm_vm_lifecycle_run_begin(&test->vm, arg->slot, 1, false) ==
	    AMDVM_VM_LIFECYCLE_OK);
	test->entered++;
	assert(pthread_cond_broadcast(&test->changed) == 0);
	while (test->closing == 0) {
		assert(pthread_cond_wait(&test->changed, &test->lock) == 0);
	}
	assert(amdvm_vm_lifecycle_run_end(&test->vm, arg->slot, false) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(pthread_cond_broadcast(&test->changed) == 0);
	assert(pthread_mutex_unlock(&test->lock) == 0);
	return NULL;
}

static void
test_concurrent_runs_drain_before_destroy(void)
{
	concurrent_lifecycle_test_t test = {};
	concurrent_vcpu_arg_t args[2];
	pthread_t threads[2];

	assert(pthread_mutex_init(&test.lock, NULL) == 0);
	assert(pthread_cond_init(&test.changed, NULL) == 0);
	assert(amdvm_vm_lifecycle_open(&test.vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_map_begin(&test.vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_map_commit(&test.vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_create(&test.vm, 0, &test.slots[0]) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_create(&test.vm, 1, &test.slots[1]) ==
	    AMDVM_VM_LIFECYCLE_OK);
	for (uint32_t i = 0; i < 2; ++i) {
		args[i].test = &test;
		args[i].slot = test.slots[i];
		assert(pthread_create(&threads[i], NULL, concurrent_vcpu_run,
		    &args[i]) == 0);
	}
	assert(pthread_mutex_lock(&test.lock) == 0);
	while (test.entered != 2) {
		assert(pthread_cond_wait(&test.changed, &test.lock) == 0);
	}
	assert(test.vm.active_runs == 2);
	assert(amdvm_vm_lifecycle_close(&test.vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_run_begin(&test.vm, test.slots[0], 1, false) ==
	    AMDVM_VM_LIFECYCLE_BAD_STATE);
	test.closing = 1;
	assert(pthread_cond_broadcast(&test.changed) == 0);
	assert(pthread_mutex_unlock(&test.lock) == 0);
	for (uint32_t i = 0; i < 2; ++i) {
		assert(pthread_join(threads[i], NULL) == 0);
	}
	assert(pthread_mutex_lock(&test.lock) == 0);
	assert(test.vm.active_runs == 0);
	assert(amdvm_vm_lifecycle_vcpu_destroy(&test.vm, test.slots[0]) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_vcpu_destroy(&test.vm, test.slots[1]) ==
	    AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_destroy(&test.vm) == AMDVM_VM_LIFECYCLE_OK);
	assert(amdvm_vm_lifecycle_can_free(&test.vm));
	assert(pthread_mutex_unlock(&test.lock) == 0);
	assert(pthread_cond_destroy(&test.changed) == 0);
	assert(pthread_mutex_destroy(&test.lock) == 0);
}

int
main(void)
{
	test_atomic_map_and_independent_vcpus();
	test_quarantine_retains_resources();
	test_concurrent_runs_drain_before_destroy();
	puts("amdvm VM/vCPU lifecycle model: PASS (parallel RUN, close/drain, quarantine)");
	return 0;
}
