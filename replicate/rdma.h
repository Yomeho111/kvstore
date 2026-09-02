#ifndef __RDMA_H
#define __RDMA_H

#include <rdma/rdma_cma.h>
#include <stddef.h>
#include <stdint.h>

#define RDMA_SERVER_PORT 20000
#define RDMA_CLIENT_PORT 20001
#define RDMA_BUFFER_SIZE 1024 * 1024 * 128
#define SMALL_BUFFER_SIZE 64
#define RDMA_CQ_DEPTH 4096
#define MAGIC 0x234324
#define RDMA_TIMEOUT_MS 100

namespace replicate
{
    struct conn_manager_t
    {
        struct ibv_mr *recv_mr;
        struct ibv_mr *send_mr;

        char *rbuff;
        char *sbuff;

        size_t rbuff_size;
        size_t sbuff_size;
    };

    struct packet_info
    {
        int magic;
        int total;
    };

} // namespace replicate

#endif // __RDMA_H