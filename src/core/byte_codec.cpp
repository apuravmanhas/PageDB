#include "pagedb/byte_codec.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace pagedb {

Result<std::uint16_t> checked_add_u16(std::uint16_t a,
                                      std::uint16_t b) noexcept {
  if (static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b) >
      std::numeric_limits<std::uint16_t>::max()) {
    return Status::Corruption("u16 addition overflow");
  }
  return Result<std::uint16_t>::Ok(static_cast<std::uint16_t>(a + b));
}

Result<std::uint32_t> checked_add_u32(std::uint32_t a,
                                      std::uint32_t b) noexcept {
  if (a > std::numeric_limits<std::uint32_t>::max() - b) {
    return Status::Corruption("u32 addition overflow");
  }
  return Result<std::uint32_t>::Ok(a + b);
}

Result<std::uint64_t> checked_add_u64(std::uint64_t a,
                                      std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return Status::Corruption("u64 addition overflow");
  }
  return Result<std::uint64_t>::Ok(a + b);
}

Result<std::uint64_t> checked_mul_u64(std::uint64_t a,
                                      std::uint64_t b) noexcept {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return Status::Corruption("u64 multiplication overflow");
  }
  return Result<std::uint64_t>::Ok(a * b);
}

Result<std::uint64_t> checked_page_offset(PageId page_id,
                                          std::size_t page_size) noexcept {
  if (!page_id.is_valid()) {
    return Status::InvalidArgument("invalid PageId sentinel");
  }
  if (page_size == 0) {
    return Status::InvalidArgument("page_size must be non-zero");
  }
  return checked_mul_u64(static_cast<std::uint64_t>(page_id.value),
                         static_cast<std::uint64_t>(page_size));
}

Status write_u8_at(std::span<std::byte> buf, std::size_t offset,
                   std::uint8_t val) noexcept {
  if (offset >= buf.size()) {
    return Status::InvalidArgument("write_u8_at out of bounds");
  }
  buf[offset] = static_cast<std::byte>(val);
  return Status::Ok();
}

Status write_u16_le_at(std::span<std::byte> buf, std::size_t offset,
                       std::uint16_t val) noexcept {
  if (offset > buf.size() || buf.size() - offset < 2) {
    return Status::InvalidArgument("write_u16_le_at out of bounds");
  }
  buf[offset] = static_cast<std::byte>(val & 0xFFU);
  buf[offset + 1] = static_cast<std::byte>((val >> 8U) & 0xFFU);
  return Status::Ok();
}

Status write_u32_le_at(std::span<std::byte> buf, std::size_t offset,
                       std::uint32_t val) noexcept {
  if (offset > buf.size() || buf.size() - offset < 4) {
    return Status::InvalidArgument("write_u32_le_at out of bounds");
  }
  buf[offset] = static_cast<std::byte>(val & 0xFFU);
  buf[offset + 1] = static_cast<std::byte>((val >> 8U) & 0xFFU);
  buf[offset + 2] = static_cast<std::byte>((val >> 16U) & 0xFFU);
  buf[offset + 3] = static_cast<std::byte>((val >> 24U) & 0xFFU);
  return Status::Ok();
}

Status write_u64_le_at(std::span<std::byte> buf, std::size_t offset,
                       std::uint64_t val) noexcept {
  if (offset > buf.size() || buf.size() - offset < 8) {
    return Status::InvalidArgument("write_u64_le_at out of bounds");
  }
  for (std::size_t i = 0; i < 8; ++i) {
    buf[offset + i] = static_cast<std::byte>((val >> (i * 8U)) & 0xFFULL);
  }
  return Status::Ok();
}

Result<std::uint8_t> read_u8_at(std::span<const std::byte> buf,
                                std::size_t offset) noexcept {
  if (offset >= buf.size()) {
    return Status::Corruption("read_u8_at out of bounds");
  }
  return Result<std::uint8_t>::Ok(std::to_integer<std::uint8_t>(buf[offset]));
}

Result<std::uint16_t> read_u16_le_at(std::span<const std::byte> buf,
                                     std::size_t offset) noexcept {
  if (offset > buf.size() || buf.size() - offset < 2) {
    return Status::Corruption("read_u16_le_at out of bounds");
  }
  const auto b0 = static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(buf[offset]));
  const auto b1 = static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(buf[offset + 1]));
  return Result<std::uint16_t>::Ok(
      static_cast<std::uint16_t>(b0 | static_cast<std::uint16_t>(b1 << 8U)));
}

