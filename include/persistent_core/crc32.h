#ifndef __KV_CRC32_H
#define __KV_CRC32_H

#include <stddef.h>
#include <stdint.h>
#include <array>
#include <crc.h>

namespace kv_persistent
{
    namespace checksum
    {
        inline constexpr uint32_t CRC32_INIT = 0xFFFFFFFFu;

        inline uint32_t crc32_update(
            uint32_t crc,
            const void *data,
            size_t len)
        {
            auto *p =
                const_cast<unsigned char *>(
                    static_cast<const unsigned char *>(data));

            /*
             * ISA-L crc32_iscsi() uses int len,
             * so handle > INT_MAX buffers in chunks.
             */
            while (len > static_cast<size_t>(INT_MAX))
            {
                crc = ::crc32_iscsi(
                    p,
                    INT_MAX,
                    crc);

                p += INT_MAX;
                len -= INT_MAX;
            }

            if (len)
            {
                crc = ::crc32_iscsi(
                    p,
                    static_cast<int>(len),
                    crc);
            }

            return crc;
        }

        inline uint32_t crc32_final(uint32_t crc)
        {
            return crc ^ 0xFFFFFFFFu;
        }

        inline uint32_t crc32(
            const void *data,
            size_t len)
        {
            return crc32_final(
                crc32_update(
                    CRC32_INIT,
                    data,
                    len));
        }
    } // namespace checksum
} // namespace kv_persistent

#endif // __KV_CRC32_H
