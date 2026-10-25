#include "pagedb/page_allocator.h"

#include <limits>
#include <unordered_set>

namespace pagedb {
namespace {

Status encode_blank_allocated_page(PageId page_id, PageType page_type,
                                   PageId aux_page_id,
                                   std::span<std::byte, kPageSize> out_page) {
  if (page_type == PageType::kFree) {
    return PageCodec::encode_free_page(page_id, aux_page_id, kInvalidLsn,
                                       out_page);
  }
  SlottedPage sp;
  Status s = sp.init(page_id, page_type, aux_page_id);
  if (!s.ok()) {
    return s;
  }
  return sp.encode(out_page);
}

}  // namespace

Result<PageId> PageAllocator::allocate_page(PageType page_type,
                                            PageId aux_page_id) {
  if (page_type != PageType::kLeaf && page_type != PageType::kInternal &&
      page_type != PageType::kFree) {
    return Status::InvalidArgument(
        "allocate_page only supports Leaf, Internal, or Free pages");
  }

  // 1. Reuse the head of the free list if available.
  if (superblock_.free_list_head.is_valid()) {
    const PageId reused_id = superblock_.free_list_head;
    PageBuffer free_buf{};
    Status rs = store_.read_page(reused_id, free_buf);
    if (!rs.ok()) {
      return rs;
    }
    auto next_free_res = PageCodec::decode_free_page(free_buf, reused_id);
    if (!next_free_res.ok()) {
      return next_free_res.status();
    }
    const PageId next_free = next_free_res.value();
    if (next_free.is_valid() &&
        (next_free == kSuperblockPageId ||
         next_free == superblock_.root_page_id ||
         next_free.value >= superblock_.next_page_id.value)) {
      return Status::Corruption("corrupt next_free_page pointer in free list");
    }

    PageBuffer blank_page{};
    Status es =
        encode_blank_allocated_page(reused_id, page_type, aux_page_id, blank_page);
    if (!es.ok()) {
      return es;
    }
    Status ws = store_.write_page(reused_id, blank_page);
    if (!ws.ok()) {
      return ws;
    }

    superblock_.free_list_head = next_free;
    Status sb_s = flush_superblock();
    if (!sb_s.ok()) {
      return sb_s;
    }
    return Result<PageId>::Ok(reused_id);
  }

  // 2. Otherwise append a new page at next_page_id.
  const PageId new_id = superblock_.next_page_id;
  if (!new_id.is_valid() ||
      new_id.value >= std::numeric_limits<std::uint32_t>::max() - 1U) {
    return Status::ResourceExhausted("PageId space exhausted");
  }

  PageBuffer blank_page{};
  Status es =
      encode_blank_allocated_page(new_id, page_type, aux_page_id, blank_page);
  if (!es.ok()) {
    return es;
  }

  Status ws = store_.write_page(new_id, blank_page);
  if (!ws.ok()) {
    return ws;
  }

  superblock_.next_page_id = PageId(new_id.value + 1U);
  Status sb_s = flush_superblock();
  if (!sb_s.ok()) {
    return sb_s;
  }
  return Result<PageId>::Ok(new_id);
}

Status PageAllocator::free_page(PageId page_id) {
  if (!page_id.is_valid() || page_id == kSuperblockPageId ||
      page_id == superblock_.root_page_id ||
      page_id.value >= superblock_.next_page_id.value) {
    return Status::InvalidArgument(
        "cannot free superblock, current root, or out-of-range PageId");
  }

  // Walk existing free list to reject double-free or free-list cycles.
  PageId curr = superblock_.free_list_head;
  std::uint32_t steps = 0;
  PageBuffer buf{};
  while (curr.is_valid()) {
    if (curr == page_id) {
      return Status::InvalidArgument("double-free of PageId already on free list");
    }
    if (++steps > superblock_.next_page_id.value) {
      return Status::Corruption("cycle detected in free list during free_page");
    }
    Status rs = store_.read_page(curr, buf);
    if (!rs.ok()) {
      return rs;
    }
    auto next_res = PageCodec::decode_free_page(buf, curr);
    if (!next_res.ok()) {
      return next_res.status();
    }
    curr = next_res.value();
  }

  PageBuffer free_page_buf{};
  Status es = PageCodec::encode_free_page(page_id, superblock_.free_list_head,
                                          kInvalidLsn, free_page_buf);
  if (!es.ok()) {
    return es;
  }
  Status ws = store_.write_page(page_id, free_page_buf);
  if (!ws.ok()) {
    return ws;
  }

  superblock_.free_list_head = page_id;
  return flush_superblock();
}

Status PageAllocator::set_root_page_id(PageId new_root) {
  if (!new_root.is_valid() || new_root == kSuperblockPageId ||
      new_root.value >= superblock_.next_page_id.value ||
      new_root == superblock_.free_list_head) {
    return Status::InvalidArgument("set_root_page_id out of allocated bounds");
  }
  superblock_.root_page_id = new_root;
  return flush_superblock();
}

Result<PageId> PageAllocator::read_next_free_page(PageId free_page_id) const {
  PageBuffer buf{};
  Status rs = store_.read_page(free_page_id, buf);
  if (!rs.ok()) {
    return rs;
  }
  return PageCodec::decode_free_page(buf, free_page_id);
}

Status PageAllocator::flush_superblock() {
  PageBuffer sb_page{};
  Status es = PageCodec::encode_superblock(superblock_, sb_page);
  if (!es.ok()) {
    return es;
  }
  return store_.write_page(kSuperblockPageId, sb_page);
}

Status PageAllocator::verify_allocation_state() const {
  if (superblock_.next_page_id.value < 2 ||
      superblock_.next_page_id.value != store_.page_count()) {
    return Status::Corruption(
        "allocator next_page_id does not match PageStore page_count");
  }
  if (!superblock_.root_page_id.is_valid() ||
      superblock_.root_page_id == kSuperblockPageId ||
      superblock_.root_page_id.value >= superblock_.next_page_id.value) {
    return Status::Corruption("allocator root_page_id is out of bounds");
  }

  PageBuffer buf{};
  for (std::uint32_t p = 0; p < superblock_.next_page_id.value; ++p) {
    Status rs = store_.read_page(PageId(p), buf);
    if (!rs.ok()) {
      return rs;
    }
  }

  // Verify free-list chain integrity and absence of cycles.
  std::unordered_set<std::uint32_t> seen_free;
  PageId curr = superblock_.free_list_head;
  while (curr.is_valid()) {
    if (curr == kSuperblockPageId || curr == superblock_.root_page_id ||
        curr.value >= superblock_.next_page_id.value) {
      return Status::Corruption("free list references invalid PageId");
    }
    if (!seen_free.insert(curr.value).second) {
      return Status::Corruption("cycle detected in allocator free list");
    }
    Status rs = store_.read_page(curr, buf);
    if (!rs.ok()) {
      return rs;
    }
    auto next_res = PageCodec::decode_free_page(buf, curr);
    if (!next_res.ok()) {
      return next_res.status();
    }
    curr = next_res.value();
  }
  return Status::Ok();
}

}  // namespace pagedb
