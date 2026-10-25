#include "pagedb/btree.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>

#include "pagedb/byte_codec.h"

namespace pagedb {
namespace {

std::span<const std::byte> sv_to_bytes(std::string_view sv) noexcept {
  return std::as_bytes(std::span<const char>(sv.data(), sv.size()));
}

std::string bytes_to_string(std::span<const std::byte> b) {
  std::string out(b.size(), '\0');
  if (!b.empty()) {
    std::memcpy(out.data(), b.data(), b.size());
  }
  return out;
}

}  // namespace

BTree::BTree(BufferPool& pool, PageAllocator& allocator) noexcept
    : pool_(pool), allocator_(allocator) {}

Status BTree::put(std::string_view key, std::string_view value) {
  return put(sv_to_bytes(key), sv_to_bytes(value));
}

Result<std::string> BTree::get(std::string_view key) {
  return get(sv_to_bytes(key));
}

Result<bool> BTree::del(std::string_view key) { return del(sv_to_bytes(key)); }

Result<std::vector<KeyValue>> BTree::scan(
    std::optional<std::string_view> start_key,
    std::optional<std::string_view> end_key) {
  std::optional<std::span<const std::byte>> start_bytes;
  std::optional<std::span<const std::byte>> end_bytes;
  if (start_key.has_value()) {
    start_bytes = sv_to_bytes(*start_key);
  }
  if (end_key.has_value()) {
    end_bytes = sv_to_bytes(*end_key);
  }
  return scan_bytes(start_bytes, end_bytes);
}

Result<std::pair<SlottedPage, PageGuard>> BTree::fetch_and_decode(
    PageId page_id) const {
  auto guard_res = pool_.fetch_page(page_id);
  if (!guard_res.ok()) {
    return guard_res.status();
  }
  PageGuard guard = std::move(guard_res.value());
  auto sp_res = SlottedPage::decode(guard.bytes(), page_id);
  if (!sp_res.ok()) {
    return sp_res.status();
  }
  return Result<std::pair<SlottedPage, PageGuard>>::Ok(
      std::make_pair(std::move(sp_res.value()), std::move(guard)));
}

Status BTree::encode_and_mark_dirty(const SlottedPage& page, PageGuard& guard) {
  Status s = page.encode(guard.mutable_bytes());
  if (!s.ok()) {
    return s;
  }
  guard.mark_dirty();
  return Status::Ok();
}

Result<BTree::LeafSearchResult> BTree::search_leaf(
    const SlottedPage& page, std::span<const std::byte> key) {
  if (page.page_type() != PageType::kLeaf) {
    return Status::Corruption("search_leaf called on non-leaf page");
  }
  std::uint16_t lo = 0;
  std::uint16_t hi = page.num_slots();
  while (lo < hi) {
    const auto mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2U);
    auto payload_res = page.slot_payload(mid);
    if (!payload_res.ok()) {
      return payload_res.status();
    }
    auto rec_res = PageCodec::decode_leaf_record(payload_res.value());
    if (!rec_res.ok()) {
      return rec_res.status();
    }
    const auto cmp = compare_bytes(rec_res.value().key, key);
    if (cmp == 0) {
      return Result<LeafSearchResult>::Ok(
          LeafSearchResult{.slot_index = mid, .found = true});
    }
    if (cmp < 0) {
      lo = static_cast<std::uint16_t>(mid + 1U);
    } else {
      hi = mid;
    }
  }
  return Result<LeafSearchResult>::Ok(
      LeafSearchResult{.slot_index = lo, .found = false});
}

Result<BTree::InternalSearchResult> BTree::search_internal(
    const SlottedPage& page, std::span<const std::byte> key) {
  if (page.page_type() != PageType::kInternal) {
    return Status::Corruption("search_internal called on non-internal page");
  }
  if (!page.aux_page_id().is_valid() ||
      page.aux_page_id() == kSuperblockPageId) {
    return Status::Corruption("internal page has invalid leftmost_child");
  }
  if (page.num_slots() == 0) {
    return Result<InternalSearchResult>::Ok(InternalSearchResult{
        .child_page_id = page.aux_page_id(), .child_index = 0});
  }

  // Find the first separator slot whose separator_key > key (upper_bound).
  std::uint16_t lo = 0;
  std::uint16_t hi = page.num_slots();
  while (lo < hi) {
    const auto mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2U);
    auto payload_res = page.slot_payload(mid);
    if (!payload_res.ok()) {
      return payload_res.status();
    }
    auto rec_res = PageCodec::decode_internal_record(payload_res.value());
    if (!rec_res.ok()) {
      return rec_res.status();
    }
    if (compare_bytes(key, rec_res.value().separator_key) < 0) {
      hi = mid;
    } else {
      lo = static_cast<std::uint16_t>(mid + 1U);
    }
  }

  if (lo == 0) {
    return Result<InternalSearchResult>::Ok(InternalSearchResult{
        .child_page_id = page.aux_page_id(), .child_index = 0});
  }
  const auto chosen_slot = static_cast<std::uint16_t>(lo - 1U);
  auto payload_res = page.slot_payload(chosen_slot);
  if (!payload_res.ok()) {
    return payload_res.status();
  }
  auto rec_res = PageCodec::decode_internal_record(payload_res.value());
  if (!rec_res.ok()) {
    return rec_res.status();
  }
  return Result<InternalSearchResult>::Ok(InternalSearchResult{
      .child_page_id = rec_res.value().right_child, .child_index = lo});
}

