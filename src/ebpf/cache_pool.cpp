#include "cache_pool.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>

namespace delta
{
    namespace
    {
        // A replica opens the stream with
        // `*5\r\n$4\r\nSYNC\r\n$<n>\r\n<rdma-ip>\r\n$<n>\r\n<rdma-port>\r\n$<n>\r\n<target-ip>\r\n$<n>\r\n<target-port>\r\n`.
        bool is_sync_command(const char *buffer, size_t size, __u64 *target_addr_key)
        {
            static constexpr const char SYNC_CMD[]{"SYNC"};
            static constexpr size_t SYNC_LEN{sizeof(SYNC_CMD) - 1};

            size_t pos{0};

            if (buffer[pos] != '*')
                return false;

            // the multi-bulk count is irrelevant, the command name follows it
            while (pos < size && buffer[pos] != '\n')
                pos++;
            pos++;

            // consumes one `$<len>\r\n<payload>\r\n`, leaving pos on the next bulk string
            const char *arg{nullptr};
            size_t arg_len{0};
            auto next_bulk = [&]() -> bool
            {
                if (pos >= size || buffer[pos] != '$')
                    return false;
                pos++;

                size_t len{0};
                size_t digits{0};
                while (pos < size && buffer[pos] >= '0' && buffer[pos] <= '9')
                {
                    len = len * 10 + static_cast<size_t>(buffer[pos] - '0');
                    if (len > INFO_BUFFER_SIZE)
                        return false;
                    digits++;
                    pos++;
                }

                if (digits == 0)
                    return false;

                if (pos + 1 >= size || buffer[pos] != '\r' || buffer[pos + 1] != '\n')
                    return false;
                pos += 2;

                if (pos + len + 2 > size)
                    return false;

                arg = buffer + pos;
                arg_len = len;
                pos += len + 2;
                return true;
            };

            if (!next_bulk() || arg_len != SYNC_LEN || strncasecmp(arg, SYNC_CMD, SYNC_LEN) != 0)
                return false;

            // the rdma endpoint is the master's business, we connect back over tcp
            if (!next_bulk() || !next_bulk())
                return false;

            if (!next_bulk() || arg_len == 0 || arg_len >= INET_ADDRSTRLEN)
                return false;

            char ip[INET_ADDRSTRLEN]{0};
            memcpy(ip, arg, arg_len);

            struct in_addr target_addr{};
            if (inet_pton(AF_INET, ip, &target_addr) != 1)
                return false;

            if (!next_bulk() || arg_len == 0)
                return false;

            unsigned long target_port{0};
            for (size_t i = 0; i < arg_len; i++)
            {
                if (arg[i] < '0' || arg[i] > '9')
                    return false;
                target_port = target_port * 10 + static_cast<unsigned long>(arg[i] - '0');
                if (target_port > 65535)
                    return false;
            }

            if (target_port == 0)
                return false;

            if (target_addr_key)
                *target_addr_key = make_addr_port_key(target_addr.s_addr, static_cast<__u16>(target_port));

            return true;
        }
    } // namespace

    CachePoolManager::CachePoolManager() : cached_slave(0), head_(nullptr), tail_(nullptr) {}

    CachePoolManager::~CachePoolManager()
    {
        if (!head_ || !tail_)
            return;
        struct Node *cur = head_->next;
        while (cur && cur != tail_)
        {
            free(cur->ptr);
            cur->ptr = nullptr;
            struct Node *tmp = cur;
            cur = cur->next;
            free(tmp);
            tmp = nullptr;
        }

        free(head_);
        head_ = nullptr;
        free(tail_);
        tail_ = nullptr;
    }

    int CachePoolManager::_init()
    {
        if (head_ || tail_)
            return -2;

        head_ = (struct Node *)malloc(sizeof(struct Node));
        if (!head_)
            return -1;

        tail_ = (struct Node *)malloc(sizeof(struct Node));
        if (!tail_)
            return -1;

        init_node(head_);
        init_node(tail_);

        head_->next = tail_;
        tail_->prev = head_;

        return 0;
    }

    CachePoolManager &CachePoolManager::instance()
    {
        static CachePoolManager instance;
        static int ret = instance._init();
        if (ret < 0)
        {
            perror("init CachePoolManager error");
            exit(-1);
        }

        return instance;
    }

    struct Node *CachePoolManager::_add_new_node()
    {
        struct Node *new_node = (struct Node *)malloc(sizeof(struct Node));
        if (!new_node)
        {
            return nullptr;
        }
        init_node(new_node);

        new_node->ptr = (char *)malloc(NODE_PAGE_SIZE);
        if (new_node->ptr == nullptr)
        {
            free(new_node);
            return nullptr;
        }

        _add_node(new_node);
        return new_node;
    }

    int CachePoolManager::verify_slave(const char *buffer, size_t size, __u64 addr_p_key)
    {
        if (!buffer || size == 0 || addr_p_key == 0)
        {
            return -1;
        }

        // this addr port has been registered before, we get SYNCFIN now, return 2 for send
        if (cache_start_hashmap_.find(addr_p_key) != cache_start_hashmap_.end())
        {
            return 2;
        }

        // a SYNC command registers the replica and pins where its cache starts
        __u64 addr_p_key_connect{0};
        if (is_sync_command(buffer, size, &addr_p_key_connect))
        {
            if (addr_p_key_connect == 0)
                return -3;
            struct Node *cur_node = tail_->prev;
            if (cur_node == head_ || cur_node->offset == NODE_PAGE_SIZE)
            {
                struct Node *new_node = _add_new_node();
                if (new_node == nullptr)
                    return -2;
                cache_start_hashmap_[addr_p_key] = std::make_unique<StartPin>(StartPin{new_node, 0, addr_p_key_connect});
                new_node->fd_start_count++;
            }
            else
            {
                cache_start_hashmap_[addr_p_key] = std::make_unique<StartPin>(StartPin{cur_node, cur_node->offset, addr_p_key_connect});
                cur_node->fd_start_count++;
            }
            cached_slave++;
            return 1;
        }

        return 0;
    }

    int CachePoolManager::cache_buffer(const char *buffer, size_t size)
    {
        if (!buffer || size == 0)
            return -1;
        if (cached_slave)
        {
            struct Node *cur_node = tail_->prev;
            if (cur_node == head_ || cur_node->offset == NODE_PAGE_SIZE)
            {
                cur_node = _add_new_node();
                if (cur_node == nullptr)
                    return -2;
            }

            size_t cached{0};
            size_t left{size};
            while (cached < size)
            {
                size_t aval_size = NODE_PAGE_SIZE - cur_node->offset;
                if (aval_size == 0)
                {
                    cur_node = _add_new_node();
                    if (cur_node == nullptr)
                        return -2;
                    aval_size = NODE_PAGE_SIZE;
                }
                size_t chunk = left > aval_size ? aval_size : left;

                memcpy(cur_node->ptr + cur_node->offset, buffer + cached, chunk);
                cached += chunk;
                cur_node->offset += chunk;
                left -= chunk;
            }
        }
        return 0;
    }

    void CachePoolManager::purge_cached_node()
    {
        struct Node *cur_node = head_->next;

        while (cur_node != tail_ && cur_node->fd_start_count == 0)
        {
            struct Node *tmp = cur_node;
            cur_node = tmp->next;
            _del_node(tmp);
        }
    }

    void CachePoolManager::close_fd(__u64 addr_p_key)
    {
        if (cache_start_hashmap_.find(addr_p_key) != cache_start_hashmap_.end())
        {
            cache_start_hashmap_.erase(addr_p_key);
            cached_slave--;
        }
    }
} // namespace delta