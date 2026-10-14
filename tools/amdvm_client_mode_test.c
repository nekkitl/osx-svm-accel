#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "../svm-kext/provider/amdvm_client_mode.h"

typedef struct race_state {
	volatile uint32_t mode;
	volatile uint32_t start;
	int result[2];
} race_state_t;

typedef struct worker_arg {
	race_state_t *state;
	uint32_t requested_mode;
	unsigned int index;
} worker_arg_t;

static void *
select_mode_worker(void *opaque)
{
	worker_arg_t *arg = (worker_arg_t *)opaque;
	while (__sync_val_compare_and_swap(&arg->state->start, 0u, 0u) == 0u) {
	}
	arg->state->result[arg->index] = amdvm_client_mode_select(
	    &arg->state->mode, arg->requested_mode);
	return 0;
}

int
main(void)
{
	for (unsigned int iteration = 0; iteration < 1000; ++iteration) {
		race_state_t state = { 0 };
		worker_arg_t args[2] = {
			{ &state, AMDVM_CLIENT_MODE_LEGACY, 0 },
			{ &state, AMDVM_CLIENT_MODE_VM_SCOPED, 1 }
		};
		pthread_t workers[2];
		assert(pthread_create(&workers[0], 0, select_mode_worker,
		    &args[0]) == 0);
		assert(pthread_create(&workers[1], 0, select_mode_worker,
		    &args[1]) == 0);
		__sync_lock_test_and_set(&state.start, 1u);
		assert(pthread_join(workers[0], 0) == 0);
		assert(pthread_join(workers[1], 0) == 0);
		assert((state.result[0] == AMDVM_CLIENT_MODE_OK &&
		    state.result[1] == AMDVM_CLIENT_MODE_CONFLICT) ||
		    (state.result[1] == AMDVM_CLIENT_MODE_OK &&
		    state.result[0] == AMDVM_CLIENT_MODE_CONFLICT));
		assert(amdvm_client_mode_accepts(&state.mode, state.mode) ==
		    AMDVM_CLIENT_MODE_OK);
		assert(amdvm_client_mode_accepts(&state.mode,
		    state.mode == AMDVM_CLIENT_MODE_LEGACY ?
		    AMDVM_CLIENT_MODE_VM_SCOPED : AMDVM_CLIENT_MODE_LEGACY) ==
		    AMDVM_CLIENT_MODE_CONFLICT);
	}

	volatile uint32_t mode = AMDVM_CLIENT_MODE_UNSELECTED;
	assert(amdvm_client_mode_accepts(&mode, AMDVM_CLIENT_MODE_LEGACY) ==
	    AMDVM_CLIENT_MODE_CONFLICT);
	assert(amdvm_client_mode_select(&mode, AMDVM_CLIENT_MODE_LEGACY) ==
	    AMDVM_CLIENT_MODE_OK);
	assert(amdvm_client_mode_select(&mode, AMDVM_CLIENT_MODE_LEGACY) ==
	    AMDVM_CLIENT_MODE_OK);
	assert(amdvm_client_mode_select(&mode, 99u) ==
	    AMDVM_CLIENT_MODE_INVALID);
	assert(amdvm_client_mode_select(0, AMDVM_CLIENT_MODE_LEGACY) ==
	    AMDVM_CLIENT_MODE_INVALID);
	puts("amdvm per-client API mode gate: PASS (1000 concurrent races)");
	return 0;
}
