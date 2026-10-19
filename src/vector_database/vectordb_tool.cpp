#include "vectordb_tool.h"
#include <math.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdlib.h>
#include <limits>
#include <stdio.h>
#include "kv_log.h"

namespace fs = std::filesystem;

namespace vectordb
{

    static float _calc_l2norm(const float *vector, uint32_t len)
    {
        if (!vector || len == 0)
            return 1.0f;

        float sum{0.0f};

#pragma omp simd reduction(+ : sum)
        for (uint32_t i = 0; i < len; i++)
        {
            sum += vector[i] * vector[i];
        }

        return sqrtf(sum);
    }

    static int _transform_vector(float *vector, uint32_t len)
    {
        if (!vector || len == 0)
            return -1;

        const float l2norm = _calc_l2norm(vector, len);

        // Reject zero vectors, NaN and infinity
        if (!std::isfinite(l2norm) || l2norm <= 0.0f)
            return -1;

        if (std::fabs(l2norm - 1.0f) < 1e-6f)
            return 0;

        const float inv_norm = 1.0f / l2norm;

#pragma omp simd
        for (uint32_t i = 0; i < len; ++i)
        {
            vector[i] *= inv_norm;
        }

        return 0;
    }

    void *VectorDbEngine::transform_vector(char *value, size_t len)
    {
        if (!value || len < sizeof(uint32_t) * 2)
            return nullptr;

        uint32_t *version = (uint32_t *)value;
        uint32_t dimension;

        if (*version != VERSION)
        {
            return nullptr;
        }

        dimension = *((uint32_t *)(value + sizeof(uint32_t)));

        if (len - sizeof(uint32_t) * 2 < static_cast<size_t>(dimension) * sizeof(float))
            return nullptr;

        if (dimension != DIMENSION)
            return nullptr;

        float *vector = (float *)(value + sizeof(uint32_t) * 2);

        if (_transform_vector(vector, dimension) < 0)
            return nullptr;

        return (void *)vector;
    }

    VectorDbEngine &VectorDbEngine::instance(const kv_config::Config *config)
    {
        static VectorDbEngine db(config->persist_mode != kv_persistent::PersistMode::NONE);
        static int ret = db._init(config);
        if (ret != 0)
        {
            KV_ERROR("error init hnsw");
            exit(-1);
        }
        return db;
    }

    int VectorDbEngine::insert_vector(const void *data, size_t &vec_id)
    {
        if (!data)
            return -1;
        if (cur_id_ == std::numeric_limits<size_t>::max())
        {
            KV_ERROR("Cannot insert vector: label space is exhausted");
            return -1;
        }
        hnsw_->addPoint(data, cur_id_);
        vec_id = cur_id_++;
        return 0;
    }

    IdContainer VectorDbEngine::query_nearest(const void *query_data, size_t k)
    {
        if (k == 0 || !query_data)
            return {};
        return hnsw_->searchKnnCloserFirst(query_data, k);
    }

    VectorDbEngine::~VectorDbEngine()
    {
        if (is_persistent_)
        {
            fs::path file_path{DEFAULT_VECTORDB_PATH};
            if (!file_path.parent_path().empty())
            {
                fs::create_directories(file_path.parent_path());
            }

            hnsw_->saveIndex(file_path.string());
        }
    }

    int VectorDbEngine::_init(const kv_config::Config *config)
    {
        if (running_)
            return -1;

        space_ = std::make_unique<hnswlib::L2Space>(DIMENSION);

        fs::path file_path = DEFAULT_VECTORDB_PATH;

        if (is_persistent_ && fs::exists(file_path) && fs::is_regular_file(file_path))
        {
#ifdef ENABLE_BRUTEFORCE
            hnsw_ = std::make_unique<VectordbCore>(
                space_.get(),
                DEFAULT_VECTORDB_PATH);
#else
            hnsw_ =
                std::make_unique<VectordbCore>(
                    space_.get(),
                    DEFAULT_VECTORDB_PATH,
                    false,
                    MAX_ELEMENTS,
                    true);

#endif
        }
        else
        {
#ifdef ENABLE_BRUTEFORCE
            hnsw_ = std::make_unique<VectordbCore>(
                space_.get(),
                MAX_ELEMENTS);
#else
            hnsw_ = std::make_unique<VectordbCore>(
                space_.get(),
                MAX_ELEMENTS,
                config->M,
                config->ef_construction,
                100,
                true);
#endif
        }

        size_t max_label = 0;
#ifdef ENABLE_BRUTEFORCE
        const size_t count = hnsw_->cur_element_count;
        for (size_t i = 0; i < count; ++i)
        {
            hnswlib::labeltype label;
            std::memcpy(&label, hnsw_->data_ + i * hnsw_->size_per_element_ + hnsw_->data_size_, sizeof(label));
            max_label = std::max(max_label, label);
        }
#else
        const size_t count = hnsw_->getCurrentElementCount();

        for (hnswlib::tableint i = 0; i < count; ++i)
        {
            max_label = std::max(max_label, hnsw_->getExternalLabel(i));
        }
#endif

        if (max_label == std::numeric_limits<size_t>::max())
        {
            KV_ERROR("Cannot allocate vector IDs: persisted label space is exhausted");
            return -1;
        }

        cur_id_ = max_label + 1;
#ifndef ENABLE_BRUTEFORCE
        hnsw_->ef_ = config->ef;
#endif
        return 0;
    }

} // namespace vectordb