#include "client.h"

namespace kv_client
{
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

        char *buf = (char *)malloc(buf_size);
        if (buf == nullptr)
            return nullptr;

        memcpy(buf, &header, HEADER_SIZE);

        memcpy(buf + HEADER_SIZE, command.c_str(), command.size());

        int ret = 0;

        ret = send(_fd, buf, buf_size, 0);
        if (ret < 0)
        {
            perror("Error send");
            goto clean;
        }

        memset(&header, 0, HEADER_SIZE);

        ret = recv(_fd, &header, HEADER_SIZE, 0);
        if (ret == 0)
        {
            printf("Connection Error\n");
            goto clean;
        }
        else if (ret < 0)
        {
            perror("Error recv");
            goto clean;
        }

        buf = (char *)realloc(buf, header.body_length + 1);
        if (buf == nullptr)
        {
            goto clean;
        }

        ret = recv(_fd, buf, header.body_length, 0);
        if (ret == 0)
        {
            printf("Connection Error\n");
            goto clean;
        }
        else if (ret < 0)
        {
            perror("Error recv");
            goto clean;
        }

        buf[header.body_length] = '\0';

        return buf;
    clean:
        free(buf);
        return nullptr;
    }
}