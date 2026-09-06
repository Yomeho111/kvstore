#include "replicate.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>

#include "allocator.h"
#include "kv_log.h"

static int rdma_wait_cm_event(struct rdma_event_channel *channel, enum rdma_cm_event_type expect, struct rdma_cm_event **out)
{

    int ret = 0;
    struct rdma_cm_event *event;
    if (0 != rdma_get_cm_event(channel, &event))
    {
        KV_ERROR("rdma_get_cm_event: %s", strerror(errno));
        return -1;
    }

    if (event->event != expect)
    { // failed

        KV_ERROR("unexpected RDMA CM event %s, expected %s",
                 rdma_event_str(event->event), rdma_event_str(expect));
        ret = -1;
        goto finished;
    }

    if (out)
    {
        *out = event;
        return 0;
    }

finished:
    rdma_ack_cm_event(event);
    return ret;
}

static int rdma_create_queuepair(struct rdma_cm_id *id, struct ibv_pd **pd, struct ibv_cq **send_cq, struct ibv_cq **recv_cq)
{
    *pd = NULL;
    *send_cq = NULL;
    *recv_cq = NULL;

    struct ibv_pd *new_pd = ibv_alloc_pd(id->verbs);
    if (NULL == new_pd)
    {
        KV_ERROR("ibv_alloc_pd: %s", strerror(errno));
        return -1;
    }

    struct ibv_cq *new_send_cq = ibv_create_cq(id->verbs, RDMA_CQ_DEPTH, NULL, NULL, 0);
    struct ibv_cq *new_recv_cq = ibv_create_cq(id->verbs, RDMA_CQ_DEPTH, NULL, NULL, 0);

    int ret = -1;
    if (NULL == new_send_cq || NULL == new_recv_cq)
    {
        KV_ERROR("ibv_create_cq: %s", strerror(errno));
    }
    else
    {
        struct ibv_qp_init_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.send_cq = new_send_cq;
        attr.recv_cq = new_recv_cq;
        attr.qp_type = IBV_QPT_RC;
        attr.cap.max_send_wr = RDMA_CQ_DEPTH;
        attr.cap.max_recv_wr = RDMA_CQ_DEPTH;
        attr.cap.max_send_sge = 1;
        attr.cap.max_recv_sge = 1;

        ret = rdma_create_qp(id, new_pd, &attr);
        if (ret)
            KV_ERROR("rdma_create_qp: %s", strerror(errno));
    }

    if (ret)
    {
        if (new_send_cq)
            ibv_destroy_cq(new_send_cq);
        if (new_recv_cq)
            ibv_destroy_cq(new_recv_cq);
        ibv_dealloc_pd(new_pd);
        return -1;
    }

    *pd = new_pd;
    *send_cq = new_send_cq;
    *recv_cq = new_recv_cq;

    return 0;
}

static int rdma_free_buffer(struct rdma_cm_id *cm_id);

static int rdma_destroy_connection(struct rdma_cm_id *id, struct ibv_pd *pd, struct ibv_cq *send_cq, struct ibv_cq *recv_cq)
{
    if (!id)
        return -1;

    // The QP has to go first: its posted work requests still reference the MRs.
    if (id->qp)
        rdma_destroy_qp(id);

    rdma_free_buffer(id);

    if (send_cq)
        ibv_destroy_cq(send_cq);
    if (recv_cq)
        ibv_destroy_cq(recv_cq);
    if (pd)
        ibv_dealloc_pd(pd);

    return 0;
}

static int rdma_alloc_buffer(struct rdma_cm_id *cm_id, struct ibv_pd *pd, size_t recv_size, size_t send_size)
{

    replicate::conn_manager_t *cm = (replicate::conn_manager_t *)allocator::kv_malloc(sizeof(replicate::conn_manager_t));
    if (!cm)
        return -1;
    memset(cm, 0, sizeof(replicate::conn_manager_t));

    cm->rbuff = (char *)allocator::kv_malloc(recv_size);
    cm->sbuff = (char *)allocator::kv_malloc(send_size);

    if (cm->rbuff && cm->sbuff)
    {
        cm->send_mr = ibv_reg_mr(pd, cm->sbuff, send_size, 0);
        cm->recv_mr = ibv_reg_mr(pd, cm->rbuff, recv_size, IBV_ACCESS_LOCAL_WRITE);
    }

    if (!cm->send_mr || !cm->recv_mr)
    {
        KV_ERROR("ibv_reg_mr: %s", strerror(errno));
        if (cm->send_mr)
            ibv_dereg_mr(cm->send_mr);
        if (cm->recv_mr)
            ibv_dereg_mr(cm->recv_mr);
        allocator::kv_free(cm->rbuff);
        allocator::kv_free(cm->sbuff);
        allocator::kv_free(cm);
        return -1;
    }

    cm->rbuff_size = recv_size;
    cm->sbuff_size = send_size;

    cm_id->context = cm;
    return 0;
}

