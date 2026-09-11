#ifndef __TCP_SERVER_H
#define __TCP_SERVER_H

#include <unordered_set>
#include <stddef.h>

#include "cache_pool.h"

namespace delta
{
    class TransferServer
    {
    public:
        static TransferServer &instance();

        int broadcast_buffer(const char *buffer, size_t size);

        int connect(__u64 addr_port_key, struct Node *node, size_t offset);

        void close_fd(int fd);

    private:
        TransferServer() = default;
        ~TransferServer();

        TransferServer(const TransferServer &) = delete;
        TransferServer(TransferServer &&) = delete;

        TransferServer &operator=(const TransferServer &) = delete;
        TransferServer &operator=(TransferServer &&) = delete;

        int _connect(__be32 saddr, __u16 sport);

        std::unordered_set<int> fd_set_;
    };
}; // namespace delta

#endif // __TCP_SERVER_H