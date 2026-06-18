#ifndef __ENGINE_INTERFACE_H
#define __ENGINE_INTERFACE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <chrono>
#include <string>
#include "engine_interface_base.h"
#include "allocator.h"
#include "kv_persistent.h"
#include "memory_utils.h"
#include "slab.hpp"
#include "timer.h"
#include "rep_manager.h"

using string = std::basic_string<
    char,
    std::char_traits<char>,
    allocator::MyAllocator<char>>;

namespace kv_engine
{
    template <typename RealEngine>
    class EngineInterface : public EngineInterfaceBase
    {
    protected:
        EngineInterface() = default;
        ~EngineInterface() = default;

    public:
        int set(char *key, size_t key_len, char *value, size_t val_len, struct kv_protocal::TimeoutSpec *timeout = nullptr, bool to_disk = true) override
        {
            if (key_len == 0 || val_len == 0)
                return -1;
            string key_s = string(key, key_len);
            string val_s = string(value, val_len);

            if (key_s.size() == 0 || val_s.size() == 0)
                return -1;

            std::lock_guard lk{lock_};
            auto &base = static_cast<RealEngine *>(this)->get_base();
            auto *node = base.get_node(key_s);
            if (node)
                return 1;

            base.insert(key_s, val_s);

            if (to_disk && store_engine.dump_record(kv_protocal::KVS_SET, key_s, val_s) < 0)
                return -2;

            if (to_disk && replicate::g_replicate && replicate::RepManager::instance().insert_node(&key_s, &val_s, kv_protocal::KVS_SET) < 0)
                return -3;

            if (timeout && timeout->tv_sec != -1 && timeout->tv_nsec != -1)
            {
                auto dur = TRANS_TIMEOUT_MILLI(timeout);

                kv_timer::TimerManager::instance().add_event(
                    dur,
                    [this, to_disk, k_s = std::move(key_s)]
                    {
                        std::lock_guard lk{this->lock_};
                        static_cast<RealEngine *>(this)->get_base().delNode(k_s);
                        if (to_disk && replicate::g_replicate && replicate::RepManager::instance().insert_node(&k_s, nullptr, kv_protocal::KVS_DEL) < 0)
                            ;
                        this->store_engine.dump_record(kv_protocal::KVS_DEL, k_s, string{});
                    });
            }
            return 0;
        }

        int get(char *key, size_t key_len, char **value) override
        {
            if (key_len == 0)
                return -1;
            string key_s = string(key, key_len);

            if (key_s.size() == 0)
                return -1;

            std::lock_guard lk{lock_};
            auto &base = static_cast<RealEngine *>(this)->get_base();
            auto *node = base.get_node(key_s);
            if (node == nullptr)
                return 0;

            *value = (char *)allocator::kv_malloc(node->value.size() + 2);
            if (*value == nullptr)
                return -2;
            memcpy(*value, node->value.c_str(), node->value.size());
            memcpy(*value + node->value.size(), "\r\n", 2);

            return node->value.size() + 2;
        }

        int modify(char *key, size_t key_len, char *value, size_t val_len, struct kv_protocal::TimeoutSpec *timeout = nullptr, bool to_disk = true) override
        {
            if (key_len == 0 || val_len == 0)
                return -1;
            string key_s = string(key, key_len);
            string val_s = string(value, val_len);

            if (key_s.size() == 0 || val_s.size() == 0)
                return -1;

            std::lock_guard lk{lock_};
            auto &base = static_cast<RealEngine *>(this)->get_base();
            auto *node = base.get_node(key_s);
            if (!node)
                return 1;

            node->value = val_s;

            if (to_disk && store_engine.dump_record(kv_protocal::KVS_MOD, key_s, val_s) < 0)
                return -2;

            if (to_disk && replicate::g_replicate && replicate::RepManager::instance().insert_node(&key_s, &val_s, kv_protocal::KVS_MOD) < 0)
                return -3;

            if (timeout && timeout->tv_sec != -1 && timeout->tv_nsec != -1)
            {
                auto dur = TRANS_TIMEOUT_MILLI(timeout);

                kv_timer::TimerManager::instance().add_event(
                    dur,
                    [this, to_disk, k_s = std::move(key_s)]
                    {
                        std::lock_guard lk{this->lock_};
                        static_cast<RealEngine *>(this)->get_base().delNode(k_s);
                        if (to_disk && replicate::g_replicate && replicate::RepManager::instance().insert_node(&k_s, nullptr, kv_protocal::KVS_DEL) < 0)
                            ;
                        this->store_engine.dump_record(kv_protocal::KVS_DEL, k_s, string{});
                    });
            }
            return 0;
        }

        int del(char *key, size_t key_len, bool to_disk = true) override
        {
            if (key_len == 0)
                return -1;
            string key_s = string(key, key_len);

            if (key_s.size() == 0)
                return -1;

            std::lock_guard lk{lock_};
            auto &base = static_cast<RealEngine *>(this)->get_base();
            if (base.delNode(key_s) == -1)
                return 1;

            if (to_disk && store_engine.dump_record(kv_protocal::KVS_DEL, key_s, string{}) < 0)
                return -2;

            if (to_disk && replicate::g_replicate && replicate::RepManager::instance().insert_node(&key_s, nullptr, kv_protocal::KVS_DEL) < 0)
                return -3;
            return 0;
        }

        int exist(char *key, size_t key_len) override
        {
            if (key_len == 0)
                return -1;
            string key_s = string(key, key_len);

            if (key_s.size() == 0)
                return -1;

            std::lock_guard lk{lock_};
            auto &base = static_cast<RealEngine *>(this)->get_base();
            auto *node = base.get_node(key_s);
            if (node == nullptr)
                return 1;
            return 0;
        }

        int init() noexcept override
        {
            return store_engine.load_record(this);
        }

        auto begin()
        {
            return static_cast<RealEngine *>(this)->get_base().begin();
        }

        auto end()
        {
            return static_cast<RealEngine *>(this)->get_base().end();
        }

        int size()
        {
            return static_cast<RealEngine *>(this)->get_base().size();
        }

        memory::SpinLock lock_;

    private:
        kv_persistent::StoreEngine store_engine;
    };
}
#endif // __ENGINE_INTERFACE_H