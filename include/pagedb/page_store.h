#pragma once

#include <cstdint>
#include <filesystem>
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

  [[nodiscard]] static Result< std::unique_ptr<PageStore> > open(
      const std::filesystem::path& db_dir,
      const PageStoreOptions& options = {});

  [[nodiscard]] Status read_page(
      PageId page_id, std::span<std::byte, kPageSize> out_page) const;

  [[nodiscard]] Status write_page(
      PageId page_id, std::span<const std::byte, kPageSize> page_image);

  [[nodiscard]] Status sync();
  [[nodiscard]] Status close() noexcept;

  [[nodiscard]] std::uint32_t page_count() const noexcept {
    return page_count_;
  }
  [[nodiscard]] const Superblock& superblock() const noexcept {
    return superblock_;
  }
  [[nodiscard]] bool is_poisoned() const noexcept { return poisoned_; }
  void poison() noexcept { poisoned_ = true; }

 private:
  std::filesystem::path db_dir_;
  DatabaseLock lock_;
  FileHandle data_file_;
  Superblock superblock_{};
  std::uint32_t page_count_{0};
  bool poisoned_{false};
};

}  // namespace pagedb
