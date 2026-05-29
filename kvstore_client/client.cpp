#include "client.h"
#include "allocator.h"

#include <errno.h>
#include <limits.h>
#include <sys/uio.h>

namespace kv_client
{

    static uint32_t command_to_idx(const string &cmd)
    {
        for (uint32_t i = kv_protocal::KVS_START + 1; i < kv_protocal::KVS_END; i++)
        {
            size_t len = strlen(kv_protocal::command_str[i]);
            if (cmd.size() == len && memcmp(cmd.data(), kv_protocal::command_str[i], len) == 0)
                return i;
        }
        return kv_protocal::KVS_INVALID;
    }

    static string token_between(const string &line, size_t begin, size_t end)
    {
        while (begin < end && (line[begin] == ' ' || line[begin] == '\t'))
            begin++;
        while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r'))
            end--;
        return string(line.data() + begin, end - begin);
    }

    static bool is_space(char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\r';
    }

    static bool is_set_or_mod(const string &command)
    {
        uint32_t command_idx = command_to_idx(command);
        return command_idx == kv_protocal::KVS_SET || command_idx == kv_protocal::KVS_MOD;
    }

    static int parse_nonnegative_long(const char *begin, const char *end, long *value)
    {
        if (begin == nullptr || end == nullptr || begin == end || value == nullptr)
            return -1;

        char *parse_end = nullptr;
        errno = 0;
        long parsed = strtol(begin, &parse_end, 10);
        if (errno != 0 || parse_end != end || parsed < 0)
            return -1;

        *value = parsed;
        return 0;
    }

    static int parse_timeout_token(const string &token, kv_protocal::TimeoutSpec *timeout, bool *has_timeout)
    {
        if (timeout == nullptr || has_timeout == nullptr)
            return -1;

        *timeout = kv_protocal::TimeoutSpec{};
        *has_timeout = false;

        size_t unit_begin = 0;
        while (unit_begin < token.size() && token[unit_begin] >= '0' && token[unit_begin] <= '9')
            unit_begin++;

        if (unit_begin == 0)
            return 0;

        if (unit_begin == token.size())
            return 0;

        long amount = 0;
        if (parse_nonnegative_long(token.c_str(), token.c_str() + unit_begin, &amount) != 0)
            return -1;

        string unit(token.data() + unit_begin, token.size() - unit_begin);

        if (unit == "h")
        {
            if (amount > LONG_MAX / 3600)
                return -1;
            timeout->tv_sec = amount * 3600;
            timeout->tv_nsec = 0;
        }
        else if (unit == "m")
        {
            if (amount > LONG_MAX / 60)
                return -1;
            timeout->tv_sec = amount * 60;
            timeout->tv_nsec = 0;
        }
        else if (unit == "s")
        {
            timeout->tv_sec = amount;
            timeout->tv_nsec = 0;
        }
        else if (unit == "ms")
        {
            timeout->tv_sec = amount / 1000;
            timeout->tv_nsec = (amount % 1000) * 1000000;
        }
        else if (unit == "us")
        {
            timeout->tv_sec = amount / 1000000;
            timeout->tv_nsec = (amount % 1000000) * 1000;
        }
        else
        {
            return -1;
        }

        *has_timeout = true;
        return 0;
    }

    static int parse_timeout_suffix(const string &value, kv_protocal::TimeoutSpec *timeout, size_t *value_end)
    {
        if (timeout == nullptr || value_end == nullptr)
            return -1;

        *timeout = kv_protocal::TimeoutSpec{};
        *value_end = value.size();

        size_t end = value.size();
        while (end > 0 && is_space(value[end - 1]))
            end--;

        size_t timeout_begin = end;
        while (timeout_begin > 0 && !is_space(value[timeout_begin - 1]))
            timeout_begin--;

        if (timeout_begin == end)
            return 0;

        string timeout_token = token_between(value, timeout_begin, end);
        bool has_timeout = false;
        if (parse_timeout_token(timeout_token, timeout, &has_timeout) != 0)
            return -1;

        if (!has_timeout)
            return 0;

        *value_end = timeout_begin;
        while (*value_end > 0 && is_space(value[*value_end - 1]))
            (*value_end)--;

        return 0;
    }

    static int parse_request_line(const string &line, KvRequest *request)
    {
        if (request == nullptr)
            return -1;

        size_t len = line.size();
        size_t cmd_begin = 0;
        while (cmd_begin < len && (line[cmd_begin] == ' ' || line[cmd_begin] == '\t'))
            cmd_begin++;

        size_t cmd_end = cmd_begin;
        while (cmd_end < len && line[cmd_end] != ' ' && line[cmd_end] != '\t')
            cmd_end++;

        size_t key_begin = cmd_end;
        while (key_begin < len && (line[key_begin] == ' ' || line[key_begin] == '\t'))
            key_begin++;

        size_t key_end = key_begin;
        while (key_end < len && line[key_end] != ' ' && line[key_end] != '\t')
            key_end++;

        if (cmd_begin == cmd_end || key_begin == key_end)
            return -1;

        request->command = token_between(line, cmd_begin, cmd_end);
        request->key = token_between(line, key_begin, key_end);
        request->value = token_between(line, key_end, len);

        request->timeout = kv_protocal::TimeoutSpec{};
        if (is_set_or_mod(request->command))
        {
            size_t value_end = request->value.size();
            if (parse_timeout_suffix(request->value, &request->timeout, &value_end) != 0)
                return -1;

            request->value = token_between(request->value, 0, value_end);
        }

        return 0;
    }

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

