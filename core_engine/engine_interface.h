#ifndef __ENGINE_INTERFACE_H
#define __ENGINE_INTERFACE_H

#include <stddef.h>

namespace kv_engine
{
    class EngineInterface
    {
    public:
        EngineInterface() {}
        virtual ~EngineInterface() {}

        virtual int set(char *key, size_t key_len, char *value, size_t val_len) = 0;

        virtual int get(char *key, size_t key_len, char **value) = 0;

        virtual int modify(char *key, size_t key_len, char *value, size_t val_len) = 0;

        virtual int del(char *key, size_t key_len) = 0;

        virtual int exist(char *key, size_t key_len) = 0;
    };
}
#endif // __ENGINE_INTERFACE_H