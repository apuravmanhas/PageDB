#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "pagedb/page_codec.h"
#include "pagedb/page_guard.h"
#include "pagedb/page_store.h"
#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

struct BufferPoolStats {
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions{0};
  std::uint64_t dirty_flushes{0};
  std::uint64_t pin_failures{0};
};

class BufferPool {
 public:
  BufferPool(PageStore& store, std::size_t capacity_frames);
  ~BufferPool() noexcept;

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // Fetches an allocated page into the buffer pool and returns a pinned guard.
  [[nodiscard]] Result<PageGuard> fetch_page(PageId page_id);

  // Flushes a specific resident page if dirty.
  [[nodiscard]] Status flush_page(PageId page_id);

  // Discards a specific unpinned resident page without writing it back.
  // Used when a page is freed and returned to the allocator free list.
  [[nodiscard]] Status discard_page(PageId page_id) noexcept;

  // Flushes all dirty frames to PageStore and synchronizes the data file.
  [[nodiscard]] Status flush_all();

  [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }
  [[nodiscard]] std::size_t resident_count() const noexcept {
    return page_table_.size();
  }
  [[nodiscard]] std::uint32_t pin_count(PageId page_id) const noexcept;
  [[nodiscard]] bool is_dirty(PageId page_id) const noexcept;
  [[nodiscard]] bool is_resident(PageId page_id) const noexcept {
    return page_table_.find(page_id) != page_table_.end();
  }
  [[nodiscard]] const BufferPoolStats& stats() const noexcept { return stats_; }

  // Debug invariant checker for frame table and pin counts.
  [[nodiscard]] Status verify_invariants() const noexcept;

 private:
  friend class PageGuard;

  struct Frame {
    PageBuffer data{};
    PageId page_id{kInvalidPageId};
    std::uint32_t pin_count{0};
    bool is_dirty{false};
    bool ref_bit{false};
  };

  struct PageIdHash {
    [[nodiscard]] std::size_t operator()(PageId id) const noexcept {
      return std::hash<std::uint32_t>{}(id.value);
    }
  };

  [[nodiscard]] Result<std::size_t> find_victim_frame();
  [[nodiscard]] std::span<const std::byte, kPageSize> frame_bytes(
      std::size_t frame_index) const noexcept;
  [[nodiscard]] std::span<std::byte, kPageSize> frame_mutable_bytes(
      std::size_t frame_index) noexcept;
  void mark_frame_dirty(std::size_t frame_index) noexcept;
  void unpin_frame(std::size_t frame_index, PageId expected_id) noexcept;

  PageStore& store_;
  std::vector<Frame> frames_;
  std::unordered_map<PageId, std::size_t, PageIdHash> page_table_;
  std::size_t clock_hand_{0};
  BufferPoolStats stats_{};
};

}  // namespace pagedb
