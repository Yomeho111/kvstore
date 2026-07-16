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

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "kv_header.h"
#include "crc32.h"

namespace kv_persistent
{

    constexpr const char *STORE_FOLDER{"data"};
    constexpr const char *STORE_FILE_ROOT{"kv"};
    constexpr size_t MAX_BYTES_PER_FILE{512 * 1024 * 1024};
    constexpr uint32_t MAGIC{0x4B565354};
    constexpr unsigned KVS_URING_DEPTH{8};

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

    int StoreEngine::dump_record(CommandType command, const string &key, const string &value)
    {
        size_t key_len = key.size();
        size_t val_len = value.size();
        if (!(command == kv_protocal::KVS_SET || command == kv_protocal::KVS_DEL || command == kv_protocal::KVS_MOD) || key_len == 0)
            return -1;

        if (fd_ < 0)
        {
            if (_open_file(file_idx_) < 0)
                return -3;
        }

        uint32_t crc = 0;
        size_t buffer_size = sizeof(MAGIC) + sizeof(crc) + sizeof(command) + sizeof(key_len) + key_len + sizeof(val_len) + val_len;

        char *buffer = (char *)allocator::kv_malloc(buffer_size);
        if (!buffer)
            return -2;

        char *cur = buffer;

        // write magic
        memcpy(cur, &MAGIC, sizeof(MAGIC));
        cur += sizeof(MAGIC);

        // reserve space for the crc32; it is filled in once the payload is serialized
        char *crc_slot = cur;
        cur += sizeof(crc);

        // the crc32 covers everything from here to the end of the record (command .. value)
        char *payload = cur;

        // write command
        memcpy(cur, &command, sizeof(command));
        cur += sizeof(command);

        // write key_len
        memcpy(cur, &key_len, sizeof(key_len));
        cur += sizeof(key_len);

        // write key
        memcpy(cur, key.data(), key_len);
        cur += key_len;

        // write value_len
        memcpy(cur, &val_len, sizeof(val_len));
        cur += sizeof(val_len);

        // write value
        if (val_len > 0)
        {
            memcpy(cur, value.data(), val_len);
        }

        // compute the crc32 over the payload and store it right after the magic
        crc = checksum::crc32(payload, buffer_size - sizeof(MAGIC) - sizeof(crc));
        memcpy(crc_slot, &crc, sizeof(crc));

        // append the serialized record to disk through io_uring
        if (_append(buffer, buffer_size) < 0)
        {
            allocator::kv_free(buffer);
            return -5;
        }

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

    int StoreEngine::_append(const char *buf, size_t len)
    {
        size_t written = 0;
        while (written < len)
        {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
                return -1;

            io_uring_prep_write(sqe, fd_, buf + written, static_cast<unsigned>(len - written), file_size + written);

            if (io_uring_submit(&ring_) < 0)
                return -1;

            struct io_uring_cqe *cqe = nullptr;
            if (io_uring_wait_cqe(&ring_, &cqe) < 0)
                return -1;

            int res = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);

            if (res <= 0)
                return -1;

            written += static_cast<size_t>(res);
        }
        return 0;
    }

    int StoreEngine::load_record(kv_engine::EngineInterfaceBase *engine)
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

