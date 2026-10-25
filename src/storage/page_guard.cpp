#include "pagedb/page_guard.h"

#include <cassert>
#include <utility>

#include "pagedb/buffer_pool.h"

namespace pagedb {

PageGuard::~PageGuard() noexcept { release(); }

PageGuard::PageGuard(PageGuard&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      frame_index_(std::exchange(other.frame_index_, 0)),
      page_id_(std::exchange(other.page_id_, kInvalidPageId)) {}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this != &other) {
    release();
    pool_ = std::exchange(other.pool_, nullptr);
    frame_index_ = std::exchange(other.frame_index_, 0);
    page_id_ = std::exchange(other.page_id_, kInvalidPageId);
  }
  return *this;
}

std::span<const std::byte, kPageSize> PageGuard::bytes() const noexcept {
  assert(is_valid());
  return pool_->frame_bytes(frame_index_);
}

std::span<std::byte, kPageSize> PageGuard::mutable_bytes() noexcept {
  assert(is_valid());
  return pool_->frame_mutable_bytes(frame_index_);
}

void PageGuard::mark_dirty() noexcept {
  assert(is_valid());
  pool_->mark_frame_dirty(frame_index_);
}

void PageGuard::release() noexcept {
  if (pool_ != nullptr) {
    BufferPool* p = std::exchange(pool_, nullptr);
    const std::size_t idx = std::exchange(frame_index_, 0);
    const PageId id = std::exchange(page_id_, kInvalidPageId);
    p->unpin_frame(idx, id);
  }
}

}  // namespace pagedb
