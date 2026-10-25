#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

// Checked integer arithmetic helpers.
[[nodiscard]] Result<std::uint16_t> checked_add_u16(std::uint16_t a,
                                                    std::uint16_t b) noexcept;
[[nodiscard]] Result<std::uint32_t> checked_add_u32(std::uint32_t a,
                                                    std::uint32_t b) noexcept;
[[nodiscard]] Result<std::uint64_t> checked_add_u64(std::uint64_t a,
                                                    std::uint64_t b) noexcept;
[[nodiscard]] Result<std::uint64_t> checked_mul_u64(std::uint64_t a,
                                                    std::uint64_t b) noexcept;
[[nodiscard]] Result<std::uint64_t> checked_page_offset(
    PageId page_id, std::size_t page_size = kPageSize) noexcept;

// Explicit little-endian positional encoders/decoders with bounds validation.
[[nodiscard]] Status write_u8_at(std::span<std::byte> buf, std::size_t offset,
                                 std::uint8_t val) noexcept;
[[nodiscard]] Status write_u16_le_at(std::span<std::byte> buf,
                                     std::size_t offset,
                                     std::uint16_t val) noexcept;
[[nodiscard]] Status write_u32_le_at(std::span<std::byte> buf,
                                     std::size_t offset,
                                     std::uint32_t val) noexcept;
[[nodiscard]] Status write_u64_le_at(std::span<std::byte> buf,
                                     std::size_t offset,
                                     std::uint64_t val) noexcept;

[[nodiscard]] Result<std::uint8_t> read_u8_at(std::span<const std::byte> buf,
                                              std::size_t offset) noexcept;
[[nodiscard]] Result<std::uint16_t> read_u16_le_at(
    std::span<const std::byte> buf, std::size_t offset) noexcept;
[[nodiscard]] Result<std::uint32_t> read_u32_le_at(
    std::span<const std::byte> buf, std::size_t offset) noexcept;
[[nodiscard]] Result<std::uint64_t> read_u64_le_at(
    std::span<const std::byte> buf, std::size_t offset) noexcept;

// Sequential cursor writer over a fixed byte span.
class ByteWriter {
 public:
  explicit ByteWriter(std::span<std::byte> buffer) noexcept
      : buffer_(buffer), pos_(0) {}

  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer_.size() - pos_;
  }

  [[nodiscard]] Status write_u8(std::uint8_t val) noexcept;
  [[nodiscard]] Status write_u16_le(std::uint16_t val) noexcept;
  [[nodiscard]] Status write_u32_le(std::uint32_t val) noexcept;
  [[nodiscard]] Status write_u64_le(std::uint64_t val) noexcept;
  [[nodiscard]] Status write_bytes(std::span<const std::byte> data) noexcept;
  [[nodiscard]] Status write_zeros(std::size_t count) noexcept;

 private:
  std::span<std::byte> buffer_;
  std::size_t pos_;
};

// Sequential cursor reader over a constant byte span.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> buffer) noexcept
      : buffer_(buffer), pos_(0) {}

  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer_.size() - pos_;
  }

  [[nodiscard]] Result<std::uint8_t> read_u8() noexcept;
  [[nodiscard]] Result<std::uint16_t> read_u16_le() noexcept;
  [[nodiscard]] Result<std::uint32_t> read_u32_le() noexcept;
  [[nodiscard]] Result<std::uint64_t> read_u64_le() noexcept;
  [[nodiscard]] Result<std::span<const std::byte>> read_bytes(
      std::size_t count) noexcept;

 private:
  std::span<const std::byte> buffer_;
  std::size_t pos_;
};

// Unsigned-byte lexicographic comparison.
[[nodiscard]] std::strong_ordering compare_bytes(
    std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept;

[[nodiscard]] std::strong_ordering compare_keys(std::string_view lhs,
                                                std::string_view rhs) noexcept;

struct UnsignedByteLess {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view lhs,
                                std::string_view rhs) const noexcept {
    return compare_keys(lhs, rhs) < 0;
  }
};

}  // namespace pagedb
