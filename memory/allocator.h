#ifndef __ALLOCATOR_H
#define __ALLOCATOR_H

#include <stddef.h>
#include <stdlib.h>
#include <string>
#include <memory>

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
        MyAllocator(const MyAllocator<U> &) {}

        T *allocate(std::size_t n)
        {
            return static_cast<T *>(malloc(n * sizeof(T)));
        }

        void deallocate(T *p, std::size_t n)
        {
            free(p);
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
}

#endif // __ALLOCATOR_H