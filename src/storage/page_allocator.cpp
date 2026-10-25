#include "pagedb/page_allocator.h"

#include <limits>

namespace pagedb {

Result<PageId> PageAllocator::allocate_page(PageType page_type,
                                            PageId aux_page_id) {
  if (page_type != PageType::kLeaf && page_type != PageType::kInternal &&
      page_type != PageType::kFree) {
    return Status::InvalidArgument(
        "allocate_page only supports Leaf, Internal, or Free pages");
  }

  const PageId new_id = superblock_.next_page_id;
  if (!new_id.is_valid() ||
      new_id.value >= std::numeric_limits<std::uint32_t>::max() - 1U) {
    return Status::ResourceExhausted("PageId space exhausted");
  }

  PageBuffer blank_page{};
  if (page_type == PageType::kFree) {
    Status s =
        PageCodec::encode_free_page(new_id, aux_page_id, kInvalidLsn, blank_page);
    if (!s.ok()) {
      return s;
    }
  } else {
    SlottedPage sp;
    Status s = sp.init(new_id, page_type, aux_page_id);
    if (!s.ok()) {
      return s;
    }
    s = sp.encode(blank_page);
    if (!s.ok()) {
      return s;
    }
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

Status PageAllocator::set_root_page_id(PageId new_root) {
  if (!new_root.is_valid() || new_root == kSuperblockPageId ||
      new_root.value >= superblock_.next_page_id.value) {
    return Status::InvalidArgument("set_root_page_id out of allocated bounds");
  }
  superblock_.root_page_id = new_root;
  return flush_superblock();
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
  return Status::Ok();
}

}  // namespace pagedb
