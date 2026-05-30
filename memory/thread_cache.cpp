#include "thread_cache.h"
#include "central_pool.h"

namespace memory
{
    ThreadCache &ThreadCache::instance()
    {
        thread_local static ThreadCache tc;
        return tc;
    }

    ThreadCache::~ThreadCache()
    {
        for (size_t index = 0; index < FREE_LIST_SIZE; index++)
        {
            if (free_lst_[index] && count_lst_[index])
                CentralPool::instance().return_batch(free_lst_[index], index, count_lst_[index]);
            free_lst_[index] = nullptr;
            count_lst_[index] = 0;
        }
    }

    void *ThreadCache::alloc(size_t size)
    {
        size_t total_size = size + ALIGNMENT;
        size_t index = SizeClass::get_index(total_size);
        if (index >= FREE_LIST_SIZE)
            return nullptr;

        if (count_lst_[index] && free_lst_[index])
        {
            char *ptr = free_lst_[index];
            free_lst_[index] = *(char **)ptr;
            count_lst_[index]--;
            *(size_t *)ptr = total_size;
            return (void *)(ptr + ALIGNMENT);
        }

        size_t batch_num{0};
        char *ptr = fetch_central_batch(index, batch_num);
        if (!ptr)
            return nullptr;

        free_lst_[index] = *(char **)ptr;
        count_lst_[index] += batch_num - 1;
        *(size_t *)ptr = total_size;
        return (void *)(ptr + ALIGNMENT);
    }

    int ThreadCache::free(void *ptr)
    {
        if (!ptr)
            return -1;
        char *cur_start = (char *)ptr - ALIGNMENT;
        size_t total_size = *(size_t *)cur_start;
        size_t index = SizeClass::get_index(total_size);

        if (index >= FREE_LIST_SIZE)
            return -1;

        *(char **)cur_start = free_lst_[index];
        free_lst_[index] = cur_start;
        count_lst_[index]++;

        if (count_lst_[index] > (total_size > THRESHHOLD_BYTES ? BIG_BATCH_SIZE : SMALL_BATCH_SIZE) * 2)
            return_central_batch(index);
        return 0;
    }

    char *ThreadCache::fetch_central_batch(size_t index, size_t &batch_num)
    {
        size_t size = SizeClass::get_size(index);

        batch_num = size > THRESHHOLD_BYTES ? BIG_BATCH_SIZE : SMALL_BATCH_SIZE;

        char *ptr = CentralPool::instance().fetch_batch(index, batch_num);
        if (ptr == nullptr || batch_num == 0)
            return nullptr;

        return ptr;
    }

    void ThreadCache::return_central_batch(size_t index)
    {
        size_t return_batch = count_lst_[index] / 2;
        if (return_batch == 0)
            return;

        char *start = free_lst_[index];
        char *cur = start;
        char *prev = nullptr;
        for (int i = 0; i < return_batch; i++)
        {
            prev = cur;
            cur = *(char **)cur;
        }

        *(char **)prev = nullptr;
        free_lst_[index] = cur;

        CentralPool::instance().return_batch(start, index, return_batch);
        count_lst_[index] -= return_batch;
    }
}