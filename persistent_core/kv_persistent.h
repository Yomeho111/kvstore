#ifndef __KV_PERSISTENT_H
#define __KV_PERSISTENT_H

#include <filesystem>
#include <string>
#include <sys/uio.h>
#include <liburing.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include "engine_interface_base.h"
#include "allocator.h"
#include "lockfree_queue.hpp"

namespace kv_persistent
{
    namespace fs = std::filesystem;

    inline constexpr const char *RDB_TMP{"kv_0.rdt.tmp"};

    // Which persistence strategy the server uses. Selected once at startup.
    enum class PersistMode
    {
        NONE, // in-memory only: nothing is loaded at startup, nothing is written
        AOF,
        RDB,
    };

    // Global persistence mode, defaults to in-memory only. Set from main().
    inline PersistMode g_persist_mode = PersistMode::NONE;

    inline constexpr const char *RDB_DEFAULT_PATH{"rdb_data/kv_0.rdt"};

    inline constexpr const char *RDB_FOLDER{"rdb_data/"};

    inline constexpr const size_t IOBUFFER_SIZE{1024 * 512};

    constexpr static inline size_t decimal_digits(size_t n)
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

    template <typename... Args>
    constexpr size_t get_resp_size(Args... lens)
    {
        constexpr size_t argc = sizeof...(Args);

        size_t total_size = 3 + decimal_digits(argc);

        total_size += ((static_cast<size_t>(lens) + decimal_digits(static_cast<size_t>(lens)) + 5) + ...);

        return total_size;
    }

    template <typename... Args>
    int format_resp(char *p, const Args &...vars)
    {

        constexpr size_t argc = sizeof...(Args);
        if constexpr (argc == 0)
            return 0;
        else
        {
            *p++ = '*';

            p = write_size_t(p, argc);

            *p++ = '\r';
            *p++ = '\n';

            auto write_bulk_string = [&p](const auto &str)
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

            (write_bulk_string(vars), ...);

            return 0;
        }
    }

    struct DataField
    {
        char *data;
        size_t size;
    };

    struct ConstDataField
    {
        const char *data;
        size_t size;
    };

    class StoreEngine
    {
        using CommandType = uint16_t;

        // io_uring pipeline depth: up to this many record writes are kept in flight.
        static constexpr unsigned AOF_DEPTH = 64;

    public:
        static StoreEngine &instance();

        int dump_record(CommandType command, const string &key, const string &value);

        int load_record(kv_engine::EngineInterfaceBase *engine);

    private:
        StoreEngine(const StoreEngine &) = delete;
        StoreEngine(StoreEngine &&) = delete;

        StoreEngine &operator=(const StoreEngine &) = delete;
        StoreEngine &operator=(StoreEngine &&) = delete;

        StoreEngine() = default;

        ~StoreEngine();

        int _init();

        int _open_file(int idx);

        int _close_file();

        int _switch_new_file();

        int _load_record(kv_engine::EngineInterfaceBase *engine, const fs::path &file_path);

        int _append(const char *buf, size_t len);

        int _commit_io_uring(int high_watermark);

        int _submit_io_write();

        int _reap_one();

        int _flush();

        int _make_up_dump_buffer(const ConstDataField &command_data, const DataField &buffer_data, const string &key, const string &value, size_t resp_size);

        bool ring_ready_ = false;
        int file_idx_ = 0;
        int fd_ = -1;
        size_t file_size = 0;

        struct write_slot_t
        {
            unsigned seq{0};
            int offset{0};
            char *iov[AOF_DEPTH]{nullptr};
            size_t written_size{0};
        } write_slot;

        int inflight_{0};

        std::mutex mtx_;

        std::condition_variable cv_;

        std::thread sync_thr_;

        std::atomic<bool> is_running_{false};

        base_component::MpscQueue<int> old_fd_que_;

        struct io_uring ring_;
    };

    // RDB snapshot store.
    //
    // Unlike the append-only StoreEngine, this does not log every write. save()
    // takes a point-in-time snapshot of the whole dataset into a single file
    // (rdb_data/kv_0.rdt), reusing the same record layout + crc32 as the AOF log.
    // Writing goes through io_uring; loading uses mmap. The write side is split into
    // a parent part (prepare/commit/discard) and a child part (child_*) because the
    // snapshot is produced by a forked child, so the server keeps serving against a
    // copy-on-write memory image. The child path avoids the custom allocator to stay
    // fork-safe.
    class SnapshotEngine
    {
        // io_uring pipeline depth: up to this many record writes are kept in flight.
        static constexpr unsigned RDB_DEPTH = 64;
        // fixed record header size: [MAGIC][CRC32][KEY_LEN]
        static constexpr size_t RDB_HDR_LEN =
            sizeof(uint32_t) + sizeof(uint32_t) + sizeof(size_t);

        // per-in-flight scratch: one writev (header + key + val_len + value) plus the
        // small fixed buffers it points at. Sized to RDB_DEPTH so a slot is reused only
        // after its write has completed.
        struct WriteSlot
        {
            struct iovec iov[4];
            char header[RDB_HDR_LEN];
            char vlen[sizeof(size_t)];
        };

    public:
        static SnapshotEngine &instance();

        // parent side (around fork)
        int prepare(const string &tmp_file_path);  // create folder + open the temp snapshot file
        int commit(const string &tmp_file_path);   // atomically rename temp -> final, close
        void discard(const string &tmp_file_path); // drop the temp file, close

        // child side (after fork)
        int child_begin();                                       // init io_uring on the inherited fd
        int child_write(const string &key, const string &value); // queue one record (writev, pipelined)
        int child_finish();                                      // drain in-flight writes + fdatasync

        // load (parent, at startup)
        int load(kv_engine::EngineInterfaceBase *engine, const string &file_path_str, bool to_disk = false);

    private:
        SnapshotEngine(const SnapshotEngine &) = delete;
        SnapshotEngine(SnapshotEngine &&) = delete;
        SnapshotEngine &operator=(const SnapshotEngine &) = delete;
        SnapshotEngine &operator=(SnapshotEngine &&) = delete;

        SnapshotEngine() = default;
        ~SnapshotEngine();

        int _reap_one(); // wait for one write completion and validate it

        bool ring_ready_ = false;
        unsigned inflight_ = 0;
        unsigned seq_ = 0;
        int fd_ = -1;
        size_t write_off_ = 0;
        WriteSlot slots_[RDB_DEPTH];

        struct io_uring ring_;
    };
} // namespace kv_persistent

#endif // __KV_PERSISTENT_H