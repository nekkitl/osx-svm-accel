#include <cpuid.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XSTATE_PAGE_SIZE 4096u
#define BATCHES 9u
#define ITERATIONS 500000u

static uint64_t
read_xcr0(void)
{
	uint32_t low, high;
	__asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
	return ((uint64_t)high << 32) | low;
}

static uint64_t
measure_xsave(void *area, uint64_t xcr0, int dirty_sse)
{
	uint32_t low = (uint32_t)xcr0;
	uint32_t high = (uint32_t)(xcr0 >> 32);
	uint64_t start = mach_absolute_time();
	for (uint32_t i = 0; i < ITERATIONS; ++i) {
		if (dirty_sse) {
			__asm__ volatile("pxor %%xmm0, %%xmm0" : : : "xmm0");
		}
		__asm__ volatile("xsave64 (%0)" : : "r"(area), "a"(low),
		    "d"(high) : "memory");
	}
	return mach_absolute_time() - start;
}

static void
sort_u64(uint64_t *values, size_t count)
{
	for (size_t i = 1; i < count; ++i) {
		uint64_t value = values[i];
		size_t j = i;
		while (j > 0 && values[j - 1] > value) {
			values[j] = values[j - 1];
			--j;
		}
		values[j] = value;
	}
}

static int
measure_mode(void *area, uint64_t xcr0, int dirty_sse,
	uint64_t *median_ns_out)
{
	uint64_t samples[BATCHES];
	mach_timebase_info_data_t timebase;
	if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
		return -1;
	}
	for (size_t i = 0; i < BATCHES; ++i) {
		uint64_t ticks = measure_xsave(area, xcr0, dirty_sse);
		__uint128_t elapsed_ns = (__uint128_t)ticks * timebase.numer /
		    timebase.denom;
		samples[i] = (uint64_t)(elapsed_ns / ITERATIONS);
	}
	sort_u64(samples, BATCHES);
	*median_ns_out = samples[BATCHES / 2];
	return 0;
}

int
main(void)
{
	unsigned int a, b, c, d;
	uint64_t xcr0, clean_ns, dirty_ns;
	void *area = NULL;
	if (!__get_cpuid(1, &a, &b, &c, &d) ||
	    (c & bit_XSAVE) == 0 || (c & bit_OSXSAVE) == 0) {
		fprintf(stderr, "XSAVE/OSXSAVE unavailable\n");
		return 2;
	}
	if (!__get_cpuid_count(0x0d, 0, &a, &b, &c, &d) ||
	    b < 576 || b > XSTATE_PAGE_SIZE) {
		fprintf(stderr, "current XSAVE image does not fit provider's 4 KiB page\n");
		return 2;
	}
	xcr0 = read_xcr0();
	if ((xcr0 & 1u) == 0 || posix_memalign(&area, XSTATE_PAGE_SIZE,
	    XSTATE_PAGE_SIZE) != 0) {
		fprintf(stderr, "could not allocate aligned XSAVE page\n");
		return 2;
	}
	memset(area, 0, XSTATE_PAGE_SIZE);
	if (measure_mode(area, xcr0, 0, &clean_ns) != 0 ||
	    measure_mode(area, xcr0, 1, &dirty_ns) != 0) {
		free(area);
		fprintf(stderr, "mach_absolute_time conversion failed\n");
		return 2;
	}
	printf("host-xsave-microbench iterations=%u batches=%u xcr0=0x%llx area=%u\n",
	    ITERATIONS, BATCHES, (unsigned long long)xcr0, XSTATE_PAGE_SIZE);
	printf("median_ns_per_xsave clean_state=%llu dirty_sse=%llu\n",
	    (unsigned long long)clean_ns, (unsigned long long)dirty_ns);
	printf("scope=user-space instruction cost only; excludes IOConnect, locks, interrupts, VMRUN, and provider validation\n");
	free(area);
	return 0;
}
