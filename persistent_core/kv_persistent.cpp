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
#include <utility>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <charconv>

#include "kv_header.h"
#include "crc32.h"
#include "allocator.h"
#include "kv_log.h"

namespace kv_persistent
{

    constexpr const char *STORE_FOLDER{"data"};
    constexpr const char *STORE_FILE_ROOT{"kv"};
    constexpr size_t MAX_BYTES_PER_FILE{512 * 1024 * 1024};
    constexpr uint32_t MAGIC{0x4B565354};
    constexpr unsigned KVS_URING_DEPTH{8};

    constexpr const char *RDB_FILE{"kv_0.rdt"};
    constexpr const char *RDB_TMP{"kv_0.rdt.tmp"};

    static inline size_t decimal_digits(size_t n)
    {
        size_t digits = 1;

        while (n >= 10)
        {
            n /= 10;
            ++digits;
        }

        return digits;
    }

    static inline char *write_size_t(char *p, size_t value)
    {
        auto [ptr, ec] = std::to_chars(p, p + 32, value);
        return ptr;
    }

    size_t get_resp_size(const size_t command_len, size_t key_len, size_t value_len)
    {
        // "*3\r\n"
        size_t total_size = 14 + command_len + key_len + decimal_digits(command_len) + decimal_digits(key_len);

        if (value_len > 0)
            total_size += decimal_digits(value_len) + 5 + value_len;

        return total_size;
    }

    int format_resp(
        const char *command,
        const size_t command_size,
        const string &key,
        const string &value,
        char *p)
    {
        /*
         * RESP:
         *
         * *3\r\n
         * $<command_len>\r\n
         * command\r\n
         * $<key_len>\r\n
         * key\r\n
         * $<value_len>\r\n
         * value\r\n
         */

        if (command_size == 0 || key.size() == 0)
            return -1;

        if (value.size() == 0)
            memcpy(p, "*2\r\n", 4);
        else
            memcpy(p, "*3\r\n", 4);
        p += 4;

        auto write_bulk_string = [&p](const string &str)
        {
            *p++ = '$';

            p = write_size_t(p, str.size());

            *p++ = '\r';
            *p++ = '\n';

            memcpy(p, str.data(), str.size());
            p += str.size();

            *p++ = '\r';
            *p++ = '\n';
        };

        write_bulk_string(command);
        write_bulk_string(key);
        if (!value.empty())
            write_bulk_string(value);

        return 0;
    }

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

    // Parses "*<n>\r\n" followed by n bulk strings. The fields point into `data`.
    // Returns the number of fields parsed, or -1 when the payload is malformed.
    static int parse_resp_array(const char *data, size_t size, ConstDataField *out, int max_fields)
    {
        size_t offset = 0;

        // reads a decimal number terminated by CRLF and steps past the CRLF
        auto read_length = [&](size_t *value) -> bool
        {
            size_t begin = offset;
            while (offset < size && data[offset] != '\r')
                ++offset;

            if (offset == begin || offset + 1 >= size || data[offset + 1] != '\n')
                return false;

            auto [ptr, ec] = std::from_chars(data + begin, data + offset, *value);
            if (ec != std::errc() || ptr != data + offset)
                return false;

            offset += 2;
            return true;
        };

        if (offset >= size || data[offset] != '*')
            return -1;
        ++offset;

        size_t count = 0;
        if (!read_length(&count) || count == 0 || count > static_cast<size_t>(max_fields))
            return -1;

        for (size_t i = 0; i < count; ++i)
        {
            if (offset >= size || data[offset] != '$')
                return -1;
            ++offset;

            size_t len = 0;
            if (!read_length(&len) || len > size - offset)
                return -1;

            out[i].data = data + offset;
            out[i].size = len;
            offset += len;

            if (offset + 1 >= size || data[offset] != '\r' || data[offset + 1] != '\n')
                return -1;
            offset += 2;
        }

        return static_cast<int>(count);
    }

    static int command_from_str(const char *data, size_t len)
    {
        for (size_t i = 0; i < sizeof(kv_protocal::command_str) / sizeof(kv_protocal::command_str[0]); ++i)
        {
            const char *name = kv_protocal::command_str[i];
            if (strlen(name) == len && memcmp(name, data, len) == 0)
                return static_cast<int>(i);
        }
        return kv_protocal::KVS_INVALID;
    }

