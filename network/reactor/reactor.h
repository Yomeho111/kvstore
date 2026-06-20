#ifndef __REACTOR_H
#define __REACTOR_H

#include <stdint.h>
#include <stddef.h>
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
#include <sys/uio.h>

#include "status.h"

#define MAX_CONN_SIZE 1048576
#ifndef PORT_NUM
#define PORT_NUM 20
#endif

#define TIME_SUB_MS(t1, t2) ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_usec - t2.tv_usec) / 1000)

namespace reactor
{

    typedef int (*ReactorCallback)(int);

    class TcpBase
    {
    public:
        virtual int set_event(int fd, uint32_t events, int ops) = 0;

        virtual int del_fd(int fd) = 0;
    };

    struct Conn
    {
        bool is_used;
        bool heartbeat_pending;
        int fd;
        network::StatusM status;
        struct ::iovec *r_iovec;
        struct ::iovec *w_iovec;
        ReactorCallback recv_cb;
        ReactorCallback send_cb;
        TcpBase *servers;
    };

    int accept_callback(int fd);

    int recv_callback(int fd);

    int send_callback(int fd);

    class ConnPool
    {
    public:
        static ConnPool *get_connpool();
        Conn *operator[](int fd);

        void clean_up_conn(int fd);

        int setup_accept_conn(int fd, TcpBase *servers);

        int setup_client_conn(int fd, TcpBase *servers);

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

    class TcpServers : public TcpBase
    {
    public:
        TcpServers(uint16_t port)
            : _port(port) {}

        int init();

        int start_eventloop();

        int set_event(int fd, uint32_t events, int ops) override;

        int del_fd(int fd) override;

        ~TcpServers();

    private:
        int init_server(uint16_t port);

        TcpServers(const TcpServers &) = delete;
        TcpServers(TcpServers &&) = delete;

        TcpServers &operator=(const TcpServers &) = delete;
        TcpServers &operator=(TcpServers &&) = delete;

        uint16_t _port;
        int _epfd;
        int _fd_list[PORT_NUM];
    };

    class TcpSlaveServer : public TcpBase
    {
    public:
        TcpSlaveServer(uint16_t port, const char *ip)
            : _port(port), _ip(ip), _heartbeat(false) {}

        int init();

        int start_eventloop();

        int set_event(int fd, uint32_t events, int ops) override;

        int del_fd(int fd) override;

        ~TcpSlaveServer();

    private:
        int init_client();

        TcpSlaveServer(const TcpSlaveServer &) = delete;
        TcpSlaveServer(TcpSlaveServer &&) = delete;

        TcpSlaveServer &operator=(const TcpSlaveServer &) = delete;
        TcpSlaveServer &operator=(TcpSlaveServer &&) = delete;

        bool _heartbeat;
        uint16_t _port;
        int _epfd;
        int _fd;
        const char *_ip;
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

} // namespace reactor
#endif // __REACTOR_H