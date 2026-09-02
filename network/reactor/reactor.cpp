#include "reactor.h"

#include <unistd.h>
#include <errno.h>
#include <string>
#include "allocator.h"
#include "kv_protocal.hpp"
#include "network_utils.h"
#include "timer.h"
#include "hiredis.h"

#define RESP_RECV_BUF_SIZE 16384
#define RESP_MAX_ARGS 64

namespace reactor
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
            clean_up_r_w(i);

            if (_pool[i].is_used)
            {
                close(_pool[i].fd);
            }
        }
    }

    void ConnPool::clean_up_r_w(int fd)
    {
        if (_pool[fd].status.req_info)
        {
            allocator::kv_free(_pool[fd].status.req_info);
            _pool[fd].status.req_info = nullptr;
        }

        if (_pool[fd].r_iovec)
        {
            for (int j = 0; j < _pool[fd].status.num_request; j++)
            {
                allocator::kv_free(_pool[fd].r_iovec[j].iov_base);
                _pool[fd].r_iovec[j].iov_base = nullptr;
            }

            allocator::kv_free(_pool[fd].r_iovec);
            _pool[fd].r_iovec = nullptr;
        }

        if (_pool[fd].w_iovec)
        {
            for (int j = 0; j < _pool[fd].status.w_iovec_size; j++)
            {
                allocator::kv_free(_pool[fd].w_iovec[j].iov_base);
                _pool[fd].w_iovec[j].iov_base = nullptr;
            }

            allocator::kv_free(_pool[fd].w_iovec);
            _pool[fd].w_iovec = nullptr;
        }
    }

    void ConnPool::clean_up_conn(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
            return;
        _pool[fd].is_used = false;
        _pool[fd].heartbeat_pending = false;
        close(fd);
        _pool[fd].fd = -1;
        _pool[fd].status.status = 0;
        clean_up_r_w(fd);

        _pool[fd].recv_cb = nullptr;
        _pool[fd].send_cb = nullptr;
        _pool[fd].status.num_request = 0;
        _pool[fd].servers = nullptr;

        if (_pool[fd].resp_reader)
        {
            redisReaderFree(static_cast<redisReader *>(_pool[fd].resp_reader));
            _pool[fd].resp_reader = nullptr;
        }
        _pool[fd].proto = PROTO_UNKNOWN;
    }

    int ConnPool::setup_accept_conn(int fd, TcpBase *servers)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid fd for register_listenfd");
            return -1;
        }
        clean_up_r_w(fd);
        _pool[fd].fd = fd;
        _pool[fd].heartbeat_pending = false;
        _pool[fd].recv_cb = accept_callback;
        _pool[fd].is_used = true;
        _pool[fd].servers = servers;

        return 0;
    }

    int ConnPool::setup_client_conn(int fd, TcpBase *servers)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid clientfd for register_clientfd");
            return -1;
        }
        clean_up_r_w(fd);
        _pool[fd].fd = fd;
        _pool[fd].heartbeat_pending = false;
        _pool[fd].is_used = true;
        _pool[fd].recv_cb = recv_callback;
        _pool[fd].send_cb = send_callback;
        _pool[fd].servers = servers;
        _pool[fd].proto = PROTO_UNKNOWN;

        return 0;
    }

    int TcpServers::set_event(int fd, uint32_t events, int ops)
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));

        ev.data.fd = fd;
        ev.events = events;

        if (epoll_ctl(_epfd, ops, fd, &ev) == -1)
        {
            perror("error epoll_ctl");
            return -1;
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

        if (listen(listenfd, 10) == -2)
        {
            perror("error listen");
            close(listenfd);
            return -1;
        }

        ConnPool *pool = ConnPool::get_connpool();
        if (pool->setup_accept_conn(listenfd, this) == -1)
        {
            perror("error setup_accept_conn");
            return -1;
        }

        if (set_event(listenfd, EPOLLIN, EPOLL_CTL_ADD) == -1)
        {
            perror("error set_event");
            pool->clean_up_conn(listenfd);
            return -1;
        }

        return listenfd;
    }

    TcpServers::~TcpServers()
    {
        auto *pool = ConnPool::get_connpool();
        for (int i = 0; i < PORT_NUM; i++)
        {
            pool->clean_up_conn(_fd_list[i]);
        }
    }

    int TcpServers::init()
    {
        _epfd = epoll_create(1);
        for (int i = 0; i < PORT_NUM; i++)
        {
            int sockfd = init_server(_port + i);
            if (sockfd == -1)
            {
                perror("error init server");
                continue;
            }
            _fd_list[i] = sockfd;
        }
        return 0;
    }

    int TcpServers::del_fd(int fd)
    {
        if (epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, NULL) == -1)
        {
            perror("error epoll_ctl");
            return -1;
        }
        return 0;
    }

    int TcpServers::start_eventloop()
    {
        auto *pool = ConnPool::get_connpool();
        while (1)
        {
            struct epoll_event events[1024];
            auto &timer_m = kv_timer::TimerManager::instance();
            int wait_time = timer_m.get_next_timeout_ms();
            int nready = epoll_wait(_epfd, events, 1024, wait_time);

            if (nready < 0)
            {
                if (errno == EINTR)
                    continue;
                perror("error epoll_wait");
                return -1;
            }

            timer_m.handle_expired();

            for (int i = 0; i < nready; i++)
            {
                int connfd = events[i].data.fd;
                Conn *conn = (*pool)[connfd];

                if (events[i].events & EPOLLIN)
                {
                    conn->recv_cb(connfd);
                }

                if (events[i].events & EPOLLOUT)
                {
                    conn->send_cb(connfd);
                }
            }
        }
        return 0;
    }

    int accept_callback(int fd)
    {
        struct sockaddr_in caddr;
        socklen_t addrlen = sizeof(caddr);
        struct epoll_event ev;

        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid sockfd");
            return -1;
        }

        auto *pool = ConnPool::get_connpool();

        int clientfd = accept(fd, (struct sockaddr *)&caddr, &addrlen);
        if (clientfd == -1)
        {
            perror("error accept");
            return -1;
        }

        Conn *sock_conn = pool->operator[](fd);

        // register the clientfd into conn_list
        if (pool->setup_client_conn(clientfd, sock_conn->servers) == -1)
        {
            perror("error register_clientfd");
            close(clientfd);
            return -1;
        }

        if (sock_conn->servers->set_event(clientfd, EPOLLIN, EPOLL_CTL_ADD) == -1)
        {
            perror("error set event");
            pool->clean_up_conn(clientfd);
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
        return 0;
    }

    int recv_callback(int fd)
    {
        int ret = 0;
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid sockfd");
            return -1;
        }

        auto *pool = ConnPool::get_connpool();

        Conn *conn = (*pool)[fd];

        if (conn->proto == PROTO_UNKNOWN)
        {
            // Peek the first byte to distinguish the native binary protocol from
            // RESP. RESP multi-bulk commands (redis-cli / hiredis / redis-benchmark)
            // always begin with '*'; the native protocol begins with a NumHeader.
            char c = 0;
            int pk = recv(fd, &c, 1, MSG_PEEK);
            if (pk == 0)
            {
                if (conn->servers->del_fd(fd) == -1)
                    perror("error del_fd");
                pool->clean_up_conn(fd);
                return 0;
            }
            if (pk < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    return 0;
                perror("Error recv");
                if (conn->servers->del_fd(fd) == -1)
                    perror("error del_fd");
                pool->clean_up_conn(fd);
                return -1;
            }
            conn->proto = (c == '*') ? PROTO_RESP : PROTO_CUSTOM;
        }

        if (conn->proto == PROTO_RESP)
            return resp_recv_callback(fd);

        switch (conn->status.status)
        {
            case network::READ_NUM_REQUEST:
            {
                struct kv_protocal::NumHeader num_header;
                memset(&num_header, 0, kv_protocal::NUM_HEADER_SIZE);

                int count = recv(fd, &num_header, kv_protocal::NUM_HEADER_SIZE, 0);
                if (count == 0)
                    goto clean;
                else if (count < 0)
                {
                    if (count == -ECONNRESET)
                        goto clean;
                    ret = -1;
                    perror("Error recv");
                    goto clean;
                }
                else if (count != kv_protocal::NUM_HEADER_SIZE)
                {
                    perror("corrupted number of request");
                    goto clean;
                }

                if (num_header.num_request > 0)
                {
                    if (kv_protocal::KvStoreProtocal::instance().process_num_request(&conn->status, num_header.num_request) != 0)
                    {
                        perror("Processing number of error failure");
                        ret = -1;
                        goto clean;
                    }
                }

                if (conn->servers->set_event(fd, EPOLLIN, EPOLL_CTL_MOD))
                {
                    perror("error set_event");
                    ret = -1;
                    goto clean;
                }
                break;
            }

            case network::READ_HEADER:
            {
                int count = recv(fd, conn->status.req_info, conn->status.num_request * kv_protocal::HEADER_SIZE, 0);
                if (count == 0)
                    goto clean;
                else if (count < 0)
                {
                    if (count == -ECONNRESET)
                        goto clean;
                    ret = -1;
                    perror("Error recv");
                    goto clean;
                }
                else if (count != conn->status.num_request * kv_protocal::HEADER_SIZE)
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

                if (conn->servers->set_event(fd, EPOLLIN, EPOLL_CTL_MOD))
                {
                    perror("error set_event");
                    ret = -1;
                    goto clean;
                }
                break;
            }

            case network::READ_BODY:
            {

                int count = readv_full(fd, conn->r_iovec, conn->status.num_request);
                if (count == 0)
                    goto clean;
                else if (count < 0)
                {
                    if (count == -ECONNRESET)
                        goto clean;
                    ret = -1;
                    perror("Error recv");
                    goto clean;
                }

                count = kv_protocal::KvStoreProtocal::instance().process_body(&conn->status, conn->r_iovec, &conn->w_iovec);
                if (count < 0)
                {
                    perror("Error handling body");
                    ret = -1;
                    goto clean;
                }
                if (conn->status.status == network::SEND_RESPONSE)
                {
                    if (conn->servers->set_event(fd, EPOLLOUT, EPOLL_CTL_MOD))
                    {
                        perror("error set_event");
                        ret = -1;
                        goto clean;
                    }
                }
                else
                {
                    if (conn->servers->set_event(fd, EPOLLIN, EPOLL_CTL_MOD))
                    {
                        perror("error set_event");
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
        if (conn->servers->del_fd(fd) == -1)
        {
            perror("error del_fd");
        }
        pool->clean_up_conn(fd);
        return ret;
    }

    int resp_recv_callback(int fd)
    {
        auto *pool = ConnPool::get_connpool();
        Conn *conn = (*pool)[fd];

        if (conn->resp_reader == nullptr)
        {
            conn->resp_reader = redisReaderCreate();
            if (conn->resp_reader == nullptr)
                goto clean;
        }

        {
            char buf[RESP_RECV_BUF_SIZE];
            int n = recv(fd, buf, sizeof(buf), 0);
            if (n == 0)
                goto clean;
            if (n < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    return 0;
                perror("Error recv");
                goto clean;
            }

            redisReader *reader = static_cast<redisReader *>(conn->resp_reader);
            if (redisReaderFeed(reader, buf, n) != REDIS_OK)
                goto clean;

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

            if (!out.empty())
            {
                struct ::iovec io;
                io.iov_base = const_cast<char *>(out.data());
                io.iov_len = out.size();
                if (writev_all(fd, &io, 1) < 0)
                {
                    perror("error send");
                    goto clean;
                }
            }

            if (close_after)
                goto clean;
        }

        return 0;

    clean:
        if (conn->servers->del_fd(fd) == -1)
            perror("error del_fd");
        pool->clean_up_conn(fd);
        return 0;
    }

    int send_callback(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid sockfd");
            return -1;
        }

        auto *pool = ConnPool::get_connpool();

        Conn *conn = (*pool)[fd];
        int ret = 0;

        if (conn->status.status == network::SEND_RESPONSE)
        {
            int count = writev_all(fd, conn->w_iovec, conn->status.w_iovec_size);
            if (count < 0)
            {
                perror("error send");
                ret = -1;
                goto clean;
            }

            if (conn->servers->set_event(fd, EPOLLIN, EPOLL_CTL_MOD))
            {
                perror("error set_event");
                ret = -1;
                goto clean;
            }

            conn->heartbeat_pending = false;
        }
        else
        {
            perror("Error send status");
            ret = -1;
            goto clean;
        }

        conn->status.status = 0;
        pool->clean_up_r_w(fd);

        return ret;
    clean:
        if (conn->servers->del_fd(fd) == -1)
        {
            perror("error del_fd");
        }
        pool->clean_up_conn(fd);
        return ret;
    }
} // namespace reactor
