#ifndef __URING_TCP_H
#define __URING_TCP_H

#define MAX_CONN_SIZE 1048576
#include <stddef.h>
#include <stdint.h>

#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <mutex>
#include <sys/epoll.h>

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <liburing.h>
#include <sys/uio.h>

#include <signal.h>

#include "status.h"

#define ACCEPT_EVENT 1
#define READ_EVENT 2
#define WRITE_EVENT 4

#define ENTRIES 1024

#ifndef PORT_NUM
#define PORT_NUM 20
#endif

#define TIME_SUB_MS(t1, t2) ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_usec - t2.tv_usec) / 1000)

namespace proactor
{
    struct Conn
    {
        bool is_used;
        uint16_t event;
        uint16_t addr_idx;
        int fd;
        network::StatusM status;

        struct ::iovec *r_iovec;
        struct ::iovec *w_iovec;
        struct ::iovec *io_iovec;
        uint32_t io_iovec_size;
        size_t io_bytes_done;
        size_t io_bytes_total;
    };

    class ConnPool
    {
    public:
        static ConnPool *get_connpool();
        Conn *operator[](int fd);

        void clean_up_conn(int fd);

        int setup_accept_conn(int fd);

        int setup_client_conn(int fd);

        void clean_up_r_w(int fd);

    private:
        ConnPool() {}
        ~ConnPool();
        ConnPool(const ConnPool &) = delete;
        ConnPool(ConnPool &&) = delete;
        ConnPool &operator=(const ConnPool &) = delete;
        ConnPool &operator=(ConnPool &&) = delete;

        Conn _pool[MAX_CONN_SIZE] = {0};
    };

    class TcpServers
    {
    public:
        TcpServers(uint16_t port) : _port(port) {}

        int init();

        int start_eventloop();

        ~TcpServers();

    private:
        int init_server(uint16_t port);

        int set_event_accept(int fd, int flags, struct ::sockaddr *addr,
                             ::socklen_t *addrlen, Conn *conn);

        int set_event_recv(int fd, Conn *conn, int flags);

        int set_event_send(int fd, Conn *conn, int flags);

        int accept_cb(Conn *conn, struct io_uring_cqe *cqe);

        int recv_cb(Conn *conn, struct io_uring_cqe *cqe);

        int send_cb(Conn *conn, struct io_uring_cqe *cqe);

        struct io_uring _ring;
        int _fd_list[PORT_NUM] = {0};
        sockaddr_in _sock_in_list[PORT_NUM] = {0};
        socklen_t _socklen_list[PORT_NUM] = {0};
        uint16_t _port;
    };

    class Timer
    {
    public:
        static Timer &get_timer()
        {
            static Timer timer;
            return timer;
        }

        int get_duration_ms()
        {
            struct timeval current;
            gettimeofday(&current, NULL);
            std::lock_guard lk{_mtx};
            int duration = TIME_SUB_MS(current, _begin);

            memcpy(&_begin, &current, sizeof(struct timeval));
            return duration;
        }

    private:
        Timer()
        {
            gettimeofday(&_begin, NULL);
        }
        Timer(const Timer &) = delete;
        Timer(Timer &&) = delete;

        Timer &operator=(const Timer &) = delete;
        Timer &operator=(Timer &&) = delete;

        ~Timer() {}

        timeval _begin;
        std::mutex _mtx;
    };
}

#endif // __URING_TCP_H