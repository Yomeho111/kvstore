#ifndef __STATUS_H
#define __STATUS_H

#include <stdint.h>

namespace network
{
    struct StatusM
    {
        uint16_t status;      // 0: header 1: body 2: response
        uint16_t buffer_size; // Next IO buffer size
    };
}

#endif // __STATUS_H