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

#include "allocator.h"
#include "kv_header.h"

using string = std::basic_string<
    char,
    std::char_traits<char>,
    allocator::MyAllocator<char>>;

namespace kv_client
{
    struct KvRequest
    {
        string command;
        string key;
        string value;
        kv_protocal::TimeoutSpec timeout;

        KvRequest()
            : timeout{} {}
        KvRequest(const string &cmd, const string &k, const string &v, const kv_protocal::TimeoutSpec &t = {})
            : command(cmd), key(k), value(v), timeout(t) {}
        KvRequest(const char *cmd, const char *k, const char *v, const kv_protocal::TimeoutSpec &t = {})
            : command(cmd), key(k), value(v), timeout(t) {}
    };

    struct KvResponse
    {
        char *data;
        uint32_t length;
    };

    struct KvBatchResponse
    {
        uint32_t num_response;
        KvResponse *responses;
    };

    class KvClient
    {
    public:
        KvClient(const std::string &ip, uint16_t port)
            : _ip(ip), _port(port), _fd(-1) {}
        ~KvClient()
        {
            if (_fd >= 0)
                close(_fd);
        }

        int init();

        char *submit_request(const string &command, const string &key, const string &value);
        char *submit_request(const string &command, const string &key, const string &value, const kv_protocal::TimeoutSpec &timeout);
        char *submit_request(const string &line);
        char *submit_request(const std::string &line);
        int submit_batch(const KvRequest *requests, uint32_t num_request, KvBatchResponse *response);

        static void free_batch_response(KvBatchResponse *response);

    private:
        std::string _ip;
        uint16_t _port;
        int _fd;
    };
} // namespace kv_client

#endif // __CLIENT_H