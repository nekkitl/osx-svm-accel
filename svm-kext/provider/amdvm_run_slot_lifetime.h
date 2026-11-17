#ifndef AMDVM_RUN_SLOT_LIFETIME_H
#define AMDVM_RUN_SLOT_LIFETIME_H

#include <stdint.h>

#define AMDVM_RUN_SLOT_CLOSING (UINT64_C(1) << 63)
#define AMDVM_RUN_SLOT_REFS_MASK (AMDVM_RUN_SLOT_CLOSING - 1u)

static inline uint64_t
amdvm_run_slot_state_load(const volatile uint64_t *state)
{
	return state == 0 ? AMDVM_RUN_SLOT_CLOSING :
	    __sync_val_compare_and_swap((volatile uint64_t *)state, 0u, 0u);
}

/* Admit one exclusive slot operation unless closing or already in use. */
static inline int
amdvm_run_slot_try_acquire(volatile uint64_t *state)
{
	uint64_t current;
	if (state == 0) return -1;
	for (;;) {
		current = amdvm_run_slot_state_load(state);
		if ((current & AMDVM_RUN_SLOT_CLOSING) != 0) return 0;
		if ((current & AMDVM_RUN_SLOT_REFS_MASK) != 0) return 0;
		if (__sync_bool_compare_and_swap(state, current, current + 1u))
			return 1;
	}
}

/* Close admission without losing a concurrent refcount update. */
static inline uint64_t
amdvm_run_slot_begin_destroy(volatile uint64_t *state)
{
	uint64_t current;
	if (state == 0) return AMDVM_RUN_SLOT_CLOSING;
	for (;;) {
		current = amdvm_run_slot_state_load(state);
		if (current & AMDVM_RUN_SLOT_CLOSING) return current;
		if (__sync_bool_compare_and_swap(state, current,
		    current | AMDVM_RUN_SLOT_CLOSING))
			return current | AMDVM_RUN_SLOT_CLOSING;
	}
}

/* Returns 1 only for the last release after destroy has begun. */
static inline int
amdvm_run_slot_release(volatile uint64_t *state)
{
	uint64_t current, next;
	if (state == 0) return -1;
	for (;;) {
		current = amdvm_run_slot_state_load(state);
		if ((current & AMDVM_RUN_SLOT_REFS_MASK) == 0) return -1;
		next = current - 1u;
		if (__sync_bool_compare_and_swap(state, current, next))
			return (next == AMDVM_RUN_SLOT_CLOSING) ? 1 : 0;
	}
}

static inline uint64_t
amdvm_run_slot_refcount(volatile uint64_t *state)
{
	return amdvm_run_slot_state_load(state) & AMDVM_RUN_SLOT_REFS_MASK;
}

static inline int
amdvm_run_slot_is_closing(volatile uint64_t *state)
{
	return (amdvm_run_slot_state_load(state) & AMDVM_RUN_SLOT_CLOSING) != 0;
}

static inline int
amdvm_run_slot_quiescent(const volatile uint64_t *state,
	const volatile uint32_t *buffer_busy)
{
	return state != 0 && buffer_busy != 0 &&
	    (amdvm_run_slot_state_load(state) & AMDVM_RUN_SLOT_REFS_MASK) == 0u &&
	    __sync_val_compare_and_swap((volatile uint32_t *)buffer_busy, 0u, 0u) == 0u;
}

#endif
