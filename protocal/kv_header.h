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
        "MULTI",
        "EXEC",
        "EXIT",
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
        KVS_MULTI,
        KVS_EXEC,
        KVS_EXIT,
        KVS_END,
        KVS_INVALID,
    };

    // Sentinel stored as the first wire byte of every NumHeader. It must never be
    // '*' (0x2A) so the RESP auto-detection (which treats a leading '*' as a Redis
    // command) can never misclassify a native request as RESP, whatever the value
    // of num_request. Packed so the tag is byte 0 with no padding on the wire.
    inline constexpr uint8_t NUM_HEADER_TAG = 0x4B; // 'K'

    struct __attribute__((packed)) NumHeader
    {
        uint8_t tag{NUM_HEADER_TAG};
        uint32_t num_request{0};
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
} // namespace kv_protocal

#endif // __KV_HEADER_H