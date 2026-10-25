#include "pagedb/page_store.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "pagedb/file_handle.h"
#include "pagedb/page_allocator.h"
#include "pagedb/page_codec.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(InitializeCloseReopenAndDecodePages) {
  test::TempDir tmp("pagedb_store_init");
  const auto db_dir = tmp.path() / "db";

  {
    auto store_res = PageStore::open(db_dir);
    PAGEDB_ASSERT_OK(store_res);
    PageStore& store = *store_res.value();

    PAGEDB_ASSERT_EQ(store.page_count(), 2U);
    PAGEDB_ASSERT_EQ(store.superblock().root_page_id, kInitialRootPageId);
    PAGEDB_ASSERT_EQ(store.superblock().next_page_id, PageId(2));

    // Concurrent open on the same directory must fail with AlreadyOpen.
    auto dup_open = PageStore::open(db_dir);
    PAGEDB_ASSERT_FALSE(dup_open.ok());
    PAGEDB_ASSERT_EQ(dup_open.code(), ErrorCode::kAlreadyOpen);

    PAGEDB_ASSERT_OK(store.close());
  }

  // Reopen existing database and decode Page 0 and Page 1.
  {
    PageStoreOptions opts{};
    opts.create_if_missing = false;
    auto store_res = PageStore::open(db_dir, opts);
    PAGEDB_ASSERT_OK(store_res);
    PageStore& store = *store_res.value();

    PageBuffer p0{};
    PageBuffer p1{};
    PAGEDB_ASSERT_OK(store.read_page(kSuperblockPageId, p0));
    PAGEDB_ASSERT_OK(store.read_page(kInitialRootPageId, p1));

    auto sb = PageCodec::decode_superblock(p0);
    PAGEDB_ASSERT_OK(sb);
    PAGEDB_ASSERT_EQ(sb.value().root_page_id, kInitialRootPageId);
    PAGEDB_ASSERT_EQ(sb.value().next_page_id, PageId(2));

    PAGEDB_ASSERT_OK(
        PageCodec::validate_btree_page_layout(p1, kInitialRootPageId));
  }
}

PAGEDB_TEST(RejectPartialTrailingPageOnOpen) {
  test::TempDir tmp("pagedb_store_partial");
  const auto db_dir = tmp.path() / "db";

  {
    auto store_res = PageStore::open(db_dir);
    PAGEDB_ASSERT_OK(store_res);
    PAGEDB_ASSERT_OK(store_res.value()->close());
  }

  // Append 128 trailing garbage bytes to pagedb.data.
  {
    auto fh = FileHandle::open(db_dir / "pagedb.data", OpenMode::kReadWrite);
    PAGEDB_ASSERT_OK(fh);
    std::array<std::byte, 128> trailing{};
    PAGEDB_ASSERT_OK(fh.value().write_exact_at(2ULL * kPageSize, trailing));
    PAGEDB_ASSERT_OK(fh.value().close());
  }

  auto reopen_res = PageStore::open(db_dir);
  PAGEDB_ASSERT_FALSE(reopen_res.ok());
  PAGEDB_ASSERT_EQ(reopen_res.code(), ErrorCode::kCorruption);
}

PAGEDB_TEST(PageAllocatorLifecycleRootUpdateAndPersistence) {
  test::TempDir tmp("pagedb_allocator");
  const auto db_dir = tmp.path() / "db";

  {
    auto store_res = PageStore::open(db_dir);
    PAGEDB_ASSERT_OK(store_res);
    PageStore& store = *store_res.value();
    PageAllocator allocator(store);

    PAGEDB_ASSERT_OK(allocator.verify_allocation_state());

    auto p2 = allocator.allocate_page(PageType::kLeaf);
    auto p3 = allocator.allocate_page(PageType::kInternal, kInitialRootPageId);
    auto p4 = allocator.allocate_page(PageType::kFree);
    PAGEDB_ASSERT_OK(p2);
    PAGEDB_ASSERT_OK(p3);
    PAGEDB_ASSERT_OK(p4);
    PAGEDB_ASSERT_EQ(p2.value(), PageId(2));
    PAGEDB_ASSERT_EQ(p3.value(), PageId(3));
    PAGEDB_ASSERT_EQ(p4.value(), PageId(4));

    PAGEDB_ASSERT_OK(allocator.set_root_page_id(PageId(3)));
    PAGEDB_ASSERT_EQ(allocator.next_page_id(), PageId(5));
    PAGEDB_ASSERT_TRUE(allocator.is_allocated(PageId(4)));
    PAGEDB_ASSERT_FALSE(allocator.is_allocated(PageId(5)));

    // Reading an unallocated PageId must fail.
    PageBuffer buf{};
    PAGEDB_ASSERT_FALSE(store.read_page(PageId(5), buf).ok());

    PAGEDB_ASSERT_OK(allocator.verify_allocation_state());
    PAGEDB_ASSERT_OK(store.sync());
    PAGEDB_ASSERT_OK(store.close());
  }

  // Reopen and verify all allocated pages and updated root_page_id persisted.
  {
    auto store_res = PageStore::open(db_dir);
    PAGEDB_ASSERT_OK(store_res);
    PageStore& store = *store_res.value();
    PageAllocator allocator(store);

    PAGEDB_ASSERT_EQ(store.page_count(), 5U);
    PAGEDB_ASSERT_EQ(allocator.root_page_id(), PageId(3));
    PAGEDB_ASSERT_EQ(allocator.next_page_id(), PageId(5));
    PAGEDB_ASSERT_OK(allocator.verify_allocation_state());
  }
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
