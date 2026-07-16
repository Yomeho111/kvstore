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
#include "engine_interface.hpp"
#include "status.h"
#include "rbtree_engine/rbtree_engine.h"
#include "array_engine/array_engine.h"
#include "hash_engine/hash_engine.h"
#include "skiplist_engine/skiplist_engine.h"
#include "allocator.h"
#include "kv_header.h"
#include "rep_manager.h"

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
                fprintf(stderr, "corrupted database\n");
                exit(0);
            }
            return prot;
        }

        int process_num_request(struct network::StatusM *status, uint32_t num_request)
        {
            if (status == nullptr || num_request == 0)
                return -1;

            status->status = network::READ_HEADER;
            status->num_request = num_request;
            status->req_info = (HeaderInfo *)allocator::kv_malloc(num_request * sizeof(HeaderInfo));
            if (status->req_info == nullptr)
                return -2;

            memset(status->req_info, 0, num_request * sizeof(HeaderInfo));
            return 0;
        }

        int process_header(struct network::StatusM *status, struct ::iovec **r_iovec)
        {
            if (status == nullptr || status->status != network::READ_HEADER || status->req_info == nullptr || status->num_request == 0)
                return -1;

            *r_iovec = (struct ::iovec *)allocator::kv_malloc(status->num_request * sizeof(struct ::iovec));
            if (*r_iovec == nullptr)
                return -2;
            memset(*r_iovec, 0, status->num_request * sizeof(struct ::iovec));

            bool is_response{false};

            for (int i = 0; i < status->num_request; i++)
            {
                if (status->req_info[i].command == KVS_START || status->req_info[i].command >= KVS_END)
                {
                    status->req_info[i].command = KVS_INVALID;
                }
                else
                {
                    if (status->req_info[i].command == KVS_RESP)
                        is_response = true;
                    (*r_iovec)[i].iov_len = status->req_info[i].body_length;
                    (*r_iovec)[i].iov_base = allocator::kv_malloc(status->req_info[i].body_length);
                    if ((*r_iovec)[i].iov_base == nullptr)
                        return -2;
                }
            }
            status->is_response = is_response;
            status->status = network::READ_BODY;
            return 0;
        }

        int process_body(struct network::StatusM *status, struct ::iovec *r_iovec, struct ::iovec **w_iovec)
        {
            if (status == nullptr || r_iovec == nullptr || w_iovec == nullptr)
                return -1;

            if (status->is_response)
            {
                status->is_response = false;
                status->status = network::READ_NUM_REQUEST;
                return 0;
            }

            if (status->req_info[0].command == KVS_REPR)
            {
                // We want to process the response for slave sync node
                auto &rep = replicate::RepManager::instance();
                if (status->req_info[0].sync_idx == -1 || (rep.get_sync_idx() - status->req_info[0].sync_idx >= MAX_REP_BUFFER_SIZE))
                {
                    return _process_full_sync(status, w_iovec);
                }
                else if (rep.get_sync_idx() - status->req_info[0].sync_idx > 0)
                {
                    return _process_delta_sync(status, w_iovec);
                }
                else
                {
                    return _process_dummy_sync(status, w_iovec);
                }
            }

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

            (*w_iovec)[1].iov_len = sizeof(HeaderInfo) * status->num_request;
            (*w_iovec)[1].iov_base = allocator::kv_malloc(sizeof(HeaderInfo) * status->num_request);
            if ((*w_iovec)[1].iov_base == nullptr)
                return -2;

            memset((*w_iovec)[1].iov_base, 0, sizeof(HeaderInfo) * status->num_request);

            for (int i = 0; i < status->num_request; i++)
            {
                char *response_body = nullptr;
                int wbuf_size = _process_body(status->req_info[i].command, status->req_info[i].body_length, status->req_info[i].key_length, static_cast<char *>(r_iovec[i].iov_base), &response_body, &status->req_info[i].timeout);
                if (wbuf_size <= 0)
                    continue;

                (*w_iovec)[i + 2].iov_base = response_body;
                (*w_iovec)[i + 2].iov_len = wbuf_size;
                ((HeaderInfo *)(*w_iovec)[1].iov_base)[i].body_length = wbuf_size;
                ((HeaderInfo *)(*w_iovec)[1].iov_base)[i].command = KVS_RESP;
            }

            sync_idx = status->req_info[0].sync_idx;

            status->status = network::SEND_RESPONSE;
            return 0;
        }

        int construct_heartbeat_packet(struct network::StatusM *status, struct ::iovec **w_iovec)
        {
            // We want to contruct a heartbeat_packet for slave sync
            if (status == nullptr || w_iovec == nullptr)
                return -1;

            // Allocate iovec array: [0] = NumHeader, [1] = HeaderInfo, [2] = body
            size_t iovec_size = 3;
            *w_iovec = static_cast<struct ::iovec *>(allocator::kv_malloc(iovec_size * sizeof(struct ::iovec)));
            if (*w_iovec == nullptr)
                return -2;
            memset(*w_iovec, 0, iovec_size * sizeof(struct ::iovec));

            // [0] NumHeader
            (*w_iovec)[0].iov_len = NUM_HEADER_SIZE;
            (*w_iovec)[0].iov_base = allocator::kv_malloc(NUM_HEADER_SIZE);
            if ((*w_iovec)[0].iov_base == nullptr)
                return -2;
            ((NumHeader *)(*w_iovec)[0].iov_base)->num_request = 1;

            // [1] HeaderInfo with sync_idx
            (*w_iovec)[1].iov_len = sizeof(HeaderInfo);
            (*w_iovec)[1].iov_base = allocator::kv_malloc(sizeof(HeaderInfo));
            if ((*w_iovec)[1].iov_base == nullptr)
                return -2;
            memset((*w_iovec)[1].iov_base, 0, sizeof(HeaderInfo));

            HeaderInfo *header = static_cast<HeaderInfo *>((*w_iovec)[1].iov_base);
            header->sync_idx = sync_idx;
            header->body_length = sizeof(int);
            header->command = KVS_REPR;

            // [2] Dummy body (int 0)
            (*w_iovec)[2].iov_len = sizeof(int);
            (*w_iovec)[2].iov_base = allocator::kv_malloc(sizeof(int));
            if ((*w_iovec)[2].iov_base == nullptr)
                return -2;
            *(static_cast<int *>((*w_iovec)[2].iov_base)) = sync_idx;

            status->w_iovec_size = iovec_size;
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

        int _process_body(uint16_t command, size_t body_length, size_t key_length, char *body, char **response, struct TimeoutSpec *timeout)
        {
            if (body == nullptr || body_length == 0 || key_length == 0) // body_length > MAX_BODY_SIZE
                return -1;

            char *tokens[MAX_TOKEN_SIZE] = {0};

            uint32_t value_length = body_length - key_length;

            int count = _split_token(body, tokens, key_length);
            if (count == -1)
                return -1;

            int wbuf_size = _process_tokens(tokens, response, command, key_length, value_length, timeout);
            if (wbuf_size == -1)
                return -1;

            return wbuf_size;
        }

        int _process_tokens(char **tokens, char **response, uint16_t command, uint32_t key_length, uint32_t value_length, struct TimeoutSpec *timeout)
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
                    ret = _engine.set(key, key_length, value, value_length, timeout);
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
                    ret = _engine.modify(key, key_length, value, value_length, timeout);
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

        int _process_full_sync(struct network::StatusM *status, struct ::iovec **w_iovec)
        {
            // Full sync: loop through the engine and send all existing KV pairs
            auto &rep = replicate::RepManager::instance();
            std::lock_guard lk{_engine.lock_};
            int num = _engine.size();

            size_t iovec_size = num + 2;
            status->w_iovec_size = iovec_size;
            *w_iovec = static_cast<struct ::iovec *>(allocator::kv_malloc(iovec_size * sizeof(struct ::iovec)));
            if (*w_iovec == nullptr)
                return -2;
            memset(*w_iovec, 0, iovec_size * sizeof(struct ::iovec));

            (*w_iovec)[0].iov_len = NUM_HEADER_SIZE;
            (*w_iovec)[0].iov_base = allocator::kv_malloc(NUM_HEADER_SIZE);
            if ((*w_iovec)[0].iov_base == nullptr)
                return -2;
            ((NumHeader *)(*w_iovec)[0].iov_base)->num_request = num;

            (*w_iovec)[1].iov_len = sizeof(HeaderInfo) * num;
            (*w_iovec)[1].iov_base = allocator::kv_malloc(sizeof(HeaderInfo) * num);
            if ((*w_iovec)[1].iov_base == nullptr)
                return -2;
            memset((*w_iovec)[1].iov_base, 0, sizeof(HeaderInfo) * num);

            int i = 0;
            for (auto it = _engine.begin(); it != _engine.end(); ++it, ++i)
            {
                auto *node = *it;
                size_t body_len = node->key.size() + node->value.size();
                char *body = static_cast<char *>(allocator::kv_malloc(body_len));
                if (body == nullptr)
                    return -2;
                memcpy(body, node->key.c_str(), node->key.size());
                memcpy(body + node->key.size(), node->value.c_str(), node->value.size());

                (*w_iovec)[i + 2].iov_base = body;
                (*w_iovec)[i + 2].iov_len = body_len;

                HeaderInfo *hdr = &((HeaderInfo *)(*w_iovec)[1].iov_base)[i];
                hdr->command = KVS_SET;
                hdr->key_length = node->key.size();
                hdr->body_length = body_len;
                hdr->sync_idx = rep.get_sync_idx();
                // No expiry for replicated entries (memset left {0,0} which would
                // schedule an immediate delete on the slave).
                hdr->timeout.tv_sec = -1;
                hdr->timeout.tv_nsec = -1;
            }
            status->status = network::SEND_RESPONSE;
            return 0;
        }

        int _process_delta_sync(struct network::StatusM *status, struct ::iovec **w_iovec)
        {
            // Delta sync: send only entries appended since slave's sync_idx
            auto &rep = replicate::RepManager::instance();
            int slave_idx = status->req_info[0].sync_idx;
            int master_idx = rep.get_sync_idx();
            int num = master_idx - slave_idx;

            size_t iovec_size = num + 2;
            status->w_iovec_size = iovec_size;
            *w_iovec = static_cast<struct ::iovec *>(allocator::kv_malloc(iovec_size * sizeof(struct ::iovec)));
            if (*w_iovec == nullptr)
                return -2;
            memset(*w_iovec, 0, iovec_size * sizeof(struct ::iovec));

            (*w_iovec)[0].iov_len = NUM_HEADER_SIZE;
            (*w_iovec)[0].iov_base = allocator::kv_malloc(NUM_HEADER_SIZE);
            if ((*w_iovec)[0].iov_base == nullptr)
                return -2;
            ((NumHeader *)(*w_iovec)[0].iov_base)->num_request = num;

            (*w_iovec)[1].iov_len = sizeof(HeaderInfo) * num;
            (*w_iovec)[1].iov_base = allocator::kv_malloc(sizeof(HeaderInfo) * num);
            if ((*w_iovec)[1].iov_base == nullptr)
                return -2;
            memset((*w_iovec)[1].iov_base, 0, sizeof(HeaderInfo) * num);

            for (int i = 0; i < num; i++)
            {
                const replicate::Node &rnode = rep[slave_idx + i];
                size_t body_len = rnode.key.size() + rnode.value.size();
                char *body = static_cast<char *>(allocator::kv_malloc(body_len));
                if (body == nullptr)
                    return -2;
                memcpy(body, rnode.key.c_str(), rnode.key.size());
                memcpy(body + rnode.key.size(), rnode.value.c_str(), rnode.value.size());

                (*w_iovec)[i + 2].iov_base = body;
                (*w_iovec)[i + 2].iov_len = body_len;

                HeaderInfo *hdr = &((HeaderInfo *)(*w_iovec)[1].iov_base)[i];
                hdr->command = rnode.command;
                hdr->key_length = rnode.key.size();
                hdr->body_length = body_len;
                hdr->sync_idx = rep.get_sync_idx();
                // No expiry for replicated entries (memset left {0,0} which would
                // schedule an immediate delete on the slave).
                hdr->timeout.tv_sec = -1;
                hdr->timeout.tv_nsec = -1;
            }
            status->status = network::SEND_RESPONSE;
            return 0;
        }

        int _process_dummy_sync(struct network::StatusM *status, struct ::iovec **w_iovec)
        {
            // if there is no update, just send back a dummy response with 0 requests
            size_t iovec_size = 1;
            status->w_iovec_size = iovec_size;
            *w_iovec = static_cast<struct ::iovec *>(allocator::kv_malloc(iovec_size * sizeof(struct ::iovec)));
            if (*w_iovec == nullptr)
                return -2;
            memset(*w_iovec, 0, iovec_size * sizeof(struct ::iovec));

            (*w_iovec)[0].iov_len = NUM_HEADER_SIZE;
            (*w_iovec)[0].iov_base = allocator::kv_malloc(NUM_HEADER_SIZE);
            if ((*w_iovec)[0].iov_base == nullptr)
                return -2;
            ((NumHeader *)(*w_iovec)[0].iov_base)->num_request = 0;

            status->status = network::SEND_RESPONSE;
            return 0;
        }

        KvProtocal(const KvProtocal &) = delete;
        KvProtocal(KvProtocal &&) = delete;

        KvProtocal &operator=(const KvProtocal &) = delete;
        KvProtocal &operator=(KvProtocal &&) = delete;

        KvEngine _engine;
        int sync_idx{-1};
    };

#ifdef RBTREE_ENGINE
    using KvStoreProtocal = KvProtocal<kv_engine::RbtreeEngine>;
#elif defined(ARRAY_ENGINE)
    using KvStoreProtocal = KvProtocal<kv_engine::ArrayEngine>;
#elif defined(HASH_ENGINE)
    using KvStoreProtocal = KvProtocal<kv_engine::HashEngine>;
#elif defined(SKIPLIST_ENGINE)
    using KvStoreProtocal = KvProtocal<kv_engine::SkiplistEngine>;
#endif
} // namespace kv_protocal

#endif // __KV_PROTOCAL_HPP