Result<std::uint32_t> read_u32_le_at(std::span<const std::byte> buf,
                                     std::size_t offset) noexcept {
  if (offset > buf.size() || buf.size() - offset < 4) {
    return Status::Corruption("read_u32_le_at out of bounds");
  }
  const auto b0 = static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(buf[offset]));
  const auto b1 = static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(buf[offset + 1]));
  const auto b2 = static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(buf[offset + 2]));
  const auto b3 = static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(buf[offset + 3]));
  return Result<std::uint32_t>::Ok(b0 | (b1 << 8U) | (b2 << 16U) | (b3 << 24U));
}

Result<std::uint64_t> read_u64_le_at(std::span<const std::byte> buf,
                                     std::size_t offset) noexcept {
  if (offset > buf.size() || buf.size() - offset < 8) {
    return Status::Corruption("read_u64_le_at out of bounds");
  }
  std::uint64_t val = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    const auto byte_val = static_cast<std::uint64_t>(
        std::to_integer<std::uint8_t>(buf[offset + i]));
    val |= (byte_val << (i * 8U));
  }
  return Result<std::uint64_t>::Ok(val);
}

Status ByteWriter::write_u8(std::uint8_t val) noexcept {
  Status s = write_u8_at(buffer_, pos_, val);
  if (s.ok()) {
    pos_ += 1;
  }
  return s;
}

Status ByteWriter::write_u16_le(std::uint16_t val) noexcept {
  Status s = write_u16_le_at(buffer_, pos_, val);
  if (s.ok()) {
    pos_ += 2;
  }
  return s;
}

Status ByteWriter::write_u32_le(std::uint32_t val) noexcept {
  Status s = write_u32_le_at(buffer_, pos_, val);
  if (s.ok()) {
    pos_ += 4;
  }
  return s;
}

Status ByteWriter::write_u64_le(std::uint64_t val) noexcept {
  Status s = write_u64_le_at(buffer_, pos_, val);
  if (s.ok()) {
    pos_ += 8;
  }
  return s;
}

Status ByteWriter::write_bytes(std::span<const std::byte> data) noexcept {
  if (remaining() < data.size()) {
    return Status::InvalidArgument("ByteWriter::write_bytes out of bounds");
  }
  if (!data.empty()) {
    std::memcpy(buffer_.data() + pos_, data.data(), data.size());
    pos_ += data.size();
  }
  return Status::Ok();
}

Status ByteWriter::write_zeros(std::size_t count) noexcept {
  if (remaining() < count) {
    return Status::InvalidArgument("ByteWriter::write_zeros out of bounds");
  }
  if (count > 0) {
    std::memset(buffer_.data() + pos_, 0, count);
    pos_ += count;
  }
  return Status::Ok();
}

Result<std::uint8_t> ByteReader::read_u8() noexcept {
  auto r = read_u8_at(buffer_, pos_);
  if (r.ok()) {
    pos_ += 1;
  }
  return r;
}

Result<std::uint16_t> ByteReader::read_u16_le() noexcept {
  auto r = read_u16_le_at(buffer_, pos_);
  if (r.ok()) {
    pos_ += 2;
  }
  return r;
}

Result<std::uint32_t> ByteReader::read_u32_le() noexcept {
  auto r = read_u32_le_at(buffer_, pos_);
  if (r.ok()) {
    pos_ += 4;
  }
  return r;
}

Result<std::uint64_t> ByteReader::read_u64_le() noexcept {
  auto r = read_u64_le_at(buffer_, pos_);
  if (r.ok()) {
    pos_ += 8;
  }
  return r;
}

Result<std::span<const std::byte>> ByteReader::read_bytes(
    std::size_t count) noexcept {
  if (remaining() < count) {
    return Status::Corruption("ByteReader::read_bytes out of bounds");
  }
  auto slice = buffer_.subspan(pos_, count);
  pos_ += count;
  return Result<std::span<const std::byte>>::Ok(slice);
}

std::strong_ordering compare_bytes(std::span<const std::byte> lhs,
                                   std::span<const std::byte> rhs) noexcept {
  const std::size_t min_len = std::min(lhs.size(), rhs.size());
  for (std::size_t i = 0; i < min_len; ++i) {
    const auto a = std::to_integer<std::uint8_t>(lhs[i]);
    const auto b = std::to_integer<std::uint8_t>(rhs[i]);
    if (a != b) {
      return a <=> b;
    }
  }
  return lhs.size() <=> rhs.size();
}

std::strong_ordering compare_keys(std::string_view lhs,
                                  std::string_view rhs) noexcept {
  const auto lhs_bytes =
      std::as_bytes(std::span<const char>(lhs.data(), lhs.size()));
  const auto rhs_bytes =
      std::as_bytes(std::span<const char>(rhs.data(), rhs.size()));
  return compare_bytes(lhs_bytes, rhs_bytes);
}

}  // namespace pagedb