    StoreEngine &StoreEngine::instance()
    {
        static StoreEngine engine;
        static int ret = engine._init();
        if (ret < 0)
        {
            KV_ERROR("StoreEngine init error");
            std::exit(-1);
        }
        return engine;
    }

    int StoreEngine::_init()
    {
        int i{0};
        bool expected = false;
        for (; i < AOF_DEPTH; i++)
        {
            write_slot.iov[i] = (char *)allocator::kv_malloc(IOBUFFER_SIZE);
            if (!write_slot.iov[i])
            {
                goto clean;
            }
        }
        write_slot.offset = 0;
        write_slot.seq = 0;
        write_slot.written_size = 0;

        if (!ring_ready_)
        {
            if (io_uring_queue_init(AOF_DEPTH, &ring_, 0) < 0)
                goto clean;
            ring_ready_ = true;
        }

        if (!is_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed))
            goto clean;

        sync_thr_ = std::thread(
            [this]
            {
                while (is_running_.load(std::memory_order_acquire))
                {
                    int old_fd{-1};
                    {
                        std::unique_lock lk{mtx_};
                        cv_.wait(lk, [this, &old_fd]
                                 { return old_fd_que_.dequeue(old_fd) || !is_running_.load(std::memory_order_acquire); });
                    }
                    do
                    {
                        if (old_fd >= 0)
                        {
                            ::fdatasync(old_fd);
                            ::close(old_fd);
                        }
                    } while (old_fd_que_.dequeue(old_fd));
                }
            });

        return 0;

    clean:

        for (i -= 1; i >= 0; i--)
        {
            allocator::kv_free(write_slot.iov[i]);
            write_slot.iov[i] = nullptr;
        }

