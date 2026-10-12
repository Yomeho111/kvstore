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

    // Sub-octave size classes (tcmalloc / jemalloc style): every power-of-two
    // octave [2^k, 2^(k+1)) is split into SUBCLASSES evenly spaced classes. This
    // bounds internal fragmentation to ~1/SUBCLASSES (12.5% for LG_SUB = 3)
    // instead of the up-to-100% waste of pure power-of-two classes -- e.g. a 1 KB
    // value (1041 B with header) used to consume a 2048 B block; it now maps to a
    // 1152 B block.
    constexpr size_t LG_SUB{3};
    constexpr size_t SUBCLASSES{1ul << LG_SUB};
    constexpr size_t LG_MIN{PADDING_INDEX};
    constexpr size_t LG_MAX{std::countr_zero(MAX_BYTES)};
    constexpr size_t FREE_LIST_SIZE{1 + (LG_MAX - LG_MIN) * SUBCLASSES};

    constexpr size_t THRESHHOLD_BYTES{4096};
    constexpr size_t SMALL_BATCH_SIZE{20};
    constexpr size_t BIG_BATCH_SIZE{10};

    class SpinLock
    {
    public:
        SpinLock()
            : flag_(ATOMIC_FLAG_INIT) {}
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
        // floor(log2(v)) for v >= 1.
        static size_t floor_log2(size_t v)
        {
            return (sizeof(size_t) * 8 - 1) - std::countl_zero(v);
        }

        static size_t round_up(size_t bytes)
        {
            if (bytes <= MIN_SIZE)
                return MIN_SIZE;
            size_t lg = floor_log2(bytes - 1);
            size_t delta = size_t(1) << (lg - LG_SUB);
            return (bytes + delta - 1) & ~(delta - 1);
        }

        static size_t get_index(size_t bytes)
        {
            if (bytes <= MIN_SIZE)
                return 0;
            size_t lg = floor_log2(bytes - 1);
            size_t j = (bytes - 1 - (size_t(1) << lg)) >> (lg - LG_SUB);
            return 1 + (lg - LG_MIN) * SUBCLASSES + j;
        }

        static size_t get_size(size_t index)
        {
            if (index == 0)
                return MIN_SIZE;
            size_t k = index - 1;
            size_t lg = LG_MIN + k / SUBCLASSES;
            size_t j = k % SUBCLASSES;
            size_t delta = size_t(1) << (lg - LG_SUB);
            return (size_t(1) << lg) + (j + 1) * delta;
        }
    };
} // namespace memory

#endif // KVSTORE_MEMORY_UTILS_H