static int rdma_free_buffer(struct rdma_cm_id *cm_id)
{
    if (!cm_id || !cm_id->context)
        return -1;
    replicate::conn_manager_t *cm = (replicate::conn_manager_t *)cm_id->context;

    ibv_dereg_mr(cm->send_mr);
    ibv_dereg_mr(cm->recv_mr);

    allocator::kv_free(cm->rbuff);
    allocator::kv_free(cm->sbuff);
    allocator::kv_free(cm);

    cm_id->context = NULL;

    return 0;
}

static int rdma_post_recv(struct rdma_cm_id *cm_id)
{

    replicate::conn_manager_t *cm = (replicate::conn_manager_t *)cm_id->context;

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(struct ibv_sge));
    sge.addr = (uintptr_t)cm->rbuff;
    sge.length = cm->rbuff_size;
    sge.lkey = cm->recv_mr->lkey;

    struct ibv_recv_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(struct ibv_recv_wr));
    wr.wr_id = (uintptr_t)cm;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    int ret = ibv_post_recv(cm_id->qp, &wr, &bad);
    if (ret)
        KV_ERROR("ibv_post_recv: %s", strerror(ret));

    return ret;
}

static int rdma_post_send(struct rdma_cm_id *cm_id, size_t length)
{

    replicate::conn_manager_t *cm = (replicate::conn_manager_t *)cm_id->context;

    if (length > cm->sbuff_size)
    {
        KV_ERROR("ibv_post_send: %zu bytes exceed the %zu byte send buffer",
                 length, cm->sbuff_size);
        return -1;
    }

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(struct ibv_sge));
    sge.addr = (uintptr_t)cm->sbuff;
    sge.length = length;
    sge.lkey = cm->send_mr->lkey;

    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(struct ibv_send_wr));
    wr.wr_id = (uintptr_t)cm;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    int ret = ibv_post_send(cm_id->qp, &wr, &bad);
    if (ret)
        KV_ERROR("ibv_post_send: %s", strerror(ret));

    return ret;
}

static int rdma_poll_wc(struct ibv_cq *cq, struct ibv_wc *out)
{

    while (1)
    {

        struct ibv_wc wc;
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0)
        {
            return -1;
        }
        else if (n == 0)
        {
            usleep(0);
            continue;
        }

        if (wc.status != IBV_WC_SUCCESS)
        {
            KV_ERROR("work completion failed: %s (opcode %d, vendor_err 0x%x)",
                     ibv_wc_status_str(wc.status), (int)wc.opcode, wc.vendor_err);
            return -1;
        }
        if (out)
        {
            *out = wc;
        }
        return 0;
    }
}