Result<PageId> BTree::find_leaf(std::span<const std::byte> key) const {
  PageId curr = allocator_.root_page_id();
  const std::uint32_t max_steps = allocator_.next_page_id().value;
  for (std::uint32_t step = 0; step < max_steps; ++step) {
    auto fd_res = fetch_and_decode(curr);
    if (!fd_res.ok()) {
      return fd_res.status();
    }
    auto [page, guard] = std::move(fd_res.value());
    if (page.page_type() == PageType::kLeaf) {
      return Result<PageId>::Ok(curr);
    }
    if (page.page_type() != PageType::kInternal) {
      return Status::Corruption("unexpected page type during B+ tree descent");
    }
    auto next_res = search_internal(page, key);
    guard.release();
    if (!next_res.ok()) {
      return next_res.status();
    }
    curr = next_res.value().child_page_id;
  }
  return Status::Corruption("cycle or excessive depth detected in B+ tree");
}

Result<std::string> BTree::get(std::span<const std::byte> key) {
  if (key.size() > kMaxKeySize) {
    return Status::InvalidArgument("get key exceeds kMaxKeySize");
  }
  auto leaf_id_res = find_leaf(key);
  if (!leaf_id_res.ok()) {
    return leaf_id_res.status();
  }
  auto fd_res = fetch_and_decode(leaf_id_res.value());
  if (!fd_res.ok()) {
    return fd_res.status();
  }
  auto [leaf_page, leaf_guard] = std::move(fd_res.value());
  auto search_res = search_leaf(leaf_page, key);
  if (!search_res.ok()) {
    return search_res.status();
  }
  if (!search_res.value().found) {
    return Status::NotFound("key not found");
  }
  auto payload_res = leaf_page.slot_payload(search_res.value().slot_index);
  if (!payload_res.ok()) {
    return payload_res.status();
  }
  auto rec_res = PageCodec::decode_leaf_record(payload_res.value());
  if (!rec_res.ok()) {
    return rec_res.status();
  }
  return Result<std::string>::Ok(bytes_to_string(rec_res.value().value));
}

Result<std::vector<KeyValue>> BTree::scan_bytes(
    std::optional<std::span<const std::byte>> start_key,
    std::optional<std::span<const std::byte>> end_key) {
  if (start_key.has_value() && start_key->size() > kMaxKeySize) {
    return Status::InvalidArgument("scan start_key exceeds kMaxKeySize");
  }
  if (end_key.has_value() && end_key->size() > kMaxKeySize) {
    return Status::InvalidArgument("scan end_key exceeds kMaxKeySize");
  }

  std::vector<KeyValue> results;
  if (start_key.has_value() && end_key.has_value() &&
      compare_bytes(*start_key, *end_key) >= 0) {
    return Result<std::vector<KeyValue>>::Ok(std::move(results));
  }

  const std::span<const std::byte> seek_key =
      start_key.has_value() ? *start_key : std::span<const std::byte>{};
  auto leaf_id_res = find_leaf(seek_key);
  if (!leaf_id_res.ok()) {
    return leaf_id_res.status();
  }

  PageId curr_leaf_id = leaf_id_res.value();
  bool first_leaf = true;
  const std::uint32_t max_leaves = allocator_.next_page_id().value;
  std::uint32_t visited_leaves = 0;

  while (curr_leaf_id.is_valid()) {
    if (++visited_leaves > max_leaves) {
      return Status::Corruption("cycle detected in leaf next_leaf chain");
    }
    auto fd_res = fetch_and_decode(curr_leaf_id);
    if (!fd_res.ok()) {
      return fd_res.status();
    }
    auto [leaf_page, leaf_guard] = std::move(fd_res.value());
    if (leaf_page.page_type() != PageType::kLeaf) {
      return Status::Corruption("leaf sibling chain points to non-leaf page");
    }

    std::uint16_t start_slot = 0;
    if (first_leaf && start_key.has_value()) {
      auto sr = search_leaf(leaf_page, *start_key);
      if (!sr.ok()) {
        return sr.status();
      }
      start_slot = sr.value().slot_index;
    }
    first_leaf = false;

    for (std::uint16_t i = start_slot; i < leaf_page.num_slots(); ++i) {
      auto payload_res = leaf_page.slot_payload(i);
      if (!payload_res.ok()) {
        return payload_res.status();
      }
      auto rec_res = PageCodec::decode_leaf_record(payload_res.value());
      if (!rec_res.ok()) {
        return rec_res.status();
      }
      const LeafRecordView& rec = rec_res.value();
      if (end_key.has_value() && compare_bytes(rec.key, *end_key) >= 0) {
        return Result<std::vector<KeyValue>>::Ok(std::move(results));
      }
      results.push_back(KeyValue{.key = bytes_to_string(rec.key),
                                 .value = bytes_to_string(rec.value)});
    }

    const PageId next_id = leaf_page.aux_page_id();
    leaf_guard.release();
    curr_leaf_id = next_id;
  }

  return Result<std::vector<KeyValue>>::Ok(std::move(results));
}

