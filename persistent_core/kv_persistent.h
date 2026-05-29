#ifndef __KV_PERSISTENT_H
#define __KV_PERSISTENT_H

#include <fstream>
#include <filesystem>
#include <string>
#include "engine_interface.h"
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
        }

        int dump_record(CommandType command, const string &key, const string &value);

        int load_record(kv_engine::EngineInterface *engine);

    private:
        StoreEngine(const StoreEngine &) = delete;
        StoreEngine(StoreEngine &&) = delete;

        StoreEngine &operator=(const StoreEngine &) = delete;
        StoreEngine &operator=(StoreEngine &&) = delete;

        int _open_file(int idx);

        void _close_file();

        int _switch_new_file();

        int _load_record(kv_engine::EngineInterface *engine, const fs::path &file_path);

        int file_idx_ = 0;
        size_t file_size = 0;
        std::fstream file_;
    };
}

#endif // __KV_PERSISTENT_H