    int StoreEngine::_load_record(kv_engine::EngineInterfaceBase *engine, const fs::path &file_path)
    {
        if (engine == nullptr)
            return -1;

        int fd = ::open(file_path.c_str(), O_RDONLY);
        if (fd < 0)
            return -1;

        size_t size = static_cast<size_t>(fs::file_size(file_path));
        if (size == 0)
        {
            ::close(fd);
            return 0;
        }

        // map the whole log file read-only and parse it directly from memory
        void *base = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (base == MAP_FAILED)
            return -1;

        const char *data = static_cast<const char *>(base);
        size_t offset = 0;
        int rc = 0;

        // copy `len` bytes out of the mapping at the current offset, bounds-checked
        auto read_at = [&](void *dst, size_t len) -> bool
        {
            if (len > size - offset)
                return false;
            memcpy(dst, data + offset, len);
            offset += len;
            return true;
        };

        while (offset < size)
        {
            uint32_t magic = 0;
            uint32_t stored_crc = 0;
            uint32_t computed_crc = checksum::CRC32_INIT;
            CommandType command = 0;
            size_t key_len = 0;
            size_t val_len = 0;

            if (!read_at(&magic, sizeof(magic)))
            {
                rc = -3;
                break;
            }
            if (magic != MAGIC)
            {
                rc = -4;
                break;
            }

            // the crc32 is stored right after the magic and covers command .. value
            if (!read_at(&stored_crc, sizeof(stored_crc)))
            {
                rc = -3;
                break;
            }

            if (!read_at(&command, sizeof(command)))
            {
                rc = -3;
                break;
            }
            computed_crc = checksum::crc32_update(computed_crc, &command, sizeof(command));

            if (!read_at(&key_len, sizeof(key_len)))
            {
                rc = -3;
                break;
            }
            computed_crc = checksum::crc32_update(computed_crc, &key_len, sizeof(key_len));

            // a valid key must be non-empty and fit within the bytes left in the file
            if (key_len == 0 || key_len > size - offset)
            {
                rc = -4;
                break;
            }
            const char *key = data + offset;
            offset += key_len;
            computed_crc = checksum::crc32_update(computed_crc, key, key_len);

            if (!read_at(&val_len, sizeof(val_len)))
            {
                rc = -3;
                break;
            }
            computed_crc = checksum::crc32_update(computed_crc, &val_len, sizeof(val_len));

            // the value must also fit within the remaining bytes of the file
            if (val_len > size - offset)
            {
                rc = -4;
                break;
            }
            const char *value = nullptr;
            if (val_len > 0)
            {
                value = data + offset;
                offset += val_len;
                computed_crc = checksum::crc32_update(computed_crc, value, val_len);
            }

            // the recomputed crc32 must match the stored one, otherwise the record is corrupt
            if (checksum::crc32_final(computed_crc) != stored_crc)
            {
                rc = -7;
                break;
            }

            // the engine copies key/value into its own storage, so passing pointers
            // into the read-only mapping is safe
            int ret = 0;
            if (command == kv_protocal::KVS_SET)
                ret = engine->set(const_cast<char *>(key), key_len, const_cast<char *>(value), val_len, nullptr, false);
            else if (command == kv_protocal::KVS_DEL)
                ret = engine->del(const_cast<char *>(key), key_len, false);
            else if (command == kv_protocal::KVS_MOD)
                ret = engine->modify(const_cast<char *>(key), key_len, const_cast<char *>(value), val_len, nullptr, false);
            else
            {
                rc = -4;
                break;
            }

            if (ret != 0)
            {
                rc = -6;
                break;
            }
        }

        ::munmap(base, size);
        return rc;
    }

    //  0  success
    // -1  failed to check whether data folder exists
    // -2  failed to create data folder
    // -3  path exists but is not a directory, or directory check failed
    // -5  failed to open data/kv_{idx}.dt
    // -6  failed to initialize io_uring
    // -7  failed to seek to end of file
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

        // set up the io_uring submission ring once; it is reused across file rotations
        if (!ring_ready_)
        {
            if (io_uring_queue_init(KVS_URING_DEPTH, &ring_, 0) < 0)
                return -6;
            ring_ready_ = true;
        }

        int fd = ::open(file_path.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd < 0)
            return -5;

        // resume appending at the end of an existing file
        off_t end = ::lseek(fd, 0, SEEK_END);
        if (end < 0)
        {
            ::close(fd);
            return -7;
        }

        fd_ = fd;
        file_size = static_cast<size_t>(end);
        return 0;
    }

    void StoreEngine::_close_file()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
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

} // namespace kv_persistent