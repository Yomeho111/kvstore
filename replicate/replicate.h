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
#include "allocator.h"

#define MAX_REP_BUFFER_SIZE 64

#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)

namespace replicate
{

    inline constexpr const char *SLAVE_TMP{".tmp"};

    inline bool g_is_master{false};

    // This is the server for master
    class MasterServer
    {
    public:
        int init();

        MasterServer(const char *ip, uint16_t port) : ip_(ip), port_(port) {}
        ~MasterServer();

        int send(const char *file_path);

        void stop();

    private:
        MasterServer(const MasterServer &) = delete;
        MasterServer(MasterServer &&) = delete;

        MasterServer &operator=(const MasterServer &) = delete;
        MasterServer &operator=(MasterServer &&) = delete;

        // data member for rdma
        struct rdma_event_channel *channel_{nullptr};
        struct rdma_cm_id *cm_id_{nullptr};
        struct ibv_pd *pd_{nullptr};
        struct ibv_cq *send_cq_{nullptr};
        struct ibv_cq *recv_cq_{nullptr};

        const char *ip_;
        uint16_t port_;
    };

    // This is the server for slave
    class SlaveServer
    {
    public:
        int init();

        int recv(const char *file_path);

        int listen();

        int stop();

        const char *get_ip() const
        {
            return ip_;
        }

        uint16_t get_port() const
        {
            return port_;
        }

        SlaveServer(const char *ip, uint16_t port) : ip_(ip), port_(port) {}
        ~SlaveServer();

    private:
        SlaveServer(const SlaveServer &) = delete;
        SlaveServer(SlaveServer &&) = delete;

        SlaveServer &operator=(const SlaveServer &) = delete;
        SlaveServer &operator=(SlaveServer &&) = delete;

        // data member for rdma
        struct rdma_event_channel *channel_{nullptr};
        struct rdma_cm_id *cm_listen_id_{nullptr};
        struct rdma_cm_id *cm_client_id_{nullptr};
        struct ibv_pd *pd_{nullptr};
        struct ibv_cq *send_cq_{nullptr};
        struct ibv_cq *recv_cq_{nullptr};

        const char *ip_;
        uint16_t port_{0};
    };
} // namespace replicate

#endif // __REPLICATE_H