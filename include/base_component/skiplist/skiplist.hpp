#ifndef __SKIPLIST_H
#define __SKIPLIST_H

#include <stddef.h>
#include <string.h>
#include <random>

#include "allocator.h"
#include "slab.hpp"

#ifndef MAX_SKIP_LEVEL
#define MAX_SKIP_LEVEL 32
#endif

#ifndef SKIPLIST_PROB
#define SKIPLIST_PROB 0.5
#endif

namespace base_component
{
    template <typename K, typename V>
    struct SkipNode
    {
        K key;
        V value;
        SkipNode *forward[MAX_SKIP_LEVEL]{nullptr};
        SkipNode(const K &k, const V &v)
            : key(k), value(v) {}
        SkipNode(K &&k, V &&v)
            : key(std::move(k)), value(std::move(v)) {}
        SkipNode() = default;

        SkipNode(SkipNode &&) noexcept = default;
        SkipNode &operator=(SkipNode &&) noexcept = default;

        SkipNode(const SkipNode &) = default;
        SkipNode &operator=(const SkipNode &) = default;
        ~SkipNode() = default;
    };

#if __cplusplus >= 202002L
#include <concepts>
    template <typename T>
    concept SkipComparable = requires(T a, T b) {
        { a > b } -> std::convertible_to<bool>;
        { a == b } -> std::convertible_to<bool>;
        { a < b } -> std::convertible_to<bool>;
    };
#endif

#if __cplusplus >= 202002L
    template <SkipComparable K, typename V>
#else
    template <typename K, typename V, typename = std::void_t<decltype(std::declval<K>() > std::declval<K>()), decltype(std::declval<K>() == std::declval<K>()), decltype(std::declval<K>() < std::declval<K>())>>
#endif
    class Skiplist
    {
    public:
        using NodeType = SkipNode<K, V>;

        Skiplist() = default;

        ~Skiplist()
        {
            NodeType *cur = head_->forward[0];
            while (cur)
            {
                NodeType *next = cur->forward[0];
                cur->~SkipNode();
                KV_NODE_FREE(NodeType, cur);
                cur = next;
            }
        }

        int insert(NodeType *node)
        {
            if (!node)
                return -1;
            NodeType *update[MAX_SKIP_LEVEL]{nullptr};

            NodeType *cur = head_;
            for (int level = current_level_; level >= 0; level--)
            {
                while (cur->forward[level] != nullptr &&
                       cur->forward[level]->key < node->key)
                {
                    cur = cur->forward[level];
                }

                update[level] = cur;
            }

            cur = cur->forward[0];
            if (cur && cur->key == node->key)
                return -1;

            int new_level = _random_level();

            if (new_level > current_level_)
            {
                for (int level = current_level_ + 1; level <= new_level; ++level)
                {
                    update[level] = head_;
                }

                current_level_ = new_level;
            }

            for (int level = 0; level <= new_level; ++level)
            {
                node->forward[level] = update[level]->forward[level];
                update[level]->forward[level] = node;
            }
            count_++;
            return 0;
        }

        int insert(const K &k, const V &v)
        {
            void *ptr = KV_NODE_ALLOC(NodeType);
            if (ptr == nullptr)
                return -1;
            NodeType *node = new (ptr) NodeType(k, v);
            if (insert(node) < 0)
            {
                node->~NodeType();
                KV_NODE_FREE(NodeType, node);
                return -1;
            }
            return 0;
        }

        void delNode(NodeType *node)
        {
            if (!node)
                return;
            NodeType *update[MAX_SKIP_LEVEL]{nullptr};

            NodeType *cur = head_;
            for (int level = current_level_; level >= 0; level--)
            {
                while (cur->forward[level] != nullptr &&
                       cur->forward[level]->key < node->key)
                {
                    cur = cur->forward[level];
                }

                update[level] = cur;
            }

            cur = cur->forward[0];
            if (cur != node)
                return;

            for (int i = 0; i <= current_level_; i++)
            {
                if (update[i]->forward[i] != node)
                    break;
                update[i]->forward[i] = node->forward[i];
            }
            node->~SkipNode();
            KV_NODE_FREE(NodeType, node);
            count_--;
        }

        int delNode(const K &k)
        {
            NodeType *node = get_node(k);
            if (node == nullptr)
                return -1;
            delNode(node);
            return 0;
        }

        struct iterator
        {
            Skiplist *arr;
            NodeType *node;

            NodeType *operator*() const
            {
                return node;
            }

            iterator &operator++()
            {
                node = node->forward[0];
                return *this;
            }

            bool operator!=(const iterator &other) const
            {
                return node != other.node;
            }
        };

        iterator begin()
        {
            return iterator{this, head_->forward[0]};
        }

        iterator end()
        {
            return iterator{this, nullptr};
        }

        int size() const
        {
            return count_;
        }

        NodeType *get_node(const K &key)
        {
            NodeType *cur = head_;
            for (int level = current_level_; level >= 0; level--)
            {
                while (cur->forward[level] != nullptr &&
                       cur->forward[level]->key < key)
                {
                    cur = cur->forward[level];
                }

                if (cur->forward[level] && cur->forward[level]->key == key)
                    return cur->forward[level];
            }
            return nullptr;
        }

    private:
        int _random_level()
        {
            int level = 0;

            while (dist(gen) < probability_ && level < MAX_SKIP_LEVEL)
            {
                ++level;
            }

            return level;
        }

        int count_{0};
        int current_level_{0};
        double probability_{SKIPLIST_PROB};
        NodeType dummy{};
        NodeType *head_{&dummy};

        std::random_device rd;
        std::mt19937 gen{rd()};
        std::uniform_real_distribution<double> dist{0.0, 1.0};
    };
} // namespace base_component

#endif // __SKIPLIST_H