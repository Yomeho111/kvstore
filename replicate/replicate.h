#ifndef __REPLICATE_H
#define __REPLICATE_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <vector>

#include "rdma.h"
#include "netbuffer.h"
#include "allocator.h"

#define MAX_REP_BUFFER_SIZE 64

#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)

#define COMMIT_FUNC kv_replication_commit

struct delta_bpf;
struct bpf_link;
struct ring_buffer;

namespace replicate
{

    // Must not be `inline static`: `static` forces internal linkage, giving every
    // translation unit its own copy, so the engine never sees main()'s value.
    inline bool g_replicate{false};

    struct Node
    {
        uint16_t command;
        size_t key_len;
        size_t value_len;
        char *key;
        char *value;
    };

    class FullSyncObject
    {
    public:
        static FullSyncObject &instance();
        int get_full_sync(Netbuffer *net_buffer);

    private:
        FullSyncObject() = default;
        ~FullSyncObject() = default;

        FullSyncObject(const FullSyncObject &) = delete;
        FullSyncObject(FullSyncObject &&) = delete;

        FullSyncObject &operator=(const FullSyncObject &) = delete;
        FullSyncObject &operator=(FullSyncObject &&) = delete;
    };

    class DeltaSyncObject
    {
    public:
        static DeltaSyncObject &instance();

        int insert_node(uint16_t command, size_t key_len, size_t value_len, const char *key, const char *value);

        int get_delta_sync(Netbuffer *net_buffer);

        void clear_sequence();

        void insert_sequence(int sequence);

        int init();

        int start();

        int ebpf_worker();

        void stop();

        bool empty();

        bool is_clear()
        {
            return is_cleared.load(std::memory_order_acquire);
        }

        void clear()
        {
            is_cleared.store(false, std::memory_order_relaxed);
        }

    private:
        DeltaSyncObject() = default;
        ~DeltaSyncObject();

        DeltaSyncObject(const DeltaSyncObject &) = delete;
        DeltaSyncObject(DeltaSyncObject &&) = delete;

        DeltaSyncObject &operator=(const DeltaSyncObject &) = delete;
        DeltaSyncObject &operator=(DeltaSyncObject &&) = delete;

        static char *get_self_exe();

        void cleanup_bpf();

        /*
         * libbpf requires a plain C-style callback.
         *
         * Therefore this function must be static.
         */
        static int handle_event_static(
            void *ctx,
            void *data,
            size_t data_sz);

        /*
         * Actual member callback.
         */
        int handle_event(
            void *data,
            size_t data_sz);

        std::vector<int, allocator::MyAllocator<int>> sequence_vec_;
        Node delta_ring_buff_[MAX_REP_BUFFER_SIZE]{0};
        int index_{0};

        std::thread ebpf_thr_;

        std::mutex mtx_;

        struct ::delta_bpf *skel_ = nullptr;
        struct ::bpf_link *uprobe_link_ = nullptr;
        struct ::ring_buffer *rb_ = nullptr;
        std::atomic<bool> initialized_{false};
        std::atomic<bool> running_{false};
        std::atomic<bool> is_cleared{false};
    };

    // This is the server for master
    class MasterServer
    {
    public:
        static MasterServer &instance();
        int init();

        int start();

        void stop();

        int sync_worker();

        void notify();

    private:
        MasterServer() = default;
        ~MasterServer();

        int _listen();

        int _send();

        MasterServer(const MasterServer &) = delete;
        MasterServer(MasterServer &&) = delete;

        MasterServer &operator=(const MasterServer &) = delete;
        MasterServer &operator=(MasterServer &&) = delete;

        // data member for rdma
        struct rdma_event_channel *channel_{nullptr};
        struct rdma_cm_id *cm_listen_id_{nullptr};
        struct rdma_cm_id *cm_client_id_{nullptr};
        struct ibv_pd *pd_{nullptr};
        struct ibv_cq *cq_{nullptr};

        Netbuffer *net_buffer_{nullptr};
        std::mutex mtx_;
        std::condition_variable cv_;

        std::atomic_bool running_{false};

        std::thread sync_thr;
    };

    // This is the server for slave
    class SlaveServer
    {
    public:
        static SlaveServer &instance(uint16_t port = 0, const char *ip = nullptr);

        int init();

        void server_loop();

    private:
        SlaveServer(uint16_t port, const char *ip) : port_(port), ip_(ip) {};
        ~SlaveServer();

        SlaveServer(const SlaveServer &) = delete;
        SlaveServer(SlaveServer &&) = delete;

        SlaveServer &operator=(const SlaveServer &) = delete;
        SlaveServer &operator=(SlaveServer &&) = delete;

        int _recv();

        // data member for rdma
        struct rdma_event_channel *channel_{nullptr};
        struct rdma_cm_id *cm_id_{nullptr};
        struct ibv_pd *pd_{nullptr};
        struct ibv_cq *cq_{nullptr};
        const char *ip_;
        uint16_t port_;

        Netbuffer *net_buffer_{nullptr};
    };
} // namespace replicate

// The eBPF uprobe traps this function's entry, so the call site must survive
// optimisation. `noinline` alone is not enough: the body is empty, so GCC's
// interprocedural analysis proves the call has no effect and deletes it.
// `noipa` blocks that analysis.
extern "C" __attribute__((noipa, visibility("default"))) void COMMIT_FUNC(int sequence);

#endif // __REPLICATE_H