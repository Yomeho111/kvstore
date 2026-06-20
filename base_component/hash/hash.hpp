#ifndef __HASH_TABLE_H
#define __HASH_TABLE_H

#include <stddef.h>
#include <string.h>
#include <functional>

#include "allocator.h"
#include "slab.hpp"

namespace base_component
{
    inline constexpr size_t DEFAULT_HASH_SIZE{16};
    inline constexpr double LOAD_FACTOR{1.0};
    template <typename K, typename V>
    struct HashNode
    {
        K key;
        V value;
        HashNode *next{nullptr};
        HashNode *prev{nullptr};
        HashNode(const K &k, const V &v)
            : key(k), value(v) {}
        HashNode(K &&k, V &&v)
            : key(std::move(k)), value(std::move(v)) {}
        HashNode() = default;

        HashNode(HashNode &&) noexcept = default;
        HashNode &operator=(HashNode &&) noexcept = default;

        HashNode(const HashNode &) = default;
        HashNode &operator=(const HashNode &) = default;
        ~HashNode() = default;
    };

#if __cplusplus >= 202002L
#include <concepts>
    template <typename T>
    concept HashEqualable = requires(T a, T b) {
        { a == b } -> std::convertible_to<bool>;
        { a != b } -> std::convertible_to<bool>;
    };
#endif

#if __cplusplus >= 202002L
    template <HashEqualable K, typename V, typename HashFunc = std::hash<K>>
#else
    template <typename K, typename V, typename HashFunc = std::hash<K>, typename = std::void_t<decltype(std::declval<K>() == std::declval<K>()), decltype(std::declval<K>() != std::declval<K>())>>
#endif
    class HashTable
    {
    public:
        using NodeType = HashNode<K, V>;

        HashTable() = default;

        ~HashTable()
        {
            if (hash_)
            {
                for (size_t i = 0; i < capacity_; i++)
                {
                    NodeType *cur = hash_[i];
                    while (cur)
                    {
                        NodeType *tmp = cur->next;
                        cur->~HashNode();
                        memory::Slab<NodeType>::instance().free(cur);
                        cur = tmp;
                    }
                    hash_[i] = nullptr;
                }
                allocator::kv_free(hash_);
                hash_ = nullptr;
            }
        }

        int insert(NodeType *node)
        {
            if (!node)
                return -1;

            // grow when the load factor threshold is reached
            if (capacity_ == 0 || count_ >= LOAD_FACTOR * capacity_)
            {
                if (_extend() < 0)
                    return -1;
            }

            size_t idx = hasher_(node->key) % capacity_;

            _add_to_bucket(node, hash_, idx, capacity_);
            count_++;
            return 0;
        }

        int insert(const K &k, const V &v)
        {

            void *ptr = memory::Slab<NodeType>::instance().malloc();
            if (ptr == nullptr)
                return -1;
            NodeType *node = new (ptr) NodeType(k, v);
            if (insert(node) < 0)
            {
                node->~NodeType();
                memory::Slab<NodeType>::instance().free(node);
                return -1;
            }
            return 0;
        }

        void delNode(NodeType *node)
        {
            if (!node || !hash_ || capacity_ == 0)
                return;

            size_t idx = hasher_(node->key) % capacity_;
            _remove_from_bucket(node, hash_, idx, capacity_);
            node->~HashNode();
            memory::Slab<NodeType>::instance().free(node);
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
            HashTable *hash{nullptr};
            size_t idx{0};
            NodeType *node{nullptr};

            NodeType *operator*() const
            {
                return node;
            }

            iterator &operator++()
            {
                if (!hash || !node)
                {
                    node = nullptr;
                    return *this;
                }

                if (node->next)
                {
                    node = node->next;
                    return *this;
                }

                for (size_t i = idx + 1; i < hash->capacity_; i++)
                {
                    if (hash->hash_[i])
                    {
                        idx = i;
                        node = hash->hash_[i];
                        return *this;
                    }
                }

                idx = hash->capacity_;
                node = nullptr;
                return *this;
            }

            bool operator!=(const iterator &other) const
            {
                return node != other.node;
            }
        };

        iterator begin()
        {
            iterator it;
            it.hash = this;
            it.idx = 0;
            it.node = nullptr;
            for (size_t i = 0; i < capacity_; i++)
            {
                if (hash_ && hash_[i])
                {
                    it.idx = i;
                    it.node = hash_[i];
                    break;
                }
            }
            return it;
        }

        iterator end()
        {
            iterator it;
            it.hash = this;
            it.idx = capacity_;
            it.node = nullptr;
            return it;
        }

        int size() const
        {
            return count_;
        }

        NodeType *get_node(const K &key)
        {
            if (!hash_ || capacity_ == 0)
                return nullptr;

            size_t idx = hasher_(key) % capacity_;
            for (NodeType *cur = hash_[idx]; cur; cur = cur->next)
            {
                if (cur->key == key)
                    return cur;
            }
            return nullptr;
        }

    private:
        void _add_to_bucket(NodeType *node, NodeType **cur_hash, size_t idx, size_t capacity)
        {
            if (!node || !cur_hash || idx >= capacity)
                return;

            node->next = cur_hash[idx];
            node->prev = nullptr;
            cur_hash[idx] = node;
        }

        void _remove_from_bucket(NodeType *node, NodeType **cur_hash, size_t idx, size_t capacity)
        {
            if (!node || !cur_hash || idx >= capacity)
                return;

            if (node->prev == nullptr)
            {
                if (node->next)
                {
                    node->next->prev = node->prev;
                }
                cur_hash[idx] = node->next;
                node->next = nullptr;
            }
            else
            {
                if (node->next)
                {
                    node->next->prev = node->prev;
                }
                node->prev->next = node->next;
                node->next = nullptr;
                node->prev = nullptr;
            }
        }

        int _rehash(NodeType **old_hash, size_t old_capacity, NodeType **new_hash, size_t new_capacity)
        {
            if (!old_hash || !new_hash || !old_capacity || !new_capacity)
                return -1;

            for (size_t i = 0; i < old_capacity; i++)
            {
                NodeType *cur = old_hash[i];
                while (cur)
                {
                    size_t hash_val = hasher_(cur->key);
                    NodeType *tmp = cur->next;
                    _remove_from_bucket(cur, old_hash, i, old_capacity);
                    size_t new_idx = hash_val % new_capacity;
                    _add_to_bucket(cur, new_hash, new_idx, new_capacity);
                    cur = tmp;
                }
            }
            return 0;
        }

        int _extend()
        {
            size_t new_capacity = capacity_ ? capacity_ * 2 : DEFAULT_HASH_SIZE;
            NodeType **new_hash = (NodeType **)allocator::kv_malloc(new_capacity * sizeof(NodeType *));
            if (new_hash == nullptr)
                return -1;
            memset(new_hash, 0, new_capacity * sizeof(NodeType *));

            if (hash_ && capacity_)
            {
                if (_rehash(hash_, capacity_, new_hash, new_capacity) < 0)
                    return -2;
                allocator::kv_free(hash_);
            }

            hash_ = new_hash;
            capacity_ = new_capacity;
            return 0;
        }

        int count_{0};
        size_t capacity_{0};
        NodeType **hash_{nullptr};
        HashFunc hasher_;
    };
} // namespace base_component

#endif // __HASH_TABLE_H