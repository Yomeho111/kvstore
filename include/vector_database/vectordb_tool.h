#ifndef __VECTORDB_TOOL_H
#define __VECTORDB_TOOL_H

#include <stddef.h>
#include <stdint.h>
#include <vector>
#include <concepts>
#include <algorithm>
#include "allocator.h"

namespace vectordb
{
    template <typename T>
    concept Comparable = requires(const T &a, const T &b) {
        { a < b } -> std::convertible_to<bool>;
        { a > b } -> std::convertible_to<bool>;
    };

    inline constexpr static uint32_t VERSION{0x01};

    inline constexpr static float INVALID_DISTANCE{10.0f};
    class VectorDbEngine
    {
    public:
        /**
         * @brief verify the vector value is valid for the system, it should be like <Version><Dimension><vector>
         * <4bytes><4bytes><Dimension * float32>
         */
        static int transform_vector(char *value, size_t len);

        static float calculate_distance(const char *v1, size_t l1, const char *v2, size_t l2);

        template <Comparable T>
        static int get_top_k(std::vector<T, allocator::MyAllocator<T>> &vec, int K)
        {
            if (vec.size() <= K)
                return 0;
            auto pivot_func = [&vec](int start, int end) -> int
            {
                int pivot = end;
                int l = start;
                int r = end - 1;
                int p = start;

                while (p <= r)
                {
                    if (vec[p] < vec[pivot])
                    {
                        std::swap(vec[l++], vec[p++]);
                    }
                    else if (vec[p] > vec[pivot])
                    {
                        std::swap(vec[p], vec[r--]);
                    }
                    else
                    {
                        p++;
                    }
                }

                pivot = p;
                std::swap(vec[pivot], vec[end]);
                return pivot;
            };

            int l = 0;
            int r = vec.size() - 1;

            while (l <= r)
            {
                int pivot = pivot_func(l, r);
                if (pivot == K - 1)
                {
                    return pivot;
                }
                else if (pivot > K - 1)
                {
                    r = pivot - 1;
                }
                else
                    l = pivot + 1;
            }
            return -1;
        }
    };
} // namespace vectordb

#endif // __VECTORDB_TOOL_H