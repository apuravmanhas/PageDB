#include "pagedb/crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "pagedb/byte_codec.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(Rfc3720StandardTestVectors) {
  // 1. Empty input -> 0x00000000
  std::span<const std::byte> empty_span{};
  PAGEDB_ASSERT_EQ(crc32c(empty_span), 0x00000000U);

  // 2. Standard ASCII "123456789" -> 0xE3069283
  constexpr std::string_view kDigits = "123456789";
  const auto digits_bytes =
      std::as_bytes(std::span<const char>(kDigits.data(), kDigits.size()));
  PAGEDB_ASSERT_EQ(crc32c(digits_bytes), 0xE3069283U);

  // 3. 32 bytes of 0x00 -> 0x8A9136AA
  std::array<std::byte, 32> zeros{};
  PAGEDB_ASSERT_EQ(crc32c(zeros), 0x8A9136AAU);

  // 4. 32 bytes of 0xFF -> 0x62A8AB43
  std::array<std::byte, 32> ones{};
  ones.fill(std::byte{0xFF});
  PAGEDB_ASSERT_EQ(crc32c(ones), 0x62A8AB43U);

  // 5. 32 incrementing bytes 0x00..0x1F -> 0x46DD794E
  std::array<std::byte, 32> inc{};
  for (std::size_t i = 0; i < 32; ++i) {
    inc[i] = static_cast<std::byte>(i);
  }
  PAGEDB_ASSERT_EQ(crc32c(inc), 0x46DD794EU);

  // 6. 32 decrementing bytes 0x1F..0x00 -> 0x113FDB5C
  std::array<std::byte, 32> dec{};
  for (std::size_t i = 0; i < 32; ++i) {
    dec[i] = static_cast<std::byte>(31 - i);
  }
  PAGEDB_ASSERT_EQ(crc32c(dec), 0x113FDB5CU);
}

PAGEDB_TEST(IncrementalExtendMatchesSingleShotAndPageCrc) {
  std::array<std::byte, kPageSize> page{};
  for (std::size_t i = 0; i < kPageSize; ++i) {
    page[i] = static_cast<std::byte>((i * 37U + 13U) & 0xFFU);
  }

  // Zero out bytes 0x24..0x27 in a reference copy and compute one-shot CRC32C.
  std::array<std::byte, kPageSize> ref = page;
  PAGEDB_ASSERT_OK(write_u32_le_at(ref, 0x24, 0U));
  const std::uint32_t expected = crc32c(ref);

  // Write a non-zero value into page[0x24..0x27] and verify compute_page_crc32c
  // ignores the checksum field bytes.
  PAGEDB_ASSERT_OK(write_u32_le_at(page, 0x24, 0xDEADBEEFU));
  PAGEDB_ASSERT_EQ(compute_page_crc32c(page, 0x24), expected);
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
