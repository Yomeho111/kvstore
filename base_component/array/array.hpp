#ifndef __ARRAY_H
#define __ARRAY_H
#include <stddef.h>
#include <string.h>

#include "allocator.h"
#include "slab.hpp"

namespace base_component
{
    inline constexpr size_t DEFAULT_ARRAY_SIZE{16};
    template <typename K, typename V>
    struct ArrNode
    {
        K key;
        V value;
        ArrNode(const K &k, const V &v) : key(k), value(v) {}
        ArrNode(K &&k, V &&v) : key(std::move(k)), value(std::move(v)) {}
        ArrNode() = default;

        ArrNode(ArrNode &&) noexcept = default;
        ArrNode &operator=(ArrNode &&) noexcept = default;

        ArrNode(const ArrNode &) = default;
        ArrNode &operator=(const ArrNode &) = default;
        ~ArrNode() = default;
    };

#if __cplusplus >= 202002L
#include <concepts>
    template <typename T>
    concept ArrayEqualable = requires(T a, T b) {
        { a == b } -> std::convertible_to<bool>;
    };
#endif

#if __cplusplus >= 202002L
    template <ArrayEqualable K, typename V>
#else
    template <typename K, typename V, typename = std::void_t<decltype(std::declval<K>() == std::declval<K>())>>
#endif
    class Array
    {
    public:
        using NodeType = ArrNode<K, V>;

        Array() = default;

        ~Array()
        {
            if (vec_)
            {
                for (size_t i = 0; i < capacity_; i++)
                {
                    if (vec_[i] != nullptr)
                    {
                        vec_[i]->~NodeType();
                        memory::Slab<NodeType>::instance().free(vec_[i]);
                        vec_[i] = nullptr;
                    }
                }

                allocator::kv_free(vec_);
                vec_ = nullptr;
            }
        }

        int insert(NodeType *node)
        {
            if (!node)
                return -1;
            for (size_t i = 0; i < capacity_; i++)
            {
                if (vec_[i] == nullptr)
                {
                    vec_[i] = node;
                    count_++;
                    return 0;
                }
            }

            // vec_.push_back(node);
            size_t new_capacity = capacity_ ? capacity_ * 2 : DEFAULT_ARRAY_SIZE;
            NodeType **ptr = (NodeType **)allocator::kv_malloc(sizeof(NodeType *) * new_capacity);
            if (ptr == nullptr)
                return -1;

            memset(ptr, 0, sizeof(NodeType *) * new_capacity);
            if (vec_)
            {
                memmove(ptr, vec_, sizeof(NodeType *) * capacity_);
                allocator::kv_free(vec_);
            }
            vec_ = ptr;
            vec_[capacity_] = node;
            capacity_ = new_capacity;

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
            if (!node)
                return;
            for (size_t i = 0; i < capacity_; i++)
            {
                if (vec_[i] == node)
                {
                    vec_[i] = nullptr;
                    node->~NodeType();
                    memory::Slab<NodeType>::instance().free(node);
                    count_--;
                    return;
                }
            }
        }

        int delNode(const K &k)
        {
            auto *node = get_node(k);
            if (node == nullptr)
                return -1;
            delNode(node);
            return 0;
        }

        struct iterator
        {
            Array *arr;
            size_t idx{0};

            NodeType *operator*() const
            {
                return arr->vec_[idx];
            }

            iterator &operator++()
            {
                ++idx;
                skip_holes();
                return *this;
            }

            bool operator!=(const iterator &other) const
            {
                return idx != other.idx;
            }

            // advance idx to the next non-null slot (or to end)
            void skip_holes()
            {
                while (idx < arr->capacity_ && arr->vec_[idx] == nullptr)
                    ++idx;
            }
        };

        iterator begin()
        {
            iterator it{this, 0};
            it.skip_holes();
            return it;
        }

        iterator end()
        {
            return iterator{this, capacity_};
        }

        int size() const
        {
            return count_;
        }

        NodeType *get_node(const K &key)
        {
            for (size_t i = 0; i < capacity_; i++)
            {
                if (vec_[i] != nullptr && vec_[i]->key == key)
                    return vec_[i];
            }

            return nullptr;
        }

    private:
        int count_{0};
        size_t capacity_{0};
        NodeType **vec_{nullptr};
    };
}

#endif // __ARRAY_H