    static void free_request_buffers(kv_protocal::RequestInfo *req_info, struct iovec *iov)
    {
        if (req_info != nullptr)
            allocator::kv_free(req_info);
        if (iov != nullptr)
            allocator::kv_free(iov);
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
        kv_protocal::TimeoutSpec timeout{};
        return submit_request(command, key, value, timeout);
    }

    char *KvClient::submit_request(const string &command,
                                   const string &key,
                                   const string &value,
                                   const kv_protocal::TimeoutSpec &timeout)
    {
        KvRequest request(command, key, value, timeout);
        KvBatchResponse batch_response{};

        if (submit_batch(&request, 1, &batch_response) != 0 || batch_response.num_response == 0)
            return nullptr;

        char *response = batch_response.responses[0].data;
        batch_response.responses[0].data = nullptr;
        free_batch_response(&batch_response);
        return response;
    }

    char *KvClient::submit_request(const string &line)
    {
        KvRequest request;
        if (parse_request_line(line, &request) != 0)
            return nullptr;

        return submit_request(request.command, request.key, request.value, request.timeout);
    }

    char *KvClient::submit_request(const std::string &line)
    {
        return submit_request(string(line.data(), line.size()));
    }

    int KvClient::submit_batch(const KvRequest *requests, uint32_t num_request, KvBatchResponse *response)
    {
        if (requests == nullptr || response == nullptr || num_request == 0)
            return -1;

        response->num_response = 0;
        response->responses = nullptr;

        kv_protocal::NumHeader num_header{num_request};
        kv_protocal::RequestInfo *req_info = static_cast<kv_protocal::RequestInfo *>(
            allocator::kv_malloc(num_request * sizeof(kv_protocal::RequestInfo)));
        if (req_info == nullptr)
            return -1;

        int iovcnt = 2;
        for (uint32_t i = 0; i < num_request; i++)
        {
            req_info[i].command = command_to_idx(requests[i].command);
            req_info[i].key_length = static_cast<uint32_t>(requests[i].key.size());
            req_info[i].body_length = static_cast<uint32_t>(requests[i].key.size() + requests[i].value.size());
            req_info[i].timeout = requests[i].timeout;
            iovcnt += requests[i].value.empty() ? 1 : 2;
        }

        struct iovec *iov = static_cast<struct iovec *>(allocator::kv_malloc(iovcnt * sizeof(struct iovec)));
        if (iov == nullptr)
        {
            free_request_buffers(req_info, nullptr);
            return -1;
        }

        int idx = 0;
        iov[idx++] = {&num_header, kv_protocal::NUM_HEADER_SIZE};
        iov[idx++] = {req_info, num_request * kv_protocal::HEADER_SIZE};
        for (uint32_t i = 0; i < num_request; i++)
        {
            iov[idx++] = {const_cast<char *>(requests[i].key.data()), requests[i].key.size()};
            if (!requests[i].value.empty())
                iov[idx++] = {const_cast<char *>(requests[i].value.data()), requests[i].value.size()};
        }

        if (writev_all(_fd, iov, iovcnt) != 0)
        {
            perror("Error send");
            free_request_buffers(req_info, iov);
            return -1;
        }

        free_request_buffers(req_info, iov);

        kv_protocal::NumHeader response_num_header{};
        if (recv_all(_fd, reinterpret_cast<char *>(&response_num_header), kv_protocal::NUM_HEADER_SIZE) != 0)
        {
            perror("Error recv response number");
            return -1;
        }

        if (response_num_header.num_request == 0)
            return -1;

        kv_protocal::KvResponseHeader *response_headers = static_cast<kv_protocal::KvResponseHeader *>(
            allocator::kv_malloc(response_num_header.num_request * sizeof(kv_protocal::KvResponseHeader)));
        if (response_headers == nullptr)
            return -1;

        if (recv_all(_fd,
                     reinterpret_cast<char *>(response_headers),
                     response_num_header.num_request * sizeof(kv_protocal::KvResponseHeader)) != 0)
        {
            perror("Error recv response header");
            allocator::kv_free(response_headers);
            return -1;
        }

        response->responses = static_cast<KvResponse *>(allocator::kv_malloc(response_num_header.num_request * sizeof(KvResponse)));
        if (response->responses == nullptr)
        {
            allocator::kv_free(response_headers);
            return -1;
        }

        response->num_response = response_num_header.num_request;
        memset(response->responses, 0, response->num_response * sizeof(KvResponse));

        for (uint32_t i = 0; i < response->num_response; i++)
        {
            uint32_t response_length = response_headers[i].response_length;
            response->responses[i].data = static_cast<char *>(allocator::kv_malloc(response_length + 1));
            if (response->responses[i].data == nullptr)
            {
                allocator::kv_free(response_headers);
                free_batch_response(response);
                return -1;
            }

            response->responses[i].length = response_length;
            if (response_length > 0 && recv_all(_fd, response->responses[i].data, response_length) != 0)
            {
                perror("Error recv response body");
                allocator::kv_free(response_headers);
                free_batch_response(response);
                return -1;
            }

            response->responses[i].data[response_length] = '\0';
        }

        allocator::kv_free(response_headers);
        return 0;
    }

    void KvClient::free_batch_response(KvBatchResponse *response)
    {
        if (response == nullptr)
            return;

        if (response->responses != nullptr)
        {
            for (uint32_t i = 0; i < response->num_response; i++)
            {
                if (response->responses[i].data != nullptr)
                    allocator::kv_free(response->responses[i].data);
            }

            allocator::kv_free(response->responses);
        }

        response->num_response = 0;
        response->responses = nullptr;
    }

}