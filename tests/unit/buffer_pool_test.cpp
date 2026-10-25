#include "pagedb/buffer_pool.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "pagedb/page_allocator.h"
#include "pagedb/page_codec.h"
#include "pagedb/page_store.h"
#include "test_harness.h"

namespace pagedb {
namespace {

std::span<const std::byte> as_bytes(std::string_view sv) noexcept {
  return std::as_bytes(std::span<const char>(sv.data(), sv.size()));
}

PAGEDB_TEST(PageGuardMoveSemanticsAndSingleUnpin) {
  test::TempDir tmp("pagedb_bp_guards");
  auto store_res = PageStore::open(tmp.path() / "db");
  PAGEDB_ASSERT_OK(store_res);
  PageStore& store = *store_res.value();

  BufferPool pool(store, 2);
  auto g1_res = pool.fetch_page(kInitialRootPageId);
  PAGEDB_ASSERT_OK(g1_res);
  PAGEDB_ASSERT_EQ(pool.pin_count(kInitialRootPageId), 1U);

  // Move construct g2 from g1.
  PageGuard g2(std::move(g1_res.value()));
  PAGEDB_ASSERT_FALSE(g1_res.value().is_valid());
  PAGEDB_ASSERT_TRUE(g2.is_valid());
  PAGEDB_ASSERT_EQ(pool.pin_count(kInitialRootPageId), 1U);

  // Release g1 (already moved-from): must not decrement pin count.
  g1_res.value().release();
  PAGEDB_ASSERT_EQ(pool.pin_count(kInitialRootPageId), 1U);

  // Fetch same page again -> cache hit, pin_count == 2.
  auto g3_res = pool.fetch_page(kInitialRootPageId);
  PAGEDB_ASSERT_OK(g3_res);
  PAGEDB_ASSERT_EQ(pool.pin_count(kInitialRootPageId), 2U);
  PAGEDB_ASSERT_EQ(pool.stats().hits, 1ULL);
  PAGEDB_ASSERT_EQ(pool.stats().misses, 1ULL);

  // Move assign g2 = std::move(g3): g2's old pin must be released first.
  g2 = std::move(g3_res.value());
  PAGEDB_ASSERT_EQ(pool.pin_count(kInitialRootPageId), 1U);

  g2.release();
  PAGEDB_ASSERT_EQ(pool.pin_count(kInitialRootPageId), 0U);
  // Second release is a no-op.
  g2.release();
  PAGEDB_ASSERT_EQ(pool.pin_count(kInitialRootPageId), 0U);
  PAGEDB_ASSERT_OK(pool.verify_invariants());
}

PAGEDB_TEST(CapacityOneAndTwoDirtyEvictionAndPinExhaustion) {
  test::TempDir tmp("pagedb_bp_evict");
  auto store_res = PageStore::open(tmp.path() / "db");
  PAGEDB_ASSERT_OK(store_res);
  PageStore& store = *store_res.value();
  PageAllocator allocator(store);

  const PageId p2 = allocator.allocate_page(PageType::kLeaf).value();
  const PageId p3 = allocator.allocate_page(PageType::kLeaf).value();

  // Capacity-1 pool: test pin exhaustion and dirty eviction.
  {
    BufferPool pool1(store, 1);
    auto g1 = pool1.fetch_page(kInitialRootPageId);
    PAGEDB_ASSERT_OK(g1);

    // While g1 is pinned, fetching p2 must fail with ResourceExhausted.
    auto exhausted = pool1.fetch_page(p2);
    PAGEDB_ASSERT_FALSE(exhausted.ok());
    PAGEDB_ASSERT_EQ(exhausted.code(), ErrorCode::kResourceExhausted);
    PAGEDB_ASSERT_EQ(pool1.stats().pin_failures, 1ULL);

    // Mutate Page 1 via SlottedPage and mark dirty.
    auto sp1 =
        SlottedPage::decode(g1.value().bytes(), kInitialRootPageId).value();
    auto rec =
        PageCodec::encode_leaf_record(as_bytes("k1"), as_bytes("v1")).value();
    PAGEDB_ASSERT_OK(sp1.insert_slot(0, rec));
    PAGEDB_ASSERT_OK(sp1.encode(g1.value().mutable_bytes()));
    g1.value().mark_dirty();
    PAGEDB_ASSERT_TRUE(pool1.is_dirty(kInitialRootPageId));
    g1.value().release();

    // Fetching p2 now evicts dirty Page 1 and writes it to PageStore.
    auto g2 = pool1.fetch_page(p2);
    PAGEDB_ASSERT_OK(g2);
    PAGEDB_ASSERT_EQ(pool1.stats().evictions, 1ULL);
    PAGEDB_ASSERT_EQ(pool1.stats().dirty_flushes, 1ULL);
    g2.value().release();

    // Fetch Page 1 back and verify our inserted record survived eviction.
    auto g1_reload = pool1.fetch_page(kInitialRootPageId);
    PAGEDB_ASSERT_OK(g1_reload);
    auto sp1_reload = SlottedPage::decode(g1_reload.value().bytes(),
                                          kInitialRootPageId)
                          .value();
    PAGEDB_ASSERT_EQ(sp1_reload.num_slots(), 1U);
    g1_reload.value().release();
    PAGEDB_ASSERT_OK(pool1.verify_invariants());
  }

  // Capacity-2 pool: test CLOCK eviction while one frame remains pinned.
  {
    BufferPool pool2(store, 2);
    auto pin1 = pool2.fetch_page(kInitialRootPageId);
    auto pin2 = pool2.fetch_page(p2);
    PAGEDB_ASSERT_OK(pin1);
    PAGEDB_ASSERT_OK(pin2);

    // Both frames pinned -> fetching p3 fails.
    PAGEDB_ASSERT_EQ(pool2.fetch_page(p3).code(),
                     ErrorCode::kResourceExhausted);

    // Unpin p2, keep Page 1 pinned -> fetching p3 must evict p2, never Page 1.
    pin2.value().release();
    auto pin3 = pool2.fetch_page(p3);
    PAGEDB_ASSERT_OK(pin3);
    PAGEDB_ASSERT_EQ(pool2.pin_count(kInitialRootPageId), 1U);
    PAGEDB_ASSERT_EQ(pool2.pin_count(p3), 1U);
    PAGEDB_ASSERT_EQ(pool2.pin_count(p2), 0U);
    PAGEDB_ASSERT_OK(pool2.verify_invariants());
  }
}

bool g_fail_pwrite = false;
bool g_fail_pread = false;

ssize_t fault_pread(int fd, void* buf, std::size_t count, off_t offset) {
  if (g_fail_pread) {
    errno = EIO;
    return -1;
  }
  return ::pread(fd, buf, count, offset);
}

ssize_t fault_pwrite(int fd, const void* buf, std::size_t count, off_t offset) {
  if (g_fail_pwrite) {
    errno = EIO;
    return -1;
  }
  return ::pwrite(fd, buf, count, offset);
}

PAGEDB_TEST(IoFailureDuringEvictionAndFetchPreservesInvariants) {
  test::TempDir tmp("pagedb_bp_fault");
  PosixIoOps ops = PosixIoOps::default_ops();
  ops.pread_fn = &fault_pread;
  ops.pwrite_fn = &fault_pwrite;

  g_fail_pread = false;
  g_fail_pwrite = false;

  PageStoreOptions opts{};
  opts.io_ops = &ops;
  auto store_res = PageStore::open(tmp.path() / "db", opts);
  PAGEDB_ASSERT_OK(store_res);
  PageStore& store = *store_res.value();
  PageAllocator allocator(store);
  const PageId p2 = allocator.allocate_page(PageType::kLeaf).value();

  BufferPool pool(store, 1);

  // 1. Test read failure on cache miss leaves frame table clean.
  g_fail_pread = true;
  PAGEDB_ASSERT_EQ(pool.fetch_page(kInitialRootPageId).code(),
                   ErrorCode::kIoError);
  PAGEDB_ASSERT_EQ(pool.resident_count(), 0U);
  PAGEDB_ASSERT_OK(pool.verify_invariants());
  g_fail_pread = false;

  // 2. Load Page 1, mark dirty, unpin, then fail pwrite during eviction for p2.
  auto g1 = pool.fetch_page(kInitialRootPageId);
  PAGEDB_ASSERT_OK(g1);
  g1.value().mark_dirty();
  g1.value().release();

  g_fail_pwrite = true;
  PAGEDB_ASSERT_EQ(pool.fetch_page(p2).code(), ErrorCode::kIoError);
  // Victim frame (Page 1) must still be resident and dirty!
  PAGEDB_ASSERT_EQ(pool.resident_count(), 1U);
  PAGEDB_ASSERT_TRUE(pool.is_dirty(kInitialRootPageId));
  PAGEDB_ASSERT_TRUE(store.is_poisoned());
  PAGEDB_ASSERT_OK(pool.verify_invariants());
  g_fail_pwrite = false;
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
