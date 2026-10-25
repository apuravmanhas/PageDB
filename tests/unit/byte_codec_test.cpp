#include "pagedb/byte_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(CheckedArithmeticBoundariesAndOverflows) {
  auto u16_ok = checked_add_u16(4000, 96);
  PAGEDB_ASSERT_OK(u16_ok);
  PAGEDB_ASSERT_EQ(u16_ok.value(), static_cast<std::uint16_t>(4096));

  auto u16_ovf = checked_add_u16(65535, 1);
  PAGEDB_ASSERT_FALSE(u16_ovf.ok());
  PAGEDB_ASSERT_EQ(u16_ovf.code(), ErrorCode::kCorruption);

  auto u32_ok = checked_add_u32(100U, 200U);
  PAGEDB_ASSERT_OK(u32_ok);
  PAGEDB_ASSERT_EQ(u32_ok.value(), 300U);

  auto u32_ovf = checked_add_u32(std::numeric_limits<std::uint32_t>::max(), 1U);
  PAGEDB_ASSERT_FALSE(u32_ovf.ok());

  auto u64_ok = checked_add_u64(1000ULL, 2000ULL);
  PAGEDB_ASSERT_OK(u64_ok);
  PAGEDB_ASSERT_EQ(u64_ok.value(), 3000ULL);

  auto u64_ovf =
      checked_add_u64(std::numeric_limits<std::uint64_t>::max(), 1ULL);
  PAGEDB_ASSERT_FALSE(u64_ovf.ok());

  auto mul_ok = checked_mul_u64(1024ULL, 4096ULL);
  PAGEDB_ASSERT_OK(mul_ok);
  PAGEDB_ASSERT_EQ(mul_ok.value(), 4194304ULL);

  auto mul_ovf =
      checked_mul_u64(std::numeric_limits<std::uint64_t>::max() / 2ULL + 2ULL, 2ULL);
  PAGEDB_ASSERT_FALSE(mul_ovf.ok());

  auto page_off0 = checked_page_offset(PageId(0));
  PAGEDB_ASSERT_OK(page_off0);
  PAGEDB_ASSERT_EQ(page_off0.value(), 0ULL);

  auto page_off5 = checked_page_offset(PageId(5));
  PAGEDB_ASSERT_OK(page_off5);
  PAGEDB_ASSERT_EQ(page_off5.value(), 5ULL * 4096ULL);

  auto page_invalid = checked_page_offset(kInvalidPageId);
  PAGEDB_ASSERT_FALSE(page_invalid.ok());
  PAGEDB_ASSERT_EQ(page_invalid.code(), ErrorCode::kInvalidArgument);
}

PAGEDB_TEST(GoldenBytesLittleEndianEncodingAndDecoding) {
  std::array<std::byte, 15> buf{};
  ByteWriter writer(buf);

  PAGEDB_ASSERT_OK(writer.write_u8(0xABU));
  PAGEDB_ASSERT_OK(writer.write_u16_le(0x1234U));
  PAGEDB_ASSERT_OK(writer.write_u32_le(0x89ABCDEFU));
  PAGEDB_ASSERT_OK(writer.write_u64_le(0x0102030405060708ULL));
  PAGEDB_ASSERT_EQ(writer.position(), 15U);
  PAGEDB_ASSERT_EQ(writer.remaining(), 0U);

  const std::array<std::uint8_t, 15> expected{
      0xAB,                    // u8
      0x34, 0x12,              // u16_le
      0xEF, 0xCD, 0xAB, 0x89,  // u32_le
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01  // u64_le
  };
  for (std::size_t i = 0; i < expected.size(); ++i) {
    PAGEDB_ASSERT_EQ(std::to_integer<std::uint8_t>(buf[i]), expected[i]);
  }

  ByteReader reader(buf);
  auto v8 = reader.read_u8();
  auto v16 = reader.read_u16_le();
  auto v32 = reader.read_u32_le();
  auto v64 = reader.read_u64_le();
  PAGEDB_ASSERT_OK(v8);
  PAGEDB_ASSERT_OK(v16);
  PAGEDB_ASSERT_OK(v32);
  PAGEDB_ASSERT_OK(v64);
  PAGEDB_ASSERT_EQ(v8.value(), 0xABU);
  PAGEDB_ASSERT_EQ(v16.value(), 0x1234U);
  PAGEDB_ASSERT_EQ(v32.value(), 0x89ABCDEFU);
  PAGEDB_ASSERT_EQ(v64.value(), 0x0102030405060708ULL);
  PAGEDB_ASSERT_EQ(reader.remaining(), 0U);
}

PAGEDB_TEST(MalformedAndOutOfBoundsReadsRejectedCleanly) {
  std::array<std::byte, 7> short_buf{};

  PAGEDB_ASSERT_FALSE(read_u8_at(short_buf, 7).ok());
  PAGEDB_ASSERT_FALSE(read_u16_le_at(short_buf, 6).ok());
  PAGEDB_ASSERT_FALSE(read_u32_le_at(short_buf, 4).ok());
  PAGEDB_ASSERT_FALSE(read_u64_le_at(short_buf, 0).ok());

  PAGEDB_ASSERT_FALSE(write_u8_at(short_buf, 7, 1U).ok());
  PAGEDB_ASSERT_FALSE(write_u16_le_at(short_buf, 6, 1U).ok());
  PAGEDB_ASSERT_FALSE(write_u32_le_at(short_buf, 4, 1U).ok());
  PAGEDB_ASSERT_FALSE(write_u64_le_at(short_buf, 0, 1ULL).ok());

  ByteReader reader(short_buf);
  PAGEDB_ASSERT_OK(reader.read_u32_le());
  PAGEDB_ASSERT_OK(reader.read_u16_le());
  // 1 byte remaining; u16 read must fail without advancing cursor.
  PAGEDB_ASSERT_FALSE(reader.read_u16_le().ok());
  PAGEDB_ASSERT_EQ(reader.remaining(), 1U);
  PAGEDB_ASSERT_OK(reader.read_u8());
  PAGEDB_ASSERT_EQ(reader.remaining(), 0U);
}

PAGEDB_TEST(UnsignedByteComparatorOrdering) {
  const std::string k_empty;
  const std::string k_00("\x00", 1);
  const std::string k_00_00("\x00\x00", 2);
  const std::string k_7f("\x7f", 1);
  const std::string k_80("\x80", 1);
  const std::string k_ff("\xff", 1);

  PAGEDB_ASSERT_TRUE(compare_keys(k_empty, k_00) < 0);
  PAGEDB_ASSERT_TRUE(compare_keys(k_00, k_00_00) < 0);
  PAGEDB_ASSERT_TRUE(compare_keys(k_00_00, k_7f) < 0);
  // Critical test: 0x7f (127) < 0x80 (128) < 0xff (255) even when char is signed.
  PAGEDB_ASSERT_TRUE(compare_keys(k_7f, k_80) < 0);
  PAGEDB_ASSERT_TRUE(compare_keys(k_80, k_ff) < 0);
  PAGEDB_ASSERT_TRUE(compare_keys(k_ff, k_ff) == 0);

  std::map<std::string, int, UnsignedByteLess> ordered;
  ordered[k_ff] = 5;
  ordered[k_80] = 4;
  ordered[k_7f] = 3;
  ordered[k_00_00] = 2;
  ordered[k_00] = 1;
  ordered[k_empty] = 0;

  int expected_rank = 0;
  for (const auto& [k, rank] : ordered) {
    (void)k;
    PAGEDB_ASSERT_EQ(rank, expected_rank);
    ++expected_rank;
  }
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
