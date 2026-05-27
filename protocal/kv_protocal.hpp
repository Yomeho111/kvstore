#ifndef __KV_PROTOCAL_HPP
#define __KV_PROTOCAL_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdlib.h>

#include "status.h"
#include "engine_interface.h"
#include "status.h"
#include "rbtree_engine/rbtree_engine.h"
#include "allocator.h"
#include "kv_header.h"

// #define MAX_BODY_SIZE 4096
#define MAX_TOKEN_SIZE 2
#define BUFFER_SIZE 128

namespace kv_protocal
{

    template <typename KvEngine>
    class KvProtocal
    {
    public:
        static KvProtocal &instance()
        {
            static KvProtocal prot;
            static int ret = prot._engine.init();
            if (ret < 0)
            {
                fprintf(stderr, "kv_protocal init failure: %d\n", ret);
                exit(-1);
            }
            return prot;
        }

        int process_num_request(struct network::StatusM *status, uint32_t num_request)
        {
            if (status == nullptr || num_request == 0)
                return -1;

            status->status = network::READ_HEADER;
            status->num_request = num_request;
            status->req_info = (RequestInfo *)allocator::kv_malloc(num_request * sizeof(RequestInfo));
            if (status->req_info == nullptr)
                return -2;

            memset(status->req_info, 0, num_request * sizeof(RequestInfo));
            return 0;
        }

        int process_header(struct network::StatusM *status, struct ::iovec **r_iovec)
        {
            if (status == nullptr || status->status != network::READ_HEADER || status->req_info == nullptr || status->num_request == 0)
                return -1;

            *r_iovec = (struct ::iovec *)allocator::kv_malloc(status->num_request * sizeof(struct ::iovec));
            if (*r_iovec == nullptr)
                return -2;

            for (int i = 0; i < status->num_request; i++)
            {
                if (status->req_info[i].command == KVS_START || status->req_info[i].command >= KVS_END)
                {
                    status->req_info[i].command = KVS_INVALID;
                }
                else
                {
                    (*r_iovec)[i].iov_len = status->req_info[i].body_length;
                    (*r_iovec)[i].iov_base = allocator::kv_malloc(status->req_info[i].body_length);
                    if ((*r_iovec)[i].iov_base == nullptr)
                        return -2;
                }
            }
            status->status = network::READ_BODY;
            return 0;
        }

        int process_body(struct network::StatusM *status, struct ::iovec *r_iovec, struct ::iovec **w_iovec)
        {
            if (status == nullptr || r_iovec == nullptr || w_iovec == nullptr)
                return -1;

            size_t iovec_size = (status->num_request + 2);
            status->w_iovec_size = iovec_size;
            size_t w_iovec_size = iovec_size * sizeof(struct ::iovec);

            *w_iovec = static_cast<struct ::iovec *>(allocator::kv_malloc(w_iovec_size));
            if (*w_iovec == nullptr)
                return -2;
            memset(*w_iovec, 0, w_iovec_size);

            (*w_iovec)[0].iov_len = NUM_HEADER_SIZE;
            (*w_iovec)[0].iov_base = allocator::kv_malloc(NUM_HEADER_SIZE);
            if ((*w_iovec)[0].iov_base == nullptr)
                return -2;

            ((NumHeader *)(*w_iovec)[0].iov_base)->num_request = status->num_request;

            (*w_iovec)[1].iov_len = sizeof(KvResponseHeader) * status->num_request;
            (*w_iovec)[1].iov_base = allocator::kv_malloc(sizeof(KvResponseHeader) * status->num_request);
            if ((*w_iovec)[1].iov_base == nullptr)
                return -2;

            memset((*w_iovec)[1].iov_base, 0, sizeof(KvResponseHeader) * status->num_request);

            for (int i = 0; i < status->num_request; i++)
            {
                char *response_body = nullptr;
                int wbuf_size = _process_body(status->req_info[i].command, status->req_info[i].body_length, status->req_info[i].key_length, static_cast<char *>(r_iovec[i].iov_base), &response_body);
                if (wbuf_size <= 0)
                    continue;

                (*w_iovec)[i + 2].iov_base = response_body;
                (*w_iovec)[i + 2].iov_len = wbuf_size;
                ((KvResponseHeader *)(*w_iovec)[1].iov_base)[i].response_length = wbuf_size;
            }

            status->status = network::SEND_RESPONSE;
            return 0;
        }

