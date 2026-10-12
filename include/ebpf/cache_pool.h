#ifndef __CACHE_POOL_H
#define __CACHE_POOL_H

#include <unordered_map>
#include <memory>
#include <queue>
#include <stddef.h>
#include <stdlib.h>

#include "delta.h"

constexpr inline const size_t NODE_PAGE_SIZE{4096};

struct Node
{
    char *ptr;
    size_t fd_start_count;
    size_t offset;
    struct Node *prev;
    struct Node *next;
};

inline void init_node(struct Node *node)
{
    if (!node)
        return;

    node->fd_start_count = 0;
    node->next = nullptr;
    node->prev = nullptr;
    node->offset = 0;
    node->ptr = nullptr;
}

namespace delta
{

    class CachePoolManager
    {
    private:
        struct StartPin
        {
            struct Node *node{nullptr};
            size_t offset{0};
            __u64 target_addr_port_key{0};
        };

        using StartPin_t = std::unique_ptr<struct StartPin>;

    public:
        static CachePoolManager &instance();

        // probe slave and set up tcp connection, 0 not slave, 1 SYNC, 2 SYNCFIN
        int verify_slave(const char *buffer, size_t size, __u64 addr_p_key);

        int cache_buffer(const char *buffer, size_t size);

        void purge_cached_node();

        void close_fd(__u64 addr_p_key);

        void get_registered_start(__u64 addr_p_key, struct Node **node, size_t *offset, __u64 *target_addr_port)
        {
            *node = cache_start_hashmap_[addr_p_key]->node;
            *offset = cache_start_hashmap_[addr_p_key]->offset;
            *target_addr_port = cache_start_hashmap_[addr_p_key]->target_addr_port_key;
        }

    private:
        CachePoolManager();
        ~CachePoolManager();

        CachePoolManager(const CachePoolManager &) = delete;
        CachePoolManager(CachePoolManager &&) = delete;

        CachePoolManager &operator=(const CachePoolManager &) = delete;
        CachePoolManager &operator=(CachePoolManager &&) = delete;

        int _init();

        void _add_node(struct Node *node)
        {
            if (!node || node == tail_ || node == head_)
                return;
            node->next = tail_;
            node->prev = tail_->prev;

            tail_->prev->next = node;
            tail_->prev = node;
        }

        void _del_node(struct Node *node)
        {
            if (!node || node == tail_ || node == head_)
                return;
            node->next->prev = node->prev;
            node->prev->next = node->next;

            free(node->ptr);
            free(node);
        }

        struct Node *_add_new_node();

        struct Node *head_;
        struct Node *tail_;

        // fd: {Node*, offset}
        std::unordered_map<__u64, StartPin_t> cache_start_hashmap_;

        size_t cached_slave;
    };
} // namespace delta

#endif // __CACHE_POOL_H