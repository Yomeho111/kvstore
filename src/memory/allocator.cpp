#include "allocator.h"

namespace allocator
{
    MyAllocator<char> alloc;

    void *kv_malloc(size_t size)
    {
        return (void *)alloc.allocate(size);
    }

    void kv_free(void *ptr)
    {
        alloc.deallocate((char *)ptr, 0);
    }
} // namespace allocator