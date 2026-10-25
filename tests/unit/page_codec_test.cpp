#include "pagedb/page_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "pagedb/byte_codec.h"
#include "pagedb/crc32c.h"
#include "test_harness.h"

namespace pagedb {
namespace {

std::span<const std::byte> as_byte_span(std::string_view sv) noexcept {
  return std::as_bytes(std::span<const char>(sv.data(), sv.size()));
}

void recompute_and_write_crc(std::span<std::byte, kPageSize> page) {
  const std::uint32_t crc = compute_page_crc32c(page, kChecksumFieldOffset);
  PAGEDB_ASSERT_OK(write_u32_le_at(page, kChecksumFieldOffset, crc));
}

PAGEDB_TEST(SuperblockEncodeDecodeAndSystematicCorruption) {
  Superblock sb{};
  sb.root_page_id = PageId(3);
  sb.next_page_id = PageId(10);
  sb.free_list_head = PageId(5);
  sb.checkpoint_lsn = Lsn(1024);
  sb.page_lsn = Lsn(2048);

  PageBuffer page{};
  PAGEDB_ASSERT_OK(PageCodec::encode_superblock(sb, page));

  // Verify trailing slack bytes are all zero.
  for (std::size_t i = kSuperblockHeaderEnd; i < kPageSize; ++i) {
    PAGEDB_ASSERT_EQ(std::to_integer<std::uint8_t>(page[i]), 0U);
  }

  auto decoded = PageCodec::decode_superblock(page);
  PAGEDB_ASSERT_OK(decoded);
  PAGEDB_ASSERT_EQ(decoded.value().root_page_id, PageId(3));
  PAGEDB_ASSERT_EQ(decoded.value().next_page_id, PageId(10));
  PAGEDB_ASSERT_EQ(decoded.value().free_list_head, PageId(5));
  PAGEDB_ASSERT_EQ(decoded.value().checkpoint_lsn, Lsn(1024));
  PAGEDB_ASSERT_EQ(decoded.value().page_lsn, Lsn(2048));

  // 1. Corrupt magic -> Corruption
  {
    PageBuffer bad = page;
    bad[0] ^= std::byte{0x01};
    PAGEDB_ASSERT_EQ(PageCodec::decode_superblock(bad).code(),
                     ErrorCode::kCorruption);
  }

  // 2. Corrupt format major version -> UnsupportedFormatVersion
  {
    PageBuffer bad = page;
    PAGEDB_ASSERT_OK(write_u16_le_at(bad, 0x04, 2U));
    recompute_and_write_crc(bad);
    PAGEDB_ASSERT_EQ(PageCodec::decode_superblock(bad).code(),
                     ErrorCode::kUnsupportedFormatVersion);
  }

  // 3. Corrupt payload byte without updating checksum -> Corruption (CRC32C)
  {
    PageBuffer bad = page;
    bad[0x3C] ^= std::byte{0xFF};
    PAGEDB_ASSERT_EQ(PageCodec::decode_superblock(bad).code(),
                     ErrorCode::kCorruption);
  }

  // 4. Non-zero header reserved field (even with valid CRC) -> Corruption
  {
    PageBuffer bad = page;
    bad[0x28] = std::byte{0x01};
    recompute_and_write_crc(bad);
    PAGEDB_ASSERT_EQ(PageCodec::decode_superblock(bad).code(),
                     ErrorCode::kCorruption);
  }

  // 5. Non-zero trailing slack byte (even with valid CRC) -> Corruption
  {
    PageBuffer bad = page;
    bad[4095] = std::byte{0x42};
    recompute_and_write_crc(bad);
    PAGEDB_ASSERT_EQ(PageCodec::decode_superblock(bad).code(),
                     ErrorCode::kCorruption);
  }

  // 6. Impossible root_page_id >= next_page_id -> Corruption
  {
    PageBuffer bad = page;
    PAGEDB_ASSERT_OK(write_u32_le_at(bad, 0x3C, 10U));  // root == next_page_id
    recompute_and_write_crc(bad);
    PAGEDB_ASSERT_EQ(PageCodec::decode_superblock(bad).code(),
                     ErrorCode::kCorruption);
  }
}

PAGEDB_TEST(SlottedPageLifecycleCompactionAndSlackZeroing) {
  SlottedPage sp;
  PAGEDB_ASSERT_OK(sp.init(PageId(2), PageType::kLeaf, PageId(7), Lsn(99)));
  PAGEDB_ASSERT_EQ(sp.num_slots(), 0U);
  PAGEDB_ASSERT_EQ(sp.contiguous_free_space(), kPageSize - kPageHeaderSize);

  const auto p0 = as_byte_span("alpha_record_payload");
  const auto p1 = as_byte_span("bravo_record_payload_longer");
  const auto p2 = as_byte_span("charlie_record");

  PAGEDB_ASSERT_OK(sp.insert_slot(0, p0));
  PAGEDB_ASSERT_OK(sp.insert_slot(1, p2));
  PAGEDB_ASSERT_OK(sp.insert_slot(1, p1));  // Insert in the middle

  PAGEDB_ASSERT_EQ(sp.num_slots(), 3U);
  PAGEDB_ASSERT_TRUE(compare_bytes(sp.slot_payload(0).value(), p0) == 0);
  PAGEDB_ASSERT_TRUE(compare_bytes(sp.slot_payload(1).value(), p1) == 0);
  PAGEDB_ASSERT_TRUE(compare_bytes(sp.slot_payload(2).value(), p2) == 0);

  // Erase middle slot and verify fragmented_free_bytes increases.
  PAGEDB_ASSERT_OK(sp.erase_slot(1));
  PAGEDB_ASSERT_EQ(sp.num_slots(), 2U);
  PAGEDB_ASSERT_EQ(sp.header().fragmented_free_bytes, p1.size());

  // Update slot 0 with shorter payload, then larger payload.
  const auto p0_short = as_byte_span("short");
  PAGEDB_ASSERT_OK(sp.update_slot(0, p0_short));
  PAGEDB_ASSERT_TRUE(compare_bytes(sp.slot_payload(0).value(), p0_short) == 0);

  const auto p0_large =
      as_byte_span("much_longer_replacement_payload_for_slot_zero_testing");
  PAGEDB_ASSERT_OK(sp.update_slot(0, p0_large));
  PAGEDB_ASSERT_TRUE(compare_bytes(sp.slot_payload(0).value(), p0_large) == 0);
  PAGEDB_ASSERT_TRUE(compare_bytes(sp.slot_payload(1).value(), p2) == 0);

  // Encode, decode, and verify round-trip + zeroed free space invariant.
  PageBuffer encoded{};
  PAGEDB_ASSERT_OK(sp.encode(encoded));

  auto decoded = SlottedPage::decode(encoded, PageId(2));
  PAGEDB_ASSERT_OK(decoded);
  PAGEDB_ASSERT_EQ(decoded.value().num_slots(), 2U);
  PAGEDB_ASSERT_EQ(decoded.value().aux_page_id(), PageId(7));
  PAGEDB_ASSERT_EQ(decoded.value().page_lsn(), Lsn(99));

  // Compact explicitly and verify fragmented_free_bytes becomes 0.
  PAGEDB_ASSERT_OK(decoded.value().compact());
  PAGEDB_ASSERT_EQ(decoded.value().header().fragmented_free_bytes, 0U);
  PAGEDB_ASSERT_TRUE(
      compare_bytes(decoded.value().slot_payload(0).value(), p0_large) == 0);
  PAGEDB_ASSERT_TRUE(
      compare_bytes(decoded.value().slot_payload(1).value(), p2) == 0);
}

PAGEDB_TEST(SlottedPageCompactionTriggeredAutomaticallyOnInsert) {
  SlottedPage sp;
  PAGEDB_ASSERT_OK(sp.init(PageId(4), PageType::kLeaf));

  std::vector<std::byte> chunk(500, std::byte{0x5A});
  // 8 slots * (500 + 4) = 4032 bytes out of 4048 usable bytes (16 bytes left).
  for (std::uint16_t i = 0; i < 8; ++i) {
    PAGEDB_ASSERT_OK(sp.insert_slot(i, chunk));
  }
  PAGEDB_ASSERT_EQ(sp.contiguous_free_space(), 16U);

  // Erase slot 3 -> frees 500 payload bytes (fragmented) + 4 slot bytes.
  PAGEDB_ASSERT_OK(sp.erase_slot(3));
  PAGEDB_ASSERT_EQ(sp.contiguous_free_space(), 20U);
  PAGEDB_ASSERT_EQ(sp.usable_free_space(), 520U);

  // Insert a 400-byte payload: requires compaction because contiguous is 20B.
  std::vector<std::byte> replacement(400, std::byte{0xA5});
  PAGEDB_ASSERT_OK(sp.insert_slot(3, replacement));
  PAGEDB_ASSERT_EQ(sp.header().fragmented_free_bytes, 0U);
  PAGEDB_ASSERT_EQ(sp.num_slots(), 8U);
  PAGEDB_ASSERT_TRUE(compare_bytes(sp.slot_payload(3).value(), replacement) ==
                     0);
}

PAGEDB_TEST(SlottedPageRejectsOverlappingSlotsAndSlackTampering) {
  SlottedPage sp;
  PAGEDB_ASSERT_OK(sp.init(PageId(5), PageType::kLeaf));
  PAGEDB_ASSERT_OK(sp.insert_slot(0, as_byte_span("first_slot_bytes")));
  PAGEDB_ASSERT_OK(sp.insert_slot(1, as_byte_span("second_slot_bytes")));

  PageBuffer page{};
  PAGEDB_ASSERT_OK(sp.encode(page));

  // 1. Corrupt slot 1 offset to overlap with slot 0, and fix CRC32C.
  {
    PageBuffer overlapping = page;
    const auto slot0_off = read_u16_le_at(overlapping, kPageHeaderSize).value();
    PAGEDB_ASSERT_OK(
        write_u16_le_at(overlapping, kPageHeaderSize + 4, slot0_off));
    recompute_and_write_crc(overlapping);
    PAGEDB_ASSERT_EQ(SlottedPage::decode(overlapping, PageId(5)).code(),
                     ErrorCode::kCorruption);
  }

  // 2. Tamper with a byte inside contiguous free space [lower, upper) and fix CRC.
  {
    PageBuffer dirty_slack = page;
    dirty_slack[200] = std::byte{0xFF};
    recompute_and_write_crc(dirty_slack);
    PAGEDB_ASSERT_EQ(SlottedPage::decode(dirty_slack, PageId(5)).code(),
                     ErrorCode::kCorruption);
  }
}

PAGEDB_TEST(LeafAndInternalRecordCodecAndLayoutValidation) {
  // Empty key and empty value are valid in v1.
  auto empty_rec = PageCodec::encode_leaf_record({}, {});
  PAGEDB_ASSERT_OK(empty_rec);
  auto dec_empty = PageCodec::decode_leaf_record(empty_rec.value());
  PAGEDB_ASSERT_OK(dec_empty);
  PAGEDB_ASSERT_EQ(dec_empty.value().key.size(), 0U);
  PAGEDB_ASSERT_EQ(dec_empty.value().value.size(), 0U);

  // Max-size key (256B) and value (512B) succeed; 1 byte over fails.
  std::vector<std::byte> max_key(kMaxKeySize, std::byte{0x11});
  std::vector<std::byte> max_val(kMaxValueSize, std::byte{0x22});
  PAGEDB_ASSERT_OK(PageCodec::encode_leaf_record(max_key, max_val));

  std::vector<std::byte> over_key(kMaxKeySize + 1, std::byte{0x11});
  std::vector<std::byte> over_val(kMaxValueSize + 1, std::byte{0x22});
  PAGEDB_ASSERT_EQ(PageCodec::encode_leaf_record(over_key, max_val).code(),
                   ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(PageCodec::encode_leaf_record(max_key, over_val).code(),
                   ErrorCode::kInvalidArgument);

  // Validate sorted vs unsorted Leaf page layout.
  SlottedPage leaf;
  PAGEDB_ASSERT_OK(leaf.init(PageId(2), PageType::kLeaf));
  auto r_a = PageCodec::encode_leaf_record(as_byte_span("a"), as_byte_span("1"));
  auto r_b = PageCodec::encode_leaf_record(as_byte_span("b"), as_byte_span("2"));
  PAGEDB_ASSERT_OK(leaf.insert_slot(0, r_a.value()));
  PAGEDB_ASSERT_OK(leaf.insert_slot(1, r_b.value()));

  PageBuffer leaf_bytes{};
  PAGEDB_ASSERT_OK(leaf.encode(leaf_bytes));
  PAGEDB_ASSERT_OK(PageCodec::validate_btree_page_layout(leaf_bytes, PageId(2)));

  // Out-of-order leaf keys must be rejected by validate_btree_page_layout.
  SlottedPage bad_leaf;
  PAGEDB_ASSERT_OK(bad_leaf.init(PageId(2), PageType::kLeaf));
  PAGEDB_ASSERT_OK(bad_leaf.insert_slot(0, r_b.value()));
  PAGEDB_ASSERT_OK(bad_leaf.insert_slot(1, r_a.value()));
  PAGEDB_ASSERT_OK(bad_leaf.encode(leaf_bytes));
  PAGEDB_ASSERT_EQ(
      PageCodec::validate_btree_page_layout(leaf_bytes, PageId(2)).code(),
      ErrorCode::kCorruption);
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
