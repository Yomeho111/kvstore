#include "uring_tcp.h"

#include <errno.h>

#include "allocator.h"
#include "kv_protocal.hpp"

namespace proactor
{
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
            if (_pool[i].rbuf != nullptr)
            {
                allocator::kv_free(_pool[i].rbuf);
                _pool[i].rbuf = nullptr;
            }
            if (_pool[i].wbuf != nullptr)
            {
                allocator::kv_free(_pool[i].wbuf);
                _pool[i].wbuf = nullptr;
            }
            if (_pool[i].is_used)
            {
                close(_pool[i].fd);
            }
        }
    }

    void ConnPool::clean_up_conn(int fd)
    {
        _pool[fd].is_used = false;
        close(fd);
        _pool[fd].fd = -1;
        _pool[fd].rbuf_size = 0;
        _pool[fd].wbuf_size = 0;
        _pool[fd].status.status = 0;
        _pool[fd].status.buffer_size = 0;
        if (_pool[fd].rbuf != nullptr)
        {
            allocator::kv_free(_pool[fd].rbuf);
            _pool[fd].rbuf = nullptr;
        }
        if (_pool[fd].wbuf != nullptr)
        {
            allocator::kv_free(_pool[fd].wbuf);
            _pool[fd].wbuf = nullptr;
        }
    }

    int ConnPool::setup_accept_conn(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid fd for register_listenfd");
            return -1;
        }
        _pool[fd].fd = fd;
        _pool[fd].is_used = true;
        _pool[fd].status.status = 0;
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
        _pool[fd].fd = fd;
        _pool[fd].status.status = 0;
        _pool[fd].status.buffer_size = kv_protocal::HEADER_SIZE;
        _pool[fd].rbuf_size = 0;
        _pool[fd].wbuf_size = 0;
        _pool[fd].is_used = true;
        _pool[fd].event = READ_EVENT;

        if (_pool[fd].rbuf != nullptr)
        {
            allocator::kv_free(_pool[fd].rbuf);
            _pool[fd].rbuf = nullptr;
        }
        if (_pool[fd].wbuf != nullptr)
        {
            allocator::kv_free(_pool[fd].wbuf);
            _pool[fd].wbuf = nullptr;
        }
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

        conn->event = READ_EVENT;
        if (conn->rbuf != nullptr)
        {
            allocator::kv_free(conn->rbuf);
            conn->rbuf = nullptr;
        }
        conn->rbuf = (char *)allocator::kv_malloc(conn->status.buffer_size + 1);
        if (conn->rbuf == nullptr)
        {
            perror("error malloc");
            return -1;
        }
        conn->rbuf_size = conn->status.buffer_size;
        io_uring_prep_recv(sqe, fd, conn->rbuf, conn->rbuf_size, flags);
        memcpy(&sqe->user_data, &conn, sizeof(conn));
        return 0;
    }

    int TcpServers::set_event_send(int fd, Conn *conn, int flags)
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&_ring);

        conn->event = WRITE_EVENT;
        io_uring_prep_send(sqe, fd, conn->wbuf, kv_protocal::HEADER_SIZE + conn->wbuf_size, flags);
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
            if (ret == -ECONNRESET)
            {
                // printf("ECONNRESET by recv.\n");
                goto clean;
            }
            errno = -ret;
            perror("error recv");
            ret = -1;
            goto clean;
        }

        switch (conn->status.status)
        {
        case 0:
        {
            if (count != conn->status.buffer_size)
            {
                perror("corrupted header");
                goto clean;
            }

            if (kv_protocal::KvStoreProtocal::instance().process_header(&conn->status, (struct kv_protocal::KvHeader *)conn->rbuf) != 0)
            {
                perror("Corrupted header");
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
        case 1:
        {
            if (count != conn->status.buffer_size)
            {
                perror("corrupted body");
                goto clean;
            }

            if (conn->wbuf != nullptr)
            {
                allocator::kv_free(conn->wbuf);
                conn->wbuf = nullptr;
            }

            conn->rbuf[conn->rbuf_size] = '\0';
            count = kv_protocal::KvStoreProtocal::instance().process_body(&conn->status, conn->rbuf, conn->rbuf_size, &conn->wbuf);
            if (count < 0)
            {
                perror("Error handling body");
                ret = -1;
                goto clean;
            }

            conn->wbuf_size = count;

            set_event_send(conn->fd, conn, 0);
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

        if (ret != kv_protocal::HEADER_SIZE + conn->wbuf_size)
        {
            perror("Error send");
            ConnPool::get_connpool()->clean_up_conn(conn->fd);
            return -1;
        }

        if (conn->wbuf != nullptr)
        {
            allocator::kv_free(conn->wbuf);
            conn->wbuf = nullptr;
        }

        conn->status.status = 0;
        conn->status.buffer_size = kv_protocal::HEADER_SIZE;

        set_event_recv(conn->fd, conn, 0);
        return 0;
    }

    int TcpServers::start_eventloop()
    {
        while (1)
        {
            io_uring_submit(&_ring);

            struct io_uring_cqe *cqe;
            if (io_uring_wait_cqe(&_ring, &cqe) < 0)
            {
                perror("error io_uring_wait_cqe");
                return -1;
            }

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
}