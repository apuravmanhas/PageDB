#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "pagedb/file_handle.h"
#include "pagedb/page_codec.h"
#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

inline constexpr std::uint64_t kWalFileMagic =
    0x5041474544425731ULL;                                // "PAGEDBW1"
inline constexpr std::uint32_t kWalRecordMagic = 0x574C5243U;  // "WLRC"
inline constexpr std::size_t kWalFileHeaderSize = 32;
inline constexpr std::size_t kWalRecordHeaderSize = 36;
inline constexpr std::size_t kWalHeaderCrcOffset = 0x18;
inline constexpr std::size_t kWalRecordCrcOffset = 0x20;

enum class WalRecordType : std::uint8_t {
  kInvalid = 0,
  kBegin = 1,
  kPageImage = 2,
  kCommit = 3,
  kAbort = 4,
  kCheckpoint = 5,
};

struct WalFileHeader {
  std::uint64_t wal_magic{kWalFileMagic};
  std::uint16_t format_major{kFormatMajorVersion};
  std::uint16_t format_minor{kFormatMinorVersion};
  std::uint32_t page_size{static_cast<std::uint32_t>(kPageSize)};
  Lsn base_lsn{Lsn(1)};
  std::uint32_t header_crc32c{0};
  std::uint32_t reserved{0};
};

struct WalRecordHeader {
  std::uint32_t record_magic{kWalRecordMagic};
  WalRecordType record_type{WalRecordType::kInvalid};
  std::array<std::uint8_t, 3> flags_reserved{0, 0, 0};
  Lsn lsn{kInvalidLsn};
  TxnId txn_id{kInvalidTxnId};
  PageId page_id{kInvalidPageId};
  std::uint32_t payload_len{0};
  std::uint32_t record_crc32c{0};
};

struct WalPageUpdate {
  Lsn lsn{kInvalidLsn};
  TxnId txn_id{kInvalidTxnId};
  PageId page_id{kInvalidPageId};
  PageBuffer page_image{};
};

struct WalRecoveryReport {
  Lsn base_lsn{Lsn(1)};
  Lsn max_committed_lsn{kInvalidLsn};
  Lsn next_lsn{Lsn(1)};
  TxnId next_txn_id{TxnId(1)};
  std::uint64_t valid_end_offset{kWalFileHeaderSize};
  bool truncated_uncommitted_tail{false};
  std::vector<WalPageUpdate> committed_updates{};
};

class WalCodec {
 public:
  [[nodiscard]] static Status encode_file_header(
      const WalFileHeader& hdr,
      std::span<std::byte, kWalFileHeaderSize> out) noexcept;

  [[nodiscard]] static Result<WalFileHeader> decode_file_header(
      std::span<const std::byte, kWalFileHeaderSize> buf) noexcept;

  [[nodiscard]] static Status encode_record(
      const WalRecordHeader& hdr, std::span<const std::byte> payload,
      std::span<std::byte> out) noexcept;

  [[nodiscard]] static Result<WalRecordHeader> decode_record_header(
      std::span<const std::byte, kWalRecordHeaderSize> buf) noexcept;

  [[nodiscard]] static Status verify_record(
      const WalRecordHeader& hdr,
      std::span<const std::byte> payload) noexcept;
};

class Wal {
 public:
  Wal() noexcept = default;
  ~Wal() noexcept = default;

  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<Wal>> open(
      const std::filesystem::path& wal_path,
      const PosixIoOps* io_ops = &PosixIoOps::default_ops());

  // Scans the WAL from offset 32, rejects mid-log corruption, truncates any
  // torn or uncommitted tail transaction back to the end of the last finished
  // transaction, and returns all committed PAGE_IMAGE updates in LSN order.
  [[nodiscard]] Result<WalRecoveryReport> recover();

  [[nodiscard]] TxnId allocate_txn_id() noexcept {
    const TxnId id = next_txn_id_;
    next_txn_id_ = TxnId(next_txn_id_.value + 1ULL);
    return id;
  }

  [[nodiscard]] Lsn peek_next_lsn() const noexcept { return next_lsn_; }
  [[nodiscard]] Lsn max_committed_lsn() const noexcept {
    return max_committed_lsn_;
  }
  [[nodiscard]] std::uint64_t write_offset() const noexcept {
    return write_offset_;
  }
  [[nodiscard]] Result<std::uint64_t> size_bytes() const {
    return file_.size_bytes();
  }

  [[nodiscard]] Result<Lsn> append_begin(TxnId txn_id);
  [[nodiscard]] Result<Lsn> append_page_image(
      TxnId txn_id, PageId page_id,
      std::span<std::byte, kPageSize> in_out_page_image);
  [[nodiscard]] Result<Lsn> append_commit(TxnId txn_id);
  [[nodiscard]] Result<Lsn> append_abort(TxnId txn_id);
  [[nodiscard]] Result<Lsn> append_checkpoint();

  [[nodiscard]] Status sync();
  [[nodiscard]] Status reset(Lsn new_base_lsn);
  void discard_unsynced_from(std::uint64_t offset, Lsn reset_next_lsn,
                             TxnId reset_next_txn_id,
                             Lsn prev_max_committed_lsn) noexcept;
  [[nodiscard]] Status close() noexcept;

 private:
  [[nodiscard]] Result<Lsn> append_empty_record(WalRecordType type,
                                                TxnId txn_id);

  std::filesystem::path wal_path_{};
  FileHandle file_{};
  WalFileHeader header_{};
  std::uint64_t write_offset_{kWalFileHeaderSize};
  Lsn next_lsn_{Lsn(1)};
  TxnId next_txn_id_{TxnId(1)};
  Lsn max_committed_lsn_{kInvalidLsn};
};

}  // namespace pagedb