Status BTree::put(std::span<const std::byte> key,
                  std::span<const std::byte> value) {
  auto encoded_res = PageCodec::encode_leaf_record(key, value);
  if (!encoded_res.ok()) {
    return encoded_res.status();
  }
  const std::vector<std::byte>& encoded_record = encoded_res.value();

  std::vector<PathEntry> path;
  PageId curr = allocator_.root_page_id();
  const std::uint32_t max_steps = allocator_.next_page_id().value;

  for (std::uint32_t step = 0; step < max_steps; ++step) {
    auto fd_res = fetch_and_decode(curr);
    if (!fd_res.ok()) {
      return fd_res.status();
    }
    auto [page, guard] = std::move(fd_res.value());

    if (page.page_type() == PageType::kLeaf) {
      auto sr_res = search_leaf(page, key);
      if (!sr_res.ok()) {
        return sr_res.status();
      }
      const LeafSearchResult sr = sr_res.value();

      if (sr.found) {
        Status us = page.update_slot(sr.slot_index, encoded_record);
        if (us.ok()) {
          return encode_and_mark_dirty(page, guard);
        }
        if (us.code() != ErrorCode::kResourceExhausted) {
          return us;
        }
        // Update overflowed the leaf: build virtual vector of N records with
        // sr.slot_index replaced by encoded_record, then split.
        std::vector<std::vector<std::byte>> all_records;
        all_records.reserve(page.num_slots());
        for (std::uint16_t i = 0; i < page.num_slots(); ++i) {
          if (i == sr.slot_index) {
            all_records.push_back(encoded_record);
          } else {
            auto pl = page.slot_payload(i).value();
            all_records.emplace_back(pl.begin(), pl.end());
          }
        }
        return split_leaf_and_propagate(curr, page, guard, all_records, path);
      }

      // Inserting a new key at sr.slot_index.
      Status is = page.insert_slot(sr.slot_index, encoded_record);
      if (is.ok()) {
        return encode_and_mark_dirty(page, guard);
      }
      if (is.code() != ErrorCode::kResourceExhausted) {
        return is;
      }

      // Insert overflowed the leaf: build virtual vector of N + 1 records.
      std::vector<std::vector<std::byte>> all_records;
      all_records.reserve(static_cast<std::size_t>(page.num_slots()) + 1U);
      for (std::uint16_t i = 0; i < page.num_slots(); ++i) {
        if (i == sr.slot_index) {
          all_records.push_back(encoded_record);
        }
        auto pl = page.slot_payload(i).value();
        all_records.emplace_back(pl.begin(), pl.end());
      }
      if (sr.slot_index == page.num_slots()) {
        all_records.push_back(encoded_record);
      }
      return split_leaf_and_propagate(curr, page, guard, all_records, path);
    }

    if (page.page_type() != PageType::kInternal) {
      return Status::Corruption("unexpected page type during put traversal");
    }
    auto next_res = search_internal(page, key);
    guard.release();
    if (!next_res.ok()) {
      return next_res.status();
    }
    path.push_back(PathEntry{.page_id = curr,
                             .child_index = next_res.value().child_index});
    curr = next_res.value().child_page_id;
  }

  return Status::Corruption("cycle or excessive depth detected in put");
}

Status BTree::split_leaf_and_propagate(
    PageId leaf_id, SlottedPage& leaf_page, PageGuard& leaf_guard,
    const std::vector<std::vector<std::byte>>& all_records,
    const std::vector<PathEntry>& path) {
  const std::size_t m = all_records.size();
  assert(m >= 2);

  // Compute cumulative byte cost C[0..m] (including 4-byte slot directory entry).
  std::vector<std::size_t> cum_cost(m + 1, 0);
  for (std::size_t i = 0; i < m; ++i) {
    cum_cost[i + 1] = cum_cost[i] + all_records[i].size() + kSlotEntrySize;
  }
  const std::size_t total_cost = cum_cost[m];
  const std::size_t max_usable = kPageSize - kPageHeaderSize;

  // Choose split_idx in [1, m - 1] minimizing |left_cost - right_cost| while
  // ensuring both sides fit within max_usable.
  std::size_t best_k = 1;
  std::size_t best_diff = std::numeric_limits<std::size_t>::max();
  for (std::size_t k = 1; k < m; ++k) {
    const std::size_t left_cost = cum_cost[k];
    const std::size_t right_cost = total_cost - left_cost;
    if (left_cost > max_usable || right_cost > max_usable) {
      continue;
    }
    const std::size_t diff =
        (left_cost > right_cost) ? (left_cost - right_cost)
                                 : (right_cost - left_cost);
    if (diff < best_diff) {
      best_diff = diff;
      best_k = k;
    }
  }

  // Extract separator key = first key of the right leaf (all_records[best_k]).
  auto sep_rec_res = PageCodec::decode_leaf_record(all_records[best_k]);
  if (!sep_rec_res.ok()) {
    return sep_rec_res.status();
  }
  const std::vector<std::byte> promoted_key(sep_rec_res.value().key.begin(),
                                            sep_rec_res.value().key.end());

  const PageId old_next_leaf = leaf_page.aux_page_id();

  // Allocate new right sibling leaf with next_leaf_page_id = old_next_leaf.
  auto right_id_res = allocator_.allocate_page(PageType::kLeaf, old_next_leaf);
  if (!right_id_res.ok()) {
    return right_id_res.status();
  }
  const PageId right_leaf_id = right_id_res.value();

  // Populate right leaf in buffer pool and release its pin before parent work.
  {
    auto right_fd = fetch_and_decode(right_leaf_id);
    if (!right_fd.ok()) {
      return right_fd.status();
    }
    auto [right_page, right_guard] = std::move(right_fd.value());
    for (std::size_t i = best_k; i < m; ++i) {
      Status s = right_page.insert_slot(right_page.num_slots(), all_records[i]);
      if (!s.ok()) {
        return s;
      }
    }
    Status es = encode_and_mark_dirty(right_page, right_guard);
    if (!es.ok()) {
      return es;
    }
  }

  // Rebuild left leaf freshly compacted with aux_page_id = right_leaf_id.
  Status init_s = leaf_page.init(leaf_id, PageType::kLeaf, right_leaf_id);
  if (!init_s.ok()) {
    return init_s;
  }
  for (std::size_t i = 0; i < best_k; ++i) {
    Status s = leaf_page.insert_slot(leaf_page.num_slots(), all_records[i]);
    if (!s.ok()) {
      return s;
    }
  }
  Status left_es = encode_and_mark_dirty(leaf_page, leaf_guard);
  leaf_guard.release();
  if (!left_es.ok()) {
    return left_es;
  }

  return insert_into_parent(leaf_id, promoted_key, right_leaf_id, path,
                            path.size());
}

