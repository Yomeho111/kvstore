#ifndef __RBTREE_ENGINE_H
#define __RBTREE_ENGINE_H

#include <string>

#include "engine_interface.h"
#include "rbtree.hpp"
#include "allocator.h"
#include "kv_persistent.h"
#include "memory_utils.h"

using string = std::basic_string<
    char,
    std::char_traits<char>,
    allocator::MyAllocator<char>>;

using Node = base_component::TreeNode<string, string>;

namespace kv_engine
{
    // 0 ok, <0 error 1 not exist
    class RbtreeEngine : public EngineInterface
    {
    public:
        RbtreeEngine() = default;
        ~RbtreeEngine() = default;

        int set(char *key, size_t key_len, char *value, size_t val_len, bool to_disk = true) override;

        int get(char *key, size_t key_len, char **value) override;

        int modify(char *key, size_t key_len, char *value, size_t val_len, bool to_disk = true) override;

        int del(char *key, size_t key_len, bool to_disk = true) override;

        int exist(char *key, size_t key_len) override;

        int init() noexcept override;

    private:
        memory::SpinLock lock_;
        base_component::RBTree<string, string> rbt;
        kv_persistent::StoreEngine store_engine;
    };
}

#endif // __RBTREE_ENGINE_H