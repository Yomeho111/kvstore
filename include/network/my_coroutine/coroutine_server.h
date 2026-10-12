#ifndef __COR_SERVER
#define __COR_SERVER

#include "hpc_socket.h"
#include "hpc_coroutine.h"

#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "kv_protocal.hpp"

#ifndef PORT_NUM
#define PORT_NUM 20
#endif

#define TIME_SUB_MS(tv1, tv2) ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

namespace hpc_coroutine
{
    class TcpServers
    {
    public:
        TcpServers(const char *ip, uint16_t port)
            : _ip(ip), _port(port) {}

        ~TcpServers()
        {
            for (int i = 0; i < PORT_NUM; i++)
            {
                if (_fd_list[i] >= 0)
                    close(_fd_list[i]);
            }
        }

        int init();

        int start_eventloop();

    private:
        TcpServers(const TcpServers &) = delete;
        TcpServers(TcpServers &&) = delete;

        TcpServers &operator=(const TcpServers &) = delete;
        TcpServers &operator=(TcpServers &&) = delete;
        const char *_ip;
        uint16_t _port;
        int _fd_list[PORT_NUM];
    };

    class TcpSlaveServer
    {
    public:
        TcpSlaveServer(uint16_t port, uint16_t port_rdma, uint16_t my_port, const char *my_ip, const char *master_ip, const char *ip_rdma)
            : _master_port(port), _port_rdma(port_rdma), _my_port(my_port), _my_ip(my_ip), _master_ip(master_ip), _ip_rdma(ip_rdma) {}

        ~TcpSlaveServer()
        {
            for (int i = 0; i < PORT_NUM; i++)
            {
                if (_fd_list[i] >= 0)
                    close(_fd_list[i]);
            }
        }

        int init();

        int start_eventloop();

    private:
        TcpSlaveServer(const TcpSlaveServer &) = delete;
        TcpSlaveServer(TcpSlaveServer &&) = delete;

        TcpSlaveServer &operator=(const TcpSlaveServer &) = delete;
        TcpSlaveServer &operator=(TcpSlaveServer &&) = delete;

        uint16_t _master_port;
        uint16_t _port_rdma;
        uint16_t _my_port;
        const char *_my_ip;
        const char *_master_ip;
        const char *_ip_rdma;
        int _fd_list[PORT_NUM];
    };
} // namespace hpc_coroutine

#endif // __COR_SERVER