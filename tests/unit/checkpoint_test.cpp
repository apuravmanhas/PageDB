#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pagedb/byte_codec.h"
#include "pagedb/database.h"
#include "pagedb/transaction.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(ExplicitCheckpointReclaimsWalAndPreservesData) {
  test::TempDir tmp("pagedb_ckpt_explicit");
  const auto db_dir = tmp.path() / "db";

  DatabaseOptions opts{};
  opts.buffer_pool_frames = kMinBufferPoolFrames;
  opts.wal_auto_checkpoint_bytes = 0;  // Disable auto-checkpoint

  std::map<std::string, std::string, UnsignedByteLess> model;

  Lsn post_ckpt_lsn = kInvalidLsn;
  {
    auto db_res = Database::open(db_dir, opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    for (int i = 0; i < 60; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "ck_%03d", i);
      std::string k = std::string(kbuf) + std::string(80, 'a');
      std::string v(180, static_cast<char>('A' + (i % 26)));
      PAGEDB_ASSERT_OK(db.put(k, v));
      model[k] = v;
    }

    auto sz_before = db.wal_size_bytes();
    PAGEDB_ASSERT_OK(sz_before);
    PAGEDB_ASSERT_TRUE(sz_before.value() > 50000ULL);

    PAGEDB_ASSERT_OK(db.checkpoint());

    auto sz_after = db.wal_size_bytes();
    PAGEDB_ASSERT_OK(sz_after);
    PAGEDB_ASSERT_EQ(sz_after.value(), 32ULL);

    post_ckpt_lsn = db.checkpoint_lsn();
    PAGEDB_ASSERT_TRUE(post_ckpt_lsn.is_valid());

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_OK(db.close());
  }

  // Reopen: verify all data survived and checkpoint_lsn is intact.
  {
    DatabaseOptions reopen_opts = opts;
    reopen_opts.create_if_missing = false;
    auto db_res = Database::open(db_dir, reopen_opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_EQ(db.checkpoint_lsn(), post_ckpt_lsn);

    auto scanned = db.scan().value();
    PAGEDB_ASSERT_EQ(scanned.size(), model.size());
    std::size_t idx = 0;
    for (const auto& [k, v] : model) {
      PAGEDB_ASSERT_EQ(scanned[idx].key, k);
      PAGEDB_ASSERT_EQ(scanned[idx].value, v);
      ++idx;
    }
    PAGEDB_ASSERT_OK(db.close());
  }
}

