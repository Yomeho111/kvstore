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

#include <signal.h>

#include "status.h"

#define MAX_CONN_SIZE 1048576
#ifndef PORT_NUM
#define PORT_NUM 20
#endif

#define TIME_SUB_MS(t1, t2) ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_usec - t2.tv_usec) / 1000)

namespace reactor
{

    typedef int (*ReactorCallback)(int);

    class TcpServers;

    struct Conn
    {
        bool is_used;
        int fd;
        network::StatusM status;
        size_t rbuf_size;
        size_t wbuf_size;
        char *rbuf;
        char *wbuf;
        ReactorCallback recv_cb;
        ReactorCallback send_cb;
        TcpServers *servers;
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

        int setup_accept_conn(int fd, TcpServers *servers);

        int setup_client_conn(int fd, TcpServers *servers);

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

        int set_event(int fd, uint32_t events, int ops);

        int del_fd(int fd);

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
#endif // __REACTOR_H