    private:
        KvProtocal() {}
        ~KvProtocal() {}

        int _split_token(char *body, char **tokens, uint32_t key_length)
        {
            if (body == nullptr || tokens == nullptr || key_length == 0)
                return -1;

            tokens[0] = body;
            tokens[1] = body + key_length;
            return 0;
        }

        int _process_body(uint16_t command, size_t body_length, size_t key_length, char *body, char **response)
        {
            if (body == nullptr || body_length == 0 || key_length == 0) // body_length > MAX_BODY_SIZE
                return -1;

            char *tokens[MAX_TOKEN_SIZE] = {0};

            uint32_t value_length = body_length - key_length;

            int count = _split_token(body, tokens, key_length);
            if (count == -1)
                return -1;

            int wbuf_size = _process_tokens(tokens, response, command, key_length, value_length);
            if (wbuf_size == -1)
                return -1;

            return wbuf_size;
        }

        int _process_tokens(char **tokens, char **response, uint16_t command, uint32_t key_length, uint32_t value_length)
        {
            if (tokens == nullptr || response == nullptr)
                return -1;

            int wbuf_size = 0;
            char wbuf[BUFFER_SIZE] = {0};
            int ret = 0;
            char *key = tokens[0];
            char *value = tokens[1];

            switch (command)
            {
            case KVS_SET:
            {
                ret = _engine.set(key, key_length, value, value_length);
                if (ret == 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "OK\r\n");
                else if (ret < 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                else if (ret > 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "EXIST\r\n");
                break;
            }
            case KVS_GET:
            {
                ret = _engine.get(key, key_length, &value);
                if (ret > 0)
                    wbuf_size = ret;
                else if (ret < 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                else if (ret == 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "NOT EXIST\r\n");
                break;
            }
            case KVS_MOD:
            {
                ret = _engine.modify(key, key_length, value, value_length);
                if (ret == 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "OK\r\n");
                else if (ret < 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                else if (ret > 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "NOT EXIST\r\n");
                break;
            }
            case KVS_DEL:
            {
                ret = _engine.del(key, key_length);
                if (ret == 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "OK\r\n");
                else if (ret < 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                else if (ret > 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "NOT EXIST\r\n");
                break;
            }
            case KVS_EXIST:
            {
                ret = _engine.exist(key, key_length);
                if (ret == 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "EXIST\r\n");
                else if (ret < 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                else if (ret > 0)
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "NOT EXIST\r\n");
                break;
            }
            default:
                wbuf_size = snprintf(wbuf, BUFFER_SIZE, "Invalid Command\r\n");
                break;
            }

            // *response = (char *)allocator::kv_malloc(wbuf_size);

            if (command == KVS_GET && ret > 0)
                *response = value;
            else
            {
                *response = (char *)allocator::kv_malloc(wbuf_size);
                if (*response == nullptr)
                    return -2;
                memcpy(*response, wbuf, wbuf_size);
            }
            return static_cast<uint32_t>(wbuf_size);
        }

        KvProtocal(const KvProtocal &) = delete;
        KvProtocal(KvProtocal &&) = delete;

        KvProtocal &operator=(const KvProtocal &) = delete;
        KvProtocal &operator=(KvProtocal &&) = delete;

        KvEngine _engine;
    };

#ifdef RBTREE_ENGINE
    using KvStoreProtocal = KvProtocal<kv_engine::RbtreeEngine>;
#endif
}

#endif // __KV_PROTOCAL_HPP