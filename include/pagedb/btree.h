#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pagedb/buffer_pool.h"
#include "pagedb/page_allocator.h"
#include "pagedb/page_codec.h"
#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

/// Minimum active byte usage (slot directory entries + payloads) before a
/// non-root node is considered underfull during deletion (~25% of 4048 bytes).
inline constexpr std::size_t kMinPageFillBytes = 1012;

struct KeyValue {
  std::string key;
  std::string value;

  friend bool operator==(const KeyValue&, const KeyValue&) = default;
};

class BTree {
 public:
  BTree(BufferPool& pool, PageAllocator& allocator) noexcept;

  BTree(const BTree&) = delete;
  BTree& operator=(const BTree&) = delete;

  /// Insert or update a key-value pair.
  [[nodiscard]] Status put(std::span<const std::byte> key,
                           std::span<const std::byte> value);
  [[nodiscard]] Status put(std::string_view key, std::string_view value);

  /// Retrieve the value associated with a key.
  /// Returns kNotFound if the key does not exist.
  [[nodiscard]] Result<std::string> get(std::span<const std::byte> key);
  [[nodiscard]] Result<std::string> get(std::string_view key);

  /// Delete a key-value pair.
  /// Returns Ok(true) if the key existed and was removed, or Ok(false) if the
  /// key was not present.
  [[nodiscard]] Result<bool> del(std::span<const std::byte> key);
  [[nodiscard]] Result<bool> del(std::string_view key);

  /// Range scan over half-open interval [start_key, end_key).
  /// Pass std::nullopt for an unbounded lower or upper bound.
  [[nodiscard]] Result<std::vector<KeyValue>> scan(
      std::optional<std::string_view> start_key = std::nullopt,
      std::optional<std::string_view> end_key = std::nullopt);
  [[nodiscard]] Result<std::vector<KeyValue>> scan_bytes(
      std::optional<std::span<const std::byte>> start_key = std::nullopt,
      std::optional<std::span<const std::byte>> end_key = std::nullopt);

  /// Returns the current height of the B+ tree (1 when root is a leaf).
  [[nodiscard]] Result<std::uint32_t> height() const;

  /// Structural integrity verifier: walks every reachable page and checks
  /// sorted keys, valid child pointers, parent-child separator invariants,
  /// uniform leaf depth, leaf sibling chain order, and exact page conservation
  /// with the allocator free list.
  [[nodiscard]] Status verify() const;

 private:
  struct LeafSearchResult {
    std::uint16_t slot_index{0};
    bool found{false};
  };

  struct InternalSearchResult {
    PageId child_page_id{kInvalidPageId};
    // 0 means aux_page_id (leftmost_child); k >= 1 means slot[k - 1].right_child.
    std::uint16_t child_index{0};
  };

  struct PathEntry {
    PageId page_id{kInvalidPageId};
    std::uint16_t child_index{0};
  };

  [[nodiscard]] static Result<LeafSearchResult> search_leaf(
      const SlottedPage& page, std::span<const std::byte> key);

  [[nodiscard]] static Result<InternalSearchResult> search_internal(
      const SlottedPage& page, std::span<const std::byte> key);

  [[nodiscard]] Result<PageId> find_leaf(
      std::span<const std::byte> key) const;

  [[nodiscard]] Result<std::pair<SlottedPage, PageGuard>> fetch_and_decode(
      PageId page_id) const;

  [[nodiscard]] static Status encode_and_mark_dirty(
      const SlottedPage& page, PageGuard& guard);

  [[nodiscard]] static std::size_t used_bytes(
      const SlottedPage& page) noexcept {
    return (kPageSize - kPageHeaderSize) - page.usable_free_space();
  }

  [[nodiscard]] Status split_leaf_and_propagate(
      PageId leaf_id, SlottedPage& leaf_page, PageGuard& leaf_guard,
      const std::vector<std::vector<std::byte>>& all_records,
      const std::vector<PathEntry>& path);

  [[nodiscard]] Status insert_into_parent(
      PageId left_child, std::span<const std::byte> separator_key,
      PageId right_child, const std::vector<PathEntry>& path,
      std::size_t path_depth);

  [[nodiscard]] Status rebalance_after_delete(
      PageId underfull_page_id, const std::vector<PathEntry>& path);

  [[nodiscard]] Status collapse_unary_root_chain();

  BufferPool& pool_;
  PageAllocator& allocator_;
};

}  // namespace pagedb
