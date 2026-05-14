#ifndef __KV_PROTOCAL_H
#define __KV_PROTOCAL_H

#include <stdint.h>
#include <stddef.h>

#include "status.h"

namespace kv_protocal
{
    struct KvHeader
    {
        uint16_t body_length;
    };

    constexpr inline const size_t HEADER_SIZE = sizeof(KvHeader);

    template <typename KvEngine>
    class KvProtocal
    {

    private:
        KvEngine _engine;
    };
}

#endif // __KV_PROTOCAL_H