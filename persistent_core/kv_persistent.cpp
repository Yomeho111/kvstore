#include "kv_persistent.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <iostream>
#include <vector>

#include "allocator.h"
#include "kv_header.h"

namespace kv_persistent
{

    constexpr const char *STORE_FOLDER{"data"};
    constexpr const char *STORE_FILE_ROOT{"kv"};
    constexpr size_t MAX_BYTES_PER_FILE{512 * 1024 * 1024};
    constexpr uint32_t MAGIC{0x4B565354};

    static bool parse_store_file_index(const fs::path &file_path, int *file_idx)
    {
        if (file_idx == nullptr)
            return false;

        std::string filename = file_path.stem().string();
        std::string prefix = std::string(STORE_FILE_ROOT) + "_";
        size_t filename_size = filename.size();

        size_t pos = filename.find(prefix);
        pos += prefix.size();

        int idx = 0;

        for (; pos < filename.size(); pos++)
        {
            if (filename[pos] < '0' || filename[pos] > '9')
                return false;
            idx = idx * 10 + (filename[pos] - '0');
        }

        *file_idx = idx;

        return true;
    }

    int StoreEngine::dump_record(CommandType command, char *key, size_t key_len, char *value, size_t val_len)
    {
        if (key == nullptr || !(command == kv_protocal::KVS_SET || command == kv_protocal::KVS_DEL || command == kv_protocal::KVS_MOD) || key_len == 0)
            return -1;

        if (!file_.is_open())
        {
            if (_open_file(file_idx_) < 0)
                return -3;
        }

        size_t buffer_size = sizeof(MAGIC) + sizeof(command) + sizeof(key_len) + key_len + sizeof(val_len) + val_len;

        char *buffer = (char *)allocator::kv_malloc(buffer_size);
        if (!buffer)
            return -2;

        char *cur = buffer;

        // write magic
        memcpy(cur, &MAGIC, sizeof(MAGIC));
        cur += sizeof(MAGIC);

        // write command
        memcpy(cur, &command, sizeof(command));
        cur += sizeof(command);

        // write key_len
        memcpy(cur, &key_len, sizeof(key_len));
        cur += sizeof(key_len);

        // write key
        memcpy(cur, key, key_len);
        cur += key_len;

        // write value_len
        memcpy(cur, &val_len, sizeof(val_len));
        cur += sizeof(val_len);

        // write value
        if (val_len > 0)
        {
            memcpy(cur, value, val_len);
        }

        file_.seekp(0, std::ios::end);
        if (!file_)
        {
            allocator::kv_free(buffer);
            return -4;
        }

        file_.write(buffer, static_cast<std::streamsize>(buffer_size));
        if (!file_)
        {
            allocator::kv_free(buffer);
            return -5;
        }

        file_.flush();
        file_size += buffer_size;

        if (file_size > MAX_BYTES_PER_FILE)
        {
            int ret = _switch_new_file();
            if (ret < 0)
            {
                allocator::kv_free(buffer);
                return -6;
            }
        }

        allocator::kv_free(buffer);
        return 0;
    }

    int StoreEngine::load_record(kv_engine::EngineInterface *engine)
    {
        if (engine == nullptr)
            return -1;

        std::error_code ec;
        fs::path folder{STORE_FOLDER};

        if (!fs::exists(folder, ec))
        {
            if (ec)
                return -1;

            return 0;
        }

        if (!fs::is_directory(folder, ec) || ec)
            return -2;

        std::vector<std::pair<int, fs::path>, allocator::MyAllocator<std::pair<int, fs::path>>> store_files;

        for (const auto &entry : fs::directory_iterator(folder))
        {
            if (entry.is_regular_file(ec) && !ec)
            {
                int idx = 0;
                const fs::path &file_path = entry.path();

                if (parse_store_file_index(file_path, &idx))
                    store_files.push_back({idx, file_path});
                else
                    return -4;
            }
        }

        std::sort(store_files.begin(), store_files.end(), [](const auto &lhs, const auto &rhs)
                  { return lhs.first < rhs.first; });

        for (const auto &store_file : store_files)
        {
            int ret = _load_record(engine, store_file.second);
            if (ret < 0)
                return ret;
        }

        file_idx_ = store_files.empty() ? 0 : store_files.back().first;
        return 0;
    }

