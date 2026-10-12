#include "vectordb_tool.h"
#include <math.h>
#include <cmath>

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

    static float _calculate_distance(const float *v1, const float *v2, uint32_t len)
    {
        float dot = 0.0f;
#pragma omp simd reduction(+ : dot)
        for (uint32_t i = 0; i < len; i++)
        {
            dot += v1[i] * v2[i];
        }
        return 1.0f - dot;
    }

    int VectorDbEngine::transform_vector(char *value, size_t len)
    {
        if (!value || len < sizeof(uint32_t) * 2)
            return -1;

        uint32_t *version = (uint32_t *)value;
        uint32_t dimension;

        if (*version != VERSION)
        {
            return -2;
        }

        dimension = *((uint32_t *)(value + sizeof(uint32_t)));

        if (len - sizeof(uint32_t) * 2 < static_cast<size_t>(dimension) * sizeof(float))
            return -3;

        float *vector = (float *)(value + sizeof(uint32_t) * 2);

        if (_transform_vector(vector, dimension) < 0)
            return -4;

        return 0;
    }

    float VectorDbEngine::calculate_distance(const char *v1, size_t l1, const char *v2, size_t l2)
    {
        if (!v1 || !v2 || l1 < sizeof(uint32_t) * 2 || l2 < sizeof(uint32_t) * 2 || l1 != l2)
            return INVALID_DISTANCE;

        const uint32_t *version1 = (const uint32_t *)v1;
        const uint32_t *version2 = (const uint32_t *)v2;
        uint32_t dimension1, dimension2;

        if (*version1 != VERSION || *version2 != VERSION)
        {
            return INVALID_DISTANCE;
        }

        dimension1 = *((const uint32_t *)(v1 + sizeof(uint32_t)));
        dimension2 = *((const uint32_t *)(v2 + sizeof(uint32_t)));

        if ((l1 - sizeof(uint32_t) * 2 < static_cast<size_t>(dimension1) * sizeof(float)) ||
            (l2 - sizeof(uint32_t) * 2 < static_cast<size_t>(dimension2) * sizeof(float)))
            return INVALID_DISTANCE;

        const float *vector1 = (const float *)(v1 + sizeof(uint32_t) * 2);
        const float *vector2 = (const float *)(v2 + sizeof(uint32_t) * 2);

        return _calculate_distance(vector1, vector2, dimension1);
    }
} // namespace vectordb