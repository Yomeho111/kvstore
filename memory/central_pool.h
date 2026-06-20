#ifndef __CENTRAL_POOL_H
#define __CENTRAL_POOL_H

#include <stddef.h>
#include "memory_utils.h"
#include "rbtree.hpp"

namespace memory
{

    class CentralPool
    {
    public:
        static CentralPool &instance();

        char *fetch_batch(size_t index, size_t &batch_num);

        void return_batch(char *start, size_t index, size_t batch_num);

    private:
        CentralPool() = default;
        ~CentralPool() = default;

        CentralPool(const CentralPool &) = delete;
        CentralPool(CentralPool &&) = delete;

        CentralPool &operator=(const CentralPool &) = delete;
        CentralPool &operator=(CentralPool &&) = delete;

        char *fetch_from_page_cache(size_t size, size_t &num_batch);

        char *free_lst_[FREE_LIST_SIZE] = {0};
        // size_t count_lst_[FREE_LIST_SIZE] = {0};
        SpinLock lock_lst_[FREE_LIST_SIZE];
    };

} // namespace memory

#endif // __CENTRAL_POOL_H