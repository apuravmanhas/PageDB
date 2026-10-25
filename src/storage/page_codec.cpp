#include "pagedb/page_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include "pagedb/byte_codec.h"
#include "pagedb/crc32c.h"

namespace pagedb {

Status PageCodec::encode_header_and_checksum(
    const PageHeader& header, std::span<std::byte, kPageSize> page) noexcept {
  if (!header.page_id.is_valid()) {
    return Status::InvalidArgument("cannot encode page with invalid PageId");
  }
  if (header.page_type != PageType::kSuperblock &&
      header.page_type != PageType::kLeaf &&
      header.page_type != PageType::kInternal &&
      header.page_type != PageType::kFree) {
    return Status::InvalidArgument("invalid PageType in header");
  }
  if (header.flags != 0 || header.reserved != 0) {
    return Status::InvalidArgument("non-zero flags or reserved field in v1");
  }
  if (header.lower_offset < kPageHeaderSize ||
      header.upper_offset < header.lower_offset ||
      header.upper_offset > kPageSize) {
    return Status::InvalidArgument("invalid lower/upper offsets in header");
  }

  ByteWriter w(page.subspan(0, kPageHeaderSize));
  (void)w.write_u32_le(header.page_magic);
  (void)w.write_u16_le(header.format_major);
  (void)w.write_u16_le(header.format_minor);
  (void)w.write_u8(static_cast<std::uint8_t>(header.page_type));
  (void)w.write_u8(header.flags);
  (void)w.write_u16_le(header.num_slots);
  (void)w.write_u32_le(header.page_id.value);
  (void)w.write_u64_le(header.page_lsn.value);
  (void)w.write_u16_le(header.lower_offset);
  (void)w.write_u16_le(header.upper_offset);
  (void)w.write_u32_le(header.fragmented_free_bytes);
  (void)w.write_u32_le(header.aux_page_id.value);
  (void)w.write_u32_le(0U);  // Zero checksum field before computing CRC32C
  (void)w.write_u64_le(header.reserved);

  const std::uint32_t crc = compute_page_crc32c(page, kChecksumFieldOffset);
  return write_u32_le_at(page, kChecksumFieldOffset, crc);
}

Result<PageHeader> PageCodec::decode_and_verify_header(
    std::span<const std::byte, kPageSize> page,
    PageId expected_page_id) noexcept {
  ByteReader r(page.subspan(0, kPageHeaderSize));
  const auto magic = r.read_u32_le().value();
  if (magic != kPageMagic) {
    return Status::Corruption("page magic mismatch");
  }

  const auto major = r.read_u16_le().value();
  const auto minor = r.read_u16_le().value();
  if (major != kFormatMajorVersion || minor != kFormatMinorVersion) {
    return Status::UnsupportedFormatVersion("unsupported page format version");
  }

  // Verify CRC32C over the entire 4096-byte page before trusting any offsets.
  const auto stored_crc = read_u32_le_at(page, kChecksumFieldOffset).value();
  const std::uint32_t computed_crc =
      compute_page_crc32c(page, kChecksumFieldOffset);
  if (stored_crc != computed_crc) {
    return Status::Corruption("page CRC32C checksum mismatch");
  }

  const auto raw_type = r.read_u8().value();
  const auto flags = r.read_u8().value();
  const auto num_slots = r.read_u16_le().value();
  const PageId page_id(r.read_u32_le().value());
  const Lsn page_lsn(r.read_u64_le().value());
  const auto lower_offset = r.read_u16_le().value();
  const auto upper_offset = r.read_u16_le().value();
  const auto fragmented_free_bytes = r.read_u32_le().value();
  const PageId aux_page_id(r.read_u32_le().value());
  (void)r.read_u32_le();  // checksum already read
  const auto reserved = r.read_u64_le().value();

  if (!page_id.is_valid()) {
    return Status::Corruption("decoded page has invalid PageId sentinel");
  }
  if (expected_page_id.is_valid() && page_id != expected_page_id) {
    return Status::Corruption("decoded PageId does not match expected PageId");
  }
  if (flags != 0 || reserved != 0) {
    return Status::Corruption("non-zero page header flags or reserved bytes");
  }

  PageType page_type = PageType::kInvalid;
  switch (raw_type) {
    case 1:
      page_type = PageType::kSuperblock;
      break;
    case 2:
      page_type = PageType::kLeaf;
      break;
    case 3:
      page_type = PageType::kInternal;
      break;
    case 4:
      page_type = PageType::kFree;
      break;
    default:
      return Status::Corruption("unknown PageType in page header");
  }

  if (lower_offset < kPageHeaderSize || upper_offset < lower_offset ||
      upper_offset > kPageSize) {
    return Status::Corruption("page header lower/upper offsets out of bounds");
  }

  PageHeader hdr{};
  hdr.page_magic = magic;
  hdr.format_major = major;
  hdr.format_minor = minor;
  hdr.page_type = page_type;
  hdr.flags = flags;
  hdr.num_slots = num_slots;
  hdr.page_id = page_id;
  hdr.page_lsn = page_lsn;
  hdr.lower_offset = lower_offset;
  hdr.upper_offset = upper_offset;
  hdr.fragmented_free_bytes = fragmented_free_bytes;
  hdr.aux_page_id = aux_page_id;
  hdr.checksum = stored_crc;
  hdr.reserved = reserved;
  return Result<PageHeader>::Ok(hdr);
}

Status PageCodec::encode_superblock(
    const Superblock& sb, std::span<std::byte, kPageSize> out_page) noexcept {
  if (!sb.root_page_id.is_valid() || sb.root_page_id == kSuperblockPageId) {
    return Status::InvalidArgument("invalid root_page_id in Superblock");
  }
  if (!sb.next_page_id.is_valid() || sb.next_page_id.value < 2 ||
      sb.root_page_id.value >= sb.next_page_id.value) {
    return Status::InvalidArgument("invalid next_page_id in Superblock");
  }
  if (sb.free_list_head.is_valid()) {
    if (sb.free_list_head == kSuperblockPageId ||
        sb.free_list_head == sb.root_page_id ||
        sb.free_list_head.value >= sb.next_page_id.value) {
      return Status::InvalidArgument("invalid free_list_head in Superblock");
    }
  }

  std::memset(out_page.data(), 0, kPageSize);

  ByteWriter w(out_page.subspan(kPageHeaderSize,
                                kSuperblockHeaderEnd - kPageHeaderSize));
  (void)w.write_u64_le(kSuperblockMagic);
  (void)w.write_u32_le(static_cast<std::uint32_t>(kPageSize));
  (void)w.write_u32_le(sb.root_page_id.value);
  (void)w.write_u32_le(sb.next_page_id.value);
  (void)w.write_u32_le(sb.free_list_head.value);
  (void)w.write_u64_le(sb.checkpoint_lsn.value);
  (void)w.write_u32_le(static_cast<std::uint32_t>(kMaxKeySize));
  (void)w.write_u32_le(static_cast<std::uint32_t>(kMaxValueSize));
  (void)w.write_zeros(24);

  PageHeader hdr{};
  hdr.page_type = PageType::kSuperblock;
  hdr.num_slots = 0;
  hdr.page_id = kSuperblockPageId;
  hdr.page_lsn = sb.page_lsn;
  hdr.lower_offset = static_cast<std::uint16_t>(kSuperblockHeaderEnd);
  hdr.upper_offset = static_cast<std::uint16_t>(kPageSize);
  hdr.fragmented_free_bytes = 0;
  hdr.aux_page_id = kInvalidPageId;

  return encode_header_and_checksum(hdr, out_page);
}

Result<Superblock> PageCodec::decode_superblock(
    std::span<const std::byte, kPageSize> page) noexcept {
  auto hdr_res = decode_and_verify_header(page, kSuperblockPageId);
  if (!hdr_res.ok()) {
    return hdr_res.status();
  }
  const PageHeader& hdr = hdr_res.value();
  if (hdr.page_type != PageType::kSuperblock) {
    return Status::Corruption("Page 0 is not a Superblock page");
  }
  if (hdr.num_slots != 0 || hdr.lower_offset != kSuperblockHeaderEnd ||
      hdr.upper_offset != kPageSize || hdr.fragmented_free_bytes != 0 ||
      hdr.aux_page_id != kInvalidPageId) {
    return Status::Corruption("invalid Superblock header fields");
  }

  ByteReader r(page.subspan(kPageHeaderSize,
                            kSuperblockHeaderEnd - kPageHeaderSize));
  const auto sb_magic = r.read_u64_le().value();
  if (sb_magic != kSuperblockMagic) {
    return Status::Corruption("Superblock magic constant mismatch");
  }
  const auto page_size = r.read_u32_le().value();
  if (page_size != kPageSize) {
    return Status::Corruption("Superblock page_size mismatch");
  }

  const PageId root_page_id(r.read_u32_le().value());
  const PageId next_page_id(r.read_u32_le().value());
  const PageId free_list_head(r.read_u32_le().value());
  const Lsn checkpoint_lsn(r.read_u64_le().value());
  const auto max_key_size = r.read_u32_le().value();
  const auto max_value_size = r.read_u32_le().value();
  const auto sb_reserved = r.read_bytes(24).value();

  if (max_key_size != kMaxKeySize || max_value_size != kMaxValueSize) {
    return Status::Corruption("Superblock key/value size limits mismatch");
  }
  for (const std::byte b : sb_reserved) {
    if (b != std::byte{0}) {
      return Status::Corruption("non-zero reserved bytes in Superblock");
    }
  }
  for (std::size_t i = kSuperblockHeaderEnd; i < kPageSize; ++i) {
    if (page[i] != std::byte{0}) {
      return Status::Corruption("non-zero trailing slack bytes in Superblock");
    }
  }

  if (!root_page_id.is_valid() || root_page_id == kSuperblockPageId) {
    return Status::Corruption("Superblock root_page_id is invalid");
  }
  if (!next_page_id.is_valid() || next_page_id.value < 2 ||
      root_page_id.value >= next_page_id.value) {
    return Status::Corruption("Superblock next_page_id is out of bounds");
  }
  if (free_list_head.is_valid()) {
    if (free_list_head == kSuperblockPageId ||
        free_list_head == root_page_id ||
        free_list_head.value >= next_page_id.value) {
      return Status::Corruption("Superblock free_list_head is invalid");
    }
  }

  Superblock sb{};
  sb.root_page_id = root_page_id;
  sb.next_page_id = next_page_id;
  sb.free_list_head = free_list_head;
  sb.checkpoint_lsn = checkpoint_lsn;
  sb.page_lsn = hdr.page_lsn;
  return Result<Superblock>::Ok(sb);
}

Status PageCodec::encode_free_page(
    PageId page_id, PageId next_free_page, Lsn page_lsn,
    std::span<std::byte, kPageSize> out_page) noexcept {
  if (!page_id.is_valid() || page_id == kSuperblockPageId) {
    return Status::InvalidArgument("invalid PageId for FreePage");
  }
  if (next_free_page == kSuperblockPageId || next_free_page == page_id) {
    return Status::InvalidArgument("invalid next_free_page pointer");
  }

  std::memset(out_page.data(), 0, kPageSize);
  PageHeader hdr{};
  hdr.page_type = PageType::kFree;
  hdr.num_slots = 0;
  hdr.page_id = page_id;
  hdr.page_lsn = page_lsn;
  hdr.lower_offset = static_cast<std::uint16_t>(kPageHeaderSize);
  hdr.upper_offset = static_cast<std::uint16_t>(kPageSize);
  hdr.fragmented_free_bytes = 0;
  hdr.aux_page_id = next_free_page;
  return encode_header_and_checksum(hdr, out_page);
}

Result<PageId> PageCodec::decode_free_page(
    std::span<const std::byte, kPageSize> page,
    PageId expected_page_id) noexcept {
  auto hdr_res = decode_and_verify_header(page, expected_page_id);
  if (!hdr_res.ok()) {
    return hdr_res.status();
  }
  const PageHeader& hdr = hdr_res.value();
  if (hdr.page_type != PageType::kFree) {
    return Status::Corruption("expected FreePage type");
  }
  if (hdr.num_slots != 0 || hdr.lower_offset != kPageHeaderSize ||
      hdr.upper_offset != kPageSize || hdr.fragmented_free_bytes != 0) {
    return Status::Corruption("invalid FreePage header offsets");
  }
  if (hdr.aux_page_id == kSuperblockPageId || hdr.aux_page_id == hdr.page_id) {
    return Status::Corruption("invalid FreePage next pointer");
  }
  for (std::size_t i = kPageHeaderSize; i < kPageSize; ++i) {
    if (page[i] != std::byte{0}) {
      return Status::Corruption("non-zero payload bytes in FreePage");
    }
  }
  return Result<PageId>::Ok(hdr.aux_page_id);
}

Result<std::vector<std::byte>> PageCodec::encode_leaf_record(
    std::span<const std::byte> key, std::span<const std::byte> value) {
  if (key.size() > kMaxKeySize) {
    return Status::InvalidArgument("key length exceeds kMaxKeySize");
  }
  if (value.size() > kMaxValueSize) {
    return Status::InvalidArgument("value length exceeds kMaxValueSize");
  }
  std::vector<std::byte> buf(4 + key.size() + value.size());
  ByteWriter w(buf);
  (void)w.write_u16_le(static_cast<std::uint16_t>(key.size()));
  (void)w.write_u16_le(static_cast<std::uint16_t>(value.size()));
  (void)w.write_bytes(key);
  (void)w.write_bytes(value);
  return Result<std::vector<std::byte>>::Ok(std::move(buf));
}

Result<LeafRecordView> PageCodec::decode_leaf_record(
    std::span<const std::byte> payload) noexcept {
  ByteReader r(payload);
  auto key_len_res = r.read_u16_le();
  if (!key_len_res.ok()) {
    return key_len_res.status();
  }
  auto val_len_res = r.read_u16_le();
  if (!val_len_res.ok()) {
    return val_len_res.status();
  }
  const std::uint16_t key_len = key_len_res.value();
  const std::uint16_t val_len = val_len_res.value();
  if (key_len > kMaxKeySize || val_len > kMaxValueSize) {
    return Status::Corruption("leaf record key/value length exceeds v1 limit");
  }
  if (r.remaining() != static_cast<std::size_t>(key_len) +
                           static_cast<std::size_t>(val_len)) {
    return Status::Corruption("leaf record length mismatch with slot payload");
  }
  auto key_bytes = r.read_bytes(key_len).value();
  auto val_bytes = r.read_bytes(val_len).value();
  return Result<LeafRecordView>::Ok(
      LeafRecordView{.key = key_bytes, .value = val_bytes});
}

Result<std::vector<std::byte>> PageCodec::encode_internal_record(
    std::span<const std::byte> separator_key, PageId right_child) {
  if (separator_key.size() > kMaxKeySize) {
    return Status::InvalidArgument("separator key length exceeds kMaxKeySize");
  }
  if (!right_child.is_valid() || right_child == kSuperblockPageId) {
    return Status::InvalidArgument("invalid right_child PageId");
  }
  std::vector<std::byte> buf(6 + separator_key.size());
  ByteWriter w(buf);
  (void)w.write_u16_le(static_cast<std::uint16_t>(separator_key.size()));
  (void)w.write_u32_le(right_child.value);
  (void)w.write_bytes(separator_key);
  return Result<std::vector<std::byte>>::Ok(std::move(buf));
}

Result<InternalRecordView> PageCodec::decode_internal_record(
    std::span<const std::byte> payload) noexcept {
  ByteReader r(payload);
  auto key_len_res = r.read_u16_le();
  if (!key_len_res.ok()) {
    return key_len_res.status();
  }
  auto child_res = r.read_u32_le();
  if (!child_res.ok()) {
    return child_res.status();
  }
  const std::uint16_t key_len = key_len_res.value();
  const PageId right_child(child_res.value());
  if (key_len > kMaxKeySize) {
    return Status::Corruption("internal record separator key exceeds kMaxKeySize");
  }
  if (!right_child.is_valid() || right_child == kSuperblockPageId) {
    return Status::Corruption("internal record has invalid right_child PageId");
  }
  if (r.remaining() != key_len) {
    return Status::Corruption("internal record length mismatch with slot payload");
  }
  auto sep_bytes = r.read_bytes(key_len).value();
  return Result<InternalRecordView>::Ok(InternalRecordView{
      .separator_key = sep_bytes, .right_child = right_child});
}

Status PageCodec::validate_btree_page_layout(
    std::span<const std::byte, kPageSize> page,
    PageId expected_page_id) noexcept {
  auto sp_res = SlottedPage::decode(page, expected_page_id);
  if (!sp_res.ok()) {
    return sp_res.status();
  }
  const SlottedPage& sp = sp_res.value();
  if (sp.page_id() == kSuperblockPageId) {
    return Status::Corruption("B+ tree page cannot reside at PageId(0)");
  }

  if (sp.page_type() == PageType::kLeaf) {
    if (sp.aux_page_id() == kSuperblockPageId ||
        sp.aux_page_id() == sp.page_id()) {
      return Status::Corruption("leaf next_leaf_page_id is invalid");
    }
    std::span<const std::byte> prev_key{};
    for (std::uint16_t i = 0; i < sp.num_slots(); ++i) {
      auto rec_res = decode_leaf_record(sp.slot_payload(i).value());
      if (!rec_res.ok()) {
        return rec_res.status();
      }
      if (i > 0 && compare_bytes(prev_key, rec_res.value().key) >= 0) {
        return Status::Corruption("leaf keys are not strictly sorted and unique");
      }
      prev_key = rec_res.value().key;
    }
    return Status::Ok();
  }

  if (sp.page_type() == PageType::kInternal) {
    if (!sp.aux_page_id().is_valid() ||
        sp.aux_page_id() == kSuperblockPageId ||
        sp.aux_page_id() == sp.page_id()) {
      return Status::Corruption("internal page leftmost_child_page_id is invalid");
    }
    // A non-root internal page with zero slots (a single child) is legal: the
    // delete policy in docs/BTREE_DESIGN.md never splits on delete, so an
    // internal node whose merge does not fit may be left unary. Routing stays
    // correct because every key goes to the leftmost child.
    std::array<std::uint32_t, kMaxSlotsPerPage + 1> children{};
    std::size_t child_count = 0;
    children[child_count++] = sp.aux_page_id().value;

    std::span<const std::byte> prev_key{};
    for (std::uint16_t i = 0; i < sp.num_slots(); ++i) {
      auto rec_res = decode_internal_record(sp.slot_payload(i).value());
      if (!rec_res.ok()) {
        return rec_res.status();
      }
      if (rec_res.value().right_child == sp.page_id()) {
        return Status::Corruption("internal page references itself as a child");
      }
      if (i > 0 &&
          compare_bytes(prev_key, rec_res.value().separator_key) >= 0) {
        return Status::Corruption(
            "internal separator keys are not strictly sorted");
      }
      prev_key = rec_res.value().separator_key;
      children[child_count++] = rec_res.value().right_child.value;
    }

    const auto children_end =
        children.begin() + static_cast<std::ptrdiff_t>(child_count);
    std::sort(children.begin(), children_end);
    if (std::adjacent_find(children.begin(), children_end) != children_end) {
      return Status::Corruption(
          "internal page references the same child more than once");
    }
    return Status::Ok();
  }

  return Status::Corruption("validate_btree_page_layout called on non-B+tree page");
}

SlottedPage::SlottedPage() noexcept { data_.fill(std::byte{0}); }

Status SlottedPage::init(PageId page_id, PageType page_type, PageId aux_page_id,
                         Lsn page_lsn) noexcept {
  if (!page_id.is_valid() || page_id == kSuperblockPageId) {
    return Status::InvalidArgument("SlottedPage requires valid non-zero PageId");
  }
  if (page_type != PageType::kLeaf && page_type != PageType::kInternal) {
    return Status::InvalidArgument(
        "SlottedPage only supports kLeaf or kInternal PageType");
  }

  data_.fill(std::byte{0});
  header_ = PageHeader{};
  header_.page_type = page_type;
  header_.num_slots = 0;
  header_.page_id = page_id;
  header_.page_lsn = page_lsn;
  header_.lower_offset = static_cast<std::uint16_t>(kPageHeaderSize);
  header_.upper_offset = static_cast<std::uint16_t>(kPageSize);
  header_.fragmented_free_bytes = 0;
  header_.aux_page_id = aux_page_id;
  return Status::Ok();
}

SlottedPage::SlotEntry SlottedPage::read_slot_unchecked(
    std::uint16_t slot_index) const noexcept {
  const std::size_t pos =
      kPageHeaderSize + static_cast<std::size_t>(slot_index) * kSlotEntrySize;
  const std::uint16_t off = read_u16_le_at(data_, pos).value();
  const std::uint16_t len = read_u16_le_at(data_, pos + 2).value();
  return SlotEntry{.offset = off, .length = len};
}

void SlottedPage::write_slot_unchecked(std::uint16_t slot_index,
                                       SlotEntry entry) noexcept {
  const std::size_t pos =
      kPageHeaderSize + static_cast<std::size_t>(slot_index) * kSlotEntrySize;
  (void)write_u16_le_at(data_, pos, entry.offset);
  (void)write_u16_le_at(data_, pos + 2, entry.length);
}

Result<SlottedPage> SlottedPage::decode(
    std::span<const std::byte, kPageSize> page_bytes,
    PageId expected_page_id) noexcept {
  auto hdr_res = PageCodec::decode_and_verify_header(page_bytes, expected_page_id);
  if (!hdr_res.ok()) {
    return hdr_res.status();
  }
  const PageHeader& hdr = hdr_res.value();
  if (hdr.page_type != PageType::kLeaf &&
      hdr.page_type != PageType::kInternal) {
    return Status::Corruption("SlottedPage::decode requires Leaf or Internal page");
  }

  const std::uint32_t expected_lower =
      static_cast<std::uint32_t>(kPageHeaderSize) +
      static_cast<std::uint32_t>(hdr.num_slots) *
          static_cast<std::uint32_t>(kSlotEntrySize);
  if (expected_lower != hdr.lower_offset) {
    return Status::Corruption("slotted page lower_offset does not match num_slots");
  }

  // Verify zeroed contiguous free space [lower_offset, upper_offset).
  for (std::size_t i = hdr.lower_offset; i < hdr.upper_offset; ++i) {
    if (page_bytes[i] != std::byte{0}) {
      return Status::Corruption(
          "non-zero byte found in slotted page contiguous free space");
    }
  }

  SlottedPage sp;
  sp.header_ = hdr;
  std::memcpy(sp.data_.data(), page_bytes.data(), kPageSize);

  // Validate all slots and check pairwise non-overlap and byte conservation.
  // num_slots <= kMaxSlotsPerPage is implied by lower_offset <= kPageSize, so
  // a fixed stack array suffices and this noexcept decoder never allocates.
  std::array<std::pair<std::uint16_t, std::uint16_t>, kMaxSlotsPerPage>
      intervals{};
  std::size_t interval_count = 0;
  std::uint64_t active_payload_bytes = 0;

  for (std::uint16_t i = 0; i < hdr.num_slots; ++i) {
    const SlotEntry slot = sp.read_slot_unchecked(i);
    if (slot.length == 0) {
      return Status::Corruption("slotted page contains zero-length slot");
    }
    if (slot.offset < hdr.upper_offset) {
      return Status::Corruption("slot offset lies below upper_offset");
    }
    auto slot_end = checked_add_u16(slot.offset, slot.length);
    if (!slot_end.ok() || slot_end.value() > kPageSize) {
      return Status::Corruption("slot extent exceeds page boundary");
    }
    active_payload_bytes += slot.length;
    intervals[interval_count++] = {slot.offset, slot_end.value()};
  }

  const auto intervals_end =
      intervals.begin() + static_cast<std::ptrdiff_t>(interval_count);
  std::sort(intervals.begin(), intervals_end);
  for (std::size_t i = 1; i < interval_count; ++i) {
    if (intervals[i - 1].second > intervals[i].first) {
      return Status::Corruption("overlapping slot payload regions detected");
    }
  }

  const std::uint64_t payload_region_bytes =
      static_cast<std::uint64_t>(kPageSize - hdr.upper_offset);
  if (active_payload_bytes +
          static_cast<std::uint64_t>(hdr.fragmented_free_bytes) !=
      payload_region_bytes) {
    return Status::Corruption(
        "slotted page active payload + fragmented_free_bytes mismatch");
  }

  return Result<SlottedPage>::Ok(sp);
}

Status SlottedPage::encode(
    std::span<std::byte, kPageSize> out_page) const noexcept {
  std::memcpy(out_page.data(), data_.data(), kPageSize);
  return PageCodec::encode_header_and_checksum(header_, out_page);
}

Result<std::span<const std::byte>> SlottedPage::slot_payload(
    std::uint16_t slot_index) const noexcept {
  if (slot_index >= header_.num_slots) {
    return Status::InvalidArgument("slot_index out of range");
  }
  const SlotEntry slot = read_slot_unchecked(slot_index);
  return Result<std::span<const std::byte>>::Ok(
      std::span<const std::byte>(data_).subspan(slot.offset, slot.length));
}

Status SlottedPage::insert_slot(std::uint16_t slot_index,
                                std::span<const std::byte> payload) {
  if (slot_index > header_.num_slots) {
    return Status::InvalidArgument("insert_slot index out of range");
  }
  if (payload.empty() || payload.size() > kPageSize - kPageHeaderSize) {
    return Status::InvalidArgument("invalid slot payload length");
  }

  const std::size_t needed = payload.size() + kSlotEntrySize;
  if (usable_free_space() < needed) {
    return Status::ResourceExhausted("insufficient free space in slotted page");
  }
  if (contiguous_free_space() < needed) {
    Status s = compact();
    if (!s.ok()) {
      return s;
    }
  }

  // Shift slots [slot_index .. num_slots - 1] right by one slot entry.
  for (std::uint16_t i = header_.num_slots; i > slot_index; --i) {
    write_slot_unchecked(i, read_slot_unchecked(i - 1));
  }

  const auto new_upper =
      static_cast<std::uint16_t>(header_.upper_offset - payload.size());
  std::memcpy(data_.data() + new_upper, payload.data(), payload.size());

  write_slot_unchecked(
      slot_index,
      SlotEntry{.offset = new_upper,
                .length = static_cast<std::uint16_t>(payload.size())});

  header_.num_slots = static_cast<std::uint16_t>(header_.num_slots + 1);
  header_.lower_offset =
      static_cast<std::uint16_t>(header_.lower_offset + kSlotEntrySize);
  header_.upper_offset = new_upper;
  return Status::Ok();
}

Status SlottedPage::update_slot(std::uint16_t slot_index,
                                std::span<const std::byte> new_payload) {
  if (slot_index >= header_.num_slots) {
    return Status::InvalidArgument("update_slot index out of range");
  }
  if (new_payload.empty() || new_payload.size() > kPageSize - kPageHeaderSize) {
    return Status::InvalidArgument("invalid new_payload length");
  }

  const SlotEntry old_slot = read_slot_unchecked(slot_index);
  const auto new_len = static_cast<std::uint16_t>(new_payload.size());

  if (new_len <= old_slot.length) {
    // Reuse existing payload slice in-place and zero-scrub any freed suffix.
    std::memcpy(data_.data() + old_slot.offset, new_payload.data(), new_len);
    const std::uint16_t diff =
        static_cast<std::uint16_t>(old_slot.length - new_len);
    if (diff > 0) {
      std::memset(data_.data() + old_slot.offset + new_len, 0, diff);
      header_.fragmented_free_bytes += diff;
    }
    write_slot_unchecked(
        slot_index, SlotEntry{.offset = old_slot.offset, .length = new_len});
    return Status::Ok();
  }

  const std::size_t extra_needed =
      static_cast<std::size_t>(new_len - old_slot.length);
  if (usable_free_space() < extra_needed) {
    return Status::ResourceExhausted(
        "insufficient space to update slot in slotted page");
  }

  if (contiguous_free_space() >= new_len) {
    // Scrub old payload bytes and allocate from contiguous free space.
    std::memset(data_.data() + old_slot.offset, 0, old_slot.length);
    header_.fragmented_free_bytes += old_slot.length;

    const auto new_upper =
        static_cast<std::uint16_t>(header_.upper_offset - new_len);
    std::memcpy(data_.data() + new_upper, new_payload.data(), new_len);
    header_.upper_offset = new_upper;
    write_slot_unchecked(slot_index,
                         SlotEntry{.offset = new_upper, .length = new_len});
    return Status::Ok();
  }

  // Reclaim old_slot first, compact active payloads, then place new_payload.
  std::memset(data_.data() + old_slot.offset, 0, old_slot.length);
  header_.fragmented_free_bytes += old_slot.length;
  write_slot_unchecked(slot_index,
                       SlotEntry{.offset = header_.upper_offset, .length = 0});

  // Compact all slots except the temporarily 0-length slot_index.
  PageBuffer scratch{};
  std::uint16_t write_cursor = static_cast<std::uint16_t>(kPageSize);
  for (std::uint16_t i = 0; i < header_.num_slots; ++i) {
    if (i == slot_index) {
      continue;
    }
    const SlotEntry s = read_slot_unchecked(i);
    write_cursor = static_cast<std::uint16_t>(write_cursor - s.length);
    std::memcpy(scratch.data() + write_cursor, data_.data() + s.offset,
                s.length);
    write_slot_unchecked(i,
                         SlotEntry{.offset = write_cursor, .length = s.length});
  }

  // Now place new_payload at write_cursor - new_len.
  write_cursor = static_cast<std::uint16_t>(write_cursor - new_len);
  std::memcpy(scratch.data() + write_cursor, new_payload.data(), new_len);
  write_slot_unchecked(slot_index,
                       SlotEntry{.offset = write_cursor, .length = new_len});

  // Copy compacted payload region back and zero-scrub free space.
  std::memset(data_.data() + header_.lower_offset, 0,
              kPageSize - header_.lower_offset);
  std::memcpy(data_.data() + write_cursor, scratch.data() + write_cursor,
              kPageSize - write_cursor);
  header_.upper_offset = write_cursor;
  header_.fragmented_free_bytes = 0;
  return Status::Ok();
}

Status SlottedPage::erase_slot(std::uint16_t slot_index) noexcept {
  if (slot_index >= header_.num_slots) {
    return Status::InvalidArgument("erase_slot index out of range");
  }
  const SlotEntry erased = read_slot_unchecked(slot_index);

  // Zero-scrub the erased payload bytes on disk/memory.
  std::memset(data_.data() + erased.offset, 0, erased.length);
  header_.fragmented_free_bytes += erased.length;

  for (std::uint16_t i = slot_index; i + 1 < header_.num_slots; ++i) {
    write_slot_unchecked(i, read_slot_unchecked(i + 1));
  }

  header_.num_slots = static_cast<std::uint16_t>(header_.num_slots - 1);
  header_.lower_offset =
      static_cast<std::uint16_t>(header_.lower_offset - kSlotEntrySize);
  // Zero the vacated slot entry at the new lower_offset.
  std::memset(data_.data() + header_.lower_offset, 0, kSlotEntrySize);

  // If the page is now completely empty, reset upper_offset to kPageSize.
  if (header_.num_slots == 0) {
    header_.upper_offset = static_cast<std::uint16_t>(kPageSize);
    header_.fragmented_free_bytes = 0;
  }
  return Status::Ok();
}

Status SlottedPage::compact() {
  if (header_.fragmented_free_bytes == 0) {
    return Status::Ok();
  }

  PageBuffer scratch{};
  std::uint16_t write_cursor = static_cast<std::uint16_t>(kPageSize);
  for (std::uint16_t i = 0; i < header_.num_slots; ++i) {
    const SlotEntry s = read_slot_unchecked(i);
    write_cursor = static_cast<std::uint16_t>(write_cursor - s.length);
    std::memcpy(scratch.data() + write_cursor, data_.data() + s.offset,
                s.length);
    write_slot_unchecked(i,
                         SlotEntry{.offset = write_cursor, .length = s.length});
  }

  std::memset(data_.data() + header_.lower_offset, 0,
              kPageSize - header_.lower_offset);
  std::memcpy(data_.data() + write_cursor, scratch.data() + write_cursor,
              kPageSize - write_cursor);
  header_.upper_offset = write_cursor;
  header_.fragmented_free_bytes = 0;
  return Status::Ok();
}

}  // namespace pagedb
