#include "replicate.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits>
#include <arpa/inet.h>

#include "allocator.h"
#include "kv_protocal.hpp"
#include "kv_log.h"
#include "delta.skel.h"

#include "delta.h"

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

static int rdma_create_queuepair(struct rdma_cm_id *id, struct ibv_pd **pd, struct ibv_cq **cq)
{

    *pd = ibv_alloc_pd(id->verbs);
    if (NULL == *pd)
    {
        KV_ERROR("ibv_alloc_pd: %s", strerror(errno));
        return -1;
    }

    *cq = ibv_create_cq(id->verbs, RDMA_CQ_DEPTH, NULL, NULL, 0);
    if (NULL == *cq)
    {
        KV_ERROR("ibv_create_cq: %s", strerror(errno));
        return -1;
    }

    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = *cq;
    attr.recv_cq = *cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = RDMA_CQ_DEPTH;
    attr.cap.max_recv_wr = RDMA_CQ_DEPTH;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;

    return rdma_create_qp(id, *pd, &attr);
    ;
}

static int rdma_destroy_queuepair(struct rdma_cm_id *id, struct ibv_pd *pd, struct ibv_cq *cq)
{
    if (!id || !pd || !cq)
        return -1;
    rdma_destroy_qp(id);

    ibv_destroy_cq(cq);

    ibv_dealloc_pd(pd);

    return 0;
}

static int rdma_alloc_buffer(struct rdma_cm_id *cm_id, struct ibv_pd *pd, size_t recv_size, size_t send_size)
{

    replicate::conn_manager_t *cm = (replicate::conn_manager_t *)allocator::kv_malloc(sizeof(replicate::conn_manager_t));
    if (!cm)
        return -1;

    cm->rbuff = (char *)allocator::kv_malloc(recv_size);
    cm->sbuff = (char *)allocator::kv_malloc(send_size);
    if (!cm->rbuff || !cm->sbuff)
        return -1;

    cm->send_mr = ibv_reg_mr(pd, cm->sbuff, send_size, 0);
    cm->recv_mr = ibv_reg_mr(pd, cm->rbuff, recv_size, IBV_ACCESS_LOCAL_WRITE);
    if (!cm->send_mr || !cm->recv_mr)
        return -1;

    cm->rbuff_size = recv_size;
    cm->sbuff_size = send_size;

    cm_id->context = cm;
    return 0;
}

