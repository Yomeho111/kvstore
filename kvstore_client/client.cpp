#include "client.h"
#include "allocator.h"

#include <errno.h>
#include <sys/uio.h>

namespace kv_client
{

    static int writev_all(int fd, struct iovec *iov, int iovcnt)
    {
        int current = 0;

        while (current < iovcnt)
        {
            ssize_t n = writev(fd, &iov[current], iovcnt - current);
            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return -1;
            }

            ssize_t remaining = n;
            while (current < iovcnt && remaining >= static_cast<ssize_t>(iov[current].iov_len))
            {
                remaining -= static_cast<ssize_t>(iov[current].iov_len);
                ++current;
            }

            if (current < iovcnt && remaining > 0)
            {
                iov[current].iov_base = static_cast<char *>(iov[current].iov_base) + remaining;
                iov[current].iov_len -= remaining;
            }
        }

        return 0;
    }

    static int recv_all(int fd, char *buf, size_t len)
    {
        size_t received = 0;
        while (received < len)
        {
            ssize_t ret = recv(fd, buf + received, len - received, 0);
            if (ret < 0)
            {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (ret == 0)
                return -1;
            received += static_cast<size_t>(ret);
        }
        return 0;
    }

    int KvClient::init()
    {
        _fd = socket(AF_INET, SOCK_STREAM, 0);
        if (_fd < 0)
            return -1;

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(struct sockaddr_in));

        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr(_ip.c_str());
        server_addr.sin_port = htons(_port);

        if (0 != connect(_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in)))
        {
            perror("connect");
            return -1;
        }
        return 0;
    }

    char *KvClient::submit_request(const string &command,
                                   const string &key,
                                   const string &value)
    {
        const bool has_value = !value.empty();

        kv_protocal::KvHeader header{};
        header.body_length =
            static_cast<uint32_t>(command.size() + key.size() + value.size());
        header.key_length = static_cast<uint32_t>(key.size());
        header.value_length = static_cast<uint32_t>(value.size());

        // ---------- send (scatter/gather, zero copy) ----------
        struct iovec iov[4];
        int iovcnt = 0;

        iov[iovcnt++] = {&header, kv_protocal::HEADER_SIZE};

        if (!command.empty())
            iov[iovcnt++] = {const_cast<char *>(command.data()), command.size()};

        if (!key.empty())
            iov[iovcnt++] = {const_cast<char *>(key.data()), key.size()};

        if (has_value)
            iov[iovcnt++] = {const_cast<char *>(value.data()), value.size()};

        if (writev_all(_fd, iov, iovcnt) != 0)
        {
            perror("Error send");
            return nullptr;
        }

        // ---------- recv header ----------
        kv_protocal::KvHeader resp_header{};
        if (recv_all(_fd,
                     reinterpret_cast<char *>(&resp_header),
                     kv_protocal::HEADER_SIZE) != 0)
        {
            perror("Error recv header");
            return nullptr;
        }

        // ---------- recv body ----------
        char *buf = static_cast<char *>(
            allocator::kv_malloc(resp_header.body_length + 1));
        if (!buf)
        {
            return nullptr;
        }

        if (recv_all(_fd, buf, resp_header.body_length) != 0)
        {
            perror("Error recv body");
            allocator::kv_free(buf);
            return nullptr;
        }

        buf[resp_header.body_length] = '\0';
        return buf;
    }

}