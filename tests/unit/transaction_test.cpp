#include "pagedb/transaction.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pagedb/byte_codec.h"
#include "pagedb/database.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(TransactionLifecycleSingleWriterExclusion) {
  test::TempDir tmp("pagedb_txn_lifecycle");
  const auto db_dir = tmp.path() / "db";

  auto db_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  PAGEDB_ASSERT_OK(db.put("key1", "val1"));

  auto txn_res = db.begin_transaction();
  PAGEDB_ASSERT_OK(txn_res);
  auto txn = std::move(txn_res.value());
  PAGEDB_ASSERT_TRUE(txn->is_active());
  PAGEDB_ASSERT_NE(txn->id(), kInvalidTxnId);

  // Attempting to begin a second concurrent transaction must fail with
  // kAlreadyOpen.
  auto dup_txn_res = db.begin_transaction();
  PAGEDB_ASSERT_FALSE(dup_txn_res.ok());
  PAGEDB_ASSERT_EQ(dup_txn_res.code(), ErrorCode::kAlreadyOpen);

  // Auto-commit operations while an explicit transaction is active must fail
  // with kAlreadyOpen.
  PAGEDB_ASSERT_EQ(db.put("k", "v").code(), ErrorCode::kAlreadyOpen);
  PAGEDB_ASSERT_EQ(db.get("key1").code(), ErrorCode::kAlreadyOpen);
  PAGEDB_ASSERT_EQ(db.del("key1").code(), ErrorCode::kAlreadyOpen);
  PAGEDB_ASSERT_EQ(db.scan().code(), ErrorCode::kAlreadyOpen);

  // Move-construct transfers ownership.
  Transaction moved_txn(std::move(*txn));
  PAGEDB_ASSERT_TRUE(moved_txn.is_active());
  PAGEDB_ASSERT_FALSE(txn->is_active());

  // Operations on moved-from transaction must fail.
  PAGEDB_ASSERT_EQ(txn->put("a", "b").code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(txn->get("a").code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(txn->del("a").code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(txn->scan().code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(txn->commit().code(), ErrorCode::kInvalidArgument);

  PAGEDB_ASSERT_OK(moved_txn.commit());
  PAGEDB_ASSERT_FALSE(moved_txn.is_active());

  // Once committed, second commit fails with kInvalidArgument.
  PAGEDB_ASSERT_EQ(moved_txn.commit().code(), ErrorCode::kInvalidArgument);

  // Now that the transaction is committed, new transactions or auto-commit ops
  // can proceed.
  PAGEDB_ASSERT_OK(db.put("key2", "val2"));
  PAGEDB_ASSERT_EQ(db.get("key2").value(), "val2");
}

PAGEDB_TEST(ReadYourOwnWritesAndRollbackAtomicity) {
  test::TempDir tmp("pagedb_txn_ryow_rollback");
  const auto db_dir = tmp.path() / "db";

  auto db_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  PAGEDB_ASSERT_OK(db.put("apple", "red"));
  PAGEDB_ASSERT_OK(db.put("banana", "yellow"));
  PAGEDB_ASSERT_OK(db.put("cherry", "dark_red"));

  auto txn_res = db.begin_transaction();
  PAGEDB_ASSERT_OK(txn_res);
  auto& txn = *txn_res.value();

  // Read-your-own-writes inside transaction:
  PAGEDB_ASSERT_EQ(txn.get("apple").value(), "red");

  // Modify an existing key:
  PAGEDB_ASSERT_OK(txn.put("apple", "green"));
  PAGEDB_ASSERT_EQ(txn.get("apple").value(), "green");

  // Insert a new key:
  PAGEDB_ASSERT_OK(txn.put("date", "brown"));
  PAGEDB_ASSERT_EQ(txn.get("date").value(), "brown");

  // Delete an existing key:
  auto del_res = txn.del("banana");
  PAGEDB_ASSERT_OK(del_res);
  PAGEDB_ASSERT_TRUE(del_res.value());
  PAGEDB_ASSERT_EQ(txn.get("banana").code(), ErrorCode::kNotFound);

  // Scan inside the transaction sees the uncommitted mutations:
  auto scanned_inside = txn.scan().value();
  PAGEDB_ASSERT_EQ(scanned_inside.size(), 3U);
  PAGEDB_ASSERT_EQ(scanned_inside[0].key, "apple");
  PAGEDB_ASSERT_EQ(scanned_inside[0].value, "green");
  PAGEDB_ASSERT_EQ(scanned_inside[1].key, "cherry");
  PAGEDB_ASSERT_EQ(scanned_inside[1].value, "dark_red");
  PAGEDB_ASSERT_EQ(scanned_inside[2].key, "date");
  PAGEDB_ASSERT_EQ(scanned_inside[2].value, "brown");

  // Explicit rollback:
  PAGEDB_ASSERT_OK(txn.rollback());
  PAGEDB_ASSERT_FALSE(txn.is_active());

  // Database must be 100% restored to pre-transaction state:
  PAGEDB_ASSERT_OK(db.verify());
  PAGEDB_ASSERT_EQ(db.get("apple").value(), "red");
  PAGEDB_ASSERT_EQ(db.get("banana").value(), "yellow");
  PAGEDB_ASSERT_EQ(db.get("cherry").value(), "dark_red");
  PAGEDB_ASSERT_EQ(db.get("date").code(), ErrorCode::kNotFound);

  auto scanned_after = db.scan().value();
  PAGEDB_ASSERT_EQ(scanned_after.size(), 3U);
  PAGEDB_ASSERT_EQ(scanned_after[0].key, "apple");
  PAGEDB_ASSERT_EQ(scanned_after[0].value, "red");
  PAGEDB_ASSERT_EQ(scanned_after[1].key, "banana");
  PAGEDB_ASSERT_EQ(scanned_after[1].value, "yellow");
  PAGEDB_ASSERT_EQ(scanned_after[2].key, "cherry");
  PAGEDB_ASSERT_EQ(scanned_after[2].value, "dark_red");
}

PAGEDB_TEST(RaiiDestructorRollbackOnEarlyReturn) {
  test::TempDir tmp("pagedb_txn_raii");
  const auto db_dir = tmp.path() / "db";

  auto db_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  PAGEDB_ASSERT_OK(db.put("base_k", "base_v"));

  {
    auto txn_res = db.begin_transaction();
    PAGEDB_ASSERT_OK(txn_res);
    auto& txn = *txn_res.value();

    for (int i = 0; i < 50; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k_%03d", i);
      PAGEDB_ASSERT_OK(txn.put(std::string(kbuf) + std::string(80, 'z'),
                               std::string(150, 'w')));
    }
    // Transaction goes out of scope without calling commit()!
  }

  // Destructor must have rolled back everything cleanly:
  PAGEDB_ASSERT_OK(db.verify());
  PAGEDB_ASSERT_EQ(db.get("base_k").value(), "base_v");
  PAGEDB_ASSERT_EQ(db.scan().value().size(), 1U);
}

PAGEDB_TEST(MultiOperationCommitDurabilityAcrossReopen) {
  test::TempDir tmp("pagedb_txn_commit_persist");
  const auto db_dir = tmp.path() / "db";

  std::map<std::string, std::string, UnsignedByteLess> expected;

  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    auto txn_res = db.begin_transaction();
    PAGEDB_ASSERT_OK(txn_res);
    auto& txn = *txn_res.value();

    for (int i = 0; i < 120; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "multi_%04d", i);
      std::string k = std::string(kbuf) + std::string(60, 'x');
      std::string v(180, static_cast<char>('A' + (i % 26)));
      PAGEDB_ASSERT_OK(txn.put(k, v));
      expected[k] = v;
    }

    for (int i = 0; i < 40; i += 3) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "multi_%04d", i);
      std::string k = std::string(kbuf) + std::string(60, 'x');
      PAGEDB_ASSERT_OK(txn.del(k));
      expected.erase(k);
    }

    PAGEDB_ASSERT_OK(txn.commit());
    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_OK(db.close());
  }

  // Reopen in a new session:
  {
    DatabaseOptions reopen_opts{};
    reopen_opts.create_if_missing = false;
    auto db_res = Database::open(db_dir, reopen_opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    PAGEDB_ASSERT_OK(db.verify());
    auto scanned = db.scan().value();
    PAGEDB_ASSERT_EQ(scanned.size(), expected.size());
    std::size_t idx = 0;
    for (const auto& [k, v] : expected) {
      PAGEDB_ASSERT_EQ(scanned[idx].key, k);
      PAGEDB_ASSERT_EQ(scanned[idx].value, v);
      ++idx;
    }
  }
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
