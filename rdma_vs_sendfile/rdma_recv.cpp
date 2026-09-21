// RDMA side of the rdma-vs-sendfile benchmark: receives a file over an RC queue
// pair. Chunks land in a registered ring of anonymous buffers and are written
// out from there: the kernel refuses long-term pinning of a writable file
// mapping, so the receive side cannot place the payload into the page cache
// directly the way the sender streams it out of one.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#define MAGIC 0x234324
#define CQ_DEPTH 256
#define CHUNK_SIZE ((size_t)4 * 1024 * 1024)
#define NUM_SLOTS 16
#define CTRL_SIZE 64
#define CTRL_READY 1
#define CTRL_DONE 2
#define MAX_FILE_SIZE ((size_t)64 * 1024 * 1024 * 1024)

struct file_info
{
    int magic;
    size_t total;
};

struct rdma_ctx
{
    struct rdma_event_channel *channel;
    struct rdma_cm_id *listen_id;
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

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
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

static int send_ctrl(struct rdma_ctx *ctx, char code)
{
    ctx->ctrl_send[0] = code;
    if (0 != post_send(ctx->id, ctx->ctrl_send, 1, ctx->ctrl_send_mr->lkey, 0))
        return -1;
    return poll_one(ctx->send_cq, NULL);
}

static int wait_cm_event(struct rdma_event_channel *channel, enum rdma_cm_event_type expect,
                         struct rdma_cm_id **out_id)
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
    else if (out_id)
    {
        *out_id = event->id;
    }

    rdma_ack_cm_event(event);
    return ret;
}

