#include "reactor.h"

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
            if (_pool[i].rbuf != nullptr)
            {
                free(_pool[i].rbuf);
                _pool[i].rbuf = nullptr;
            }
            if (_pool[i].wbuf != nullptr)
            {
                free(_pool[i].wbuf);
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
        if (fd < 0 || fd >= MAX_CONN_SIZE)
            return;
        _pool[fd].is_used = false;
        close(fd);
        _pool[fd].fd = -1;
        _pool[fd].rbuf_size = 0;
        _pool[fd].wbuf_size = 0;
        _pool[fd].servers = nullptr;
        if (_pool[fd].rbuf != nullptr)
        {
            free(_pool[fd].rbuf);
            _pool[fd].rbuf = nullptr;
        }
        if (_pool[fd].wbuf != nullptr)
        {
            free(_pool[fd].wbuf);
            _pool[fd].wbuf = nullptr;
        }

        _pool[fd].recv_cb = nullptr;
        _pool[fd].send_cb = nullptr;
    }

    int ConnPool::setup_accept_conn(int fd, TcpServers *servers)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid fd for register_listenfd");
            return -1;
        }
        _pool[fd].fd = fd;
        _pool[fd].recv_cb = accept_callback;
        _pool[fd].is_used = true;
        _pool[fd].servers = servers;

        return 0;
    }

    int ConnPool::setup_client_conn(int fd, TcpServers *servers)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid clientfd for register_clientfd");
            return -1;
        }
        _pool[fd].fd = fd;
        _pool[fd].rbuf_size = 0;
        _pool[fd].wbuf_size = 0;
        _pool[fd].is_used = true;
        _pool[fd].recv_cb = recv_callback;
        _pool[fd].send_cb = send_callback;
        _pool[fd].servers = servers;

        _pool[fd].rbuf = (char *)malloc(sizeof(char) * BUFFER_SIZE);
        _pool[fd].wbuf = (char *)malloc(sizeof(char) * BUFFER_SIZE);
        if (_pool[fd].rbuf == NULL || _pool[fd].wbuf == NULL)
        {
            perror("Error assign wbuffer or rbuffer");
            clean_up_conn(fd);
            return -1;
        }
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
            int nready = epoll_wait(_epfd, events, 1024, -1);

            if (nready < 0)
            {
                perror("error epoll_wait");
                return -1;
            }

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

        // char *client_ip = inet_ntoa(caddr.sin_addr);

        // printf("%s connect to the server\n", client_ip);

        // register the clientfd into conn_list
        if (pool->setup_client_conn(clientfd, sock_conn->servers) == -1)
        {
            perror("error register_clientfd");
            return -1;
        }

        if (sock_conn->servers->set_event(clientfd, EPOLLIN, EPOLL_CTL_ADD) == -1)
        {
            perror("error set event");
            pool->clean_up_conn(clientfd);
            return -1;
        }

        if (clientfd % 1000 == 0)
        {
            int duration = Timer::get_timer().get_duration_ms();
            printf("Connection num: %d, elipse: %d\n", clientfd, duration);
            fflush(stdout);
        }
        return 0;
    }

    int recv_callback(int fd)
    {
        if (fd < 0 || fd >= MAX_CONN_SIZE)
        {
            perror("Invalid sockfd");
            return -1;
        }

        auto *pool = ConnPool::get_connpool();

        Conn *conn = (*pool)[fd];

        memset(conn->rbuf, 0, BUFFER_SIZE);
        int count = recv(fd, conn->rbuf, BUFFER_SIZE, 0);
        if (count == 0)
        {
            // printf("Connection %d disconnected\n", fd);
            if (conn->servers->del_fd(fd) == -1)
            {
                perror("error del_fd");
            }
            pool->clean_up_conn(fd);
            return -1;
        }
        else if (count < 0)
        {
            if (count == -ECONNRESET)
                return 0;
            perror("Error recv");
            if (conn->servers->del_fd(fd) == -1)
            {
                perror("error del_fd");
            }
            pool->clean_up_conn(fd);
            return -1;
        }

        conn->rbuf_size = count;

        // printf("recv: %d, %s\n", count, conn->rbuf);

        if (conn->servers->set_event(fd, EPOLLOUT, EPOLL_CTL_MOD))
        {
            perror("error set_event");
            pool->clean_up_conn(fd);
            return -1;
        }

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

        // write into the wbuf
        memcpy(conn->wbuf, conn->rbuf, conn->rbuf_size);
        conn->wbuf_size = conn->rbuf_size;

        int count = send(fd, conn->wbuf, conn->wbuf_size, 0);
        if (count < 0)
        {
            perror("error send");
            if (conn->servers->del_fd(fd) == -1)
            {
                perror("error del_fd");
            }
            pool->clean_up_conn(fd);
            return -1;
        }
        if (conn->servers->set_event(fd, EPOLLIN, EPOLL_CTL_MOD))
        {
            perror("error set_event");
            if (conn->servers->del_fd(fd) == -1)
            {
                perror("error del_fd");
            }
            pool->clean_up_conn(fd);
            return -1;
        }

        // printf("send: %ld, %s\n", conn->wbuf_size, conn->wbuf);

        return 0;
    }
}