    int StoreEngine::_load_record(kv_engine::EngineInterface *engine, const fs::path &file_path)
    {
        if (engine == nullptr)
            return -1;
        std::ifstream file(file_path, std::ios::binary);
        if (!file)
        {
            return -1;
        }

        size_t size = static_cast<size_t>(fs::file_size(file_path));
        size_t offset = 0;

        auto read_exact = [&](void *dst, size_t len) -> int
        {
            if (offset + len > size)
                return -3;

            file.read(static_cast<char *>(dst), static_cast<std::streamsize>(len));
            if (file.gcount() != static_cast<std::streamsize>(len))
                return -3;

            offset += len;
            return 0;
        };

        while (offset < size)
        {
            uint32_t magic = 0;
            CommandType command = 0;
            size_t key_len = 0;
            size_t val_len = 0;

            if (read_exact(&magic, sizeof(magic)) < 0)
                return -3;

            if (magic != MAGIC)
                return -4;

            if (read_exact(&command, sizeof(command)) < 0)
                return -3;

            if (read_exact(&key_len, sizeof(key_len)) < 0)
                return -3;

            if (key_len == 0)
                return -4;

            char *key = static_cast<char *>(allocator::kv_malloc(key_len));
            if (key == nullptr)
                return -5;

            if (read_exact(key, key_len) < 0)
            {
                allocator::kv_free(key);
                return -3;
            }

            if (read_exact(&val_len, sizeof(val_len)) < 0)
            {
                allocator::kv_free(key);
                return -3;
            }

            char *value = nullptr;
            if (val_len > 0)
            {
                value = static_cast<char *>(allocator::kv_malloc(val_len));
                if (value == nullptr)
                {
                    allocator::kv_free(key);
                    return -5;
                }

                if (read_exact(value, val_len) < 0)
                {
                    allocator::kv_free(value);
                    allocator::kv_free(key);
                    return -3;
                }
            }

            int ret = 0;

            if (command == kv_protocal::KVS_SET)
            {
                ret = engine->set(key, key_len, value, val_len, false);
            }
            else if (command == kv_protocal::KVS_DEL)
            {
                ret = engine->del(key, key_len, false);
            }
            else if (command == kv_protocal::KVS_MOD)
            {
                ret = engine->modify(key, key_len, value, val_len, false);
            }
            else
            {
                if (value != nullptr)
                    allocator::kv_free(value);
                allocator::kv_free(key);
                return -4;
            }

            if (value != nullptr)
                allocator::kv_free(value);
            allocator::kv_free(key);

            if (ret != 0)
                return -6;
        }

        return 0;
    }

    //  0  success
    // -1  failed to check whether data folder exists
    // -2  failed to create data folder
    // -3  path exists but is not a directory, or directory check failed
    // -4  failed to check whether data/kv_{idx}.dt exists
    // -5  failed to create new file
    // -6  failed to open file with fstream
    // -7  failed to get current file size / end position
    int StoreEngine::_open_file(int idx)
    {
        _close_file();

        std::error_code ec;
        fs::path folder{STORE_FOLDER};

        if (!fs::exists(folder, ec))
        {
            if (ec)
                return -1;

            fs::create_directories(folder, ec);
            if (ec)
                return -2;
        }

        if (!fs::is_directory(folder, ec) || ec)
            return -3;

        fs::path file_path =
            folder / (std::string(STORE_FILE_ROOT) + "_" + std::to_string(idx) + ".dt");

        bool existed = fs::exists(file_path, ec);
        if (ec)
            return -4;

        if (!existed)
        {
            std::ofstream create_file(file_path, std::ios::binary);
            if (!create_file.is_open())
                return -5;

            create_file.close();
        }

        file_.open(
            file_path,
            std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);

        if (!file_.is_open())
        {
            _close_file();
            return -6;
        }

        if (existed)
        {
            std::streampos pos = file_.tellg();
            if (pos == std::streampos(-1))
                pos = file_.tellp();

            if (pos == std::streampos(-1))
            {
                _close_file();
                return -7;
            }

            file_size = static_cast<size_t>(pos);
        }

        return 0;
    }

    void StoreEngine::_close_file()
    {
        if (file_ && file_.is_open())
        {
            file_.flush();
            file_.close();
            file_.clear();
        }
        file_size = 0;
    }

    int StoreEngine::_switch_new_file()
    {
        _close_file();

        int next_idx = file_idx_ + 1;
        int ret = _open_file(next_idx);
        if (ret < 0)
            return ret;

        file_idx_ = next_idx;
        return 0;
    }

}