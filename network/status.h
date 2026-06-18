#ifndef __STATUS_H
#define __STATUS_H

#include <stdint.h>
#include "kv_header.h"

namespace network
{
    enum NetWorkStatus
    {
        READ_NUM_REQUEST = 0,
        READ_HEADER,
        READ_BODY,
        SEND_RESPONSE,
    };

    struct StatusM
    {
        bool is_response;
        uint16_t status; // 0: num_request 1: header 2: body 3: response
        uint32_t num_request;
        uint32_t w_iovec_size;
        struct kv_protocal::HeaderInfo *req_info;
    };
}

#endif // __STATUS_H