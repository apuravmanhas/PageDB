#include "pagedb/buffer_pool.h"

#include <algorithm>
#include <cassert>

namespace pagedb {

BufferPool::BufferPool(PageStore& store, std::size_t capacity_frames)
    : store_(store), frames_(capacity_frames) {
  assert(capacity_frames > 0);
}

BufferPool::~BufferPool() noexcept {
  // A PageGuard holds a raw BufferPool*; destroying the pool while a guard is
  // alive would leave that guard dangling. Dirty frames are intentionally not
  // flushed here because a destructor cannot report I/O errors; callers must
  // invoke flush_all() explicitly.
  for ([[maybe_unused]] const Frame& f : frames_) {
    assert(f.pin_count == 0 && "BufferPool destroyed while a page is pinned");
  }
}

Result<PageGuard> BufferPool::fetch_page(PageId page_id) {
  if (page_id == kSuperblockPageId) {
    // The superblock has a single in-memory owner (see ADR 0006). Caching it
    // here as well would let a stale frame overwrite a newer superblock.
    return Status::InvalidArgument(
        "the superblock page is not managed by the buffer pool");
  }
  if (!page_id.is_valid() || page_id.value >= store_.page_count()) {
    return Status::InvalidArgument("fetch_page PageId out of allocated range");
  }

  // 1. Check if page is already resident in the buffer pool.
  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    const std::size_t idx = it->second;
    Frame& frame = frames_[idx];
    assert(frame.page_id == page_id);
    ++frame.pin_count;
    frame.ref_bit = true;
    ++stats_.hits;
    return Result<PageGuard>::Ok(PageGuard(this, idx, page_id));
  }

  // 2. Cache miss: locate a free or evictable frame via CLOCK.
  auto victim_res = find_victim_frame();
  if (!victim_res.ok()) {
    ++stats_.pin_failures;
    return victim_res.status();
  }
  const std::size_t idx = victim_res.value();
  Frame& frame = frames_[idx];
  assert(frame.pin_count == 0);

  // 3. If the victim frame holds a valid resident page, flush if dirty before
  // unmapping.
  if (frame.page_id.is_valid()) {
    if (frame.is_dirty) {
      Status ws = store_.write_page(frame.page_id, frame.data);
      if (!ws.ok()) {
        // Leave victim frame intact and dirty on writeback failure.
        return ws;
      }
      frame.is_dirty = false;
      ++stats_.dirty_flushes;
    }
    page_table_.erase(frame.page_id);
    frame.page_id = kInvalidPageId;
    ++stats_.evictions;
  }

  // 4. Load the requested page from PageStore into the clean victim frame.
  Status rs = store_.read_page(page_id, frame.data);
  if (!rs.ok()) {
    frame.data.fill(std::byte{0});
    return rs;
  }

  frame.page_id = page_id;
  frame.pin_count = 1;
  frame.is_dirty = false;
  frame.ref_bit = true;
  page_table_.emplace(page_id, idx);
  ++stats_.misses;

  return Result<PageGuard>::Ok(PageGuard(this, idx, page_id));
}

Result<std::size_t> BufferPool::find_victim_frame() {
  const std::size_t n = frames_.size();
  if (n == 0) {
    return Status::ResourceExhausted("BufferPool has zero capacity");
  }

  // First pass: prefer any unused frame.
  for (std::size_t i = 0; i < n; ++i) {
    if (!frames_[i].page_id.is_valid()) {
      assert(frames_[i].pin_count == 0);
      return Result<std::size_t>::Ok(i);
    }
  }

  // Second pass: CLOCK sweep (up to 2 * n steps to clear reference bits).
  for (std::size_t step = 0; step < 2 * n; ++step) {
    const std::size_t idx = clock_hand_;
    clock_hand_ = (clock_hand_ + 1) % n;

    Frame& f = frames_[idx];
    if (f.pin_count > 0) {
      continue;
    }
    if (f.ref_bit) {
      f.ref_bit = false;
    } else {
      return Result<std::size_t>::Ok(idx);
    }
  }

  return Status::ResourceExhausted(
      "all buffer pool frames are currently pinned");
}

