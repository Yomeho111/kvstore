#ifndef __KV_PROTOCAL_HPP
#define __KV_PROTOCAL_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "status.h"
#include "engine_interface.h"
#include "status.h"
#include "rbtree_engine/rbtree_engine.h"
#include "allocator.h"

// #define MAX_BODY_SIZE 4096
#define MAX_TOKEN_SIZE 3
#define BUFFER_SIZE 128

namespace kv_protocal
{
    inline constexpr const char *command[] = {
        "START",
        "SET",
        "GET",
        "DEL",
        "MOD",
        "EXIST",
        "END",
    };

    enum CommandIdx
    {
        KVS_START = 0,
        KVS_SET,
        KVS_GET,
        KVS_DEL,
        KVS_MOD,
        KVS_EXIST,
        KVS_END,
    };

    struct KvHeader
    {
        uint32_t body_length;
        uint32_t key_length;
        uint32_t value_length;
    };

    constexpr inline const size_t HEADER_SIZE = sizeof(KvHeader);

    template <typename KvEngine>
    class KvProtocal
    {
    public:
        static KvProtocal &instance()
        {
            static KvProtocal prot;
            return prot;
        }

        int process_header(struct network::StatusM *status, struct KvHeader *header)
        {
            if (status == nullptr || header == nullptr)
                return -1;

            // if (body_length > MAX_BODY_SIZE)
            //     return -1;

            // change status to 1 for body recv
            status->status = 1;
            status->buffer_size = header->body_length;
            status->sent_data = 0;
            status->key_length = header->key_length;
            status->value_length = header->value_length;
            return 0;
        }

        int process_body(struct network::StatusM *status, char *body, size_t body_length, char **response)
        {
            if (status == nullptr || body == nullptr) // body_length > MAX_BODY_SIZE
                return -1;

            char *tokens[MAX_TOKEN_SIZE] = {0};

            uint32_t command_length = status->buffer_size - status->key_length - status->value_length;

            int count = _split_token(body, tokens, command_length, status->key_length, status->value_length);
            if (count == -1)
                return -1;

            int wbuf_size = _process_tokens(tokens, response, command_length, status->key_length, status->value_length);
            if (wbuf_size == -1)
                return -1;

            status->status = 2;
            status->buffer_size = wbuf_size;
            return wbuf_size;
        }

    private:
        KvProtocal() {}
        ~KvProtocal() {}

        int _split_token(char *body, char **tokens, uint32_t command_length, uint32_t key_length, uint32_t value_length)
        {
            if (body == nullptr || tokens == nullptr)
                return -1;

            if (command_length == 0)
                return -1;

            tokens[0] = body;
            if (key_length == 0)
                return -1;
            tokens[1] = body + command_length;

            if (value_length > 0)
            {
                tokens[2] = tokens[1] + key_length;
                return 3;
            }
            return 2;
        }

        int _process_tokens(char **tokens, char **response, uint32_t command_length, uint32_t key_length, uint32_t value_length)
        {
            if (tokens == nullptr || response == nullptr)
                return -1;

            int cmd = KVS_START;

            for (; cmd < KVS_END; cmd++)
            {
                if (strncmp(tokens[0], command[cmd], command_length) == 0)
                    break;
            }

            int wbuf_size = 0;
            char wbuf[BUFFER_SIZE] = {0};
            int ret = 0;
            char *key = tokens[1];
            char *value = tokens[2];
            kv_protocal::KvHeader header;

            memset(&header, 0, kv_protocal::HEADER_SIZE);

            switch (cmd)
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

            header.body_length = static_cast<uint32_t>(wbuf_size);
            *response = (char *)allocator::kv_malloc(kv_protocal::HEADER_SIZE + wbuf_size);

            memcpy(*response, &header, sizeof(header));

            if (cmd == KVS_GET && ret > 0)
            {
                memcpy(*response + sizeof(header), value, wbuf_size);
                allocator::kv_free(value);
            }
            else
                memcpy(*response + sizeof(header), wbuf, wbuf_size);

            return wbuf_size;
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