Status BTree::insert_into_parent(PageId left_child,
                                 std::span<const std::byte> separator_key,
                                 PageId right_child,
                                 const std::vector<PathEntry>& path,
                                 std::size_t path_depth) {
  // Base case: left_child was the root page -> allocate a new root internal node.
  if (path_depth == 0) {
    auto new_root_res =
        allocator_.allocate_page(PageType::kInternal, left_child);
    if (!new_root_res.ok()) {
      return new_root_res.status();
    }
    const PageId new_root_id = new_root_res.value();

    auto sep_payload_res =
        PageCodec::encode_internal_record(separator_key, right_child);
    if (!sep_payload_res.ok()) {
      return sep_payload_res.status();
    }

    {
      auto root_fd = fetch_and_decode(new_root_id);
      if (!root_fd.ok()) {
        return root_fd.status();
      }
      auto [root_page, root_guard] = std::move(root_fd.value());
      Status is = root_page.insert_slot(0, sep_payload_res.value());
      if (!is.ok()) {
        return is;
      }
      Status es = encode_and_mark_dirty(root_page, root_guard);
      if (!es.ok()) {
        return es;
      }
    }

    return allocator_.set_root_page_id(new_root_id);
  }

  const PathEntry& parent_entry = path[path_depth - 1];
  const PageId parent_id = parent_entry.page_id;
  const std::uint16_t insert_idx = parent_entry.child_index;

  auto sep_payload_res =
      PageCodec::encode_internal_record(separator_key, right_child);
  if (!sep_payload_res.ok()) {
    return sep_payload_res.status();
  }
  const std::vector<std::byte>& new_sep_payload = sep_payload_res.value();

  auto parent_fd = fetch_and_decode(parent_id);
  if (!parent_fd.ok()) {
    return parent_fd.status();
  }
  auto [parent_page, parent_guard] = std::move(parent_fd.value());

  if (insert_idx > parent_page.num_slots()) {
    return Status::Corruption("parent child_index out of range during split");
  }

  Status is = parent_page.insert_slot(insert_idx, new_sep_payload);
  if (is.ok()) {
    return encode_and_mark_dirty(parent_page, parent_guard);
  }
  if (is.code() != ErrorCode::kResourceExhausted) {
    return is;
  }

  // Parent internal node is full: build virtual sequence of M = num_slots + 1
  // internal records and split.
  std::vector<std::vector<std::byte>> all_records;
  all_records.reserve(static_cast<std::size_t>(parent_page.num_slots()) + 1U);
  for (std::uint16_t i = 0; i < parent_page.num_slots(); ++i) {
    if (i == insert_idx) {
      all_records.push_back(new_sep_payload);
    }
    auto pl = parent_page.slot_payload(i).value();
    all_records.emplace_back(pl.begin(), pl.end());
  }
  if (insert_idx == parent_page.num_slots()) {
    all_records.push_back(new_sep_payload);
  }

  const std::size_t m = all_records.size();
  assert(m >= 3);

  std::vector<std::size_t> cum_cost(m + 1, 0);
  for (std::size_t i = 0; i < m; ++i) {
    cum_cost[i + 1] = cum_cost[i] + all_records[i].size() + kSlotEntrySize;
  }
  const std::size_t max_usable = kPageSize - kPageHeaderSize;

  // Choose pushed-up index k in [1, m - 2] minimizing |left_cost - right_cost|.
  // Left receives [0..k-1] (cost cum_cost[k]), k is pushed up to grandparent,
  // Right receives [k+1..m-1] (cost cum_cost[m] - cum_cost[k+1]).
  std::size_t best_k = m / 2;
  std::size_t best_diff = std::numeric_limits<std::size_t>::max();
  for (std::size_t k = 1; k + 1 < m; ++k) {
    const std::size_t left_cost = cum_cost[k];
    const std::size_t right_cost = cum_cost[m] - cum_cost[k + 1];
    if (left_cost > max_usable || right_cost > max_usable) {
      continue;
    }
    const std::size_t diff =
        (left_cost > right_cost) ? (left_cost - right_cost)
                                 : (right_cost - left_cost);
    if (diff < best_diff) {
      best_diff = diff;
      best_k = k;
    }
  }

  auto pushed_rec_res = PageCodec::decode_internal_record(all_records[best_k]);
  if (!pushed_rec_res.ok()) {
    return pushed_rec_res.status();
  }
  const std::vector<std::byte> pushed_key(
      pushed_rec_res.value().separator_key.begin(),
      pushed_rec_res.value().separator_key.end());
  const PageId right_leftmost_child = pushed_rec_res.value().right_child;

  auto right_internal_res =
      allocator_.allocate_page(PageType::kInternal, right_leftmost_child);
  if (!right_internal_res.ok()) {
    return right_internal_res.status();
  }
  const PageId right_internal_id = right_internal_res.value();

  {
    auto right_fd = fetch_and_decode(right_internal_id);
    if (!right_fd.ok()) {
      return right_fd.status();
    }
    auto [right_page, right_guard] = std::move(right_fd.value());
    for (std::size_t i = best_k + 1; i < m; ++i) {
      Status s = right_page.insert_slot(right_page.num_slots(), all_records[i]);
      if (!s.ok()) {
        return s;
      }
    }
    Status es = encode_and_mark_dirty(right_page, right_guard);
    if (!es.ok()) {
      return es;
    }
  }

  const PageId left_leftmost_child = parent_page.aux_page_id();
  Status init_s =
      parent_page.init(parent_id, PageType::kInternal, left_leftmost_child);
  if (!init_s.ok()) {
    return init_s;
  }
  for (std::size_t i = 0; i < best_k; ++i) {
    Status s = parent_page.insert_slot(parent_page.num_slots(), all_records[i]);
    if (!s.ok()) {
      return s;
    }
  }
  Status left_es = encode_and_mark_dirty(parent_page, parent_guard);
  parent_guard.release();
  if (!left_es.ok()) {
    return left_es;
  }

  return insert_into_parent(parent_id, pushed_key, right_internal_id, path,
                            path_depth - 1);
}

