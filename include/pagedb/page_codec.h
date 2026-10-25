#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

using PageBuffer = std::array<std::byte, kPageSize>;

inline constexpr std::size_t kSuperblockHeaderEnd = 112;  // 0x70
inline constexpr std::size_t kSlotEntrySize = 4;
inline constexpr std::size_t kChecksumFieldOffset = 0x24;
// Upper bound on num_slots for any decodable slotted page: the slot directory
// must fit in [kPageHeaderSize, kPageSize).
inline constexpr std::size_t kMaxSlotsPerPage =
    (kPageSize - kPageHeaderSize) / kSlotEntrySize;

struct PageHeader {
  std::uint32_t page_magic{kPageMagic};
  std::uint16_t format_major{kFormatMajorVersion};
  std::uint16_t format_minor{kFormatMinorVersion};
  PageType page_type{PageType::kInvalid};
  std::uint8_t flags{0};
  std::uint16_t num_slots{0};
  PageId page_id{kInvalidPageId};
  Lsn page_lsn{kInvalidLsn};
  std::uint16_t lower_offset{static_cast<std::uint16_t>(kPageHeaderSize)};
  std::uint16_t upper_offset{static_cast<std::uint16_t>(kPageSize)};
  std::uint32_t fragmented_free_bytes{0};
  PageId aux_page_id{kInvalidPageId};
  std::uint32_t checksum{0};
  std::uint64_t reserved{0};
};

struct Superblock {
  PageId root_page_id{kInitialRootPageId};
  PageId next_page_id{PageId(2)};
  PageId free_list_head{kInvalidPageId};
  Lsn checkpoint_lsn{kInvalidLsn};
  Lsn page_lsn{kInvalidLsn};
};

struct LeafRecordView {
  std::span<const std::byte> key;
  std::span<const std::byte> value;
};

struct InternalRecordView {
  std::span<const std::byte> separator_key;
  PageId right_child{kInvalidPageId};
};

class PageCodec {
 public:
  // Encodes the 48-byte common header into page[0..48) and computes + writes
  // the CRC32C checksum over all 4096 bytes into page[0x24..0x28).
  [[nodiscard]] static Status encode_header_and_checksum(
      const PageHeader& header, std::span<std::byte, kPageSize> page) noexcept;

  // Validates magic, format version, CRC32C (before parsing offsets), expected
  // PageId, flags, and reserved fields.
  [[nodiscard]] static Result<PageHeader> decode_and_verify_header(
      std::span<const std::byte, kPageSize> page,
      PageId expected_page_id) noexcept;

  // Superblock (Page 0) encode/decode.
  [[nodiscard]] static Status encode_superblock(
      const Superblock& sb, std::span<std::byte, kPageSize> out_page) noexcept;
  [[nodiscard]] static Result<Superblock> decode_superblock(
      std::span<const std::byte, kPageSize> page) noexcept;

  // Free page encode/decode.
  [[nodiscard]] static Status encode_free_page(
      PageId page_id, PageId next_free_page, Lsn page_lsn,
      std::span<std::byte, kPageSize> out_page) noexcept;
  [[nodiscard]] static Result<PageId> decode_free_page(
      std::span<const std::byte, kPageSize> page,
      PageId expected_page_id) noexcept;

  // Leaf and Internal record codecs.
  [[nodiscard]] static Result<std::vector<std::byte>> encode_leaf_record(
      std::span<const std::byte> key, std::span<const std::byte> value);
  [[nodiscard]] static Result<LeafRecordView> decode_leaf_record(
      std::span<const std::byte> payload) noexcept;

  [[nodiscard]] static Result<std::vector<std::byte>> encode_internal_record(
      std::span<const std::byte> separator_key, PageId right_child);
  [[nodiscard]] static Result<InternalRecordView> decode_internal_record(
      std::span<const std::byte> payload) noexcept;

  // Validates sorted unique keys and record boundaries on a raw Leaf or
  // Internal page image.
  [[nodiscard]] static Status validate_btree_page_layout(
      std::span<const std::byte, kPageSize> page,
      PageId expected_page_id) noexcept;
};

class SlottedPage {
 public:
  SlottedPage() noexcept;

  [[nodiscard]] Status init(PageId page_id, PageType page_type,
                            PageId aux_page_id = kInvalidPageId,
                            Lsn page_lsn = kInvalidLsn) noexcept;

  [[nodiscard]] static Result<SlottedPage> decode(
      std::span<const std::byte, kPageSize> page_bytes,
      PageId expected_page_id) noexcept;

  [[nodiscard]] Status encode(
      std::span<std::byte, kPageSize> out_page) const noexcept;

  [[nodiscard]] const PageHeader& header() const noexcept { return header_; }
  [[nodiscard]] PageId page_id() const noexcept { return header_.page_id; }
  [[nodiscard]] PageType page_type() const noexcept {
    return header_.page_type;
  }
  [[nodiscard]] std::uint16_t num_slots() const noexcept {
    return header_.num_slots;
  }
  [[nodiscard]] PageId aux_page_id() const noexcept {
    return header_.aux_page_id;
  }
  void set_aux_page_id(PageId id) noexcept { header_.aux_page_id = id; }
  [[nodiscard]] Lsn page_lsn() const noexcept { return header_.page_lsn; }
  void set_page_lsn(Lsn lsn) noexcept { header_.page_lsn = lsn; }

  [[nodiscard]] std::size_t contiguous_free_space() const noexcept {
    return static_cast<std::size_t>(header_.upper_offset -
                                    header_.lower_offset);
  }
  [[nodiscard]] std::size_t usable_free_space() const noexcept {
    return contiguous_free_space() +
           static_cast<std::size_t>(header_.fragmented_free_bytes);
  }

  [[nodiscard]] Result<std::span<const std::byte>> slot_payload(
      std::uint16_t slot_index) const noexcept;

  [[nodiscard]] Status insert_slot(std::uint16_t slot_index,
                                   std::span<const std::byte> payload);
  [[nodiscard]] Status update_slot(std::uint16_t slot_index,
                                   std::span<const std::byte> new_payload);
  [[nodiscard]] Status erase_slot(std::uint16_t slot_index) noexcept;
  [[nodiscard]] Status compact();

 private:
  struct SlotEntry {
    std::uint16_t offset{0};
    std::uint16_t length{0};
  };

  [[nodiscard]] SlotEntry read_slot_unchecked(
      std::uint16_t slot_index) const noexcept;
  void write_slot_unchecked(std::uint16_t slot_index, SlotEntry entry) noexcept;

  PageHeader header_{};
  PageBuffer data_{};
};

}  // namespace pagedb