Status BufferPool::flush_page(PageId page_id) {
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return Status::Ok();
  }
  Frame& frame = frames_[it->second];
  if (!frame.is_dirty) {
    return Status::Ok();
  }
  Status ws = store_.write_page(frame.page_id, frame.data);
  if (!ws.ok()) {
    return ws;
  }
  frame.is_dirty = false;
  ++stats_.dirty_flushes;
  return Status::Ok();
}

Status BufferPool::discard_page(PageId page_id) noexcept {
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return Status::Ok();
  }
  Frame& frame = frames_[it->second];
  if (frame.pin_count > 0) {
    return Status::InvalidArgument("cannot discard a pinned buffer pool frame");
  }
  frame.data.fill(std::byte{0});
  frame.page_id = kInvalidPageId;
  frame.is_dirty = false;
  frame.ref_bit = false;
  page_table_.erase(it);
  return Status::Ok();
}

void BufferPool::discard_all_pages() noexcept {
  for (Frame& frame : frames_) {
    assert(frame.pin_count == 0);
    frame.data.fill(std::byte{0});
    frame.page_id = kInvalidPageId;
    frame.is_dirty = false;
    frame.ref_bit = false;
  }
  page_table_.clear();
}

Status BufferPool::flush_dirty_pages() {
  for (Frame& frame : frames_) {
    if (frame.page_id.is_valid() && frame.is_dirty) {
      Status ws = store_.write_page(frame.page_id, frame.data);
      if (!ws.ok()) {
        return ws;
      }
      frame.is_dirty = false;
      ++stats_.dirty_flushes;
    }
  }
  return Status::Ok();
}

void BufferPool::update_resident_frame(
    PageId page_id, std::span<const std::byte, kPageSize> image) noexcept {
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return;
  }
  Frame& frame = frames_[it->second];
  assert(frame.pin_count == 0);
  assert(!frame.is_dirty);
  std::copy(image.begin(), image.end(), frame.data.begin());
}

Status BufferPool::flush_all() {
  Status s = flush_dirty_pages();
  if (!s.ok()) {
    return s;
  }
  return store_.sync();
}

std::uint32_t BufferPool::pin_count(PageId page_id) const noexcept {
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return 0;
  }
  return frames_[it->second].pin_count;
}

bool BufferPool::is_dirty(PageId page_id) const noexcept {
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }
  return frames_[it->second].is_dirty;
}

Status BufferPool::verify_invariants() const noexcept {
  std::size_t valid_frames = 0;
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    const Frame& f = frames_[i];
    if (!f.page_id.is_valid()) {
      if (f.pin_count != 0 || f.is_dirty) {
        return Status::Corruption(
            "unmapped buffer pool frame has non-zero pin_count or is_dirty");
      }
      continue;
    }
    ++valid_frames;
    auto it = page_table_.find(f.page_id);
    if (it == page_table_.end() || it->second != i) {
      return Status::Corruption(
          "buffer pool frame mapping disagrees with page_table_");
    }
  }
  if (valid_frames != page_table_.size()) {
    return Status::Corruption(
        "buffer pool page_table_ size disagrees with resident frame count");
  }
  return Status::Ok();
}

std::span<const std::byte, kPageSize> BufferPool::frame_bytes(
    std::size_t frame_index) const noexcept {
  assert(frame_index < frames_.size());
  assert(frames_[frame_index].pin_count > 0);
  return frames_[frame_index].data;
}

std::span<std::byte, kPageSize> BufferPool::frame_mutable_bytes(
    std::size_t frame_index) noexcept {
  assert(frame_index < frames_.size());
  assert(frames_[frame_index].pin_count > 0);
  return frames_[frame_index].data;
}

void BufferPool::mark_frame_dirty(std::size_t frame_index) noexcept {
  assert(frame_index < frames_.size());
  assert(frames_[frame_index].pin_count > 0);
  frames_[frame_index].is_dirty = true;
}

void BufferPool::unpin_frame(std::size_t frame_index,
                             PageId expected_id) noexcept {
  assert(frame_index < frames_.size());
  Frame& frame = frames_[frame_index];
  assert(frame.page_id == expected_id);
  assert(frame.pin_count > 0);
  --frame.pin_count;
}

}  // namespace pagedb