Result<bool> BTree::del(std::span<const std::byte> key) {
  if (key.size() > kMaxKeySize) {
    return Status::InvalidArgument("del key exceeds kMaxKeySize");
  }

  std::vector<PathEntry> path;
  PageId curr = allocator_.root_page_id();
  const std::uint32_t max_steps = allocator_.next_page_id().value;

  for (std::uint32_t step = 0; step < max_steps; ++step) {
    auto fd_res = fetch_and_decode(curr);
    if (!fd_res.ok()) {
      return fd_res.status();
    }
    auto [page, guard] = std::move(fd_res.value());

    if (page.page_type() == PageType::kLeaf) {
      auto sr_res = search_leaf(page, key);
      if (!sr_res.ok()) {
        return sr_res.status();
      }
      if (!sr_res.value().found) {
        return Result<bool>::Ok(false);
      }

      Status es = page.erase_slot(sr_res.value().slot_index);
      if (!es.ok()) {
        return es;
      }
      Status enc_s = encode_and_mark_dirty(page, guard);
      const std::size_t remaining_bytes = used_bytes(page);
      guard.release();
      if (!enc_s.ok()) {
        return enc_s;
      }

      if (!path.empty() && remaining_bytes < kMinPageFillBytes) {
        Status rb_s = rebalance_after_delete(curr, path);
        if (!rb_s.ok()) {
          return rb_s;
        }
      }
      return Result<bool>::Ok(true);
    }

    if (page.page_type() != PageType::kInternal) {
      return Status::Corruption("unexpected page type during del traversal");
    }
    auto next_res = search_internal(page, key);
    guard.release();
    if (!next_res.ok()) {
      return next_res.status();
    }
    path.push_back(PathEntry{.page_id = curr,
                             .child_index = next_res.value().child_index});
    curr = next_res.value().child_page_id;
  }

  return Status::Corruption("cycle or excessive depth detected in del");
}