        return -1;
    }

    StoreEngine::~StoreEngine()
    {
        _close_file();

        is_running_.store(false, std::memory_order_release);

        cv_.notify_all();

        if (sync_thr_.joinable())
            sync_thr_.join();

        if (ring_ready_)
        {
            io_uring_queue_exit(&ring_);
            ring_ready_ = false;
        }

        for (int i = 0; i < AOF_DEPTH; i++)
        {
            if (write_slot.iov[i])
            {
                allocator::kv_free(write_slot.iov[i]);
                write_slot.iov[i] = nullptr;
            }
        }
    }

    int StoreEngine::dump_record(CommandType command, const string &key, const string &value)
    {
        size_t key_len = key.size();
        if (!(command == kv_protocal::KVS_SET || command == kv_protocal::KVS_DEL || command == kv_protocal::KVS_MOD) || key_len == 0)
            return -1;

        if (fd_ < 0)
        {
            if (_open_file(file_idx_) < 0)
                return -2;
        }
        const char *command_str = kv_protocal::command_str[command];
        size_t command_size = strnlen(command_str, 32);
        size_t resp_size = get_resp_size(command_size, key.size(), value.size());
        size_t buffer_size = sizeof(MAGIC) + sizeof(uint32_t) + sizeof(resp_size) + resp_size;

        if (IOBUFFER_SIZE - write_slot.offset >= buffer_size)
        {
            _make_up_dump_buffer({command_str, command_size}, {write_slot.iov[write_slot.seq] + write_slot.offset, buffer_size}, key, value, resp_size);
            write_slot.offset += buffer_size;

            if (write_slot.offset == IOBUFFER_SIZE)
            {
                if (_flush() < 0)
                    return -3;
            }
        }
        else
        {
            if (_flush() < 0)
                return -3;

            if (IOBUFFER_SIZE - write_slot.offset >= buffer_size)
            {
                _make_up_dump_buffer({command_str, command_size}, {write_slot.iov[write_slot.seq] + write_slot.offset, buffer_size}, key, value, resp_size);
                write_slot.offset += buffer_size;

                if (write_slot.offset == IOBUFFER_SIZE)
                {
                    if (_flush() < 0)
                        return -3;
                }
            }
            else
            {
                char *buffer = (char *)allocator::kv_malloc(buffer_size);
                if (!buffer)
                    return -4;

                _make_up_dump_buffer({command_str, command_size}, {buffer, buffer_size}, key, value, resp_size);

                if (_append(buffer, buffer_size) < 0)
                {
                    allocator::kv_free(buffer);
                    return -5;
                }
                allocator::kv_free(buffer);
            }
        }

        file_size += buffer_size;

        if (_switch_new_file() < 0)
        {
            return -6;
        }

        return 0;
    }

    int StoreEngine::_make_up_dump_buffer(const ConstDataField &command_data, const DataField &buffer_data, const string &key, const string &value, size_t resp_size)
    {
        uint32_t crc{0};
        char *cur = buffer_data.data;
        memcpy(cur, &MAGIC, sizeof(MAGIC));
        cur += sizeof(MAGIC);

        // reserve space for the crc32; it is filled in once the payload is serialized
        char *crc_slot = cur;
        cur += sizeof(crc);

        // the crc32 covers everything from here to the end of the record (command .. value)
        char *payload = cur;

        // write resp_size
        memcpy(cur, &resp_size, sizeof(resp_size));
        cur += sizeof(resp_size);

        // format resp
        format_resp(command_data.data, command_data.size, key, value, cur);

        // compute the crc32 over the payload and store it right after the magic
        crc = checksum::crc32(payload, buffer_data.size - sizeof(MAGIC) - sizeof(crc));
        memcpy(crc_slot, &crc, sizeof(crc));
        return 0;
    }

    int StoreEngine::_append(const char *buf, size_t len)
    {
        size_t submitted{0};
        size_t left{len};
        while (submitted < len)
        {
            size_t space = IOBUFFER_SIZE - write_slot.offset;
            size_t write_len = space > left ? left : space;
            memcpy(write_slot.iov[write_slot.seq] + write_slot.offset, buf + submitted, write_len);

            submitted += write_len;
            left -= write_len;
            write_slot.offset += write_len;

            if (write_slot.offset == IOBUFFER_SIZE)
            {
                if (_flush() < 0)
                    return -1;
            }
        }
        return 0;
    }

    int StoreEngine::_submit_io_write()
    {
        if (write_slot.offset == 0)
            return 0;

        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            if (io_uring_submit(&ring_) < 0)
                return -2;
            sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
                return -2;
        }

        io_uring_prep_write(sqe, fd_, write_slot.iov[write_slot.seq], write_slot.offset, write_slot.written_size);

        io_uring_sqe_set_data64(sqe, write_slot.offset);

        write_slot.written_size += write_slot.offset;
        inflight_++;
        write_slot.seq = (write_slot.seq + 1) & (AOF_DEPTH - 1);
        write_slot.offset = 0;
        return 0;
    }

    int StoreEngine::_commit_io_uring(int high_watermark)
    {
        if (!ring_ready_)
            return 0;

        if (io_uring_submit(&ring_) < 0)
            return -1;
        while (inflight_ > high_watermark)
            if (_reap_one() < 0)
                return -2;
        return 0;
    }

    int StoreEngine::_reap_one()
    {
        struct io_uring_cqe *cqe = nullptr;
        if (io_uring_wait_cqe(&ring_, &cqe) < 0)
            return -1;

        int res = cqe->res;
        size_t expected = io_uring_cqe_get_data64(cqe);
        io_uring_cqe_seen(&ring_, cqe);
        inflight_--;

        // a write to a regular file writes everything unless it errored
        if (res < 0 || static_cast<size_t>(res) != expected)
            return -1;
        return 0;
    }

    int StoreEngine::_flush()
    {
        if (inflight_ >= AOF_DEPTH - 1)
        {
            if (_commit_io_uring(AOF_DEPTH / 2) < 0)
                return -1;
        }
        if (_submit_io_write())
            return -2;
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
            size_t resp_size = 0;

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

            if (!read_at(&stored_crc, sizeof(stored_crc)))
            {
                rc = -3;
                break;
            }

            // the crc32 covers the length prefix together with the RESP payload
            const char *payload = data + offset;
            if (!read_at(&resp_size, sizeof(resp_size)))
            {
                rc = -3;
                break;
            }

            if (resp_size == 0 || resp_size > size - offset)
            {
                rc = -4;
                break;
            }
            const char *resp = data + offset;
            offset += resp_size;

            if (checksum::crc32(payload, sizeof(resp_size) + resp_size) != stored_crc)
            {
                rc = -7;
                break;
            }

            // [0] command, [1] key, [2] value (absent when the value is empty)
            ConstDataField fields[3];
            int field_count = parse_resp_array(resp, resp_size, fields, 3);
            if (field_count < 2 || fields[1].size == 0)
            {
                rc = -4;
                break;
            }

            const char *key = fields[1].data;
            size_t key_len = fields[1].size;
            const char *value = field_count > 2 ? fields[2].data : nullptr;
            size_t val_len = field_count > 2 ? fields[2].size : 0;

            // the engine copies key/value into its own storage, so passing pointers
            // into the read-only mapping is safe
            int ret = 0;
            switch (command_from_str(fields[0].data, fields[0].size))
            {
                case kv_protocal::KVS_SET:
                    ret = engine->set(key, key_len, value, val_len, nullptr, false);
                    break;
                case kv_protocal::KVS_DEL:
                    ret = engine->del(key, key_len, false);
                    break;
                case kv_protocal::KVS_MOD:
                    ret = engine->modify(key, key_len, value, val_len, nullptr, false);
                    break;
                default:
                    rc = -4;
                    break;
            }

            if (rc != 0)
                break;

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
        write_slot.written_size = file_size;
        return 0;
    }

    int StoreEngine::_close_file()
    {
        if (fd_ >= 0)
        {
            if (_submit_io_write() < 0)
                return -1;
            if (_commit_io_uring(0))
                return -2;

            int old_fd = std::exchange(fd_, -1);
            old_fd_que_.enqueue(old_fd);
            cv_.notify_one();
        }

        file_size = 0;
        write_slot.written_size = 0;
        return 0;
    }

    int StoreEngine::_switch_new_file()
    {
        if (file_size > MAX_BYTES_PER_FILE)
        {
            _close_file();

            int next_idx = file_idx_ + 1;
            int ret = _open_file(next_idx);
            if (ret < 0)
                return ret;

            file_idx_ = next_idx;
        }
        return 0;
    }

    // ------------------------------ RDB snapshot ------------------------------

    SnapshotEngine &SnapshotEngine::instance()
    {
        static SnapshotEngine engine;
        return engine;
    }

    SnapshotEngine::~SnapshotEngine()
    {
        if (ring_ready_)
        {
            io_uring_queue_exit(&ring_);
            ring_ready_ = false;
        }
        if (fd_ >= 0)
            ::close(fd_);
    }

    int SnapshotEngine::prepare()
    {
        std::error_code ec;
        fs::path folder{RDB_FOLDER};

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

        fs::path tmp_path = folder / RDB_TMP;
        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return -5;

        fd_ = fd;
        write_off_ = 0;
        return 0;
    }

    int SnapshotEngine::child_begin()
    {
        if (io_uring_queue_init(RDB_DEPTH, &ring_, 0) < 0)
            return -6;
        ring_ready_ = true;
        inflight_ = 0;
        seq_ = 0;
        return 0;
    }

    int SnapshotEngine::_reap_one()
    {
        struct io_uring_cqe *cqe = nullptr;
        if (io_uring_wait_cqe(&ring_, &cqe) < 0)
            return -1;

        int res = cqe->res;
        size_t expected = reinterpret_cast<size_t>(io_uring_cqe_get_data(cqe));
        io_uring_cqe_seen(&ring_, cqe);
        inflight_--;

        // a writev to a regular file writes everything unless it errored
        if (res < 0 || static_cast<size_t>(res) != expected)
            return -1;
        return 0;
    }

    int SnapshotEngine::child_write(const string &key, const string &value)
    {
        size_t key_len = key.size();
        size_t val_len = value.size();
        if (key_len == 0)
            return -1;

        // crc32 over command .. value, computed incrementally without allocating
        uint32_t crc = checksum::CRC32_INIT;
        crc = checksum::crc32_update(crc, &key_len, sizeof(key_len));
        crc = checksum::crc32_update(crc, key.data(), key_len);
        crc = checksum::crc32_update(crc, &val_len, sizeof(val_len));
        if (val_len > 0)
            crc = checksum::crc32_update(crc, value.data(), val_len);
        crc = checksum::crc32_final(crc);

        // bound the number of in-flight writes; submit what is queued, then drain about
        // half so submits/completions happen in batches rather than per record
        if (inflight_ >= RDB_DEPTH)
        {
            if (io_uring_submit(&ring_) < 0)
                return -1;
            while (inflight_ > RDB_DEPTH / 2)
                if (_reap_one() < 0)
                    return -1;
        }

        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            if (io_uring_submit(&ring_) < 0)
                return -1;
            sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
                return -1;
        }

        // a slot is reused only after its previous write completed (inflight_ < DEPTH)
        WriteSlot *slot = &slots_[seq_ % RDB_DEPTH];

        char *cur = slot->header;
        memcpy(cur, &MAGIC, sizeof(MAGIC));
        cur += sizeof(MAGIC);
        memcpy(cur, &crc, sizeof(crc));
        cur += sizeof(crc);
        memcpy(cur, &key_len, sizeof(key_len));
        memcpy(slot->vlen, &val_len, sizeof(val_len));

        // one writev per record: [header][key][val_len][value] -> a single SQE
        int n = 0;
        slot->iov[n].iov_base = slot->header;
        slot->iov[n].iov_len = RDB_HDR_LEN;
        ++n;
        slot->iov[n].iov_base = const_cast<char *>(key.data());
        slot->iov[n].iov_len = key_len;
        ++n;
        slot->iov[n].iov_base = slot->vlen;
        slot->iov[n].iov_len = sizeof(val_len);
        ++n;
        if (val_len > 0)
        {
            slot->iov[n].iov_base = const_cast<char *>(value.data());
            slot->iov[n].iov_len = val_len;
            ++n;
        }

        size_t rec_len = RDB_HDR_LEN + key_len + sizeof(val_len) + val_len;
        io_uring_prep_writev(sqe, fd_, slot->iov, n, write_off_);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(rec_len));

        write_off_ += rec_len;
        ++seq_;
        ++inflight_;
        return 0;
    }

    int SnapshotEngine::child_finish()
    {
        // flush queued writes, wait for every in-flight completion, then fsync
        if (io_uring_submit(&ring_) < 0)
            return -1;
        while (inflight_ > 0)
            if (_reap_one() < 0)
                return -1;

        if (fd_ >= 0 && ::fdatasync(fd_) < 0)
            return -1;
        return 0;
    }

    int SnapshotEngine::commit()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }

        std::error_code ec;
        fs::path folder{RDB_FOLDER};
        fs::rename(folder / RDB_TMP, folder / RDB_FILE, ec);
        if (ec)
            return -1;
        return 0;
    }

    void SnapshotEngine::discard()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }

        std::error_code ec;
        fs::remove(fs::path{RDB_FOLDER} / RDB_TMP, ec);
    }

    int SnapshotEngine::load(kv_engine::EngineInterfaceBase *engine, const string &file_path_str, bool to_disk)
    {
        if (engine == nullptr)
            return -1;

        std::error_code ec;
        fs::path file_path{file_path_str};

        if (!fs::exists(file_path, ec))
        {
            if (ec)
                return -1;
            return 0; // no snapshot yet
        }

        int fd = ::open(file_path.c_str(), O_RDONLY);
        if (fd < 0)
            return -1;

        size_t size = static_cast<size_t>(fs::file_size(file_path));
        if (size == 0)
        {
            ::close(fd);
            return 0;
        }

        void *base = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (base == MAP_FAILED)
            return -1;

        const char *data = static_cast<const char *>(base);
        size_t offset = 0;
        int rc = 0;

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

            if (!read_at(&stored_crc, sizeof(stored_crc)))
            {
                rc = -3;
                break;
            }

            if (!read_at(&key_len, sizeof(key_len)))
            {
                rc = -3;
                break;
            }
            computed_crc = checksum::crc32_update(computed_crc, &key_len, sizeof(key_len));

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

            if (checksum::crc32_final(computed_crc) != stored_crc)
            {
                rc = -7;
                break;
            }

            int ret = 0;

            ret = engine->set(const_cast<char *>(key), key_len, const_cast<char *>(value), val_len, nullptr, to_disk);

            if (ret != 0)
            {
                rc = -6;
                break;
            }
        }

        ::munmap(base, size);
        return rc;
    }

} // namespace kv_persistent