static int rdma_free_buffer(struct rdma_cm_id *cm_id)
{
    if (!cm_id)
        return -1;
    replicate::conn_manager_t *cm = (replicate::conn_manager_t *)cm_id->context;

    ibv_dereg_mr(cm->send_mr);
    ibv_dereg_mr(cm->recv_mr);

    allocator::kv_free(cm->rbuff);
    allocator::kv_free(cm->sbuff);

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
    FullSyncObject &FullSyncObject::instance()
    {
        static FullSyncObject fs_obj;
        return fs_obj;
    }

    int FullSyncObject::get_full_sync(Netbuffer *net_buffer)
    {
        if (!net_buffer)
            return -1;

        auto &prot = kv_protocal::KvStoreProtocal::instance();
        auto &engine = prot._engine;
        engine.lock();
        for (auto it = engine.begin(); it != engine.end(); ++it)
        {
            auto *node = *it;
            uint16_t command = kv_protocal::KVS_SET;
            size_t key_len = node->key.size();
            size_t value_len = node->value.size();
            size_t total_size = sizeof(uint16_t) + sizeof(size_t) * 2 + node->key.size() + node->value.size();
            char *buffer = (char *)allocator::kv_malloc(total_size);
            if (!buffer)
            {
                engine.unlock();
                return -1;
            }

            memcpy(buffer, &command, sizeof(uint16_t));
            memcpy(buffer + sizeof(uint16_t), &key_len, sizeof(size_t));
            memcpy(buffer + sizeof(uint16_t) + sizeof(size_t), &value_len, sizeof(size_t));

            memcpy(buffer + sizeof(uint16_t) + sizeof(size_t) * 2, node->key.c_str(), key_len);
            if (value_len)
            {
                memcpy(buffer + sizeof(uint16_t) + sizeof(size_t) * 2 + key_len, node->value.c_str(), value_len);
            }

            net_buffer->write_buffer(buffer, total_size);
            allocator::kv_free(buffer);
        }
        engine.unlock();
        return 0;
    }

    DeltaSyncObject &DeltaSyncObject::instance()
    {
        static DeltaSyncObject ds_obj;
        static int ret = ds_obj.init();
        if (ret < 0)
        {
            KV_ERROR("bad delta server");
            exit(0);
        }
        return ds_obj;
    }

    bool DeltaSyncObject::empty()
    {
        std::lock_guard lk{mtx_};
        return sequence_vec_.empty();
    }

    int DeltaSyncObject::insert_node(uint16_t command, size_t key_len, size_t value_len, const char *key, const char *value)
    {
        char *k = (char *)allocator::kv_malloc(key_len);
        if (!k)
            return -1;

        memcpy(k, key, key_len);

        char *v{nullptr};
        if (value_len)
        {
            v = (char *)allocator::kv_malloc(value_len);
            if (!v)
                return -1;
            memcpy(v, value, value_len);
        }

        std::lock_guard lk{mtx_};
        if (delta_ring_buff_[index_].key)
            allocator::kv_free(delta_ring_buff_[index_].key);
        if (delta_ring_buff_[index_].value)
            allocator::kv_free(delta_ring_buff_[index_].value);

        delta_ring_buff_[index_].command = command;
        delta_ring_buff_[index_].key_len = key_len;
        delta_ring_buff_[index_].value_len = value_len;
        delta_ring_buff_[index_].key = k;
        delta_ring_buff_[index_].value = v;

        kv_replication_commit(index_);
        int i = index_;
        index_ = (index_ + 1) % MAX_REP_BUFFER_SIZE;
        return i;
    }

    int DeltaSyncObject::get_delta_sync(Netbuffer *net_buffer)
    {
        std::lock_guard lk{mtx_};
        for (int &sequence : sequence_vec_)
        {
            Node &node = delta_ring_buff_[sequence];
            uint16_t command = node.command;
            size_t key_len = node.key_len;
            size_t value_len = node.value_len;
            size_t total_size = sizeof(uint16_t) + sizeof(size_t) * 2 + key_len + value_len;
            char *buffer = (char *)allocator::kv_malloc(total_size);
            if (!buffer)
            {
                return -1;
            }

            memcpy(buffer, &command, sizeof(uint16_t));
            memcpy(buffer + sizeof(uint16_t), &key_len, sizeof(size_t));
            memcpy(buffer + sizeof(uint16_t) + sizeof(size_t), &value_len, sizeof(size_t));

            memcpy(buffer + sizeof(uint16_t) + sizeof(size_t) * 2, node.key, key_len);
            if (value_len)
            {
                memcpy(buffer + sizeof(uint16_t) + sizeof(size_t) * 2 + key_len, node.value, value_len);
            }

            net_buffer->write_buffer(buffer, total_size);
            allocator::kv_free(buffer);

            allocator::kv_free(delta_ring_buff_[sequence].key);
            allocator::kv_free(delta_ring_buff_[sequence].value);
            delta_ring_buff_[sequence].key = nullptr;
            delta_ring_buff_[sequence].value = nullptr;
        }
        return 0;
    }

    void DeltaSyncObject::clear_sequence()
    {
        std::lock_guard lk{mtx_};
        sequence_vec_.clear();
    }

    void DeltaSyncObject::insert_sequence(int sequence)
    {
        std::lock_guard lk{mtx_};
        if (sequence_vec_.size() >= MAX_REP_BUFFER_SIZE)
        {
            sequence_vec_.clear();
            is_cleared.store(true, std::memory_order_release);
            return;
        }
        sequence_vec_.push_back(sequence);
    }

    int DeltaSyncObject::init()
    {
        if (initialized_)
        {
            return 0;
        }

        char *binary_path = get_self_exe();

        if (!binary_path || strnlen(binary_path, PATH_MAX) == 0)
        {
            KV_ERROR("KVTracer: failed to resolve /proc/self/exe");

            if (binary_path)
                allocator::kv_free(binary_path);

            return -1;
        }

        /*
         * ============================================================
         * 1. Open skeleton
         * ============================================================
         */
        skel_ = delta_bpf__open();

        if (!skel_)
        {
            KV_ERROR("KVTracer: delta_bpf__open() failed");

            goto fail;
        }

        /*
         * ============================================================
         * 2. Load BPF program/maps into kernel
         * ============================================================
         */
        if (delta_bpf__load(skel_) != 0)
        {
            KV_ERROR("KVTracer: delta_bpf__load() failed (needs CAP_BPF/CAP_PERFMON)");

            goto fail;
        }

        /*
         * ============================================================
         * 3. Attach uprobe
         * ============================================================
         *
         * Target:
         *
         * extern "C"
         * __attribute__((noinline, visibility("default")))
         * void kv_replication_commit(int sequence);
         *
         * Because of extern "C", the symbol name is exactly:
         *
         *     kv_replication_commit
         *
         * No C++ name mangling.
         */
        {
            struct bpf_uprobe_opts opts = {};

            opts.sz = sizeof(opts);
            opts.func_name = STRINGIFY(COMMIT_FUNC);
            opts.retprobe = false;

            uprobe_link_ =
                bpf_program__attach_uprobe_opts(
                    skel_->progs.kv_replication_commit_enter,

                    /*
                     * Only attach to current process.
                     *
                     * All threads in this process can trigger
                     * the uprobe.
                     */
                    getpid(),

                    binary_path,

                    /*
                     * Offset is 0 because func_name is used.
                     */
                    0,

                    &opts);

            if (!uprobe_link_)
            {
                KV_ERROR("KVTracer: failed to attach uprobe to %s:%s: %s (needs CAP_SYS_ADMIN)",
                         binary_path,
                         STRINGIFY(COMMIT_FUNC),
                         strerror(errno));

                goto fail;
            }
        }

        /*
         * ============================================================
         * 4. Create ring buffer consumer
         * ============================================================
         */
        rb_ = ring_buffer__new(
            bpf_map__fd(skel_->maps.rb),

            &DeltaSyncObject::handle_event_static,

            /*
             * This pointer is returned to us as callback ctx.
             */
            this,

            nullptr);

        if (!rb_)
        {
            KV_ERROR("KVTracer: ring_buffer__new() failed: %s", strerror(errno));

            goto fail;
        }

        initialized_.store(
            true,
            std::memory_order_release);

        if (binary_path)
            allocator::kv_free(binary_path);

        return 0;

    fail:
        if (binary_path)
            allocator::kv_free(binary_path);

        cleanup_bpf();

        return -1;
    }

    int DeltaSyncObject::start()
    {
        if (!initialized_.load(std::memory_order_acquire))
        {
            KV_ERROR("KVTracer: start() called before init()");

            return -1;
        }

        bool expected = false;

        if (!running_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel))
        {
            /*
             * Already running.
             */
            return 0;
        }

        ebpf_thr_ = std::thread(
            [this]()
            {
                ebpf_worker();
            });

        return 0;
    }

    void DeltaSyncObject::stop()
    {
        running_.store(
            false,
            std::memory_order_release);

        if (ebpf_thr_.joinable())
        {
            ebpf_thr_.join();
        }

        cleanup_bpf();
    }

    char *DeltaSyncObject::get_self_exe()
    {
        char *path = (char *)allocator::kv_malloc(PATH_MAX);
        if (!path)
            return nullptr;

        ssize_t len =
            readlink(
                "/proc/self/exe",
                path,
                PATH_MAX - 1);

        if (len < 0 || static_cast<size_t>(len) >= PATH_MAX)
        {
            allocator::kv_free(path);
            return nullptr;
        }

        path[len] = '\0';

        return path;
    }

    void DeltaSyncObject::cleanup_bpf()
    {
        initialized_.store(
            false,
            std::memory_order_release);

        /*
         * Ring buffer references BPF map fd,
         * so destroy it before skeleton.
         */
        if (rb_)
        {
            ring_buffer__free(rb_);
            rb_ = nullptr;
        }

        /*
         * This uprobe was manually attached,
         * so destroy the link manually.
         */
        if (uprobe_link_)
        {
            bpf_link__destroy(uprobe_link_);
            uprobe_link_ = nullptr;
        }

        /*
         * Destroy skeleton last.
         */
        if (skel_)
        {
            delta_bpf__destroy(skel_);
            skel_ = nullptr;
        }
    }

    int DeltaSyncObject::ebpf_worker()
    {
        int err{0};
        while (running_.load(std::memory_order_acquire))
        {
            err = ring_buffer__poll(rb_, 100);
            // Any signal (a debugger attaching, a profiler, ...) interrupts
            // epoll_wait; that is a spurious wakeup, not a failure.
            if (err == -EINTR)
            {
                err = 0;
                continue;
            }

            if (err < 0)
            {
                KV_ERROR("error polling the delta ring buffer: %d", err);
                break;
            }
        }
        return err;
    }

    /*
     * libbpf requires a plain C-style callback.
     *
     * Therefore this function must be static.
     */
    int DeltaSyncObject::handle_event_static(
        void *ctx,
        void *data,
        size_t data_sz)
    {
        auto *self =
            static_cast<DeltaSyncObject *>(ctx);

        if (!self)
        {
            return -1;
        }

        return self->handle_event(
            data,
            data_sz);
    }

    /*
     * Actual member callback.
     */
    int DeltaSyncObject::handle_event(
        void *data,
        size_t data_sz)
    {
        if (!data)
        {
            return -1;
        }

        if (data_sz < sizeof(DeltaEvent))
        {
            KV_ERROR("DeltaSyncObject: invalid event size: got=%zu expected=%zu",
                     data_sz,
                     sizeof(DeltaEvent));

            return -1;
        }

        const auto *event =
            static_cast<const DeltaEvent *>(data);

        insert_sequence(event->sequence);

        MasterServer::instance().notify();

        return 0;
    }

    DeltaSyncObject::~DeltaSyncObject()
    {
        stop();
        for (int i = 0; i < MAX_REP_BUFFER_SIZE; i++)
        {
            if (delta_ring_buff_[i].key)
                allocator::kv_free(delta_ring_buff_[i].key);
            if (delta_ring_buff_[i].value)
                allocator::kv_free(delta_ring_buff_[i].value);
        }
    }

    MasterServer &MasterServer::instance()
    {
        static MasterServer server;
        static int ret = server.init();
        if (ret < 0)
        {
            KV_ERROR("bad master server");
            exit(0);
        }
        return server;
    }

    int MasterServer::init()
    {
        // initialize the netbuffer
        void *ptr = allocator::kv_malloc(sizeof(Netbuffer));
        if (!ptr)
            return -1;

        net_buffer_ = new (ptr) Netbuffer(RDMA_BUFFER_SIZE);
        if (net_buffer_->init() != 0)
            return -2;

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
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(RDMA_SERVER_PORT);
        if (0 != rdma_bind_addr(cm_listen_id_, (struct sockaddr *)&server_addr))
        {
            KV_ERROR("rdma_bind_addr on port %d: %s", RDMA_SERVER_PORT, strerror(errno));
            return -3;
        }
        return 0;
    }

    int MasterServer::_listen()
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

        if (0 != rdma_create_queuepair(cm_client_id_, &pd_, &cq_))
        {
            KV_ERROR("rdma_create_queuepair: %s", strerror(errno));
            return -3;
        }

        rdma_alloc_buffer(cm_client_id_, pd_, SMALL_BUFFER_SIZE, RDMA_BUFFER_SIZE);

        // iWARP tears the connection down when a message arrives with an empty
        // receive queue, so the replica's acknowledgement buffer must be posted
        // before the connection is accepted.
        if (0 != rdma_post_recv(cm_client_id_))
            return -3;

        struct rdma_conn_param param;
        memset(&param, 0, sizeof(param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.retry_count = 3;

        if (0 != rdma_accept(cm_client_id_, &param))
        {
            KV_ERROR("rdma_accept: %s", strerror(errno));
            return -3;
        }

        rdma_wait_cm_event(cm_client_id_->channel, RDMA_CM_EVENT_ESTABLISHED, NULL);

        return 0;
    }

    int MasterServer::_send()
    {
        if (!cm_client_id_)
            return -1;
        replicate::conn_manager_t *cm = (replicate::conn_manager_t *)cm_client_id_->context;
        size_t buffer_size = net_buffer_->get_size();
        if (buffer_size == 0)
            return 0;

        struct packet_info pi;
        memset(&pi, 0, sizeof(pi));
        pi.magic = MAGIC;
        pi.total = buffer_size;

        if (buffer_size > cm->sbuff_size - sizeof(struct packet_info))
        {
            KV_ERROR("sync payload of %zu bytes exceeds the send buffer", buffer_size);
            return -1;
        }

        memcpy(cm->sbuff, &pi, sizeof(struct packet_info));
        net_buffer_->get_buffer(cm->sbuff + sizeof(struct packet_info), buffer_size);

        if (0 != rdma_post_send(cm_client_id_, sizeof(struct packet_info) + buffer_size))
            return -1;

        struct ibv_wc wc;
        if (0 != rdma_poll_wc(cq_, &wc) || wc.opcode != IBV_WC_SEND)
            return -1;

        // Wait for the replica to acknowledge before the send buffer is reused.
        if (0 != rdma_poll_wc(cq_, &wc) || wc.opcode != IBV_WC_RECV)
            return -1;

        if (0 != rdma_post_recv(cm_client_id_))
            return -1;

        return 0;
    }

    int MasterServer::start()
    {
        bool expected = false;

        if (!running_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel))
        {
            /*
             * Already running.
             */
            return 0;
        }

        sync_thr = std::thread(
            [this]
            {
                sync_worker();
            });

        return 0;
    }

    void MasterServer::stop()
    {
        running_.store(false, std::memory_order_release);

        cv_.notify_all();

        if (sync_thr.joinable())
            sync_thr.join();

        DeltaSyncObject::instance().stop();

        if (cm_client_id_)
        {
            rdma_disconnect(cm_client_id_);

            rdma_free_buffer(cm_client_id_);

            rdma_destroy_queuepair(cm_client_id_, pd_, cq_);

            rdma_destroy_id(cm_client_id_);
        }

        if (cm_listen_id_)
            rdma_destroy_id(cm_listen_id_);

        if (channel_)
            rdma_destroy_event_channel(channel_);
    }

    int MasterServer::sync_worker()
    {
        _listen();

        if (FullSyncObject::instance().get_full_sync(net_buffer_) != 0)
            return -1;

        _send();

        auto &delta = DeltaSyncObject::instance();
        delta.start();

        while (running_.load(std::memory_order_acquire))
        {
            std::unique_lock lk{mtx_};
            cv_.wait(lk, [this, &delta]
                     { return !delta.empty() || !running_.load(std::memory_order_acquire) ||
                              delta.is_clear(); });

            if (!running_.load(std::memory_order_acquire))
                return 0;

            if (delta.is_clear())
            {
                // The delta ring overflowed, so ship the whole dataset instead.
                if (FullSyncObject::instance().get_full_sync(net_buffer_) != 0)
                    return -1;

                // Without this the predicate stays satisfied and the master
                // resends the full dataset in a tight loop forever.
                delta.clear();
            }
            else if (delta.get_delta_sync(net_buffer_) != 0)
                return -1;

            delta.clear_sequence();

            _send();
        }
        return 0;
    }

    void MasterServer::notify()
    {
        cv_.notify_one();
    }

    MasterServer::~MasterServer()
    {
        stop();
    }

    SlaveServer &SlaveServer::instance(uint16_t port, const char *ip)
    {
        static SlaveServer slave{port, ip};
        static int ret = slave.init();
        if (ret < 0)
        {
            KV_ERROR("bad slave server");
            exit(0);
        }
        return slave;
    }

    int SlaveServer::init()
    {
        // initialize the netbuffer
        void *ptr = allocator::kv_malloc(sizeof(Netbuffer));
        if (!ptr)
            return -1;

        net_buffer_ = new (ptr) Netbuffer(RDMA_BUFFER_SIZE);
        if (net_buffer_->init() != 0)
            return -2;

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

        KV_INFO("connecting to master at %s:%u over RDMA", ip_, (unsigned)port_);

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

        if (0 != rdma_create_queuepair(cm_id_, &pd_, &cq_))
        {
            KV_ERROR("rdma_create_queuepair: %s", strerror(errno));
            return 0;
        }

        rdma_alloc_buffer(cm_id_, pd_, RDMA_BUFFER_SIZE, SMALL_BUFFER_SIZE);

        // The master starts pushing as soon as the connection is established, so
        // the receive buffer has to be posted before connecting.
        if (0 != rdma_post_recv(cm_id_))
            return -1;

        struct rdma_conn_param param;
        memset(&param, 0, sizeof(param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.retry_count = 3;

        if (0 != rdma_connect(cm_id_, &param))
        {
            KV_ERROR("rdma_connect: %s", strerror(errno));
            return -1;
        }

        rdma_wait_cm_event(channel_, RDMA_CM_EVENT_ESTABLISHED, NULL);

        return 0;
    }

    int SlaveServer::_recv()
    {
        if (!cm_id_)
            return -1;
        replicate::conn_manager_t *cm = (replicate::conn_manager_t *)cm_id_->context;

        struct ibv_wc wc;
        if (0 != rdma_poll_wc(cq_, &wc) || wc.opcode != IBV_WC_RECV)
            return -1;

        struct packet_info pi;
        memset(&pi, 0, sizeof(pi));
        memcpy(&pi, cm->rbuff, sizeof(struct packet_info));
        if (pi.magic != MAGIC)
            return -2;
        size_t total_size = pi.total;

        if (net_buffer_->write_buffer(cm->rbuff + sizeof(struct packet_info), total_size) < 0)
            return -3;

        // Re-arm before acknowledging: the master may push again as soon as the
        // acknowledgement lands, and iWARP drops the connection if no receive is posted.
        if (0 != rdma_post_recv(cm_id_))
            return -4;

        memcpy(cm->sbuff, &pi, sizeof(struct packet_info));
        if (0 != rdma_post_send(cm_id_, sizeof(struct packet_info)))
            return -5;

        if (0 != rdma_poll_wc(cq_, &wc) || wc.opcode != IBV_WC_SEND)
            return -6;

        return 0;
    }

    void SlaveServer::server_loop()
    {
        while (1)
        {
            int ret = _recv();
            if (ret < 0)
            {
                KV_ERROR("replica stopped receiving from the master: %d", ret);
                return;
            }

            size_t size = net_buffer_->get_size();
            if (size == 0)
                continue;

            char *buff = net_buffer_->get_buffer(size);

            if (kv_protocal::KvStoreProtocal::instance().process_sync_payload(buff, size) < 0)
                return;
        }
    }

    SlaveServer::~SlaveServer()
    {
        rdma_disconnect(cm_id_);

        rdma_free_buffer(cm_id_);

        rdma_destroy_queuepair(cm_id_, pd_, cq_);

        rdma_destroy_id(cm_id_);

        rdma_destroy_event_channel(channel_);
    }
} // namespace replicate

extern "C" __attribute__((noipa, visibility("default"))) void COMMIT_FUNC(int sequence)
{
    // Keeps `sequence` live in the first argument register for the uprobe to read.
    asm volatile("" : : "r"(sequence) : "memory");
}
