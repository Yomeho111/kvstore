#ifndef KVSTORE_MEMORY_UTILS_H
#define KVSTORE_MEMORY_UTILS_H

#include <atomic>
#include <stddef.h>
#include <type_traits>
#include <bit>

namespace memory
{

    constexpr size_t PAGE_SIZE{4096UL};
    constexpr size_t ALIGNMENT{sizeof(void *) * 2};

    constexpr size_t MAX_BYTES{128 * 1024};
    constexpr size_t PADDING_INDEX{std::countr_zero(ALIGNMENT) + 1};
    constexpr size_t MIN_SIZE{1ul << PADDING_INDEX};
    constexpr size_t FREE_LIST_SIZE{std::countr_zero(MAX_BYTES) - PADDING_INDEX + 1};

    constexpr size_t THRESHHOLD_BYTES{4096};
    constexpr size_t SMALL_BATCH_SIZE{20};
    constexpr size_t BIG_BATCH_SIZE{10};

    class SpinLock
    {
    public:
        SpinLock() : flag_(ATOMIC_FLAG_INIT) {}
        ~SpinLock() {}

        void lock()
        {
            while (flag_.test_and_set(std::memory_order_acquire))
            {
#ifdef __x86_64__
                __asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
                __asm__ __volatile__("yield");
#endif
            }
        }

        void unlock()
        {
            flag_.clear(std::memory_order_release);
        }

    private:
        SpinLock(const SpinLock &) = delete;
        SpinLock(SpinLock &&) = delete;

        SpinLock &operator=(const SpinLock &) = delete;
        SpinLock &operator=(SpinLock &&) = delete;
        std::atomic_flag flag_;
    };

    class SizeClass
    {
    public:
        static size_t round_up(size_t bytes)
        {
            if (bytes < MIN_SIZE)
                return MIN_SIZE;
            bytes--;
            for (int shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
            {
                bytes |= (bytes >> shift);
            }
            return bytes + 1;
        }

        static size_t get_index(size_t bytes)
        {
            bytes = round_up(bytes);
            return std::countr_zero(bytes) - PADDING_INDEX;
        }

        static size_t get_size(size_t index)
        {
            return 1ul << (index + PADDING_INDEX);
        }
    };
}

#endif // KVSTORE_MEMORY_UTILS_H