#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "pagedb/types.h"

namespace pagedb {

// Incremental Castagnoli CRC32C (polynomial 0x1EDC6F41 / reversed 0x82F63B78).
[[nodiscard]] std::uint32_t crc32c_extend(
    std::uint32_t crc, std::span<const std::byte> data) noexcept;

// One-shot Castagnoli CRC32C over a byte span.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> data) noexcept;

// Computes CRC32C over a 4096-byte page while treating the 4-byte checksum
// field at [checksum_offset, checksum_offset + 4) as 0x00000000.
[[nodiscard]] std::uint32_t compute_page_crc32c(
    std::span<const std::byte, kPageSize> page,
    std::size_t checksum_offset = 0x24) noexcept;

}  // namespace pagedb
