#ifndef __PAGE_ALLOCATOR_H
#define __PAGE_ALLOCATOR_H

#include <mutex>
#include <vector>
#include <stddef.h>
#include <stdint.h>
#include "rbtree.hpp"
#include "slab.hpp"
#include "memory_utils.h"

namespace memory
{

    struct RawPage
    {
        void *ptr;
        size_t page_num;
    };

    class PageAllocator
    {

    public:
        static PageAllocator &instance();

        void *alloc_page(size_t num_pages);

    private:
        PageAllocator() {}
        ~PageAllocator();

        PageAllocator(const PageAllocator &) = delete;
        PageAllocator(PageAllocator &&) = delete;

        PageAllocator &operator=(const PageAllocator &) = delete;
        PageAllocator &operator=(PageAllocator &&) = delete;

        char *system_alloc_page(size_t num_pages);

        void _put_on_free_list(char *page, size_t num_pages);

        std::mutex mtx_;
        base_component::RBTree<size_t, char *> free_list_;
        std::vector<RawPage> reg_;
    };

}

#endif // __PAGE_ALLOCATOR_H