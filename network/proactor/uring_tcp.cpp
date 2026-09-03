#include "uring_tcp.h"

#include <errno.h>
#include <string>

#include "allocator.h"
#include "kv_protocal.hpp"
#include "network_utils.h"
#include "timer.h"
#include "hiredis.h"
#include "kv_log.h"

#define RESP_RECV_BUF_SIZE 16384
#define RESP_MAX_ARGS 64

namespace proactor
{
    static void free_io_iovec(Conn *conn)
    {
        if (conn == nullptr || conn->io_iovec == nullptr)
            return;

        allocator::kv_free(conn->io_iovec);
        conn->io_iovec = nullptr;
        conn->io_iovec_size = 0;
    }

    static int make_iovec_view(const struct iovec *src, uint32_t src_count, size_t skip, struct iovec **dst, uint32_t *dst_count)
    {
        if (src == nullptr || dst == nullptr || dst_count == nullptr || src_count == 0)
            return -1;

        uint32_t first = 0;
        while (first < src_count && skip >= src[first].iov_len)
        {
            skip -= src[first].iov_len;
            first++;
        }

        if (first == src_count)
            return -1;

        *dst_count = src_count - first;
        *dst = static_cast<struct iovec *>(allocator::kv_malloc(*dst_count * sizeof(struct iovec)));
        if (*dst == nullptr)
            return -1;

        memcpy(*dst, src + first, *dst_count * sizeof(struct iovec));
        if (skip > 0)
        {
            (*dst)[0].iov_base = static_cast<char *>((*dst)[0].iov_base) + skip;
            (*dst)[0].iov_len -= skip;
        }

        return 0;
    }

    static int prepare_iovec_io(Conn *conn, struct iovec *src, uint32_t src_count)
    {
        if (conn == nullptr || src == nullptr || src_count == 0)
            return -1;

        free_io_iovec(conn);

        if (conn->io_bytes_total == 0)
            conn->io_bytes_total = network::iovec_total_len(src, src_count);

        return make_iovec_view(src, src_count, conn->io_bytes_done, &conn->io_iovec, &conn->io_iovec_size);
    }

    static void reset_iovec_io(Conn *conn)
    {
        if (conn == nullptr)
            return;

        free_io_iovec(conn);
        conn->io_bytes_done = 0;
        conn->io_bytes_total = 0;
    }

    ConnPool *ConnPool::get_connpool()
    {
        static thread_local ConnPool pool;
        return &pool;
    }

    Conn *ConnPool::operator[](int fd)
    {
        if (fd >= MAX_CONN_SIZE || fd < 0) [[unlikely]]
        {
            KV_ERROR("invalid Conn");
            return nullptr;
        }
        return &_pool[fd];
    }
    ConnPool::~ConnPool()
    {
        for (int i = 0; i < MAX_CONN_SIZE; i++)
        {
            clean_up_r_w(i);

            if (_pool[i].is_used)
            {
                close(_pool[i].fd);
            }
        }
    }

    void ConnPool::clean_up_r_w(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
            return;

        reset_iovec_io(&_pool[fd]);

        if (_pool[fd].status.req_info)
        {
            allocator::kv_free(_pool[fd].status.req_info);
            _pool[fd].status.req_info = nullptr;
        }

        if (_pool[fd].r_iovec)
        {
            if (_pool[fd].status.status == network::READ_NUM_REQUEST || _pool[fd].status.num_request == 0)
            {
                allocator::kv_free(_pool[fd].r_iovec[0].iov_base);
                _pool[fd].r_iovec[0].iov_base = nullptr;
            }
            else
            {
                for (uint32_t j = 0; j < _pool[fd].status.num_request; j++)
                {
                    allocator::kv_free(_pool[fd].r_iovec[j].iov_base);
                    _pool[fd].r_iovec[j].iov_base = nullptr;
                }
            }

            allocator::kv_free(_pool[fd].r_iovec);
            _pool[fd].r_iovec = nullptr;
        }

        if (_pool[fd].w_iovec)
        {
            for (uint32_t j = 0; j < _pool[fd].status.w_iovec_size; j++)
            {
                allocator::kv_free(_pool[fd].w_iovec[j].iov_base);
                _pool[fd].w_iovec[j].iov_base = nullptr;
            }

            allocator::kv_free(_pool[fd].w_iovec);
            _pool[fd].w_iovec = nullptr;
        }

        _pool[fd].status.w_iovec_size = 0;
    }

