#ifndef __CLIENT_H
#define __CLIENT_H

#include <string>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <mutex>

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "kv_protocal.hpp"

using string = std::basic_string<
    char,
    std::char_traits<char>,
    allocator::MyAllocator<char>>;

namespace kv_client
{
    class KvClient
    {
    public:
        KvClient(const std::string &ip, uint16_t port) : _ip(ip), _port(port), _fd(-1) {}
        ~KvClient()
        {
            if (_fd >= 0)
                close(_fd);
        }

        int init();

        char *submit_request(const string &command, const string &key, const string &value);

    private:
        std::string _ip;
        uint16_t _port;
        int _fd;
    };
}

#endif // __CLIENT_H