static int accept_peer(struct rdma_ctx *ctx, const char *ip, uint16_t port)
{
    ctx->channel = rdma_create_event_channel();
    if (!ctx->channel)
    {
        fprintf(stderr, "rdma_create_event_channel: %s\n", strerror(errno));
        return -1;
    }

    if (0 != rdma_create_id(ctx->channel, &ctx->listen_id, NULL, RDMA_PS_TCP))
    {
        fprintf(stderr, "rdma_create_id: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (1 != inet_pton(AF_INET, ip, &addr.sin_addr))
    {
        fprintf(stderr, "'%s' is not a valid IPv4 address\n", ip);
        return -1;
    }

    if (0 != rdma_bind_addr(ctx->listen_id, (struct sockaddr *)&addr))
    {
        fprintf(stderr, "rdma_bind_addr %s:%u: %s\n", ip, port, strerror(errno));
        return -1;
    }

    if (0 != rdma_listen(ctx->listen_id, 1))
    {
        fprintf(stderr, "rdma_listen: %s\n", strerror(errno));
        return -1;
    }

    printf("waiting for an RDMA sender on %s:%u\n", ip, port);
    fflush(stdout);

    if (0 != wait_cm_event(ctx->channel, RDMA_CM_EVENT_CONNECT_REQUEST, &ctx->id))
        return -1;

    if (0 != create_queuepair(ctx))
        return -1;
    if (0 != alloc_ctrl_buffers(ctx))
        return -1;

    // Armed before accepting: the sender pushes the header right after the
    // connection is established.
    if (0 != post_recv(ctx->id, ctx->ctrl_recv, CTRL_SIZE, ctx->ctrl_recv_mr->lkey, 0))
        return -1;

    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 7;
    param.rnr_retry_count = 7;

    if (0 != rdma_accept(ctx->id, &param))
    {
        fprintf(stderr, "rdma_accept: %s\n", strerror(errno));
        return -1;
    }

    return wait_cm_event(ctx->channel, RDMA_CM_EVENT_ESTABLISHED, NULL);
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
    if (ctx->listen_id)
        rdma_destroy_id(ctx->listen_id);
    if (ctx->channel)
        rdma_destroy_event_channel(ctx->channel);
    memset(ctx, 0, sizeof(*ctx));
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <ip> <port> [out-file]\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    unsigned long port = strtoul(argv[2], NULL, 10);
    if (port == 0 || port > 65535)
    {
        fprintf(stderr, "'%s' is not a valid port\n", argv[2]);
        return 1;
    }
    const char *path = (argc > 3) ? argv[3] : "./bigfile.rdma.out";

    struct rdma_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = 1;
    int fd = -1;
    char *ring = NULL;
    struct ibv_mr *ring_mr = NULL;
    size_t total = 0;
    size_t got = 0;
    size_t nchunks = 0;
    size_t posted = 0;
    int slot = 0;
    struct ibv_wc wc;

    if (0 != accept_peer(&ctx, ip, (uint16_t)port))
        goto out;

    if (0 != poll_one(ctx.recv_cq, &wc))
        goto out;
    if (wc.byte_len < sizeof(struct file_info))
    {
        fprintf(stderr, "short header: %u bytes\n", wc.byte_len);
        goto out;
    }

    {
        struct file_info fi;
        memcpy(&fi, ctx.ctrl_recv, sizeof(fi));
        if (fi.magic != MAGIC)
        {
            fprintf(stderr, "bad header magic 0x%x\n", fi.magic);
            goto out;
        }
        if (fi.total > MAX_FILE_SIZE)
        {
            fprintf(stderr, "announced file size %zu exceeds the %zu byte limit\n",
                    fi.total, (size_t)MAX_FILE_SIZE);
            goto out;
        }
        total = fi.total;
    }

    printf("receiving %zu bytes into %s\n", total, path);
    fflush(stdout);

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        goto out;
    }

    if (total > 0)
    {
        if (0 != ftruncate(fd, (off_t)total))
        {
            fprintf(stderr, "ftruncate %s: %s\n", path, strerror(errno));
            goto out;
        }

        ring = (char *)aligned_alloc(4096, NUM_SLOTS * CHUNK_SIZE);
        if (!ring)
        {
            fprintf(stderr, "aligned_alloc(%zu bytes) failed\n", NUM_SLOTS * CHUNK_SIZE);
            goto out;
        }
        memset(ring, 0, NUM_SLOTS * CHUNK_SIZE);

        std::chrono::steady_clock::time_point reg_start = std::chrono::steady_clock::now();
        ring_mr = ibv_reg_mr(ctx.pd, ring, NUM_SLOTS * CHUNK_SIZE, IBV_ACCESS_LOCAL_WRITE);
        if (!ring_mr)
        {
            fprintf(stderr, "ibv_reg_mr(%zu bytes): %s (check 'ulimit -l')\n",
                    NUM_SLOTS * CHUNK_SIZE, strerror(errno));
            goto out;
        }
        report("register", NUM_SLOTS * CHUNK_SIZE, elapsed_s(reg_start));

        // RC delivers in order, so completions come back in the order the slots
        // were posted and the stream can be written out sequentially.
        nchunks = (total + CHUNK_SIZE - 1) / CHUNK_SIZE;
        while (posted < nchunks && posted < NUM_SLOTS)
        {
            if (0 != post_recv(ctx.id, ring + posted * CHUNK_SIZE, CHUNK_SIZE, ring_mr->lkey, posted))
                goto out;
            ++posted;
        }
    }

    if (0 != send_ctrl(&ctx, CTRL_READY))
        goto out;

    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        size_t done = 0;
        while (done < nchunks)
        {
            if (0 != poll_one(ctx.recv_cq, &wc))
                goto out;
            if (wc.opcode != IBV_WC_RECV || wc.wr_id != (uint64_t)slot || wc.byte_len == 0 ||
                wc.byte_len > total - got)
            {
                fprintf(stderr, "unexpected chunk: wr_id %llu, %u bytes, %zu bytes left\n",
                        (unsigned long long)wc.wr_id, wc.byte_len, total - got);
                goto out;
            }

            if (0 != write_full(fd, ring + (size_t)slot * CHUNK_SIZE, wc.byte_len))
            {
                fprintf(stderr, "write %s: %s\n", path, strerror(errno));
                goto out;
            }
            got += wc.byte_len;
            ++done;

            if (posted < nchunks)
            {
                if (0 != post_recv(ctx.id, ring + (size_t)slot * CHUNK_SIZE, CHUNK_SIZE, ring_mr->lkey, slot))
                    goto out;
                ++posted;
            }
            slot = (slot + 1) % NUM_SLOTS;
        }

        double transfer_s = elapsed_s(start);

        if (0 != send_ctrl(&ctx, CTRL_DONE))
            goto out;

        report("rdma recv", total, transfer_s);

        std::chrono::steady_clock::time_point flush_start = std::chrono::steady_clock::now();
        fsync(fd);
        report("flush", total, elapsed_s(flush_start));
        rc = 0;
    }

out:
    if (ring_mr)
        ibv_dereg_mr(ring_mr);
    free(ring);
    if (fd >= 0)
        close(fd);
    if (ctx.id)
        rdma_disconnect(ctx.id);
    ctx_destroy(&ctx);
    return rc;
}
