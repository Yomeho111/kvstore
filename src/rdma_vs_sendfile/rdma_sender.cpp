// RDMA side of the rdma-vs-sendfile benchmark: sends a file over an RC queue
// pair with two-sided SEND/RECV. The payload is posted straight out of the file
// mapping, so there is no user-space copy on the data path.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#define MAGIC 0x234324
#define CQ_DEPTH 256
#define RESOLVE_TIMEOUT_MS 2000
#define CHUNK_SIZE ((size_t)4 * 1024 * 1024)
#define MAX_INFLIGHT 16
#define CTRL_SIZE 64
#define CTRL_READY 1
#define CTRL_DONE 2

struct file_info
{
    int magic;
    size_t total;
};

struct rdma_ctx
{
    struct rdma_event_channel *channel;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;

    char *ctrl_send;
    char *ctrl_recv;
    struct ibv_mr *ctrl_send_mr;
    struct ibv_mr *ctrl_recv_mr;
};

static double elapsed_s(const std::chrono::steady_clock::time_point &start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static void report(const char *tag, size_t bytes, double sec)
{
    double mb = (double)bytes / (1024.0 * 1024.0);
    double rate = sec > 0.0 ? mb / sec : 0.0;
    double gbps = sec > 0.0 ? (double)bytes * 8.0 / sec / 1e9 : 0.0;
    printf("[%s] %.2f MB in %.3f s -> %.2f MB/s (%.2f Gb/s)\n", tag, mb, sec, rate, gbps);
    fflush(stdout);
}

static int wait_cm_event(struct rdma_event_channel *channel, enum rdma_cm_event_type expect)
{
    struct rdma_cm_event *event = NULL;
    if (0 != rdma_get_cm_event(channel, &event))
    {
        fprintf(stderr, "rdma_get_cm_event: %s\n", strerror(errno));
        return -1;
    }

    int ret = 0;
    if (event->event != expect)
    {
        fprintf(stderr, "unexpected RDMA CM event %s, expected %s\n",
                rdma_event_str(event->event), rdma_event_str(expect));
        ret = -1;
    }

    rdma_ack_cm_event(event);
    return ret;
}

static int poll_one(struct ibv_cq *cq, struct ibv_wc *out)
{
    while (1)
    {
        struct ibv_wc wc;
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0)
        {
            fprintf(stderr, "ibv_poll_cq failed\n");
            return -1;
        }
        if (n == 0)
            continue;

        if (wc.status != IBV_WC_SUCCESS)
        {
            fprintf(stderr, "work completion failed: %s (opcode %d, vendor_err 0x%x)\n",
                    ibv_wc_status_str(wc.status), (int)wc.opcode, wc.vendor_err);
            return -1;
        }
        if (out)
            *out = wc;
        return 0;
    }
}

