#pragma once

#include <cstdint>

#include "pagedb/page_codec.h"
#include "pagedb/page_store.h"
#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

class PageAllocator {
 public:
  explicit PageAllocator(PageStore& store) noexcept
      : store_(store), superblock_(store.superblock()) {}

  // Allocates a page (reusing the free-list head if available, otherwise
  // appending at next_page_id), initializes it on disk as an empty page of
  // `page_type`, and updates the Superblock.
  [[nodiscard]] Result<PageId> allocate_page(
      PageType page_type, PageId aux_page_id = kInvalidPageId);

  // Marks an allocated non-root, non-superblock page as PageType::kFree and
  // pushes it onto the Superblock free_list_head.
  [[nodiscard]] Status free_page(PageId page_id);

  [[nodiscard]] Status set_root_page_id(PageId new_root);

  [[nodiscard]] PageId root_page_id() const noexcept {
    return superblock_.root_page_id;
  }
  [[nodiscard]] PageId next_page_id() const noexcept {
    return superblock_.next_page_id;
  }
  [[nodiscard]] PageId free_list_head() const noexcept {
    return superblock_.free_list_head;
  }
  [[nodiscard]] Result<PageId> read_next_free_page(PageId free_page_id) const;
  [[nodiscard]] bool is_allocated(PageId page_id) const noexcept {
    return page_id.is_valid() && page_id.value < superblock_.next_page_id.value;
  }

  void reload_superblock() noexcept { superblock_ = store_.superblock(); }
  [[nodiscard]] Status flush_superblock();

  // Debug verifier checking allocator bounds and physical page headers.
  [[nodiscard]] Status verify_allocation_state() const;

 private:
  PageStore& store_;
  Superblock superblock_;
};

}  // namespace pagedb
