#ifndef __ALLOCATOR_H
#define __ALLOCATOR_H

#include <stddef.h>
#include <stdlib.h>
#include <string>
#include <memory>
#include <stdio.h>

#ifdef ENABLE_MEMORY_POOL
#include "thread_cache.h"
#include "memory_utils.h"
#include "big_memory_allocator.h"
#endif

namespace allocator
{
    void *kv_malloc(size_t size);

    void kv_free(void *ptr);

    template <typename T>
    struct MyAllocator
    {
        using value_type = T;

        MyAllocator() = default;

        template <typename U>
        MyAllocator(const MyAllocator<U> &)
        {
        }

        T *allocate(std::size_t n)
        {
            size_t size = n * sizeof(T);
#ifdef ENABLE_MEMORY_POOL
            if (size + memory::ALIGNMENT <= memory::MAX_BYTES)
            {
                return (T *)memory::ThreadCache::instance().alloc(size);
            }
            return (T *)memory::BigMemoryAllocator::instance().alloc(size);
#else
            return static_cast<T *>(::malloc(size));
#endif
        }

        void deallocate(T *p, std::size_t n)
        {
            if (!p)
                return;
#ifdef ENABLE_MEMORY_POOL
            char *ptr = reinterpret_cast<char *>(p);
            size_t size = *(size_t *)(ptr - memory::ALIGNMENT);
            if (size <= memory::MAX_BYTES)
            {
                memory::ThreadCache::instance().free(p);
                return;
            }
            memory::BigMemoryAllocator::instance().free(p);
#else
            ::free(p);
#endif
        }
    };

    template <typename T, typename U>
    bool operator==(const MyAllocator<T> &, const MyAllocator<U> &) noexcept
    {
        return true;
    }

    template <typename T, typename U>
    bool operator!=(const MyAllocator<T> &, const MyAllocator<U> &) noexcept
    {
        return false;
    }
} // namespace allocator

#endif // __ALLOCATOR_H