    void ConnPool::clean_up_conn(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
            return;
        if (_pool[fd].is_used && _pool[fd].fd >= 0)
            close(_pool[fd].fd);
        _pool[fd].fd = -1;
        _pool[fd].status.status = network::READ_NUM_REQUEST;
        clean_up_r_w(fd);

        _pool[fd].status.num_request = 0;
        _pool[fd].io_bytes_done = 0;
        _pool[fd].io_bytes_total = 0;
        _pool[fd].heartbeat_pending = false;
        _pool[fd].expect_reply = false;

        if (_pool[fd].resp_reader)
        {
            redisReaderFree(static_cast<redisReader *>(_pool[fd].resp_reader));
            _pool[fd].resp_reader = nullptr;
        }
        if (_pool[fd].resp_buf)
        {
            allocator::kv_free(_pool[fd].resp_buf);
            _pool[fd].resp_buf = nullptr;
        }
        if (_pool[fd].resp_out)
        {
            allocator::kv_free(_pool[fd].resp_out);
            _pool[fd].resp_out = nullptr;
        }
        _pool[fd].resp_out_len = 0;
        _pool[fd].resp_out_done = 0;
        _pool[fd].resp_close_after = false;
        _pool[fd].proto = PROTO_UNKNOWN;
        _pool[fd].is_used = false;
    }

    int ConnPool::setup_accept_conn(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            KV_ERROR("Invalid fd for register_listenfd");
            return -1;
        }
        clean_up_r_w(fd);
        _pool[fd].fd = fd;
        _pool[fd].is_used = true;
        _pool[fd].event = ACCEPT_EVENT;