Status BTree::rebalance_after_delete(PageId underfull_page_id,
                                     const std::vector<PathEntry>& path) {
  PageId curr_id = underfull_page_id;
  std::size_t depth = path.size();

  while (depth > 0) {
    --depth;
    const PageId parent_id = path[depth].page_id;
    const std::uint16_t c = path[depth].child_index;

    auto parent_fd = fetch_and_decode(parent_id);
    if (!parent_fd.ok()) {
      return parent_fd.status();
    }
    auto [parent_page, parent_guard] = std::move(parent_fd.value());

    if (parent_page.num_slots() == 0) {
      parent_guard.release();
      break;
    }

    struct SiblingCandidate {
      PageId left_id{kInvalidPageId};
      PageId right_id{kInvalidPageId};
      std::uint16_t sep_slot{0};
      bool curr_is_left{false};
    };

    std::vector<SiblingCandidate> candidates;
    if (c < parent_page.num_slots()) {
      auto rec_res = PageCodec::decode_internal_record(
          parent_page.slot_payload(c).value());
      if (!rec_res.ok()) {
        return rec_res.status();
      }
      candidates.push_back(
          SiblingCandidate{.left_id = curr_id,
                           .right_id = rec_res.value().right_child,
                           .sep_slot = c,
                           .curr_is_left = true});
    }
    if (c > 0) {
      PageId left_sib = parent_page.aux_page_id();
      if (c >= 2) {
        auto rec_res = PageCodec::decode_internal_record(
            parent_page.slot_payload(static_cast<std::uint16_t>(c - 2U))
                .value());
        if (!rec_res.ok()) {
          return rec_res.status();
        }
        left_sib = rec_res.value().right_child;
      }
      candidates.push_back(SiblingCandidate{
          .left_id = left_sib,
          .right_id = curr_id,
          .sep_slot = static_cast<std::uint16_t>(c - 1U),
          .curr_is_left = false});
    }

    // 1. Try merging with a sibling (strictly shrinks parent_page).
    bool merged = false;
    std::size_t parent_used_after_merge = 0;

    for (const SiblingCandidate& cand : candidates) {
      auto sep_rec_res = PageCodec::decode_internal_record(
          parent_page.slot_payload(cand.sep_slot).value());
      if (!sep_rec_res.ok()) {
        return sep_rec_res.status();
      }
      const InternalRecordView sep_rec = sep_rec_res.value();

      auto left_fd = fetch_and_decode(cand.left_id);
      if (!left_fd.ok()) {
        return left_fd.status();
      }
      auto [left_page, left_guard] = std::move(left_fd.value());

      auto right_fd = fetch_and_decode(cand.right_id);
      if (!right_fd.ok()) {
        return right_fd.status();
      }
      auto [right_page, right_guard] = std::move(right_fd.value());

      const std::size_t max_usable = kPageSize - kPageHeaderSize;
      bool can_merge = false;
      if (left_page.page_type() == PageType::kLeaf) {
        can_merge = (used_bytes(left_page) + used_bytes(right_page) <= max_usable);
      } else {
        const std::size_t pull_cost =
            kSlotEntrySize + 6U + sep_rec.separator_key.size();
        can_merge = (used_bytes(left_page) + used_bytes(right_page) + pull_cost <=
                     max_usable);
      }

      if (!can_merge) {
        right_guard.release();
        left_guard.release();
        continue;
      }

      // Execute merge: left_page absorbs right_page.
      if (left_page.page_type() == PageType::kLeaf) {
        for (std::uint16_t i = 0; i < right_page.num_slots(); ++i) {
          Status s = left_page.insert_slot(
              left_page.num_slots(), right_page.slot_payload(i).value());
          if (!s.ok()) {
            return s;
          }
        }
        left_page.set_aux_page_id(right_page.aux_page_id());
      } else {
        auto pulled_res = PageCodec::encode_internal_record(
            sep_rec.separator_key, right_page.aux_page_id());
        if (!pulled_res.ok()) {
          return pulled_res.status();
        }
        Status s =
            left_page.insert_slot(left_page.num_slots(), pulled_res.value());
        if (!s.ok()) {
          return s;
        }
        for (std::uint16_t i = 0; i < right_page.num_slots(); ++i) {
          s = left_page.insert_slot(left_page.num_slots(),
                                    right_page.slot_payload(i).value());
          if (!s.ok()) {
            return s;
          }
        }
      }

      Status left_es = encode_and_mark_dirty(left_page, left_guard);
      left_guard.release();
      right_guard.release();
      if (!left_es.ok()) {
        return left_es;
      }

      // Discard right_id from BufferPool and return it to PageAllocator free list.
      Status disc_s = pool_.discard_page(cand.right_id);
      if (!disc_s.ok()) {
        return disc_s;
      }
      Status free_s = allocator_.free_page(cand.right_id);
      if (!free_s.ok()) {
        return free_s;
      }

      Status erase_s = parent_page.erase_slot(cand.sep_slot);
      if (!erase_s.ok()) {
        return erase_s;
      }
      Status parent_es = encode_and_mark_dirty(parent_page, parent_guard);
      parent_used_after_merge = used_bytes(parent_page);
      parent_guard.release();
      if (!parent_es.ok()) {
        return parent_es;
      }

      merged = true;
      break;
    }

    if (merged) {
      if (depth > 0 && parent_used_after_merge < kMinPageFillBytes) {
        curr_id = parent_id;
        continue;
      }
      break;
    }

    // 2. Merge did not fit; try conservative redistribution (only when the
    // replacement separator key fits in parent_page without splitting).
    for (const SiblingCandidate& cand : candidates) {
      auto sep_rec_res = PageCodec::decode_internal_record(
          parent_page.slot_payload(cand.sep_slot).value());
      if (!sep_rec_res.ok()) {
        return sep_rec_res.status();
      }
      const std::vector<std::byte> old_sep_key(
          sep_rec_res.value().separator_key.begin(),
          sep_rec_res.value().separator_key.end());

      auto left_fd = fetch_and_decode(cand.left_id);
      if (!left_fd.ok()) {
        return left_fd.status();
      }
      auto [left_page, left_guard] = std::move(left_fd.value());

      auto right_fd = fetch_and_decode(cand.right_id);
      if (!right_fd.ok()) {
        return right_fd.status();
      }
      auto [right_page, right_guard] = std::move(right_fd.value());

      const SlottedPage& donor = cand.curr_is_left ? right_page : left_page;
      if (donor.num_slots() < 2) {
        right_guard.release();
        left_guard.release();
        continue;
      }

      if (left_page.page_type() == PageType::kLeaf) {
        if (cand.curr_is_left) {
          // Borrow slot 0 from right_page into end of left_page.
          const auto slot0_pl = right_page.slot_payload(0).value();
          const std::vector<std::byte> borrowed_rec(slot0_pl.begin(),
                                                    slot0_pl.end());
          const auto slot1_rec =
              PageCodec::decode_leaf_record(right_page.slot_payload(1).value())
                  .value();
          const std::vector<std::byte> new_sep_key(slot1_rec.key.begin(),
                                                   slot1_rec.key.end());

          if (left_page.usable_free_space() <
                  borrowed_rec.size() + kSlotEntrySize ||
              parent_page.usable_free_space() + old_sep_key.size() <
                  new_sep_key.size()) {
            right_guard.release();
            left_guard.release();
            continue;
          }

          auto new_sep_pl =
              PageCodec::encode_internal_record(new_sep_key, cand.right_id);
          if (!new_sep_pl.ok()) {
            return new_sep_pl.status();
          }
          Status s = left_page.insert_slot(left_page.num_slots(), borrowed_rec);
          if (!s.ok()) {
            return s;
          }
          s = right_page.erase_slot(0);
          if (!s.ok()) {
            return s;
          }
          s = parent_page.update_slot(cand.sep_slot, new_sep_pl.value());
          if (!s.ok()) {
            return s;
          }
        } else {
          // Borrow last slot from left_page into slot 0 of right_page.
          const auto last_idx =
              static_cast<std::uint16_t>(left_page.num_slots() - 1U);
          const auto last_pl = left_page.slot_payload(last_idx).value();
          const std::vector<std::byte> borrowed_rec(last_pl.begin(),
                                                    last_pl.end());
          const auto last_rec =
              PageCodec::decode_leaf_record(borrowed_rec).value();
          const std::vector<std::byte> new_sep_key(last_rec.key.begin(),
                                                   last_rec.key.end());

          if (right_page.usable_free_space() <
                  borrowed_rec.size() + kSlotEntrySize ||
              parent_page.usable_free_space() + old_sep_key.size() <
                  new_sep_key.size()) {
            right_guard.release();
            left_guard.release();
            continue;
          }

          auto new_sep_pl =
              PageCodec::encode_internal_record(new_sep_key, cand.right_id);
          if (!new_sep_pl.ok()) {
            return new_sep_pl.status();
          }
          Status s = right_page.insert_slot(0, borrowed_rec);
          if (!s.ok()) {
            return s;
          }
          s = left_page.erase_slot(last_idx);
          if (!s.ok()) {
            return s;
          }
          s = parent_page.update_slot(cand.sep_slot, new_sep_pl.value());
          if (!s.ok()) {
            return s;
          }
        }
      } else {
        // Internal node redistribution.
        if (cand.curr_is_left) {
          // Pull down old_sep_key into end of left_page (right_child =
          // right_page.aux_page_id()), and push up slot 0 of right_page.
          const auto slot0_rec = PageCodec::decode_internal_record(
                                     right_page.slot_payload(0).value())
                                     .value();
          const std::vector<std::byte> new_sep_key(
              slot0_rec.separator_key.begin(), slot0_rec.separator_key.end());
          const PageId new_right_aux = slot0_rec.right_child;

          const std::size_t pull_cost =
              kSlotEntrySize + 6U + old_sep_key.size();
          if (left_page.usable_free_space() < pull_cost ||
              parent_page.usable_free_space() + old_sep_key.size() <
                  new_sep_key.size()) {
            right_guard.release();
            left_guard.release();
            continue;
          }

          auto pulled_pl = PageCodec::encode_internal_record(
              old_sep_key, right_page.aux_page_id());
          if (!pulled_pl.ok()) {
            return pulled_pl.status();
          }
          auto new_sep_pl =
              PageCodec::encode_internal_record(new_sep_key, cand.right_id);
          if (!new_sep_pl.ok()) {
            return new_sep_pl.status();
          }

          Status s =
              left_page.insert_slot(left_page.num_slots(), pulled_pl.value());
          if (!s.ok()) {
            return s;
          }
          s = right_page.erase_slot(0);
          if (!s.ok()) {
            return s;
          }
          right_page.set_aux_page_id(new_right_aux);
          s = parent_page.update_slot(cand.sep_slot, new_sep_pl.value());
          if (!s.ok()) {
            return s;
          }
        } else {
          // Pull down old_sep_key into slot 0 of right_page (right_child =
          // right_page.aux_page_id()), and push up last slot of left_page.
          const auto last_idx =
              static_cast<std::uint16_t>(left_page.num_slots() - 1U);
          const auto last_rec = PageCodec::decode_internal_record(
                                    left_page.slot_payload(last_idx).value())
                                    .value();
          const std::vector<std::byte> new_sep_key(
              last_rec.separator_key.begin(), last_rec.separator_key.end());
          const PageId borrowed_child = last_rec.right_child;

          const std::size_t pull_cost =
              kSlotEntrySize + 6U + old_sep_key.size();
          if (right_page.usable_free_space() < pull_cost ||
              parent_page.usable_free_space() + old_sep_key.size() <
                  new_sep_key.size()) {
            right_guard.release();
            left_guard.release();
            continue;
          }

          auto pulled_pl = PageCodec::encode_internal_record(
              old_sep_key, right_page.aux_page_id());
          if (!pulled_pl.ok()) {
            return pulled_pl.status();
          }
          auto new_sep_pl =
              PageCodec::encode_internal_record(new_sep_key, cand.right_id);
          if (!new_sep_pl.ok()) {
            return new_sep_pl.status();
          }

          Status s = right_page.insert_slot(0, pulled_pl.value());
          if (!s.ok()) {
            return s;
          }
          right_page.set_aux_page_id(borrowed_child);
          s = left_page.erase_slot(last_idx);
          if (!s.ok()) {
            return s;
          }
          s = parent_page.update_slot(cand.sep_slot, new_sep_pl.value());
          if (!s.ok()) {
            return s;
          }
        }
      }

      Status s1 = encode_and_mark_dirty(left_page, left_guard);
      Status s2 = encode_and_mark_dirty(right_page, right_guard);
      Status s3 = encode_and_mark_dirty(parent_page, parent_guard);
      right_guard.release();
      left_guard.release();
      parent_guard.release();
      if (!s1.ok()) {
        return s1;
      }
      if (!s2.ok()) {
        return s2;
      }
      if (!s3.ok()) {
        return s3;
      }
      break;
    }

    parent_guard.release();
    break;
  }

  return collapse_unary_root_chain();
}

