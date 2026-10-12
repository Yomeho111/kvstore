#include "tcp_server.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include "delta.h"
#include "cache_pool.h"

namespace delta
{

    static ssize_t send_all(int fd, const char *buf, size_t len)
    {
        size_t sent = 0;

        while (sent < len)
        {
            ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);

            if (n > 0)
            {
                sent += static_cast<size_t>(n);
                continue;
            }

            if (n == -1 && errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        return static_cast<ssize_t>(sent);
    }

    TransferServer::~TransferServer()
    {
        for (int fd : fd_set_)
        {
            close(fd);
        }
    }

    TransferServer &TransferServer::instance()
    {
        static TransferServer server;
        return server;
    }

    int TransferServer::broadcast_buffer(const char *buffer, size_t size)
    {
        for (auto it = fd_set_.begin(); it != fd_set_.end();)
        {
            if (send_all(*it, buffer, size) == -1)
            {
                ::close(*it);
                it = fd_set_.erase(it);
                continue;
            }
            ++it;
        }
        return 0;
    }

    int TransferServer::connect(__u64 addr_port_key, struct Node *node, size_t offset)
    {
        __be32 saddr;
        __u16 sport;
        parse_addr_port_key(addr_port_key, &saddr, &sport);

        char ip[INET_ADDRSTRLEN]{0};
        inet_ntop(AF_INET, &saddr, ip, sizeof(ip));

        int fd = _connect(saddr, sport);
        if (fd < 0)
        {
            return -1;
        }

        fd_set_.insert(fd);

        if (!node || !node->ptr || node->offset <= offset)
        {
            // invalid cached, skip cache transfer
            return 1;
        }

        // send this partial page
        ssize_t n = send_all(fd, node->ptr + offset, node->offset - offset);
        if (n == -1)
        {
            close_fd(fd);
            return -2;
        }

        // send all other cache, the list tail is the only node without a page
        struct Node *cur_node = node->next;
        while (cur_node && cur_node->ptr)
        {
            if (send_all(fd, cur_node->ptr, cur_node->offset) == -1)
            {
                close_fd(fd);
                return -2;
            }
            cur_node = cur_node->next;
        }

        return 0;
    }

    void TransferServer::close_fd(int fd)
    {
        if (fd_set_.count(fd))
        {
            ::close(fd);
            fd_set_.erase(fd);
        }
    }

    int TransferServer::_connect(__be32 saddr, __u16 sport)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return -1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(sport);
        addr.sin_addr.s_addr = saddr;

        // TransferServer::connect would shadow the socket call
        if (::connect(
                fd,
                reinterpret_cast<sockaddr *>(&addr),
                sizeof(addr)) < 0)
        {
            ::close(fd);
            return -1;
        }

        return fd;
    }

} // namespace delta