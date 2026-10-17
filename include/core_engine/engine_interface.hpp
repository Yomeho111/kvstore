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
#include <mutex>
#include <filesystem>
#include <vector>
#include <utility>
#include <limits>
#include "engine_interface_base.h"
#include "allocator.h"
#include "kv_persistent.h"
#include "memory_utils.h"
#include "slab.hpp"
#include "timer.h"

#ifdef ENABLE_RAG
#include <cmath>
#include "vectordb_tool.h"
#endif

namespace kv_engine
{
    namespace fs = std::filesystem;

#ifdef ENABLE_RAG
    // the struct for vector database search
    struct Vnode
    {
        float distance;
        const string *key_ptr;

        bool operator<(const Vnode &other) const
        {
            return distance < other.distance;
        }

        bool operator>(const Vnode &other) const
        {
            return distance > other.distance;
        }
    };

#endif

    template <typename RealEngine>
    class EngineInterface : public EngineInterfaceBase
    {
    protected:
        EngineInterface() = default;
        ~EngineInterface() = default;

    public:
        int set(const char *key, size_t key_len, const char *value, size_t val_len, struct kv_protocal::TimeoutSpec *timeout = nullptr, bool to_disk = true) override
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

            if (timeout && timeout->tv_sec != -1 && timeout->tv_nsec != -1)
            {
                auto dur = TRANS_TIMEOUT_MILLI(timeout);

                kv_timer::TimerManager::instance().add_event(
                    dur,
                    [this, to_disk, key_s]() mutable
                    {
                        std::lock_guard lk{this->lock_};
                        static_cast<RealEngine *>(this)->get_base().delNode(key_s);
                        string value{};
                        if (to_disk && kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF)
                            kv_persistent::StoreEngine::instance().dump_record(kv_protocal::KVS_DEL, key_s, value);
                    });
            }

            if (to_disk && kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF &&
                kv_persistent::StoreEngine::instance().dump_record(kv_protocal::KVS_SET, key_s, val_s) < 0)
                return -2;

            return 0;
        }

        int get(const char *key, size_t key_len, char **value) override
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

#ifdef ENABLE_RAG
        int vsearch(const void *vector, size_t K, char **response, float threshold = std::numeric_limits<float>::infinity()) override
        {
            if (!vector || K <= 0)
                return -1;

            if (static_cast<double>(threshold) <= 1e-8)
                return 0;

            std::lock_guard lk{lock_};
            auto &vdb = vectordb::VectorDbEngine::instance();

            auto results = vdb.query_nearest(vector, K);
            if (results.empty())
                return 0;

            size_t result_count = results.size();
            size_t response_size{0};
            size_t valid_count{0};
            std::vector<string *, allocator::MyAllocator<string *>> uuid_vec;
            auto &base = static_cast<RealEngine *>(this)->get_base();
            uuid_vec.reserve(result_count);

            for (size_t index = 0; index < result_count; index++)
            {
                auto &[distance, vec_id] = results[index];
                if (distance > threshold)
                    break;
                char buffer[32];
                auto [ptr, ec] = std::to_chars(
                    buffer,
                    buffer + sizeof(buffer),
                    vec_id);

                if (ec != std::errc{})
                {
                    return -1;
                }

                string vec_id_str(buffer, ptr);
                auto *node = base.get_node(vec_id_str);
                uuid_vec.push_back(&node->value);
                response_size += node->value.size();
                valid_count++;
            }

            response_size += valid_count - 1;

            *response = (char *)allocator::kv_malloc(response_size + 2);
            if (*response == nullptr)
                return -3;

            char *cur = *response;
            size_t index = 0;
            for (; index + 1 < valid_count; index++)
            {
                memcpy(cur, uuid_vec[index]->c_str(), uuid_vec[index]->size());
                cur += uuid_vec[index]->size();
                *cur = ',';
                cur++;
            }

            memcpy(cur, uuid_vec[index]->c_str(), uuid_vec[index]->size());
            cur += uuid_vec[index]->size();

            memcpy(cur, "\r\n", 2);

            return response_size + 2;
        }
#endif

        int modify(const char *key, size_t key_len, const char *value, size_t val_len, struct kv_protocal::TimeoutSpec *timeout = nullptr, bool to_disk = true) override
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

            if (timeout && timeout->tv_sec != -1 && timeout->tv_nsec != -1)
            {
                auto dur = TRANS_TIMEOUT_MILLI(timeout);

                kv_timer::TimerManager::instance().add_event(
                    dur,
                    [this, to_disk, key_s]() mutable
                    {
                        std::lock_guard lk{this->lock_};
                        static_cast<RealEngine *>(this)->get_base().delNode(key_s);
                        string value{};
                        if (to_disk && kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF)
                            kv_persistent::StoreEngine::instance().dump_record(kv_protocal::KVS_DEL, key_s, value);
                    });
            }

            if (to_disk && kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF &&
                kv_persistent::StoreEngine::instance().dump_record(kv_protocal::KVS_MOD, key_s, val_s) < 0)
                return -2;

            return 0;
        }

        int del(const char *key, size_t key_len, bool to_disk = true) override
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

            string val_s{};
            if (to_disk && kv_persistent::g_persist_mode == kv_persistent::PersistMode::AOF &&
                kv_persistent::StoreEngine::instance().dump_record(kv_protocal::KVS_DEL, key_s, val_s) < 0)
                return -2;

            return 0;
        }

        int exist(const char *key, size_t key_len) override
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
            if (kv_persistent::g_persist_mode == kv_persistent::PersistMode::NONE)
                return 0;
            if (kv_persistent::g_persist_mode == kv_persistent::PersistMode::RDB)
                return load_snapshot();
            return kv_persistent::StoreEngine::instance().load_record(this);
        }

        // Fork a child that writes a point-in-time RDB snapshot of the whole dataset.
        // The parent only holds the lock across fork() and then keeps serving.
        int save(const string &tmp_file_path, bool for_temp = false)
        {
            auto &snapshot_engine = kv_persistent::SnapshotEngine::instance();
            if (snapshot_engine.prepare(tmp_file_path) < 0)
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
                snapshot_engine.discard(tmp_file_path);
                return -1;
            }

            lock_.unlock();

            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }

            if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            {
                snapshot_engine.discard(tmp_file_path);
                return -1;
            }

            if (for_temp)
                return 0;
            return snapshot_engine.commit(tmp_file_path);
        }

        void remove_temp_file(const string &tmp_file_name)
        {
            kv_persistent::SnapshotEngine::instance().discard(tmp_file_name);
            std::error_code ec;
            const fs::path path{kv_persistent::RDB_FOLDER};

            if (!fs::exists(path, ec))
                return;

            if (!fs::is_directory(path, ec))
                return;

            if (!fs::is_empty(path, ec))
                return;

            fs::remove(path, ec);
        }

        int load_snapshot(const string &file_path = kv_persistent::RDB_DEFAULT_PATH, bool to_disk = false)
        {
            return kv_persistent::SnapshotEngine::instance().load(this, file_path, to_disk);
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
            std::lock_guard lk{lock_};
            return static_cast<RealEngine *>(this)->get_base().size();
        }

        void lock() noexcept
        {
            lock_.lock();
        }

        void unlock() noexcept
        {
            lock_.unlock();
        }

    private:
        memory::SpinLock lock_;
    };
} // namespace kv_engine
#endif // __ENGINE_INTERFACE_H