Status BTree::collapse_unary_root_chain() {
  const std::uint32_t max_steps = allocator_.next_page_id().value;
  for (std::uint32_t step = 0; step < max_steps; ++step) {
    const PageId root_id = allocator_.root_page_id();
    auto root_fd = fetch_and_decode(root_id);
    if (!root_fd.ok()) {
      return root_fd.status();
    }
    auto [root_page, root_guard] = std::move(root_fd.value());
    if (root_page.page_type() != PageType::kInternal ||
        root_page.num_slots() > 0) {
      return Status::Ok();
    }
    const PageId promoted_child = root_page.aux_page_id();
    root_guard.release();

    Status s = allocator_.set_root_page_id(promoted_child);
    if (!s.ok()) {
      return s;
    }
    s = pool_.discard_page(root_id);
    if (!s.ok()) {
      return s;
    }
    s = allocator_.free_page(root_id);
    if (!s.ok()) {
      return s;
    }
  }
  return Status::Corruption("cycle detected while collapsing unary root chain");
}

Result<std::uint32_t> BTree::height() const {
  PageId curr = allocator_.root_page_id();
  const std::uint32_t max_steps = allocator_.next_page_id().value;
  std::uint32_t h = 0;
  for (std::uint32_t step = 0; step < max_steps; ++step) {
    ++h;
    auto fd_res = fetch_and_decode(curr);
    if (!fd_res.ok()) {
      return fd_res.status();
    }
    auto [page, guard] = std::move(fd_res.value());
    if (page.page_type() == PageType::kLeaf) {
      return Result<std::uint32_t>::Ok(h);
    }
    if (page.page_type() != PageType::kInternal) {
      return Status::Corruption("invalid page type in height()");
    }
    curr = page.aux_page_id();
  }
  return Status::Corruption("cycle detected in height()");
}

