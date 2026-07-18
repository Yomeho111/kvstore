#ifndef __ENGINE_INTERFACE_H
#define __ENGINE_INTERFACE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <chrono>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
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

            if (base.insert(key_s, val_s) < 0)
                return -1;

            if (to_disk && kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF &&
                store_engine.dump_record(kv_protocal::KVS_SET, key_s, val_s) < 0)
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
                        if (kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF)
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

            if (to_disk && kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF &&
                store_engine.dump_record(kv_protocal::KVS_MOD, key_s, val_s) < 0)
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
                        if (kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF)
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

            if (to_disk && kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF &&
                store_engine.dump_record(kv_protocal::KVS_DEL, key_s, string{}) < 0)
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
            if (kv_persistent::g_persist_mode == kv_persistent::PersistMode::RDB)
                return snapshot_engine.load(this);
            return store_engine.load_record(this);
        }

        // Fork a child that writes a point-in-time RDB snapshot of the whole dataset.
        // The parent only holds the lock across fork() and then keeps serving.
        int save()
        {
            if (snapshot_engine.prepare() < 0)
                return -1;

            lock_.lock();
            pid_t pid = fork();
            if (pid == 0)
            {
                // child: dump every live pair against the COW image, then exit
                int rc = 0;
                if (snapshot_engine.child_begin() < 0)
                    _exit(1);
                for (auto it = this->begin(); it != this->end(); ++it)
                {
                    auto *node = *it;
                    if (snapshot_engine.child_write(node->key, node->value) < 0)
                    {
                        rc = 1;
                        break;
                    }
                }
                if (rc == 0 && snapshot_engine.child_finish() < 0)
                    rc = 1;
                _exit(rc);
            }

            if (pid < 0)
            {
                lock_.unlock();
                snapshot_engine.discard();
                return -1;
            }

            lock_.unlock();

            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return snapshot_engine.commit();

            snapshot_engine.discard();
            return -1;
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
        // RDB snapshot engine. Kept as a member so its ~6KB io_uring pipeline buffer
        // lives in the engine's storage (static, for the singleton) instead of on the
        // small SIGUSR1 dump thread's stack. Only exercised in RDB mode.
        kv_persistent::SnapshotEngine snapshot_engine;
    };
} // namespace kv_engine
#endif // __ENGINE_INTERFACE_H