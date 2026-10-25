#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pagedb {

inline constexpr std::size_t kPageSize = 4096;
inline constexpr std::size_t kPageHeaderSize = 48;
inline constexpr std::size_t kMaxKeySize = 256;
inline constexpr std::size_t kMaxValueSize = 512;

inline constexpr std::uint32_t kPageMagic = 0x50474442U;  // "PGDB"
inline constexpr std::uint64_t kSuperblockMagic = 0x5041474544425631ULL;  // "PAGEDBV1"
inline constexpr std::uint16_t kFormatMajorVersion = 1;
inline constexpr std::uint16_t kFormatMinorVersion = 0;

struct PageId {
  std::uint32_t value{std::numeric_limits<std::uint32_t>::max()};

  constexpr PageId() = default;
  constexpr explicit PageId(std::uint32_t v) noexcept : value(v) {}

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return value != std::numeric_limits<std::uint32_t>::max();
  }

  friend constexpr auto operator<=>(PageId, PageId) = default;
};

inline constexpr PageId kInvalidPageId{std::numeric_limits<std::uint32_t>::max()};
inline constexpr PageId kSuperblockPageId{0U};
inline constexpr PageId kInitialRootPageId{1U};

struct Lsn {
  std::uint64_t value{0ULL};

  constexpr Lsn() = default;
  constexpr explicit Lsn(std::uint64_t v) noexcept : value(v) {}

  [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0ULL; }

  friend constexpr auto operator<=>(Lsn, Lsn) = default;
};

inline constexpr Lsn kInvalidLsn{0ULL};

struct TxnId {
  std::uint64_t value{0ULL};

  constexpr TxnId() = default;
  constexpr explicit TxnId(std::uint64_t v) noexcept : value(v) {}

  [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0ULL; }

  friend constexpr auto operator<=>(TxnId, TxnId) = default;
};

inline constexpr TxnId kInvalidTxnId{0ULL};

enum class PageType : std::uint8_t {
  kInvalid = 0,
  kSuperblock = 1,
  kLeaf = 2,
  kInternal = 3,
  kFree = 4,
};

}  // namespace pagedb
