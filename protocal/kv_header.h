#ifndef __KV_HEADER_H
#define __KV_HEADER_H

#include <stddef.h>
#include <stdint.h>

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
        KVS_INVALID,
    };

    struct NumHeader
    {
        uint32_t num_request;
    };

    constexpr inline const size_t NUM_HEADER_SIZE = sizeof(NumHeader);

    struct RequestInfo
    {
        uint32_t command;
        uint32_t body_length;
        uint32_t key_length;
    };

    constexpr inline const size_t HEADER_SIZE = sizeof(struct RequestInfo);

    struct KvResponseHeader
    {
        uint32_t response_length;
    };
}

#endif // __KV_HEADER_H