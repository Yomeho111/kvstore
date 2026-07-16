#ifndef __KV_CRC32_H
#define __KV_CRC32_H

#include <stddef.h>
#include <stdint.h>
#include <array>

namespace kv_persistent
{
    namespace checksum
    {
        // Standard CRC-32 (IEEE 802.3), reflected, polynomial 0xEDB88320.
        inline constexpr uint32_t CRC32_POLY = 0xEDB88320u;
        inline constexpr uint32_t CRC32_INIT = 0xFFFFFFFFu;

        inline constexpr std::array<uint32_t, 256> make_crc32_table()
        {
            std::array<uint32_t, 256> table{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t crc = i;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc & 1u) ? (crc >> 1) ^ CRC32_POLY : (crc >> 1);
                table[i] = crc;
            }
            return table;
        }

        inline constexpr std::array<uint32_t, 256> CRC32_TABLE = make_crc32_table();

        // Feed more bytes into a running (not yet finalized) crc value.
        inline uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
        {
            const uint8_t *bytes = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < len; ++i)
                crc = CRC32_TABLE[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
            return crc;
        }

        // Turn a running crc value into the final checksum.
        inline uint32_t crc32_final(uint32_t crc)
        {
            return crc ^ 0xFFFFFFFFu;
        }

        // One-shot checksum over a contiguous buffer.
        inline uint32_t crc32(const void *data, size_t len)
        {
            return crc32_final(crc32_update(CRC32_INIT, data, len));
        }
    } // namespace checksum
} // namespace kv_persistent

#endif // __KV_CRC32_H
