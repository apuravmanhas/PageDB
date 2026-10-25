#pragma once

#include <cstddef>
#include <span>

#include "pagedb/page_codec.h"
#include "pagedb/types.h"

namespace pagedb {

class BufferPool;

class PageGuard {
 public:
  PageGuard() noexcept = default;
  ~PageGuard() noexcept;

  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;

  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;

  [[nodiscard]] bool is_valid() const noexcept { return pool_ != nullptr; }
  [[nodiscard]] PageId page_id() const noexcept { return page_id_; }

  [[nodiscard]] std::span<const std::byte, kPageSize> bytes() const noexcept;
  [[nodiscard]] std::span<std::byte, kPageSize> mutable_bytes() noexcept;

  void mark_dirty() noexcept;
  void release() noexcept;

 private:
  friend class BufferPool;

  PageGuard(BufferPool* pool, std::size_t frame_index, PageId page_id) noexcept
      : pool_(pool), frame_index_(frame_index), page_id_(page_id) {}

  BufferPool* pool_{nullptr};
  std::size_t frame_index_{0};
  PageId page_id_{kInvalidPageId};
};

}  // namespace pagedb
