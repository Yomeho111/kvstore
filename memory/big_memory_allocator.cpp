#include "big_memory_allocator.h"
#include <stdlib.h>

namespace memory
{
    BigMemoryAllocator &BigMemoryAllocator::instance()
    {
        static BigMemoryAllocator alloc;
        return alloc;
    }

    void *BigMemoryAllocator::alloc(size_t size)
    {
        size_t total_size = size + ALIGNMENT;
        char *ptr = (char *)malloc(total_size);
        *(size_t *)ptr = total_size;
        return (void *)(ptr + ALIGNMENT);
    }

    int BigMemoryAllocator::free(void *ptr)
    {
        char *ptr_start = (char *)ptr - ALIGNMENT;
        ::free(ptr_start);
        return 0;
    }
} // namespace memory