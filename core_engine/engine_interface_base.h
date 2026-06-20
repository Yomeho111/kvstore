#ifndef __ENGINE_INTERFACE_BASE_H
#define __ENGINE_INTERFACE_BASE_H

#include <stddef.h>
#include <stdint.h>

#include "kv_header.h"

namespace kv_engine
{
    class EngineInterfaceBase
    {
    public:
        virtual ~EngineInterfaceBase() = default;

        virtual int set(char *key, size_t key_len, char *value, size_t val_len, struct kv_protocal::TimeoutSpec *timeout = nullptr, bool to_disk = true) = 0;

        virtual int get(char *key, size_t key_len, char **value) = 0;

        virtual int modify(char *key, size_t key_len, char *value, size_t val_len, struct kv_protocal::TimeoutSpec *timeout = nullptr, bool to_disk = true) = 0;

        virtual int del(char *key, size_t key_len, bool to_disk = true) = 0;

        virtual int exist(char *key, size_t key_len) = 0;

        virtual int init() noexcept = 0;
    };
} // namespace kv_engine

#endif // __ENGINE_INTERFACE_BASE_H