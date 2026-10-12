#ifndef __LOCKFREE_QUEUE
#define __LOCKFREE_QUEUE

#include <atomic>
#include <type_traits>
#include "slab.hpp"

namespace base_component
{

    template <typename T>
    class MpscQueue
    {

    public:
        MpscQueue() : head_(new (KV_NODE_ALLOC(Node)) Node()), tail_(head_.load(std::memory_order_relaxed))
        {
            Node *front = head_.load(std::memory_order_relaxed);
            front->next.store(nullptr, std::memory_order_relaxed);
        }

        ~MpscQueue()
        {
            T output;
            while (dequeue(output))
                ;

            Node *front = head_.load(std::memory_order_relaxed);
            front->~Node();
            KV_NODE_FREE(Node, front);
        }

        // wait free
        void enqueue(T data)
        {
            Node *node = new (KV_NODE_ALLOC(Node)) Node(std::move(data));
            Node *prevHead = head_.exchange(node, std::memory_order_acq_rel);
            prevHead->next.store(node, std::memory_order_release);
        }

        bool dequeue(T &result)
        {
            Node *tail = tail_.load(std::memory_order_relaxed);
            Node *next = tail->next.load(std::memory_order_acquire);

            if (!next)
                return false;

            result = std::move(next->data);
            tail_.store(next, std::memory_order_release);
            tail->~Node();
            KV_NODE_FREE(Node, tail);
            return true;
        }

    private:
        struct Node
        {
            Node() = default;
            explicit Node(T &&d) : data(std::move(d))
            {
                next.store(nullptr, std::memory_order_relaxed);
            }

            ~Node() {}

            T data;
            std::atomic<Node *> next;
        };

        std::atomic<Node *> head_;
        std::atomic<Node *> tail_;

        MpscQueue(const MpscQueue &) = delete;
        MpscQueue &operator=(const MpscQueue &) = delete;

        MpscQueue(MpscQueue &&) = delete;
        MpscQueue &operator=(MpscQueue &&) = delete;
    };

} // namespace base_component

#endif // __LOCKFREE_QUEUE