        return 0;
    }

    int ConnPool::setup_client_conn(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            KV_ERROR("Invalid clientfd for register_clientfd");
            return -1;
        }
        clean_up_r_w(fd);
        _pool[fd].fd = fd;
        _pool[fd].status.status = network::READ_NUM_REQUEST;
        _pool[fd].is_used = true;
        _pool[fd].event = READ_EVENT;
        _pool[fd].heartbeat_pending = false;
        _pool[fd].expect_reply = false;
        _pool[fd].proto = PROTO_UNKNOWN;
        _pool[fd].resp_reader = nullptr;
        _pool[fd].resp_buf = nullptr;
        _pool[fd].resp_out = nullptr;
        _pool[fd].resp_out_len = 0;
        _pool[fd].resp_out_done = 0;
        _pool[fd].resp_close_after = false;

        return 0;
    }

    int TcpServers::init_server(uint16_t port)
    {
        int listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listenfd == -1)
        {
            KV_ERROR("error socket");
            return -1;
        }

        // Allow reuse of the address (fixes "Address already in use" error)
        int opt = 1;
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in servaddr;
        memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
        servaddr.sin_port = htons(port);

        if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1)
        {
            KV_ERROR("bind error");
            close(listenfd);
            return -1;
        }

        if (listen(listenfd, 10) == -1)
        {
            KV_ERROR("error listen");
            close(listenfd);
            return -1;
        }

        ConnPool *pool = ConnPool::get_connpool();
        if (pool->setup_accept_conn(listenfd) == -1)
        {
            KV_ERROR("error setup_accept_conn");
            return -1;
        }
        return listenfd;
    }

    int TcpServers::set_event_accept(int fd, int flags, struct ::sockaddr *addr,
                                     ::socklen_t *addrlen, Conn *conn)
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&_ring);

        io_uring_prep_accept(sqe, fd, addr, addrlen, 0);
        memcpy(&sqe->user_data, &conn, sizeof(sqe->user_data));
        return 0;
    }

    int TcpServers::set_event_recv(int fd, Conn *conn, int flags)
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&_ring);
        if (sqe == nullptr)
            return -1;

        conn->event = READ_EVENT;

        switch (conn->status.status)
        {
            case network::READ_NUM_REQUEST:
            {
                if (network::alloc_single_iovec(&conn->r_iovec, kv_protocal::NUM_HEADER_SIZE) != 0)
                {
                    KV_ERROR("error malloc");
                    return -1;
                }
                io_uring_prep_recv(sqe, fd, conn->r_iovec[0].iov_base, conn->r_iovec[0].iov_len, flags | MSG_WAITALL);
                break;
            }
            case network::READ_HEADER:
            {
                io_uring_prep_recv(sqe, fd, conn->status.req_info, conn->status.num_request * kv_protocal::HEADER_SIZE, flags | MSG_WAITALL);
                break;
            }
            case network::READ_BODY:
            {
                if (prepare_iovec_io(conn, conn->r_iovec, conn->status.num_request) != 0)
                    return -1;
                io_uring_prep_readv(sqe, fd, conn->io_iovec, conn->io_iovec_size, 0);
                break;
            }
            default:
                return -1;
        }

        memcpy(&sqe->user_data, &conn, sizeof(conn));
        return 0;
    }

    int TcpServers::set_event_send(int fd, Conn *conn, int flags)
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&_ring);
        if (sqe == nullptr)
            return -1;

        conn->event = WRITE_EVENT;
        if (prepare_iovec_io(conn, conn->w_iovec, conn->status.w_iovec_size) != 0)
            return -1;
        io_uring_prep_writev(sqe, fd, conn->io_iovec, conn->io_iovec_size, 0);
        memcpy(&sqe->user_data, &conn, sizeof(conn));
        return 0;
    }

    int TcpServers::init()
    {
        struct io_uring_params params;
        memset(&params, 0, sizeof(params));

        if (io_uring_queue_init_params(ENTRIES, &_ring, &params) < 0)
        {
            KV_ERROR("error io_uring_queue_init_params");
            return -1;
        }

        auto *pool = ConnPool::get_connpool();

        for (int i = 0; i < PORT_NUM; i++)
        {
            int sockfd = init_server(_port + i);
            if (sockfd == -1)
            {
                KV_ERROR("error init server");
                continue;
            }
            _socklen_list[i] = sizeof(_sock_in_list[i]);
            _fd_list[i] = sockfd;

            Conn *conn = (*pool)[sockfd];
            conn->addr_idx = i;
            set_event_accept(sockfd, 0, (struct sockaddr *)&_sock_in_list[i], &_socklen_list[i], conn);
        }

        return 0;
    }

    int TcpServers::accept_cb(Conn *conn, struct io_uring_cqe *cqe)
    {
        set_event_accept(conn->fd, 0, (struct sockaddr *)&_sock_in_list[conn->addr_idx], &_socklen_list[conn->addr_idx], conn);

        int clientfd = cqe->res;

        if (clientfd < 0)
        {
            errno = -clientfd;
            KV_ERROR("error accept");
            return -1;
        }

        ConnPool *pool = ConnPool::get_connpool();
        if (pool->setup_client_conn(clientfd) == -1)
        {
            return -1;
        }
#ifdef TIMER
        if (clientfd % 1000 == 0)
        {
            int duration = Timer::get_timer().get_duration_ms();
            printf("Connection num: %d, elipse: %d\n", clientfd, duration);
            fflush(stdout);
        }
#endif

        Conn *client_conn = pool->operator[](clientfd);

        // Peek the first byte to detect the wire protocol before committing to the
        // native state machine. RESP commands (redis-cli / hiredis / redis-benchmark)
        // always begin with '*'. MSG_PEEK does not consume the byte.
        struct io_uring_sqe *sqe = io_uring_get_sqe(&_ring);
        if (sqe == nullptr)
        {
            KV_ERROR("Error get_sqe for protocol detection");
            pool->clean_up_conn(clientfd);
            return -1;
        }
        client_conn->event = READ_EVENT;
        io_uring_prep_recv(sqe, clientfd, &client_conn->peek_byte, 1, MSG_PEEK);
        memcpy(&sqe->user_data, &client_conn, sizeof(client_conn));
        return 0;
    }

    int TcpServers::recv_cb(Conn *conn, struct io_uring_cqe *cqe)
    {
        int count = cqe->res;
        int ret = 0;

        if (count == 0)
        {
            goto clean;
        }
        else if (count < 0)
        {
            if (count == -ECONNRESET)
            {
                // printf("ECONNRESET by recv.\n");
                goto clean;
            }
            errno = -count;
            KV_ERROR("error recv");
            ret = -1;
            goto clean;
        }

        if (conn->proto == PROTO_UNKNOWN)
        {
            // Completion of the 1-byte detection peek.
            conn->proto = (conn->peek_byte == '*') ? PROTO_RESP : PROTO_CUSTOM;
            if (conn->proto == PROTO_RESP)
            {
                if (resp_issue_recv(conn) == -1)
                {
                    ret = -1;
                    goto clean;
                }
                return 0;
            }
            // Native protocol: issue the normal NumHeader recv (the peeked byte is
            // still queued because MSG_PEEK did not consume it).
            if (set_event_recv(conn->fd, conn, 0) == -1)
            {
                KV_ERROR("Error set_event_recv");
                ret = -1;
                goto clean;
            }
            return 0;
        }

        if (conn->proto == PROTO_RESP)
            return resp_recv_cb(conn, cqe);

        switch (conn->status.status)
        {
            case network::READ_NUM_REQUEST:
            {
                if (count != kv_protocal::NUM_HEADER_SIZE)
                {
                    KV_ERROR("corrupted number of request");
                    goto clean;
                }

                uint32_t num_request = static_cast<kv_protocal::NumHeader *>(conn->r_iovec[0].iov_base)->num_request;
                network::free_single_iovec(&conn->r_iovec);

                if (num_request > 0)
                {
                    if (kv_protocal::KvStoreProtocal::instance().process_num_request(&conn->status, num_request) != 0)
                    {
                        KV_ERROR("Processing number of request failure");
                        ret = -1;
                        goto clean;
                    }
                }

                if (set_event_recv(conn->fd, conn, 0) == -1)
                {
                    KV_ERROR("Error set_event_recv");
                    ret = -1;
                    goto clean;
                }
                break;
            }

            case network::READ_HEADER:
            {
                if (count != static_cast<int>(conn->status.num_request * kv_protocal::HEADER_SIZE))
                {
                    KV_ERROR("corrupted header");
                    goto clean;
                }

                if (kv_protocal::KvStoreProtocal::instance().process_header(&conn->status, &conn->r_iovec) != 0)
                {
                    KV_ERROR("Processing header failure");
                    ret = -1;
                    goto clean;
                }

                if (set_event_recv(conn->fd, conn, 0) == -1)
                {
                    KV_ERROR("Error set_event_recv");
                    ret = -1;
                    goto clean;
                }
                break;
            }

            case network::READ_BODY:
            {
                conn->io_bytes_done += static_cast<size_t>(count);
                free_io_iovec(conn);

                if (conn->io_bytes_done < conn->io_bytes_total)
                {
                    if (set_event_recv(conn->fd, conn, 0) == -1)
                    {
                        KV_ERROR("Error set_event_recv");
                        ret = -1;
                        goto clean;
                    }
                    break;
                }

                reset_iovec_io(conn);

                count = kv_protocal::KvStoreProtocal::instance().process_body(&conn->status, conn->r_iovec, &conn->w_iovec);
                if (count < 0)
                {
                    KV_ERROR("Error handling body");
                    ret = -1;
                    goto clean;
                }
                if (conn->status.status == network::SEND_RESPONSE)
                {
                    if (set_event_send(conn->fd, conn, 0) == -1)
                    {
                        KV_ERROR("Error set_event_send");
                        ret = -1;
                        goto clean;
                    }
                }
                else
                {
                    if (set_event_recv(conn->fd, conn, 0) == -1)
                    {
                        KV_ERROR("Error set_event_recv");
                        ret = -1;
                        goto clean;
                    }
                }
                break;
            }
            default:
                KV_ERROR("Error recv status");
                ret = -1;
                goto clean;
        }

        return ret;
    clean:
        ConnPool::get_connpool()->clean_up_conn(conn->fd);
        return ret;
    }

    int TcpServers::send_cb(Conn *conn, struct io_uring_cqe *cqe)
    {
        if (conn->proto == PROTO_RESP)
            return resp_send_cb(conn, cqe);

        int ret = cqe->res;

        if (ret < 0)
        {
            errno = -ret;
            KV_ERROR("Error send");
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return -1;
        }

        conn->io_bytes_done += static_cast<size_t>(ret);
        free_io_iovec(conn);

        if (conn->io_bytes_done < conn->io_bytes_total)
        {
            if (set_event_send(conn->fd, conn, 0) == -1)
            {
                KV_ERROR("Error set_event_send");
                ConnPool::get_connpool()->clean_up_conn(conn->fd);
                return -1;
            }
            return 0;
        }

        reset_iovec_io(conn);

        ConnPool::get_connpool()->clean_up_r_w(conn->fd);
        conn->status.status = network::READ_NUM_REQUEST;
        conn->status.num_request = 0;

        if (set_event_recv(conn->fd, conn, 0) == -1)
        {
            KV_ERROR("Error set_event_recv");
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return -1;
        }
        return 0;
    }

    int TcpServers::resp_issue_recv(Conn *conn)
    {
        if (conn->resp_reader == nullptr)
        {
            conn->resp_reader = redisReaderCreate();
            if (conn->resp_reader == nullptr)
                return -1;
        }
        if (conn->resp_buf == nullptr)
        {
            conn->resp_buf = static_cast<char *>(allocator::kv_malloc(RESP_RECV_BUF_SIZE));
            if (conn->resp_buf == nullptr)
                return -1;
        }

        struct io_uring_sqe *sqe = io_uring_get_sqe(&_ring);
        if (sqe == nullptr)
            return -1;

        conn->event = READ_EVENT;
        io_uring_prep_recv(sqe, conn->fd, conn->resp_buf, RESP_RECV_BUF_SIZE, 0);
        memcpy(&sqe->user_data, &conn, sizeof(conn));
        return 0;
    }

    int TcpServers::resp_issue_send(Conn *conn)
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&_ring);
        if (sqe == nullptr)
            return -1;

        conn->event = WRITE_EVENT;
        io_uring_prep_send(sqe, conn->fd, conn->resp_out + conn->resp_out_done,
                           conn->resp_out_len - conn->resp_out_done, 0);
        memcpy(&sqe->user_data, &conn, sizeof(conn));
        return 0;
    }

    int TcpServers::resp_recv_cb(Conn *conn, struct io_uring_cqe *cqe)
    {
        int count = cqe->res;
        if (count <= 0)
        {
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return 0;
        }

        redisReader *reader = static_cast<redisReader *>(conn->resp_reader);
        if (redisReaderFeed(reader, conn->resp_buf, count) != REDIS_OK)
        {
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return 0;
        }

        std::string out;
        bool close_after = false;
        void *reply = nullptr;

        // A single read may carry several pipelined commands; drain them all.
        while (redisReaderGetReply(reader, &reply) == REDIS_OK && reply != nullptr)
        {
            redisReply *rr = static_cast<redisReply *>(reply);
            if (rr->type == REDIS_REPLY_ARRAY && rr->elements > 0)
            {
                int argc = static_cast<int>(rr->elements);
                if (argc > RESP_MAX_ARGS)
                {
                    out += "-ERR too many arguments\r\n";
                }
                else
                {
                    char *argv[RESP_MAX_ARGS];
                    size_t argvlen[RESP_MAX_ARGS];
                    for (int i = 0; i < argc; i++)
                    {
                        argv[i] = rr->element[i]->str;
                        argvlen[i] = rr->element[i]->len;
                    }
                    if (kv_protocal::KvStoreProtocal::instance().process_resp_command(argc, argv, argvlen, out) == 1)
                        close_after = true;
                }
            }
            else
            {
                out += "-ERR protocol error\r\n";
            }
            freeReplyObject(reply);
            reply = nullptr;
        }

        if (reader->err)
        {
            out += "-ERR protocol error\r\n";
            close_after = true;
        }

        if (out.empty())
        {
            if (close_after)
            {
                ConnPool::get_connpool()->clean_up_conn(conn->fd);
                return 0;
            }
            // Need more bytes to complete a command.
            if (resp_issue_recv(conn) == -1)
            {
                ConnPool::get_connpool()->clean_up_conn(conn->fd);
                return -1;
            }
            return 0;
        }

        conn->resp_out = static_cast<char *>(allocator::kv_malloc(out.size()));
        if (conn->resp_out == nullptr)
        {
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return -1;
        }
        memcpy(conn->resp_out, out.data(), out.size());
        conn->resp_out_len = out.size();
        conn->resp_out_done = 0;
        conn->resp_close_after = close_after;

        if (resp_issue_send(conn) == -1)
        {
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return -1;
        }
        return 0;
    }

    int TcpServers::resp_send_cb(Conn *conn, struct io_uring_cqe *cqe)
    {
        int ret = cqe->res;
        if (ret < 0)
        {
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return 0;
        }

        conn->resp_out_done += static_cast<size_t>(ret);
        if (conn->resp_out_done < conn->resp_out_len)
        {
            // Partial send; queue the remainder.
            if (resp_issue_send(conn) == -1)
            {
                ConnPool::get_connpool()->clean_up_conn(conn->fd);
                return -1;
            }
            return 0;
        }

        allocator::kv_free(conn->resp_out);
        conn->resp_out = nullptr;
        conn->resp_out_len = 0;
        conn->resp_out_done = 0;

        if (conn->resp_close_after)
        {
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return 0;
        }

        if (resp_issue_recv(conn) == -1)
        {
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return -1;
        }
        return 0;
    }

    int TcpServers::start_eventloop()
    {
        while (1)
        {
            io_uring_submit(&_ring);

            auto &timer_m = kv_timer::TimerManager::instance();
            int wait_time = timer_m.get_next_timeout_ms();
            int ret = 0;

            if (wait_time >= 0)
            {
                __kernel_timespec ts;
                ts.tv_sec = wait_time / 1000;
                ts.tv_nsec = (wait_time % 1000) * 1000000;

                struct io_uring_cqe *cqe;
                ret = io_uring_wait_cqe_timeout(&_ring, &cqe, &ts);
            }
            else
            {
                struct io_uring_cqe *cqe;
                ret = io_uring_wait_cqe(&_ring, &cqe);
            }

            if (ret == -ETIME)
            {
                timer_m.handle_expired();
                continue;
            }
            else if (ret < 0)
            {
                KV_ERROR("error io_uring_wait_cqe");
                return -1;
            }

            timer_m.handle_expired();

            struct io_uring_cqe *cqes[1024];
            int nready = io_uring_peek_batch_cqe(&_ring, cqes, 1024);

            for (int i = 0; i < nready; i++)
            {
                Conn *conn;
                memcpy(&conn, &cqes[i]->user_data, sizeof(conn));

                if (conn->event == ACCEPT_EVENT)
                {
                    if (accept_cb(conn, cqes[i]) == -1)
                    {
                        KV_ERROR("error accept_cb");
                        return -1;
                    }
                }
                else if (conn->event == READ_EVENT)
                {
                    if (recv_cb(conn, cqes[i]) == -1)
                    {
                        KV_ERROR("error recv_cb");
                        return -1;
                    }
                }
                else if (conn->event == WRITE_EVENT)
                {
                    if (send_cb(conn, cqes[i]) == -1)
                    {
                        KV_ERROR("error send_cb");
                        return -1;
                    }
                }
            }

            io_uring_cq_advance(&_ring, nready);
        }
    }

    TcpServers::~TcpServers()
    {
        auto *pool = ConnPool::get_connpool();
        for (int i = 0; i < PORT_NUM; i++)
        {
            pool->clean_up_conn(_fd_list[i]);
        }
    }
} // namespace proactor