namespace replicate
{
    int MasterServer::init()
    {
        // initialize rdma server
        channel_ = rdma_create_event_channel();
        if (!channel_)
        {
            KV_ERROR("rdma_create_event_channel: %s", strerror(errno));
            return -3;
        }

        if (0 != rdma_create_id(channel_, &cm_id_, NULL, RDMA_PS_TCP))
        {
            KV_ERROR("rdma_create_id: %s", strerror(errno));
            return -3;
        }

        struct sockaddr_in dst_addr;
        memset(&dst_addr, 0, sizeof(dst_addr));
        dst_addr.sin_family = AF_INET;
        dst_addr.sin_port = htons(port_);
        if (1 != inet_pton(AF_INET, ip_, &dst_addr.sin_addr))
        {
            KV_ERROR("'%s' is not a valid IPv4 address", ip_ ? ip_ : "");
            return 0;
        }

        if (0 != rdma_resolve_addr(cm_id_, NULL, (struct sockaddr *)&dst_addr, RDMA_TIMEOUT_MS))
        {
            KV_ERROR("rdma_resolve_addr %s: %s (the address must be on an RDMA netdev)",
                     ip_, strerror(errno));
            return 0;
        }

        rdma_wait_cm_event(channel_, RDMA_CM_EVENT_ADDR_RESOLVED, NULL);

        if (0 != rdma_resolve_route(cm_id_, RDMA_TIMEOUT_MS))
        {
            KV_ERROR("rdma_resolve_route: %s", strerror(errno));
            return 0;
        }
        rdma_wait_cm_event(channel_, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL);

        if (0 != rdma_create_queuepair(cm_id_, &pd_, &send_cq_, &recv_cq_))
        {
            KV_ERROR("rdma_create_queuepair: %s", strerror(errno));
            return 0;
        }

        if (0 != rdma_alloc_buffer(cm_id_, pd_, SMALL_BUFFER_SIZE, RDMA_BUFFER_SIZE))
        {
            KV_ERROR("rdma_alloc_buffer failed");
            return -1;
        }

        // Must be armed before the connection comes up: the slave grants the first
        // credit as soon as it is established.
        if (0 != rdma_post_recv(cm_id_))
            return -1;

        struct rdma_conn_param param;
        memset(&param, 0, sizeof(param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.retry_count = 3;
        param.rnr_retry_count = 7;

        if (0 != rdma_connect(cm_id_, &param))
        {
            KV_ERROR("rdma_connect: %s", strerror(errno));
            return -1;
        }

        rdma_wait_cm_event(channel_, RDMA_CM_EVENT_ESTABLISHED, NULL);

        return 0;
    }

    int MasterServer::send(const char *file_path)
    {
        if (!cm_id_)
            return -1;
        replicate::conn_manager_t *cm = (replicate::conn_manager_t *)cm_id_->context;

        int fd = open(file_path, O_RDWR);
        if (fd < 0)
        {
            return -1;
        }

        struct stat st;

        if (fstat(fd, &st) != 0)
        {
            ::close(fd);
            return -2;
        }

        size_t total = st.st_size;

        char *p = (char *)::mmap(nullptr, total, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED)
        {
            return -3;
        }

        size_t idx = 0;
        struct ibv_wc wc;

        struct file_info type;
        memset(&type, 0, sizeof(type));

        type.magic = MAGIC;
        type.total = total;
        memcpy(cm->sbuff, &type, sizeof(file_info));

        rdma_post_send(cm_id_, sizeof(file_info));
        if (0 != rdma_poll_wc(send_cq_, &wc) || wc.opcode != IBV_WC_SEND)
        {
            KV_ERROR("rdma_poll_wc");
            ::munmap(p, total);
            return -4;
        }

        while (idx < total)
        {
            // The slave grants one credit per chunk once its receive is armed.
            if (0 != rdma_poll_wc(recv_cq_, &wc) || wc.opcode != IBV_WC_RECV)
            {
                KV_ERROR("rdma_poll_wc waiting for a credit");
                ::munmap(p, total);
                return -4;
            }
            if (0 != rdma_post_recv(cm_id_))
            {
                ::munmap(p, total);
                return -4;
            }

            size_t chunk = total - idx;
            if (chunk > cm->sbuff_size)
                chunk = cm->sbuff_size;

            memcpy(cm->sbuff, p + idx, chunk);
            idx += chunk;
            rdma_post_send(cm_id_, chunk);

            if (0 != rdma_poll_wc(send_cq_, &wc) || wc.opcode != IBV_WC_SEND)
            {
                KV_ERROR("rdma_poll_wc");
                ::munmap(p, total);
                return -4;
            }
        }

        ::munmap(p, total);

        return 0;
    }

    void MasterServer::stop()
    {

        if (cm_id_)
        {
            rdma_disconnect(cm_id_);

            rdma_destroy_connection(cm_id_, pd_, send_cq_, recv_cq_);

            rdma_destroy_id(cm_id_);
            cm_id_ = nullptr;
        }

        pd_ = nullptr;
        send_cq_ = nullptr;
        recv_cq_ = nullptr;

        if (channel_)
        {
            rdma_destroy_event_channel(channel_);
            channel_ = nullptr;
        }
    }

    MasterServer::~MasterServer()
    {
        stop();
    }

    int SlaveServer::init()
    {
        // initialize rdma server
        channel_ = rdma_create_event_channel();
        if (!channel_)
        {
            KV_ERROR("rdma_create_event_channel: %s", strerror(errno));
            return -3;
        }

        if (0 != rdma_create_id(channel_, &cm_listen_id_, NULL, RDMA_PS_TCP))
        {
            KV_ERROR("rdma_create_id: %s", strerror(errno));
            return -3;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(RDMA_SLAVE_PORT);

        if (1 != inet_pton(AF_INET, ip_, &server_addr.sin_addr))
        {
            KV_ERROR("'%s' is not a valid IPv4 address", ip_ ? ip_ : "");
            return 0;
        }
        if (0 != rdma_bind_addr(cm_listen_id_, (struct sockaddr *)&server_addr))
        {
            KV_ERROR("rdma_bind_addr on port %d: %s", RDMA_SLAVE_PORT, strerror(errno));
            return -3;
        }
        return 0;
    }

    int SlaveServer::listen()
    {
        if (0 != rdma_listen(cm_listen_id_, 10))
        {
            KV_ERROR("rdma_listen: %s", strerror(errno));
            return -3;
        }

        struct rdma_cm_event *event = NULL;
        rdma_wait_cm_event(channel_, RDMA_CM_EVENT_CONNECT_REQUEST, &event);
        cm_client_id_ = event->id;
        rdma_ack_cm_event(event);

        KV_INFO("replica connected over RDMA");

        if (0 != rdma_create_queuepair(cm_client_id_, &pd_, &send_cq_, &recv_cq_))
        {
            KV_ERROR("rdma_create_queuepair: %s", strerror(errno));
            return -3;
        }

        // The receive buffer has to be able to hold a whole chunk sent by the master.
        if (0 != rdma_alloc_buffer(cm_client_id_, pd_, RDMA_BUFFER_SIZE, SMALL_BUFFER_SIZE))
        {
            KV_ERROR("rdma_alloc_buffer failed");
            return -3;
        }

        if (0 != rdma_post_recv(cm_client_id_))
            return -3;

        struct rdma_conn_param param;
        memset(&param, 0, sizeof(param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.retry_count = 3;
        param.rnr_retry_count = 7;

        if (0 != rdma_accept(cm_client_id_, &param))
        {
            KV_ERROR("rdma_accept: %s", strerror(errno));
            return -3;
        }

        rdma_wait_cm_event(cm_client_id_->channel, RDMA_CM_EVENT_ESTABLISHED, NULL);

        return 0;
    }

    int SlaveServer::recv(const char *file_path)
    {
        if (!cm_client_id_)
            return -1;
        replicate::conn_manager_t *cm = (replicate::conn_manager_t *)cm_client_id_->context;

        struct ibv_wc wc;
        if (0 != rdma_poll_wc(recv_cq_, &wc) || wc.opcode != IBV_WC_RECV)
            return -1;

        struct file_info fi;
        memset(&fi, 0, sizeof(fi));
        memcpy(&fi, cm->rbuff, sizeof(struct file_info));
        if (fi.magic != MAGIC)
            return -2;
        size_t total_size = fi.total;

        int fd = open(file_path, O_RDWR | O_CREAT, 0666);
        if (fd < 0)
        {
            return -3;
        }

        ftruncate(fd, total_size);

        char *p = (char *)mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED)
        {
            close(fd);
            return -4;
        }

        size_t idx = 0;
        while (idx < total_size)
        {
            if (0 != rdma_post_recv(cm_client_id_))
            {
                munmap(p, total_size);
                close(fd);
                return -5;
            }

            // Grant a credit only once the receive is armed, otherwise the chunk
            // would arrive with no buffer to land in.
            cm->sbuff[0] = 1;
            if (0 != rdma_post_send(cm_client_id_, 1))
            {
                munmap(p, total_size);
                close(fd);
                return -5;
            }
            if (0 != rdma_poll_wc(send_cq_, &wc) || wc.opcode != IBV_WC_SEND)
            {
                KV_ERROR("rdma_poll_wc granting a credit");
                munmap(p, total_size);
                close(fd);
                return -5;
            }

            if (0 != rdma_poll_wc(recv_cq_, &wc) || wc.opcode != IBV_WC_RECV)
            {
                KV_ERROR("rdma_poll_wc while receiving %s", file_path);
                munmap(p, total_size);
                close(fd);
                return -5;
            }

            size_t chunk = total_size - idx;
            if (chunk > cm->rbuff_size)
                chunk = cm->rbuff_size;

            memcpy(p + idx, cm->rbuff, chunk);
            idx += chunk;
        }

        msync(p, total_size, MS_SYNC);
        munmap(p, total_size);
        close(fd);

        return 0;
    }

    SlaveServer::~SlaveServer()
    {
        stop();
    }

    int SlaveServer::stop()
    {
        if (cm_client_id_)
        {
            rdma_disconnect(cm_client_id_);

            rdma_destroy_connection(cm_client_id_, pd_, send_cq_, recv_cq_);

            rdma_destroy_id(cm_client_id_);
            cm_client_id_ = nullptr;
        }

        pd_ = nullptr;
        send_cq_ = nullptr;
        recv_cq_ = nullptr;

        if (cm_listen_id_)
        {
            rdma_destroy_id(cm_listen_id_);
            cm_listen_id_ = nullptr;
        }

        if (channel_)
        {
            rdma_destroy_event_channel(channel_);
            channel_ = nullptr;
        }

        return 0;
    }

} // namespace replicate
