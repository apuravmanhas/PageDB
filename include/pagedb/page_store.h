#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>

#include "pagedb/file_handle.h"
#include "pagedb/page_codec.h"
#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

struct PageStoreOptions {
  bool create_if_missing{true};
  bool error_if_exists{false};
  bool defer_validation_for_recovery{false};
  const PosixIoOps* io_ops{&PosixIoOps::default_ops()};
};

class PageStore {
 public:
  PageStore() = default;
  ~PageStore() noexcept = default;

  PageStore(const PageStore&) = delete;
  PageStore& operator=(const PageStore&) = delete;

  PageStore(PageStore&&) noexcept = default;
  PageStore& operator=(PageStore&&) noexcept = default;

  [[nodiscard]] static Result<std::unique_ptr<PageStore>> open(
      const std::filesystem::path& db_dir,
      const PageStoreOptions& options = {});

  [[nodiscard]] Status read_page(
      PageId page_id, std::span<std::byte, kPageSize> out_page) const;

  [[nodiscard]] Status write_page(
      PageId page_id, std::span<const std::byte, kPageSize> page_image);

  // Transaction staging support (enforces Invariant W-1 No-Steal).
  // While a transaction is active, write_page() stages modified page images in
  // memory without modifying pagedb.data, and read_page() serves staged images
  // first before falling through to pagedb.data.
  void begin_transaction();
  void abort_transaction() noexcept;
  [[nodiscard]] bool in_transaction() const noexcept { return in_transaction_; }
  [[nodiscard]] std::map<PageId, PageBuffer>& staged_pages() noexcept {
    return staged_pages_;
  }
  void set_staged_superblock_lsn(Lsn lsn) noexcept {
    staged_superblock_.page_lsn = lsn;
  }

  // Installs all staged pages to pagedb.data (writing PageId(0) last) and ends
  // the active transaction. Must only be called after WAL commit sync succeeds.
  [[nodiscard]] Status install_staged_pages();

  // Recovery helpers used by Database::open() before metadata validation.
  [[nodiscard]] Status write_raw_page_for_recovery(
      PageId page_id, std::span<const std::byte, kPageSize> page_image);
  [[nodiscard]] Result<Superblock> read_raw_superblock_for_recovery() const;
  [[nodiscard]] Status truncate_to_page_count(std::uint32_t num_pages);
  [[nodiscard]] Status validate_and_load_metadata(
      Lsn max_committed_wal_lsn = kInvalidLsn, bool has_wal_records = false);

  [[nodiscard]] Status sync();
  [[nodiscard]] Status close() noexcept;

  [[nodiscard]] std::uint32_t page_count() const noexcept {
    return in_transaction_ ? staged_page_count_ : page_count_;
  }
  [[nodiscard]] const Superblock& superblock() const noexcept {
    return in_transaction_ ? staged_superblock_ : superblock_;
  }
  [[nodiscard]] bool is_poisoned() const noexcept { return poisoned_; }
  void poison() noexcept { poisoned_ = true; }

 private:
  std::filesystem::path db_dir_;
  DatabaseLock lock_;
  FileHandle data_file_;
  Superblock superblock_{};
  std::uint32_t page_count_{0};

  bool in_transaction_{false};
  std::map<PageId, PageBuffer> staged_pages_{};
  Superblock staged_superblock_{};
  std::uint32_t staged_page_count_{0};

  bool poisoned_{false};
};

}  // namespace pagedb
