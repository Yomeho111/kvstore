#ifndef __VECTORDB_TOOL_H
#define __VECTORDB_TOOL_H

#include <stddef.h>
#include <stdint.h>
#include <vector>
#include <concepts>
#include <algorithm>
#include <memory>
#include "allocator.h"
#include "kv_config.h"

#include "hnswlib/hnswlib.h"

#define DEFAULT_VECTORDB_PATH "vdb_data/vectordb.bin"

namespace vectordb
{
    template <typename T>
    concept Comparable = requires(const T &a, const T &b) {
        { a < b } -> std::convertible_to<bool>;
        { a > b } -> std::convertible_to<bool>;
    };

    inline constexpr static uint32_t VERSION{0x01};

    inline constexpr static size_t DIMENSION{768};

    inline constexpr static size_t MAX_ELEMENTS{100000};

    using IdContainer = std::vector<std::pair<float, size_t>>;

    class VectorDbEngine
    {

#ifdef ENABLE_BRUTEFORCE
        using VectordbCore = hnswlib::BruteforceSearch<float>;
#else
        using VectordbCore = hnswlib::HierarchicalNSW<float>;
#endif
    public:
        /**
         * @brief verify the vector value is valid for the system, it should be like <Version><Dimension><vector>
         * <4bytes><4bytes><Dimension * float32>
         */
        static void *transform_vector(char *value, size_t len);

        static VectorDbEngine &instance(const kv_config::Config *config = nullptr);

        int insert_vector(const void *data, size_t &vec_id);

        IdContainer query_nearest(const void *query_data, size_t k);

    private:
        VectorDbEngine(bool is_persistent) : is_persistent_(is_persistent) {}
        VectorDbEngine(const VectorDbEngine &) = delete;
        VectorDbEngine(VectorDbEngine &&) = delete;
        VectorDbEngine &operator=(const VectorDbEngine &) = delete;
        VectorDbEngine &operator=(VectorDbEngine &&) = delete;

        ~VectorDbEngine();

        bool is_persistent_{false};
        bool running_{false};
        int _init(const kv_config::Config *config);

        size_t cur_id_{1};

        std::unique_ptr<hnswlib::L2Space> space_;
        std::unique_ptr<VectordbCore> hnsw_;
    };
} // namespace vectordb

#endif // __VECTORDB_TOOL_H