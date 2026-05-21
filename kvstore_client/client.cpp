#include "client.h"
#include "allocator.h"

#include <errno.h>

namespace kv_client
{
    static int send_all(int fd, const char *buf, size_t len)
    {
        size_t sent = 0;
        while (sent < len)
        {
            ssize_t ret = send(fd, buf + sent, len - sent, 0);
            if (ret < 0)
            {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (ret == 0)
                return -1;
            sent += static_cast<size_t>(ret);
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

    char *KvClient::submit_request(const std::string &command)
    {
        struct KvHeader header;
        memset(&header, 0, HEADER_SIZE);

        header.body_length = static_cast<uint16_t>(command.size());

        size_t buf_size = command.size() + HEADER_SIZE;

        char *buf = (char *)allocator::kv_malloc(buf_size);
        if (buf == nullptr)
            return nullptr;

        memcpy(buf, &header, HEADER_SIZE);

        memcpy(buf + HEADER_SIZE, command.c_str(), command.size());

        if (send_all(_fd, buf, buf_size) != 0)
        {
            perror("Error send");
            goto clean;
        }

        allocator::kv_free(buf);
        buf = nullptr;

        memset(&header, 0, HEADER_SIZE);

        if (recv_all(_fd, reinterpret_cast<char *>(&header), HEADER_SIZE) != 0)
        {
            perror("Error recv");
            goto clean;
        }

        buf = (char *)allocator::kv_malloc(header.body_length + 1);
        if (buf == nullptr)
        {
            goto clean;
        }

        if (recv_all(_fd, buf, header.body_length) != 0)
        {
            perror("Error recv");
            goto clean;
        }

        buf[header.body_length] = '\0';

        return buf;
    clean:
        allocator::kv_free(buf);
        return nullptr;
    }
}