#ifndef __RBTREE_ENGINE_H
#define __RBTREE_ENGINE_H

#include <string>

#include "engine_interface.h"
#include "rbtree.hpp"
#include "allocator.h"

using string = std::basic_string<
    char,
    std::char_traits<char>,
    allocator::MyAllocator<char>>;

template <>
struct TreeNode<string, string> : public TreeNodeBase<TreeNode<string, string>>
{
    string key{};
    string value{};
    TreeNode(const string &k, const string &v) : TreeNodeBase(), key(k), value(v) {}
    TreeNode() : TreeNodeBase() {}
    using type = string;
};

using Node = TreeNode<string, string>;

namespace kv_engine
{
    // 0 ok, <0 error 1 not exist
    class RbtreeEngine : public EngineInterface
    {
    public:
        int set(char *key, size_t key_len, char *value, size_t val_len) override;

        int get(char *key, size_t key_len, char **value) override;

        int modify(char *key, size_t key_len, char *value, size_t val_len) override;

        int del(char *key, size_t key_len) override;

        int exist(char *key, size_t key_len) override;

    private:
        RBTree<string, string> rbt;
    };
}

#endif // __RBTREE_ENGINE_H