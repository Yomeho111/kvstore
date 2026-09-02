#ifndef __KV_PERSISTENT_H
#define __KV_PERSISTENT_H

#include <filesystem>
#include <string>
#include <sys/uio.h>
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

    // Which persistence strategy the server uses. Selected once at startup.
    enum class PersistMode
    {
        NONE, // in-memory only: nothing is loaded at startup, nothing is written
        AOF,
        RDB,
    };

    // Global persistence mode, defaults to in-memory only. Set from main().
    inline PersistMode g_persist_mode = PersistMode::NONE;

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
        using CommandType = uint16_t;

        // io_uring pipeline depth: up to this many record writes are kept in flight.
        static constexpr unsigned RDB_DEPTH = 64;
        // fixed record header size: [MAGIC][CRC32][COMMAND][KEY_LEN]
        static constexpr size_t RDB_HDR_LEN =
            sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(size_t);

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
        SnapshotEngine() = default;
        ~SnapshotEngine();

        // parent side (around fork)
        int prepare();  // create folder + open the temp snapshot file
        int commit();   // atomically rename temp -> final, close
        void discard(); // drop the temp file, close

        // child side (after fork)
        int child_begin();                                       // init io_uring on the inherited fd
        int child_write(const string &key, const string &value); // queue one record (writev, pipelined)
        int child_finish();                                      // drain in-flight writes + fdatasync

        // load (parent, at startup)
        int load(kv_engine::EngineInterfaceBase *engine);

    private:
        SnapshotEngine(const SnapshotEngine &) = delete;
        SnapshotEngine(SnapshotEngine &&) = delete;
        SnapshotEngine &operator=(const SnapshotEngine &) = delete;
        SnapshotEngine &operator=(SnapshotEngine &&) = delete;

        int _reap_one(); // wait for one write completion and validate it

        int fd_ = -1;
        size_t write_off_ = 0;
        struct io_uring ring_;
        bool ring_ready_ = false;
        unsigned inflight_ = 0;
        unsigned seq_ = 0;
        WriteSlot slots_[RDB_DEPTH];
    };
} // namespace kv_persistent

#endif // __KV_PERSISTENT_H