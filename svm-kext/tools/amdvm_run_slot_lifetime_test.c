#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../provider/amdvm_run_slot_lifetime.h"

typedef struct Race {
	volatile uint64_t state;
	volatile uint32_t buffer_busy;
	volatile uint32_t stop;
} Race;

static void *
acquirer(void *opaque)
{
	Race *race = (Race *)opaque;
	while (!__sync_val_compare_and_swap(&race->stop, 0u, 0u)) {
		int rc = amdvm_run_slot_try_acquire(&race->state);
		if (rc == 1) (void)amdvm_run_slot_release(&race->state);
	}
	return 0;
}

int
main(void)
{
	volatile uint64_t state = 0;
	volatile uint32_t busy = 0;
	assert(amdvm_run_slot_try_acquire(&state) == 1);
	assert(amdvm_run_slot_try_acquire(&state) == 0);
	assert(amdvm_run_slot_refcount(&state) == 1);
	assert(amdvm_run_slot_quiescent(&state, &busy) == 0);
	assert(amdvm_run_slot_begin_destroy(&state) == AMDVM_RUN_SLOT_CLOSING + 1u);
	assert(amdvm_run_slot_try_acquire(&state) == 0);
	assert(amdvm_run_slot_release(&state) == 1);
	assert(amdvm_run_slot_quiescent(&state, &busy) == 1);

	/* A newly created slot starts a fresh lifetime and is runnable again. */
	state = 0;
	assert(amdvm_run_slot_try_acquire(&state) == 1);
	assert(amdvm_run_slot_release(&state) == 0);
	assert(amdvm_run_slot_begin_destroy(&state) == AMDVM_RUN_SLOT_CLOSING);

	/* A reserved RUN buffer also pins storage after the RUN ref is released. */
	state = 0;
	busy = 1;
	assert(amdvm_run_slot_begin_destroy(&state) == AMDVM_RUN_SLOT_CLOSING);
	assert(amdvm_run_slot_quiescent(&state, &busy) == 0);
	busy = 0;
	assert(amdvm_run_slot_quiescent(&state, &busy) == 1);

	Race race;
	memset(&race, 0, sizeof(race));
	pthread_t thread;
	assert(pthread_create(&thread, 0, acquirer, &race) == 0);
	for (unsigned i = 0; i < 10000; ++i) {
		int rc = amdvm_run_slot_try_acquire(&race.state);
		if (rc == 1) (void)amdvm_run_slot_release(&race.state);
	}
	(void)amdvm_run_slot_begin_destroy(&race.state);
	for (unsigned i = 0; i < 10000; ++i)
		assert(amdvm_run_slot_try_acquire(&race.state) == 0);
	__sync_lock_test_and_set(&race.stop, 1u);
	assert(pthread_join(thread, 0) == 0);
	assert(amdvm_run_slot_refcount(&race.state) == 0);
	assert(amdvm_run_slot_quiescent(&race.state, &race.buffer_busy) == 1);
	puts("amdvm_run_slot_lifetime_test: PASS");
	return 0;
}
