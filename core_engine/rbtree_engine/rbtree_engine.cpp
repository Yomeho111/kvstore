#include "rbtree_engine.h"
#include <string.h>

#include "slab.hpp"

namespace kv_engine
{
    int RbtreeEngine::set(char *key, size_t key_len, char *value, size_t val_len)
    {
        if (key_len == 0 || val_len == 0)
            return -1;
        string key_s = string(key, key_len);
        string val_s = string(value, val_len);

        if (key_s.size() == 0 || val_s.size() == 0)
            return -1;

        auto *node = rbt.get_node(key_s);
        if (node)
            return 1;

        rbt.insert(key_s, val_s);

        return 0;
    }

    int RbtreeEngine::get(char *key, size_t key_len, char **value)
    {
        if (key_len == 0)
            return -1;
        string key_s = string(key, key_len);

        if (key_s.size() == 0)
            return -1;

        auto *node = rbt.get_node(key_s);
        if (node == nullptr)
            return 0;

        *value = (char *)allocator::kv_malloc(node->value.size() + 2);
        if (*value == nullptr)
            return -2;
        memcpy(*value, node->value.c_str(), node->value.size());
        memcpy(*value + node->value.size(), "\r\n", 2);

        return node->value.size() + 2;
    }

    int RbtreeEngine::modify(char *key, size_t key_len, char *value, size_t val_len)
    {
        if (key_len == 0 || val_len == 0)
            return -1;
        string key_s = string(key, key_len);
        string val_s = string(value, val_len);

        if (key_s.size() == 0 || val_s.size() == 0)
            return -1;

        auto *node = rbt.get_node(key_s);
        if (!node)
            return 1;

        node->value = val_s;
        return 0;
    }

    int RbtreeEngine::del(char *key, size_t key_len)
    {
        if (key_len == 0)
            return -1;
        string key_s = string(key, key_len);

        if (key_s.size() == 0)
            return -1;

        if (rbt.delNode(key_s) == -1)
            return 1;
        return 0;
    }

    int RbtreeEngine::exist(char *key, size_t key_len)
    {
        if (key_len == 0)
            return -1;
        string key_s = string(key, key_len);

        if (key_s.size() == 0)
            return -1;

        auto *node = rbt.get_node(key_s);
        if (node == nullptr)
            return 1;
        return 0;
    }
}