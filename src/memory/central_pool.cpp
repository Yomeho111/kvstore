#include "central_pool.h"

#include <mutex>
#include "page_allocator.h"

namespace memory
{
    CentralPool &CentralPool::instance()
    {
        static CentralPool *cp = new CentralPool();
        return *cp;
    }

    char *CentralPool::fetch_batch(size_t index, size_t &batch_num)
    {
        if (index >= FREE_LIST_SIZE || batch_num == 0)
            return nullptr;

        std::lock_guard lk{lock_lst_[index]};

        if (!free_lst_[index])
        {
            size_t size = SizeClass::get_size(index);
            size_t target_batch_num = batch_num;
            char *start = fetch_from_page_cache(size, target_batch_num);
            if (start == nullptr)
            {
                batch_num = 0;
                return nullptr;
            }

            // split the page into memory item
            char *cur = start;
            for (int i = 0; i < target_batch_num - 1; i++)
            {
                *(char **)cur = cur + size;
                cur = cur + size;
            }
            *(char **)cur = nullptr;

            batch_num = target_batch_num;
            return start;
        }

        char *ptr = free_lst_[index];

        // Try to get the target batch num
        char *prev{nullptr};
        char *cur{ptr};
        size_t real_num_batch{0};

        for (int i = 0; (i < batch_num) && (cur); i++)
        {
            prev = cur;
            cur = *(char **)cur;
            real_num_batch++;
        }

        free_lst_[index] = cur;
        if (prev)
            *(char **)prev = nullptr;
        batch_num = real_num_batch;

        return ptr;
    }

    void CentralPool::return_batch(char *start, size_t index, size_t batch_num)
    {
        if (start == nullptr || index >= FREE_LIST_SIZE || batch_num == 0)
            return;

        std::lock_guard lk{lock_lst_[index]};
        char *cur = start;
        for (int i = 0; i < batch_num - 1; i++)
        {
            cur = *(char **)cur;
        }

        *(char **)cur = free_lst_[index];
        free_lst_[index] = start;
    }

    char *CentralPool::fetch_from_page_cache(size_t size, size_t &num_batch)
    {
        void *ptr = nullptr;
        size_t total_page = (size * num_batch + PAGE_SIZE - 1) / PAGE_SIZE;
        num_batch = total_page * PAGE_SIZE / size;

        ptr = PageAllocator::instance().alloc_page(total_page);
        return (char *)ptr;
    }
} // namespace memory