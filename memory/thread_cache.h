#ifndef __THREAD_CACHE_H
#define __THREAD_CACHE_H

#include "memory_utils.h"

namespace memory
{
    class ThreadCache
    {
    public:
        static ThreadCache &instance();

        void *alloc(size_t size);

        int free(void *ptr);

    private:
        ThreadCache() = default;
        ~ThreadCache();

        ThreadCache(const ThreadCache &) = delete;
        ThreadCache(ThreadCache &&) = delete;

        ThreadCache &operator=(const ThreadCache &) = delete;
        ThreadCache &operator=(ThreadCache &&) = delete;

        char *fetch_central_batch(size_t index, size_t &batch_num);

        void return_central_batch(size_t index);

        char *free_lst_[FREE_LIST_SIZE] = {0}; // first 8 bytes next ptr, second 8 bytes prev ptr
        size_t count_lst_[FREE_LIST_SIZE] = {0};
    };
} // namespace memory

#endif // __THREAD_CACHE_H