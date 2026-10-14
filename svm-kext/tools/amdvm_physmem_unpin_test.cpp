#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task_ref.h>
#include "../provider/amdvm_physmem.h"

static unsigned int gTaskReferences;
static unsigned int gTaskReleases;
static unsigned int gDescriptorReleases;
static unsigned int gScatterFailureMode;
static unsigned int gScatterAllocations;
int gIOMallocFailAfter = -1;

class FakeBufferDescriptor : public IOBufferMemoryDescriptor {
public:
	void *bytes = nullptr;
	size_t length = 0;
	explicit FakeBufferDescriptor(size_t requested) : length(requested)
	{
		assert(posix_memalign(&bytes, 4096, requested) == 0);
		memset(bytes, 0xa5, requested);
		++gScatterAllocations;
	}
	void *getBytesNoCopy() override { return bytes; }
	IOByteCount getLength() override { return length; }
	addr64_t getPhysicalSegment(IOByteCount offset, IOByteCount *segment_length,
		uint32_t) override
	{
		if (segment_length) *segment_length =
		    gScatterFailureMode == 2u && offset == 4096u ? 2048u : 4096u;
		if (gScatterFailureMode == 1u && offset == 4096u) return 0;
		return UINT64_C(0x100000) + (offset / 4096u) * 8192u;
	}
	void release() override
	{
		for (size_t i = 0; i < length; ++i)
			assert(static_cast<unsigned char *>(bytes)[i] == 0);
		free(bytes);
		bytes = nullptr;
		++gDescriptorReleases;
		delete this;
	}
};

task_t kernel_task = reinterpret_cast<task_t>(0x1);

void task_reference(task_t task)
{
	assert(task != nullptr);
	++gTaskReferences;
}

void task_deallocate(task_t task)
{
	assert(task != nullptr);
	++gTaskReleases;
}

class FaultInjectDescriptor : public IOMemoryDescriptor {
public:
	unsigned int complete_calls = 0;
	IOReturn complete(IODirection) override
	{
		++complete_calls;
		return complete_calls == 1 ? -1 : kIOReturnSuccess;
	}
	void release() override
	{
		++gDescriptorReleases;
	}
};

IOReturn IOMemoryDescriptor::complete(IODirection) { return kIOReturnSuccess; }
IOReturn IOMemoryDescriptor::prepare(IODirection) { return kIOReturnSuccess; }
addr64_t IOMemoryDescriptor::getPhysicalSegment(IOByteCount,
	IOByteCount *length, uint32_t)
{
	if (length) *length = 4096;
	return 0x1000;
}
IOByteCount IOMemoryDescriptor::getLength() { return 4096; }
void IOMemoryDescriptor::release() { ++gDescriptorReleases; }
IOMemoryDescriptor *IOMemoryDescriptor::withAddressRange(uint64_t, size_t,
	IODirection, void *)
{
	return nullptr;
}
IOBufferMemoryDescriptor *IOBufferMemoryDescriptor::inTaskWithOptions(void *,
	uint32_t, size_t size, size_t)
{
	return new FakeBufferDescriptor(size);
}
void *IOBufferMemoryDescriptor::getBytesNoCopy() { return nullptr; }

int main()
{
	FaultInjectDescriptor descriptor;
	int task_token;
	amdvm_pinned_user_region_t region = {
		&descriptor, &task_token, 0x4000, 0x2000
	};

	assert(amdvm_physmem_unpin_user_region(&region) == -1);
	assert(descriptor.complete_calls == 1);
	assert(region.memory_descriptor == &descriptor);
	assert(region.owning_task == &task_token);
	assert(region.user_address == 0x4000);
	assert(region.size == 0x2000);
	assert(gDescriptorReleases == 0);
	assert(gTaskReleases == 0);

	assert(amdvm_physmem_unpin_user_region(&region) == 0);
	assert(descriptor.complete_calls == 2);
	assert(region.memory_descriptor == nullptr);
	assert(region.owning_task == nullptr);
	assert(region.user_address == 0);
	assert(region.size == 0);
	assert(gDescriptorReleases == 1);
	assert(gTaskReleases == 1);

	amdvm_physmem_region_t scatter = {};
	gScatterFailureMode = 0;
	assert(amdvm_physmem_alloc_scatter(&scatter, 3u * 4096u, 40u) == 0);
	assert(scatter.scatter == 1u && scatter.page_count == 3u);
	assert(scatter.page_pas[0] == UINT64_C(0x100000));
	assert(scatter.page_pas[1] == UINT64_C(0x102000));
	assert(scatter.page_pas[2] == UINT64_C(0x104000));
	assert(amdvm_physmem_validate(&scatter) == 0);
	assert(amdvm_physmem_page_pa(&scatter, 2u) == UINT64_C(0x104000));
	uint64_t resolved_pa = 0;
	assert(amdvm_physmem_resolve(&scatter,
	    static_cast<unsigned char *>(scatter.virtual_address) + 4096u + 128u,
	    64u, &resolved_pa) == 0);
	assert(resolved_pa == UINT64_C(0x102080));
	assert(amdvm_physmem_resolve(&scatter,
	    static_cast<unsigned char *>(scatter.virtual_address) + 8191u,
	    2u, &resolved_pa) == -1);
	amdvm_physmem_free(&scatter);
	assert(scatter.memory_descriptor == nullptr && scatter.page_pas == nullptr);

	unsigned int release_count = gDescriptorReleases;
	gScatterFailureMode = 1u;
	assert(amdvm_physmem_alloc_scatter(&scatter, 3u * 4096u, 40u) == -1);
	assert(scatter.memory_descriptor == nullptr && scatter.page_pas == nullptr);
	assert(gDescriptorReleases == release_count + 1u);
	gScatterFailureMode = 2u;
	assert(amdvm_physmem_alloc_scatter(&scatter, 3u * 4096u, 40u) == -1);
	assert(scatter.memory_descriptor == nullptr && scatter.page_indexes_by_pa == nullptr);
	assert(gDescriptorReleases == release_count + 2u);
	gScatterFailureMode = 0;
	gIOMallocFailAfter = 0;
	assert(amdvm_physmem_alloc_scatter(&scatter, 3u * 4096u, 40u) == -1);
	assert(scatter.memory_descriptor == nullptr && scatter.page_pas == nullptr);
	assert(gDescriptorReleases == release_count + 3u);
	gIOMallocFailAfter = 1;
	assert(amdvm_physmem_alloc_scatter(&scatter, 3u * 4096u, 40u) == -1);
	assert(scatter.memory_descriptor == nullptr && scatter.page_indexes_by_pa == nullptr);
	assert(gDescriptorReleases == release_count + 4u);
	puts("amdvm_physmem_unpin_test: retryable unpin and scatter allocation, translation, scrub, segment-failure, and partial-metadata-allocation cleanup PASS");
	return 0;
}
