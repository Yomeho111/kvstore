#include "rbtree_engine.h"
#include <string.h>
#include <stdio.h>
#include <chrono>

#include "slab.hpp"
#include "timer.h"

namespace kv_engine
{
    int RbtreeEngine::set(char *key, size_t key_len, char *value, size_t val_len, struct kv_protocal::TimeoutSpec *timeout, bool to_disk)
    {
        if (key_len == 0 || val_len == 0)
            return -1;
        string key_s = string(key, key_len);
        string val_s = string(value, val_len);

        if (key_s.size() == 0 || val_s.size() == 0)
            return -1;

        std::lock_guard lk{lock_};
        auto *node = rbt.get_node(key_s);
        if (node)
            return 1;

        rbt.insert(key_s, val_s);

        if (to_disk && store_engine.dump_record(kv_protocal::KVS_SET, key_s, val_s) < 0)
            return -2;

        if (timeout && timeout->tv_sec != -1 && timeout->tv_nsec != -1)
        {
            auto dur = TRANS_TIMEOUT_MILLI(timeout);

            kv_timer::TimerManager::instance().add_event(
                dur,
                [this, k_s = std::move(key_s)]
                {
                    std::lock_guard lk{this->lock_};
                    this->rbt.delNode(k_s);
                    this->store_engine.dump_record(kv_protocal::KVS_DEL, k_s, string{});
                });
        }
        return 0;
    }

    int RbtreeEngine::get(char *key, size_t key_len, char **value)
    {
        if (key_len == 0)
            return -1;
        string key_s = string(key, key_len);

        if (key_s.size() == 0)
            return -1;

        std::lock_guard lk{lock_};
        auto *node = rbt.get_node(key_s);
        if (node == nullptr)
            return 0;

        *value = (char *)allocator::kv_malloc(node->value.size() + 2);
        if (*value == nullptr)
            return -2;
        memcpy(*value, node->value.c_str(), node->value.size());
        memcpy(*value + node->value.size(), "\r\n", 2);

        return node->value.size() + 2;
    }

    int RbtreeEngine::modify(char *key, size_t key_len, char *value, size_t val_len, struct kv_protocal::TimeoutSpec *timeout, bool to_disk)
    {
        if (key_len == 0 || val_len == 0)
            return -1;
        string key_s = string(key, key_len);
        string val_s = string(value, val_len);

        if (key_s.size() == 0 || val_s.size() == 0)
            return -1;

        std::lock_guard lk{lock_};
        auto *node = rbt.get_node(key_s);
        if (!node)
            return 1;

        node->value = val_s;

        if (to_disk && store_engine.dump_record(kv_protocal::KVS_MOD, key_s, val_s) < 0)
            return -2;

        if (timeout && timeout->tv_sec != -1 && timeout->tv_nsec != -1)
        {
            auto dur = TRANS_TIMEOUT_MILLI(timeout);

            kv_timer::TimerManager::instance().add_event(
                dur,
                [this, k_s = std::move(key_s)]
                {
                    std::lock_guard lk{this->lock_};
                    this->rbt.delNode(k_s);
                    this->store_engine.dump_record(kv_protocal::KVS_DEL, k_s, string{});
                });
        }
        return 0;
    }

    int RbtreeEngine::del(char *key, size_t key_len, bool to_disk)
    {
        if (key_len == 0)
            return -1;
        string key_s = string(key, key_len);

        if (key_s.size() == 0)
            return -1;

        std::lock_guard lk{lock_};
        if (rbt.delNode(key_s) == -1)
            return 1;

        if (to_disk && store_engine.dump_record(kv_protocal::KVS_DEL, key_s, string{}) < 0)
            return -2;
        return 0;
    }

    int RbtreeEngine::exist(char *key, size_t key_len)
    {
        if (key_len == 0)
            return -1;
        string key_s = string(key, key_len);

        if (key_s.size() == 0)
            return -1;

        std::lock_guard lk{lock_};
        auto *node = rbt.get_node(key_s);
        if (node == nullptr)
            return 1;
        return 0;
    }

    int RbtreeEngine::init() noexcept
    {
        return store_engine.load_record(this);
    }
}