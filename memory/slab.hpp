#ifndef __SLAB_H
#define __SLAB_H

#include <stddef.h>
#include <type_traits>
#include <vector>
#include <mutex>
#include <stdlib.h>
#include <string.h>

#include "memory_utils.h"

namespace memory
{

#if __cplusplus >= 202002L
    template <size_t N>
    concept ExpoOfTwo = (N != 0) && ((N & (N - 1)) == 0) && N <= PAGE_SIZE && N >= 8;
#endif

#if __cplusplus >= 202002L
    template <size_t N>
        requires ExpoOfTwo<N>
#else
    template <size_t N, typename = std::enable_if_t<(N != 0) && ((N & (N - 1)) == 0) && N <= PAGE_SIZE && N >= 8>>
#endif
    class SlabN
    {
    public:
        static SlabN &instance()
        {
            static SlabN slab_;
            return slab_;
        }

        void *malloc()
        {
            void *ptr = nullptr;
            std::lock_guard lk{lock_};
            if (next_free_)
            {
                ptr = (void *)next_free_;
                next_free_ = *(char **)next_free_;
                return ptr;
            }

            char *page = alloc_page();
            if (!page)
                return nullptr;

            ptr = (void *)page;
            next_free_ = *(char **)page;
            return ptr;
        }

        void free(void *ptr)
        {
            if (!ptr)
                return;
            std::lock_guard lk{lock_};
            *(char **)ptr = next_free_;
            next_free_ = (char *)ptr;
        }

    private:
        SlabN() : next_free_(nullptr) {}
        ~SlabN()
        {
            for (auto *ptr : page_lst_)
                ::free(ptr);
        }

        char *alloc_page()
        {
            char *page = nullptr;
            constexpr static int free_count = PAGE_SIZE / N;

            int ret = posix_memalign((void **)&page, N, PAGE_SIZE);
            if (ret)
            {
                return nullptr;
            }

            memset(page, 0, PAGE_SIZE);

            char *cur = page;
            for (int i = 0; i < free_count - 1; i++)
            {
                *(char **)cur = cur + N;
                cur = cur + N;
            }

            *(char **)cur = nullptr;

            page_lst_.push_back(page);

            return page;
        }

        void *get_free_node()
        {
            std::lock_guard lk{lock_};
            if (next_free_)
            {
                void *ptr = (void *)next_free_;
                next_free_ = *(char **)next_free_;
                return ptr;
            }
            return nullptr;
        }

        SpinLock lock_;
        char *next_free_;
        std::vector<char *> page_lst_;
    };

    inline constexpr size_t ceil_pow2_and_lower8(size_t n)
    {
        if (n <= 8)
            return 8;
        n--;
        for (int shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
        {
            n |= (n >> shift);
        }
        return n + 1;
    }

    template <typename T>
    using Slab = SlabN<ceil_pow2_and_lower8(sizeof(T))>;

}

#endif // __SLAB_H