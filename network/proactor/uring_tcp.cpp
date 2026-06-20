#include "uring_tcp.h"

#include <errno.h>

#include "allocator.h"
#include "kv_protocal.hpp"
#include "network_utils.h"
#include "timer.h"

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
            perror("invalid Conn");
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
        _pool[fd].is_used = false;
    }

    int ConnPool::setup_accept_conn(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid fd for register_listenfd");
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
            perror("Invalid clientfd for register_clientfd");
            return -1;
        }
        clean_up_r_w(fd);
        _pool[fd].fd = fd;
        _pool[fd].status.status = network::READ_NUM_REQUEST;
        _pool[fd].is_used = true;
        _pool[fd].event = READ_EVENT;
        _pool[fd].heartbeat_pending = false;
        _pool[fd].expect_reply = false;

        return 0;
    }

    int TcpServers::init_server(uint16_t port)
    {
        int listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listenfd == -1)
        {
            perror("error socket");
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
            perror("bind error");
            close(listenfd);
            return -1;
        }

        if (listen(listenfd, 10) == -1)
        {
            perror("error listen");
            close(listenfd);
            return -1;
        }

        ConnPool *pool = ConnPool::get_connpool();
        if (pool->setup_accept_conn(listenfd) == -1)
        {
            perror("error setup_accept_conn");
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
                    perror("error malloc");
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
            perror("error io_uring_queue_init_params");
            return -1;
        }

        auto *pool = ConnPool::get_connpool();

        for (int i = 0; i < PORT_NUM; i++)
        {
            int sockfd = init_server(_port + i);
            if (sockfd == -1)
            {
                perror("error init server");
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
            perror("error accept");
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

        if (set_event_recv(clientfd, pool->operator[](clientfd), 0) == -1)
        {
            perror("Error set_event_recv");
            pool->clean_up_conn(clientfd);
            return -1;
        }
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
            perror("error recv");
            ret = -1;
            goto clean;
        }

        switch (conn->status.status)
        {
            case network::READ_NUM_REQUEST:
            {
                if (count != kv_protocal::NUM_HEADER_SIZE)
                {
                    perror("corrupted number of request");
                    goto clean;
                }

                uint32_t num_request = static_cast<kv_protocal::NumHeader *>(conn->r_iovec[0].iov_base)->num_request;
                network::free_single_iovec(&conn->r_iovec);

                if (num_request > 0)
                {
                    if (kv_protocal::KvStoreProtocal::instance().process_num_request(&conn->status, num_request) != 0)
                    {
                        perror("Processing number of request failure");
                        ret = -1;
                        goto clean;
                    }
                }

                if (set_event_recv(conn->fd, conn, 0) == -1)
                {
                    perror("Error set_event_recv");
                    ret = -1;
                    goto clean;
                }
                break;
            }

            case network::READ_HEADER:
            {
                if (count != static_cast<int>(conn->status.num_request * kv_protocal::HEADER_SIZE))
                {
                    perror("corrupted header");
                    goto clean;
                }

                if (kv_protocal::KvStoreProtocal::instance().process_header(&conn->status, &conn->r_iovec) != 0)
                {
                    perror("Processing header failure");
                    ret = -1;
                    goto clean;
                }

                if (set_event_recv(conn->fd, conn, 0) == -1)
                {
                    perror("Error set_event_recv");
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
                        perror("Error set_event_recv");
                        ret = -1;
                        goto clean;
                    }
                    break;
                }

                reset_iovec_io(conn);

                count = kv_protocal::KvStoreProtocal::instance().process_body(&conn->status, conn->r_iovec, &conn->w_iovec);
                if (count < 0)
                {
                    perror("Error handling body");
                    ret = -1;
                    goto clean;
                }
                if (conn->status.status == network::SEND_RESPONSE)
                {
                    if (set_event_send(conn->fd, conn, 0) == -1)
                    {
                        perror("Error set_event_send");
                        ret = -1;
                        goto clean;
                    }
                }
                else
                {
                    if (set_event_recv(conn->fd, conn, 0) == -1)
                    {
                        perror("Error set_event_recv");
                        ret = -1;
                        goto clean;
                    }
                }
                break;
            }
            default:
                perror("Error recv status");
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
        int ret = cqe->res;

        if (ret < 0)
        {
            errno = -ret;
            perror("Error send");
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return -1;
        }

        conn->io_bytes_done += static_cast<size_t>(ret);
        free_io_iovec(conn);

        if (conn->io_bytes_done < conn->io_bytes_total)
        {
            if (set_event_send(conn->fd, conn, 0) == -1)
            {
                perror("Error set_event_send");
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
            perror("Error set_event_recv");
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
                perror("error io_uring_wait_cqe");
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
                        perror("error accept_cb");
                        return -1;
                    }
                }
                else if (conn->event == READ_EVENT)
                {
                    if (recv_cb(conn, cqes[i]) == -1)
                    {
                        perror("error recv_cb");
                        return -1;
                    }
                }
                else if (conn->event == WRITE_EVENT)
                {
                    if (send_cb(conn, cqes[i]) == -1)
                    {
                        perror("error send_cb");
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

    int TcpSlaveServer::init_client()
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            perror("socket");
            return -1;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(_port);

        // Convert "127.0.0.1" to binary IP format
        if (inet_pton(AF_INET, _ip, &server_addr.sin_addr) <= 0)
        {
            perror("inet_pton");
            close(fd);
            return -1;
        }

        if (connect(fd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
        {
            perror("connect");
            close(fd);
            return -1;
        }

        ConnPool *pool = ConnPool::get_connpool();
        if (pool->setup_client_conn(fd) == -1)
        {
            perror("error setup_client_conn");
            close(fd);
            return -1;
        }

        return fd;
    }

    int TcpSlaveServer::set_event_recv(int fd, Conn *conn, int flags)
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
                    perror("error malloc");
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

    int TcpSlaveServer::set_event_send(int fd, Conn *conn, int flags)
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

    int TcpSlaveServer::recv_cb(Conn *conn, struct io_uring_cqe *cqe)
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
                goto clean;
            errno = -count;
            perror("error recv");
            ret = -1;
            goto clean;
        }

        switch (conn->status.status)
        {
            case network::READ_NUM_REQUEST:
            {
                if (count != kv_protocal::NUM_HEADER_SIZE)
                {
                    perror("corrupted number of request");
                    goto clean;
                }

                uint32_t num_request = static_cast<kv_protocal::NumHeader *>(conn->r_iovec[0].iov_base)->num_request;
                network::free_single_iovec(&conn->r_iovec);

                if (num_request == 0)
                {
                    // no-update heartbeat response: exchange done, go idle
                    conn->heartbeat_pending = false;
                    conn->expect_reply = false;
                    conn->status.status = network::READ_NUM_REQUEST;
                    conn->status.num_request = 0;
                    break;
                }

                if (kv_protocal::KvStoreProtocal::instance().process_num_request(&conn->status, num_request) != 0)
                {
                    perror("Processing number of request failure");
                    ret = -1;
                    goto clean;
                }

                if (set_event_recv(conn->fd, conn, 0) == -1)
                {
                    perror("Error set_event_recv");
                    ret = -1;
                    goto clean;
                }
                break;
            }

            case network::READ_HEADER:
            {
                if (count != static_cast<int>(conn->status.num_request * kv_protocal::HEADER_SIZE))
                {
                    perror("corrupted header");
                    goto clean;
                }

                if (kv_protocal::KvStoreProtocal::instance().process_header(&conn->status, &conn->r_iovec) != 0)
                {
                    perror("Processing header failure");
                    ret = -1;
                    goto clean;
                }

                if (set_event_recv(conn->fd, conn, 0) == -1)
                {
                    perror("Error set_event_recv");
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
                        perror("Error set_event_recv");
                        ret = -1;
                        goto clean;
                    }
                    break;
                }

                reset_iovec_io(conn);

                count = kv_protocal::KvStoreProtocal::instance().process_body(&conn->status, conn->r_iovec, &conn->w_iovec);
                if (count < 0)
                {
                    perror("Error handling body");
                    ret = -1;
                    goto clean;
                }
                if (conn->status.status == network::SEND_RESPONSE)
                {
                    if (set_event_send(conn->fd, conn, 0) == -1)
                    {
                        perror("Error set_event_send");
                        ret = -1;
                        goto clean;
                    }
                }
                else
                {
                    if (set_event_recv(conn->fd, conn, 0) == -1)
                    {
                        perror("Error set_event_recv");
                        ret = -1;
                        goto clean;
                    }
                }
                break;
            }
            default:
                perror("Error recv status");
                ret = -1;
                goto clean;
        }

        return ret;
    clean:
        ConnPool::get_connpool()->clean_up_conn(conn->fd);
        return ret;
    }

    int TcpSlaveServer::send_cb(Conn *conn, struct io_uring_cqe *cqe)
    {
        int ret = cqe->res;

        if (ret < 0)
        {
            errno = -ret;
            perror("Error send");
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return -1;
        }

        conn->io_bytes_done += static_cast<size_t>(ret);
        free_io_iovec(conn);

        if (conn->io_bytes_done < conn->io_bytes_total)
        {
            if (set_event_send(conn->fd, conn, 0) == -1)
            {
                perror("Error set_event_send");
                ConnPool::get_connpool()->clean_up_conn(conn->fd);
                return -1;
            }
            return 0;
        }

        reset_iovec_io(conn);

        ConnPool::get_connpool()->clean_up_r_w(conn->fd);
        conn->status.status = network::READ_NUM_REQUEST;
        conn->status.num_request = 0;

        if (conn->expect_reply)
        {
            // we just sent the heartbeat; wait for the master's sync reply
            conn->expect_reply = false;
            if (set_event_recv(conn->fd, conn, 0) == -1)
            {
                perror("Error set_event_recv");
                ConnPool::get_connpool()->clean_up_conn(conn->fd);
                return -1;
            }
        }
        else
        {
            // we just sent our response to the master; exchange done, go idle
            conn->heartbeat_pending = false;
        }
        return 0;
    }

    int TcpSlaveServer::init()
    {
        struct io_uring_params params;
        memset(&params, 0, sizeof(params));

        if (io_uring_queue_init_params(ENTRIES, &_ring, &params) < 0)
        {
            perror("error io_uring_queue_init_params");
            return -1;
        }

        int fd = init_client();
        if (fd < 0)
        {
            perror("init");
            return -1;
        }
        _fd = fd;
        return 0;
    }

    int TcpSlaveServer::start_eventloop()
    {
        auto *pool = ConnPool::get_connpool();
        Conn *client = (*pool)[_fd];

        while (1)
        {
            io_uring_submit(&_ring);

            __kernel_timespec ts;
            ts.tv_sec = 5;
            ts.tv_nsec = 0;

            struct io_uring_cqe *cqe;
            int ret = io_uring_wait_cqe_timeout(&_ring, &cqe, &ts);

            if (ret == -ETIME)
            {
                // set up heartbeat packet only when the connection is idle and no heartbeat is in flight
                if (client == nullptr || client->heartbeat_pending || client->w_iovec != nullptr || client->status.status == network::SEND_RESPONSE)
                    continue;

                int hret = kv_protocal::KvStoreProtocal::instance().construct_heartbeat_packet(&client->status, &client->w_iovec);
                if (hret < 0)
                {
                    perror("Error handling heartbeat packet");
                    return -1;
                }

                client->heartbeat_pending = true;
                client->expect_reply = true;

                if (set_event_send(_fd, client, 0) == -1)
                {
                    perror("error set_event_send");
                    return -1;
                }
                continue;
            }
            else if (ret < 0)
            {
                perror("error io_uring_wait_cqe_timeout");
                return -1;
            }

            struct io_uring_cqe *cqes[1024];
            int nready = io_uring_peek_batch_cqe(&_ring, cqes, 1024);

            for (int i = 0; i < nready; i++)
            {
                Conn *conn;
                memcpy(&conn, &cqes[i]->user_data, sizeof(conn));

                if (conn->event == READ_EVENT)
                {
                    if (recv_cb(conn, cqes[i]) == -1)
                    {
                        perror("error recv_cb");
                        return -1;
                    }
                }
                else if (conn->event == WRITE_EVENT)
                {
                    if (send_cb(conn, cqes[i]) == -1)
                    {
                        perror("error send_cb");
                        return -1;
                    }
                }
            }

            io_uring_cq_advance(&_ring, nready);
        }
        return 0;
    }

    TcpSlaveServer::~TcpSlaveServer()
    {
        ConnPool::get_connpool()->clean_up_conn(_fd);
        io_uring_queue_exit(&_ring);
    }
} // namespace proactor