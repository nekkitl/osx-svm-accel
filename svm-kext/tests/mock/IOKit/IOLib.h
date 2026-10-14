#ifndef AMDVM_TEST_IOLIB_H
#define AMDVM_TEST_IOLIB_H

#include "IOMemoryDescriptor.h"
#include <stdlib.h>

extern int gIOMallocFailAfter;
static inline void *IOMalloc(size_t size)
{
	if (gIOMallocFailAfter == 0) {
		gIOMallocFailAfter = -1;
		return NULL;
	}
	if (gIOMallocFailAfter > 0) --gIOMallocFailAfter;
	return malloc(size);
}
static inline void IOFree(void *pointer, size_t size)
{
	(void)size;
	free(pointer);
}

#endif
