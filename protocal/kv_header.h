#ifndef __KV_HEADER_H
#define __KV_HEADER_H

#include <stddef.h>
#include <stdint.h>

#define TRANS_TIMEOUT_MILLI(timeout) std::chrono::duration_cast<std::chrono::milliseconds>( \
    std::chrono::seconds(timeout->tv_sec) +                                                 \
    std::chrono::nanoseconds(timeout->tv_nsec))

namespace kv_protocal
{
    inline constexpr const char *command_str[] = {
        "START",
        "SET",
        "GET",
        "DEL",
        "MOD",
        "EXIST",
        "REPR",
        "RESP",
        "END",
        "INVALID",
    };

    enum CommandIdx
    {
        KVS_START = 0,
        KVS_SET,
        KVS_GET,
        KVS_DEL,
        KVS_MOD,
        KVS_EXIST,
        KVS_REPR,
        KVS_RESP,
        KVS_END,
        KVS_INVALID,
    };

    struct NumHeader
    {
        uint32_t num_request;
    };

    constexpr inline const size_t NUM_HEADER_SIZE = sizeof(NumHeader);

    struct TimeoutSpec
    {
        long tv_sec{-1};
        long tv_nsec{-1};
    };

    struct HeaderInfo
    {
        uint32_t command{0};
        uint32_t key_length{0};
        uint32_t body_length{0};
        int sync_idx{-1};
        struct TimeoutSpec timeout;
    };

    constexpr inline const size_t HEADER_SIZE = sizeof(struct HeaderInfo);
}

#endif // __KV_HEADER_H