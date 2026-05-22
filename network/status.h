#ifndef __STATUS_H
#define __STATUS_H

#include <stdint.h>

namespace network
{
    struct StatusM
    {
        uint16_t status;       // 0: header 1: body 2: response
        uint32_t buffer_size;  // Next IO buffer size
        uint32_t sent_data;    // Data bytes sent
        uint32_t key_length;   // bytes number of key
        uint32_t value_length; // bytes number of value
    };
}

#endif // __STATUS_H