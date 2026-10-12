#include "page_allocator.h"

#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>

#define NEW_PAGE_FACTOR 2

namespace memory
{

    PageAllocator &PageAllocator::instance()
    {
        static PageAllocator *pa = new PageAllocator();
        return *pa;
    }

    PageAllocator::~PageAllocator()
    {
        for (auto &page : reg_)
        {
            munmap(page.ptr, PAGE_SIZE * page.page_num);
        }
    }

    char *PageAllocator::system_alloc_page(size_t num_pages)
    {
        size_t size = num_pages * PAGE_SIZE;

        void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED)
            return nullptr;

        reg_.emplace_back(ptr, num_pages);

        return (char *)ptr;
    }

    void *PageAllocator::alloc_page(size_t num_pages)
    {
        std::lock_guard lk{mtx_};
        auto avail_node = free_list_.lower_bound(num_pages);
        if (avail_node != free_list_.end() && avail_node->second)
        {
            size_t cur_page_num = avail_node->first;
            char *cur_page = avail_node->second;

            // get the pages out of the tree
            avail_node->second = *(char **)cur_page;
            *(char **)cur_page = nullptr;

            // split the page if the cur_page size is bigger than we want
            if (cur_page_num > num_pages)
            {
                char *new_page = cur_page + (PAGE_SIZE * num_pages);
                _put_on_free_list(new_page, cur_page_num - num_pages);
            }

            return cur_page;
        }

        size_t new_page_num = num_pages * NEW_PAGE_FACTOR;
        char *page = system_alloc_page(new_page_num);
        if (page == nullptr)
            return nullptr;

        if (new_page_num > num_pages)
        {
            char *new_page = page + (PAGE_SIZE * num_pages);
            _put_on_free_list(new_page, new_page_num - num_pages);
        }

        return (void *)page;
    }

    void PageAllocator::_put_on_free_list(char *page, size_t num_pages)
    {
        if (page == nullptr || num_pages == 0)
            return;
        auto node = free_list_.find(num_pages);
        if (node != free_list_.end())
        {
            *(char **)page = node->second;
            node->second = page;
        }
        else
        {
            *(char **)page = nullptr;
            free_list_[num_pages] = page;
        }
    }

} // namespace memory