Status BTree::verify() const {
  const PageId root_id = allocator_.root_page_id();
  const std::uint32_t total_pages = allocator_.next_page_id().value;
  if (!root_id.is_valid() || root_id == kSuperblockPageId ||
      root_id.value >= total_pages) {
    return Status::Corruption("BTree::verify invalid root_page_id");
  }

  std::unordered_set<std::uint32_t> visited_tree_pages;
  std::vector<PageId> dfs_leaf_order;
  std::optional<std::uint32_t> expected_leaf_depth;

  struct VerifyFrame {
    PageId page_id;
    std::uint32_t depth;
    std::optional<std::vector<std::byte>> lower_fence;  // inclusive
    std::optional<std::vector<std::byte>> upper_fence;  // exclusive
  };

  std::vector<VerifyFrame> stack;
  stack.push_back(VerifyFrame{.page_id = root_id,
                              .depth = 1,
                              .lower_fence = std::nullopt,
                              .upper_fence = std::nullopt});

  while (!stack.empty()) {
    VerifyFrame vf = std::move(stack.back());
    stack.pop_back();

    if (!vf.page_id.is_valid() || vf.page_id == kSuperblockPageId ||
        vf.page_id.value >= total_pages) {
      return Status::Corruption("B+ tree child PageId out of allocated bounds");
    }
    if (!visited_tree_pages.insert(vf.page_id.value).second) {
      return Status::Corruption(
          "B+ tree contains cycle or shared child pointer");
    }

    auto guard_res = pool_.fetch_page(vf.page_id);
    if (!guard_res.ok()) {
      return guard_res.status();
    }
    PageGuard guard = std::move(guard_res.value());

    Status layout_s =
        PageCodec::validate_btree_page_layout(guard.bytes(), vf.page_id);
    if (!layout_s.ok()) {
      return layout_s;
    }

    auto sp_res = SlottedPage::decode(guard.bytes(), vf.page_id);
    if (!sp_res.ok()) {
      return sp_res.status();
    }
    SlottedPage page = std::move(sp_res.value());
    guard.release();

    if (page.page_type() == PageType::kLeaf) {
      if (!expected_leaf_depth.has_value()) {
        expected_leaf_depth = vf.depth;
      } else if (*expected_leaf_depth != vf.depth) {
        return Status::Corruption("B+ tree leaves do not have uniform depth");
      }

      for (std::uint16_t i = 0; i < page.num_slots(); ++i) {
        const auto rec =
            PageCodec::decode_leaf_record(page.slot_payload(i).value()).value();
        if (vf.lower_fence.has_value() &&
            compare_bytes(rec.key, *vf.lower_fence) < 0) {
          return Status::Corruption(
              "leaf key violates parent lower separator fence");
        }
        if (vf.upper_fence.has_value() &&
            compare_bytes(rec.key, *vf.upper_fence) >= 0) {
          return Status::Corruption(
              "leaf key violates parent upper separator fence");
        }
      }
      dfs_leaf_order.push_back(vf.page_id);
      continue;
    }

    if (page.page_type() != PageType::kInternal) {
      return Status::Corruption("non-Leaf/non-Internal page reachable in B+ tree");
    }
    if (vf.page_id == root_id && page.num_slots() == 0) {
      return Status::Corruption("root internal page must not be unary (0 slots)");
    }

    // Collect separators and children, then push onto stack in reverse order
    // so dfs_leaf_order visits leaves left-to-right.
    std::vector<std::vector<std::byte>> sep_keys;
    std::vector<PageId> children;
    sep_keys.reserve(page.num_slots());
    children.reserve(static_cast<std::size_t>(page.num_slots()) + 1U);
    children.push_back(page.aux_page_id());

    for (std::uint16_t i = 0; i < page.num_slots(); ++i) {
      const auto rec =
          PageCodec::decode_internal_record(page.slot_payload(i).value())
              .value();
      if (vf.lower_fence.has_value() &&
          compare_bytes(rec.separator_key, *vf.lower_fence) < 0) {
        return Status::Corruption(
            "internal separator key violates lower fence");
      }
      if (vf.upper_fence.has_value() &&
          compare_bytes(rec.separator_key, *vf.upper_fence) >= 0) {
        return Status::Corruption(
            "internal separator key violates upper fence");
      }
      sep_keys.emplace_back(rec.separator_key.begin(),
                            rec.separator_key.end());
      children.push_back(rec.right_child);
    }

    for (std::size_t c = children.size(); c > 0; --c) {
      const std::size_t idx = c - 1;
      std::optional<std::vector<std::byte>> child_lower =
          (idx == 0) ? vf.lower_fence : sep_keys[idx - 1];
      std::optional<std::vector<std::byte>> child_upper =
          (idx == sep_keys.size()) ? vf.upper_fence : sep_keys[idx];
      stack.push_back(VerifyFrame{.page_id = children[idx],
                                  .depth = vf.depth + 1U,
                                  .lower_fence = std::move(child_lower),
                                  .upper_fence = std::move(child_upper)});
    }
  }

  // Verify leaf next_leaf_page_id sibling chain matches DFS left-to-right order.
  if (dfs_leaf_order.empty()) {
    return Status::Corruption("B+ tree has zero leaves");
  }
  for (std::size_t i = 0; i < dfs_leaf_order.size(); ++i) {
    auto fd = fetch_and_decode(dfs_leaf_order[i]);
    if (!fd.ok()) {
      return fd.status();
    }
    const PageId next_leaf = fd.value().first.aux_page_id();
    const PageId expected_next =
        (i + 1 < dfs_leaf_order.size()) ? dfs_leaf_order[i + 1] : kInvalidPageId;
    if (next_leaf != expected_next) {
      return Status::Corruption(
          "leaf next_leaf_page_id chain does not match tree leaf order");
    }
  }

  // Verify free-list pages are disjoint from B+ tree pages and together
  // partition [1, total_pages).
  std::unordered_set<std::uint32_t> free_pages;
  PageId free_curr = allocator_.free_list_head();
  while (free_curr.is_valid()) {
    if (free_curr == kSuperblockPageId || free_curr.value >= total_pages) {
      return Status::Corruption("free list PageId out of bounds");
    }
    if (!free_pages.insert(free_curr.value).second) {
      return Status::Corruption("cycle in free list");
    }
    if (visited_tree_pages.contains(free_curr.value)) {
      return Status::Corruption(
          "PageId is simultaneously live in B+ tree and on free list");
    }
    // A freed page must never remain resident in BufferPool.
    if (pool_.is_resident(free_curr)) {
      return Status::Corruption("free-list page is still resident in BufferPool");
    }
    auto next_res = allocator_.read_next_free_page(free_curr);
    if (!next_res.ok()) {
      return next_res.status();
    }
    free_curr = next_res.value();
  }

  if (visited_tree_pages.size() + free_pages.size() !=
      static_cast<std::size_t>(total_pages - 1U)) {
    return Status::Corruption(
        "page leak detected: live B+ tree pages + free list != allocated pages");
  }

  return pool_.verify_invariants();
}

}  // namespace pagedb
