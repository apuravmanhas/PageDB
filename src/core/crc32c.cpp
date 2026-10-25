#include "pagedb/crc32c.h"

#include <array>
#include <cassert>

namespace pagedb {
namespace {

constexpr std::uint32_t kCastagnoliReversedPoly = 0x82F63B78U;

consteval std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 1U) != 0U) {
        crc = (crc >> 1U) ^ kCastagnoliReversedPoly;
      } else {
        crc >>= 1U;
      }
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

}  // namespace

std::uint32_t crc32c_extend(std::uint32_t crc,
                            std::span<const std::byte> data) noexcept {
  std::uint32_t c = ~crc;
  for (const std::byte b : data) {
    const auto idx =
        static_cast<std::uint8_t>((c ^ std::to_integer<std::uint8_t>(b)) & 0xFFU);
    c = (c >> 8U) ^ kCrc32cTable[idx];
  }
  return ~c;
}

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  return crc32c_extend(0U, data);
}

std::uint32_t compute_page_crc32c(std::span<const std::byte, kPageSize> page,
                                  std::size_t checksum_offset) noexcept {
  assert(checksum_offset + 4 <= kPageSize);
  static constexpr std::array<std::byte, 4> kZeroChecksum{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};

  std::uint32_t crc = 0U;
  crc = crc32c_extend(crc, page.subspan(0, checksum_offset));
  crc = crc32c_extend(crc, kZeroChecksum);
  crc = crc32c_extend(
      crc, page.subspan(checksum_offset + 4, kPageSize - (checksum_offset + 4)));
  return crc;
}

}  // namespace pagedb
