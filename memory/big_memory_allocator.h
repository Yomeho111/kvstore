#ifndef __BIG_MEMORY_ALLOCATOR
#define __BIG_MEMORY_ALLOCATOR

#include <stddef.h>

#include "memory_utils.h"

namespace memory
{
    class BigMemoryAllocator
    {
    public:
        static BigMemoryAllocator &instance();

        void *alloc(size_t size);

        int free(void *ptr);

    private:
        BigMemoryAllocator() = default;
        ~BigMemoryAllocator() = default;

        BigMemoryAllocator(const BigMemoryAllocator &) = delete;
        BigMemoryAllocator(BigMemoryAllocator &&) = delete;

        BigMemoryAllocator &operator=(const BigMemoryAllocator &) = delete;
        BigMemoryAllocator &operator=(BigMemoryAllocator &&) = delete;
    };
}

#endif // __BIG_MEMORY_ALLOCATOR