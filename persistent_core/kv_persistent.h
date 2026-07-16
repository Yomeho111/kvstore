#ifndef __KV_PERSISTENT_H
#define __KV_PERSISTENT_H

#include <filesystem>
#include <string>
#include <liburing.h>
#include "engine_interface_base.h"
#include "allocator.h"

using string = std::basic_string<
    char,
    std::char_traits<char>,
    allocator::MyAllocator<char>>;

namespace kv_persistent
{
    namespace fs = std::filesystem;
    class StoreEngine
    {
        using CommandType = uint16_t;

    public:
        StoreEngine() = default;
        ~StoreEngine()
        {
            _close_file();
            if (ring_ready_)
                io_uring_queue_exit(&ring_);
        }

        int dump_record(CommandType command, const string &key, const string &value);

        int load_record(kv_engine::EngineInterfaceBase *engine);

    private:
        StoreEngine(const StoreEngine &) = delete;
        StoreEngine(StoreEngine &&) = delete;

        StoreEngine &operator=(const StoreEngine &) = delete;
        StoreEngine &operator=(StoreEngine &&) = delete;

        int _open_file(int idx);

        void _close_file();

        int _switch_new_file();

        int _load_record(kv_engine::EngineInterfaceBase *engine, const fs::path &file_path);

        int _append(const char *buf, size_t len);

        int file_idx_ = 0;
        size_t file_size = 0;
        int fd_ = -1;
        struct io_uring ring_;
        bool ring_ready_ = false;
    };
} // namespace kv_persistent

#endif // __KV_PERSISTENT_H