PAGEDB_TEST(AutomaticCheckpointOnWalSizeThreshold) {
  test::TempDir tmp("pagedb_ckpt_auto");
  const auto db_dir = tmp.path() / "db";

  DatabaseOptions opts{};
  opts.buffer_pool_frames = kMinBufferPoolFrames;
  // Set small threshold (30KB) so auto-checkpoint fires multiple times
  opts.wal_auto_checkpoint_bytes = 30ULL * 1024ULL;

  std::map<std::string, std::string, UnsignedByteLess> model;

  {
    auto db_res = Database::open(db_dir, opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    for (int i = 0; i < 100; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "auto_%04d", i);
      std::string k = std::string(kbuf) + std::string(60, 'x');
      std::string v(200, static_cast<char>('a' + (i % 26)));
      PAGEDB_ASSERT_OK(db.put(k, v));
      model[k] = v;
    }

    // Auto-checkpoint must have triggered and kept WAL size bounded.
    auto wal_sz = db.wal_size_bytes();
    PAGEDB_ASSERT_OK(wal_sz);
    PAGEDB_ASSERT_TRUE(wal_sz.value() < 60ULL * 1024ULL);
    PAGEDB_ASSERT_TRUE(db.checkpoint_lsn().is_valid());

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_OK(db.close());
  }

  // Reopen and verify full consistency.
  {
    DatabaseOptions reopen_opts = opts;
    reopen_opts.create_if_missing = false;
    auto db_res = Database::open(db_dir, reopen_opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    PAGEDB_ASSERT_OK(db.verify());
    auto scanned = db.scan().value();
    PAGEDB_ASSERT_EQ(scanned.size(), model.size());
    std::size_t idx = 0;
    for (const auto& [k, v] : model) {
      PAGEDB_ASSERT_EQ(scanned[idx].key, k);
      PAGEDB_ASSERT_EQ(scanned[idx].value, v);
      ++idx;
    }
  }
}

PAGEDB_TEST(CrashAndRecoveryAcrossCheckpointBoundary) {
  test::TempDir tmp("pagedb_ckpt_crash");
  const auto db_dir = tmp.path() / "db";
  const auto data_path = db_dir / "pagedb.data";

  DatabaseOptions opts{};
  opts.buffer_pool_frames = kMinBufferPoolFrames;
  opts.wal_auto_checkpoint_bytes = 0;

  std::map<std::string, std::string, UnsignedByteLess> model;

  {
    auto db_res = Database::open(db_dir, opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    // Insert 50 records prior to checkpoint.
    for (int i = 0; i < 50; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "pre_%03d", i);
      std::string k = std::string(kbuf) + std::string(70, 'p');
      std::string v(160, 'P');
      PAGEDB_ASSERT_OK(db.put(k, v));
      model[k] = v;
    }

    PAGEDB_ASSERT_OK(db.checkpoint());
    PAGEDB_ASSERT_EQ(db.wal_size_bytes().value(), 32ULL);

    // Insert 30 records post-checkpoint.
    for (int i = 0; i < 30; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "post_%03d", i);
      std::string k = std::string(kbuf) + std::string(70, 'q');
      std::string v(160, 'Q');
      PAGEDB_ASSERT_OK(db.put(k, v));
      model[k] = v;
    }

    PAGEDB_ASSERT_OK(db.close());
  }

  // Inject a crash artifact: tear PageId(1) in pagedb.data
  {
    auto fh_res = FileHandle::open(data_path, OpenMode::kReadWrite);
    PAGEDB_ASSERT_OK(fh_res);
    std::array<std::byte, 40> tear{};
    tear.fill(std::byte{0xCC});
    PAGEDB_ASSERT_OK(fh_res.value().write_exact_at(kPageSize + 32, tear));
    PAGEDB_ASSERT_OK(fh_res.value().close());
  }

  // Reopen: recovery replays post-checkpoint WAL records, repairs PageId(1),
  // and restores all 80 records.
  {
    DatabaseOptions reopen_opts = opts;
    reopen_opts.create_if_missing = false;
    auto db_res = Database::open(db_dir, reopen_opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    PAGEDB_ASSERT_OK(db.verify());
    auto scanned = db.scan().value();
    PAGEDB_ASSERT_EQ(scanned.size(), model.size());
    std::size_t idx = 0;
    for (const auto& [k, v] : model) {
      PAGEDB_ASSERT_EQ(scanned[idx].key, k);
      PAGEDB_ASSERT_EQ(scanned[idx].value, v);
      ++idx;
    }
  }
}

PAGEDB_TEST(CheckpointForbiddenDuringActiveTransaction) {
  test::TempDir tmp("pagedb_ckpt_exclusion");
  const auto db_dir = tmp.path() / "db";

  auto db_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  PAGEDB_ASSERT_OK(db.put("k", "v"));

  auto txn_res = db.begin_transaction();
  PAGEDB_ASSERT_OK(txn_res);

  // Calling checkpoint while a transaction is active must return kAlreadyOpen.
  PAGEDB_ASSERT_EQ(db.checkpoint().code(), ErrorCode::kAlreadyOpen);

  PAGEDB_ASSERT_OK(txn_res.value()->rollback());

  // After rollback, checkpoint succeeds cleanly.
  PAGEDB_ASSERT_OK(db.checkpoint());
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
