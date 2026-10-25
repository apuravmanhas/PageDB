#include "pagedb/database.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "pagedb/byte_codec.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(DatabaseOpenOptionsLockingAndCloseLifecycle) {
  test::TempDir tmp("pagedb_db_lifecycle");
  const auto db_dir = tmp.path() / "db";

  // Reject buffer_pool_frames < 4.
  {
    DatabaseOptions bad_opts{};
    bad_opts.buffer_pool_frames = 3;
    auto res = Database::open(db_dir, bad_opts);
    PAGEDB_ASSERT_FALSE(res.ok());
    PAGEDB_ASSERT_EQ(res.code(), ErrorCode::kInvalidArgument);
  }

  // Reject create_if_missing = false when directory doesn't exist.
  {
    DatabaseOptions missing_opts{};
    missing_opts.create_if_missing = false;
    auto res = Database::open(db_dir, missing_opts);
    PAGEDB_ASSERT_FALSE(res.ok());
    PAGEDB_ASSERT_EQ(res.code(), ErrorCode::kNotFound);
  }

  // Open new database, verify concurrent open fails with kAlreadyOpen.
  auto db_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();
  PAGEDB_ASSERT_TRUE(db.is_open());

  auto dup_res = Database::open(db_dir);
  PAGEDB_ASSERT_FALSE(dup_res.ok());
  PAGEDB_ASSERT_EQ(dup_res.code(), ErrorCode::kAlreadyOpen);

  PAGEDB_ASSERT_OK(db.put("alpha", "1"));
  PAGEDB_ASSERT_EQ(db.get("alpha").value(), "1");
  PAGEDB_ASSERT_OK(db.verify());
  PAGEDB_ASSERT_OK(db.close());
  PAGEDB_ASSERT_FALSE(db.is_open());

  // Operations on closed Database must return kInvalidArgument.
  PAGEDB_ASSERT_EQ(db.put("k", "v").code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(db.get("k").code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(db.del("k").code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(db.scan().code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(db.verify().code(), ErrorCode::kInvalidArgument);

  // Reject error_if_exists = true on existing database.
  {
    DatabaseOptions exists_opts{};
    exists_opts.error_if_exists = true;
    auto res = Database::open(db_dir, exists_opts);
    PAGEDB_ASSERT_FALSE(res.ok());
    PAGEDB_ASSERT_EQ(res.code(), ErrorCode::kAlreadyExists);
  }
}

PAGEDB_TEST(ReopenPersistenceUnderEvictionAndFreeListReuse) {
  test::TempDir tmp("pagedb_db_persist");
  const auto db_dir = tmp.path() / "db";

  DatabaseOptions opts{};
  // Minimum 4-frame buffer pool to force heavy eviction on every session.
  opts.buffer_pool_frames = kMinBufferPoolFrames;

  std::map<std::string, std::string, UnsignedByteLess> model;

  // Session 1: Insert 240 records forcing multi-level B+ tree splits & evictions.
  std::uint32_t peak_page_count = 0;
  {
    auto db_res = Database::open(db_dir, opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    for (int i = 0; i < 240; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "user_%04d", i);
      std::string k = std::string(kbuf) + std::string(120, 'x');
      std::string v(280, static_cast<char>('a' + (i % 26)));
      PAGEDB_ASSERT_OK(db.put(k, v));
      model[k] = v;
    }

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_TRUE(db.tree_height().value() >= 3U);
    PAGEDB_ASSERT_TRUE(db.buffer_pool_stats().evictions > 0ULL);
    peak_page_count = db.page_count();
    PAGEDB_ASSERT_OK(db.close());
  }

  // Session 2: Reopen, verify all 240 records survived, delete 200 records so
  // pages are merged and returned to the persistent free list, and close.
  {
    DatabaseOptions reopen_opts = opts;
    reopen_opts.create_if_missing = false;
    auto db_res = Database::open(db_dir, reopen_opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_EQ(db.page_count(), peak_page_count);

    int deleted = 0;
    for (auto it = model.begin(); it != model.end() && deleted < 200;) {
      auto del_res = db.del(it->first);
      PAGEDB_ASSERT_OK(del_res);
      PAGEDB_ASSERT_TRUE(del_res.value());
      it = model.erase(it);
      ++deleted;
    }

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_TRUE(db.free_list_head().is_valid());
    PAGEDB_ASSERT_OK(db.close());
  }

  // Session 3: Reopen, insert 150 new records (reusing persistent free-list
  // pages without growing the file), and verify final contents against model.
  {
    DatabaseOptions reopen_opts = opts;
    reopen_opts.create_if_missing = false;
    auto db_res = Database::open(db_dir, reopen_opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_TRUE(db.free_list_head().is_valid());

    for (int i = 0; i < 150; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "reused_%04d", i);
      std::string k = std::string(kbuf) + std::string(100, 'y');
      std::string v(240, 'R');
      PAGEDB_ASSERT_OK(db.put(k, v));
      model[k] = v;
    }

    PAGEDB_ASSERT_OK(db.verify());
    // File page count must not have exceeded peak_page_count!
    PAGEDB_ASSERT_EQ(db.page_count(), peak_page_count);

    auto scanned = db.scan().value();
    PAGEDB_ASSERT_EQ(scanned.size(), model.size());
    std::size_t idx = 0;
    for (const auto& [mk, mv] : model) {
      PAGEDB_ASSERT_EQ(scanned[idx].key, mk);
      PAGEDB_ASSERT_EQ(scanned[idx].value, mv);
      ++idx;
    }

    PAGEDB_ASSERT_OK(db.close());
  }
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