static int create_queuepair(struct rdma_ctx *ctx)
{
    ctx->pd = ibv_alloc_pd(ctx->id->verbs);
    if (!ctx->pd)
    {
        fprintf(stderr, "ibv_alloc_pd: %s\n", strerror(errno));
        return -1;
    }

    ctx->send_cq = ibv_create_cq(ctx->id->verbs, CQ_DEPTH, NULL, NULL, 0);
    ctx->recv_cq = ibv_create_cq(ctx->id->verbs, CQ_DEPTH, NULL, NULL, 0);
    if (!ctx->send_cq || !ctx->recv_cq)
    {
        fprintf(stderr, "ibv_create_cq: %s\n", strerror(errno));
        return -1;
    }

    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = ctx->send_cq;
    attr.recv_cq = ctx->recv_cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = CQ_DEPTH;
    attr.cap.max_recv_wr = CQ_DEPTH;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;

    if (0 != rdma_create_qp(ctx->id, ctx->pd, &attr))
    {
        fprintf(stderr, "rdma_create_qp: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int alloc_ctrl_buffers(struct rdma_ctx *ctx)
{
    ctx->ctrl_send = (char *)calloc(1, CTRL_SIZE);
    ctx->ctrl_recv = (char *)calloc(1, CTRL_SIZE);
    if (!ctx->ctrl_send || !ctx->ctrl_recv)
        return -1;

    ctx->ctrl_send_mr = ibv_reg_mr(ctx->pd, ctx->ctrl_send, CTRL_SIZE, 0);
    ctx->ctrl_recv_mr = ibv_reg_mr(ctx->pd, ctx->ctrl_recv, CTRL_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->ctrl_send_mr || !ctx->ctrl_recv_mr)
    {
        fprintf(stderr, "ibv_reg_mr(ctrl): %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int post_recv(struct rdma_cm_id *id, void *addr, size_t len, uint32_t lkey, uint64_t wr_id)
{
    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)addr;
    sge.length = (uint32_t)len;
    sge.lkey = lkey;

    struct ibv_recv_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    int ret = ibv_post_recv(id->qp, &wr, &bad);
    if (ret)
        fprintf(stderr, "ibv_post_recv: %s\n", strerror(ret));
    return ret;
}

static int post_send(struct rdma_cm_id *id, void *addr, size_t len, uint32_t lkey, uint64_t wr_id)
{
    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)addr;
    sge.length = (uint32_t)len;
    sge.lkey = lkey;

    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    int ret = ibv_post_send(id->qp, &wr, &bad);
    if (ret)
        fprintf(stderr, "ibv_post_send: %s\n", strerror(ret));
    return ret;
}

static int connect_peer(struct rdma_ctx *ctx, const char *ip, uint16_t port)
{
    ctx->channel = rdma_create_event_channel();
    if (!ctx->channel)
    {
        fprintf(stderr, "rdma_create_event_channel: %s\n", strerror(errno));
        return -1;
    }

    if (0 != rdma_create_id(ctx->channel, &ctx->id, NULL, RDMA_PS_TCP))
    {
        fprintf(stderr, "rdma_create_id: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (1 != inet_pton(AF_INET, ip, &dst.sin_addr))
    {
        fprintf(stderr, "'%s' is not a valid IPv4 address\n", ip);
        return -1;
    }

    if (0 != rdma_resolve_addr(ctx->id, NULL, (struct sockaddr *)&dst, RESOLVE_TIMEOUT_MS))
    {
        fprintf(stderr, "rdma_resolve_addr %s: %s (the address must be on an RDMA netdev)\n",
                ip, strerror(errno));
        return -1;
    }
    if (0 != wait_cm_event(ctx->channel, RDMA_CM_EVENT_ADDR_RESOLVED))
        return -1;

    if (0 != rdma_resolve_route(ctx->id, RESOLVE_TIMEOUT_MS))
    {
        fprintf(stderr, "rdma_resolve_route: %s\n", strerror(errno));
        return -1;
    }
    if (0 != wait_cm_event(ctx->channel, RDMA_CM_EVENT_ROUTE_RESOLVED))
        return -1;

    if (0 != create_queuepair(ctx))
        return -1;
    if (0 != alloc_ctrl_buffers(ctx))
        return -1;

    // Armed before the connection comes up: the receiver answers as soon as it
    // is established.
    if (0 != post_recv(ctx->id, ctx->ctrl_recv, CTRL_SIZE, ctx->ctrl_recv_mr->lkey, 0))
        return -1;

    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 7;
    param.rnr_retry_count = 7;

    if (0 != rdma_connect(ctx->id, &param))
    {
        fprintf(stderr, "rdma_connect: %s\n", strerror(errno));
        return -1;
    }

    return wait_cm_event(ctx->channel, RDMA_CM_EVENT_ESTABLISHED);
}

static void ctx_destroy(struct rdma_ctx *ctx)
{
    if (ctx->id && ctx->id->qp)
        rdma_destroy_qp(ctx->id);
    if (ctx->ctrl_send_mr)
        ibv_dereg_mr(ctx->ctrl_send_mr);
    if (ctx->ctrl_recv_mr)
        ibv_dereg_mr(ctx->ctrl_recv_mr);
    free(ctx->ctrl_send);
    free(ctx->ctrl_recv);
    if (ctx->send_cq)
        ibv_destroy_cq(ctx->send_cq);
    if (ctx->recv_cq)
        ibv_destroy_cq(ctx->recv_cq);
    if (ctx->pd)
        ibv_dealloc_pd(ctx->pd);
    if (ctx->id)
        rdma_destroy_id(ctx->id);
    if (ctx->channel)
        rdma_destroy_event_channel(ctx->channel);
    memset(ctx, 0, sizeof(*ctx));
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <ip> <port> [file]\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    unsigned long port = strtoul(argv[2], NULL, 10);
    if (port == 0 || port > 65535)
    {
        fprintf(stderr, "'%s' is not a valid port\n", argv[2]);
        return 1;
    }
    const char *path = (argc > 3) ? argv[3] : "./bigfile.dat";

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 1;
    }

    struct stat st;
    if (0 != fstat(fd, &st))
    {
        fprintf(stderr, "fstat %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    size_t total = (size_t)st.st_size;

    struct rdma_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = 1;
    char *p = NULL;
    struct ibv_mr *file_mr = NULL;

    if (0 != connect_peer(&ctx, ip, (uint16_t)port))
        goto out;

    printf("connected to %s:%lu, sending %s (%zu bytes)\n", ip, port, path, total);

    if (total > 0)
    {
        p = (char *)mmap(NULL, total, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (p == MAP_FAILED)
        {
            fprintf(stderr, "mmap %s: %s\n", path, strerror(errno));
            p = NULL;
            goto out;
        }

        std::chrono::steady_clock::time_point reg_start = std::chrono::steady_clock::now();
        file_mr = ibv_reg_mr(ctx.pd, p, total, 0);
        if (!file_mr)
        {
            fprintf(stderr, "ibv_reg_mr(%zu bytes): %s (check 'ulimit -l')\n", total, strerror(errno));
            goto out;
        }
        report("register", total, elapsed_s(reg_start));
    }

    {
        struct file_info fi;
        memset(&fi, 0, sizeof(fi));
        fi.magic = MAGIC;
        fi.total = total;
        memcpy(ctx.ctrl_send, &fi, sizeof(fi));

        struct ibv_wc wc;
        if (0 != post_send(ctx.id, ctx.ctrl_send, sizeof(fi), ctx.ctrl_send_mr->lkey, 0) ||
            0 != poll_one(ctx.send_cq, &wc))
            goto out;

        // Wait until the receiver has its buffers armed before streaming.
        if (0 != poll_one(ctx.recv_cq, &wc))
            goto out;
        if (wc.byte_len < 1 || ctx.ctrl_recv[0] != CTRL_READY)
        {
            fprintf(stderr, "unexpected control message from the receiver\n");
            goto out;
        }
        if (0 != post_recv(ctx.id, ctx.ctrl_recv, CTRL_SIZE, ctx.ctrl_recv_mr->lkey, 0))
            goto out;

        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        size_t off = 0;
        int inflight = 0;
        while (off < total)
        {
            while (inflight >= MAX_INFLIGHT)
            {
                if (0 != poll_one(ctx.send_cq, NULL))
                    goto out;
                --inflight;
            }

            size_t chunk = total - off;
            if (chunk > CHUNK_SIZE)
                chunk = CHUNK_SIZE;

            if (0 != post_send(ctx.id, p + off, chunk, file_mr->lkey, off))
                goto out;
            off += chunk;
            ++inflight;
        }

        while (inflight > 0)
        {
            if (0 != poll_one(ctx.send_cq, NULL))
                goto out;
            --inflight;
        }

        // The receiver acks once the last chunk has landed in its file mapping.
        if (0 != poll_one(ctx.recv_cq, &wc))
            goto out;
        if (wc.byte_len < 1 || ctx.ctrl_recv[0] != CTRL_DONE)
        {
            fprintf(stderr, "unexpected control message from the receiver\n");
            goto out;
        }

        report("rdma send", total, elapsed_s(start));
        rc = 0;
    }

out:
    if (file_mr)
        ibv_dereg_mr(file_mr);
    if (p)
        munmap(p, total);
    close(fd);
    if (ctx.id)
        rdma_disconnect(ctx.id);
    ctx_destroy(&ctx);
    return rc;
}
