#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "pagedb/byte_codec.h"
#include "pagedb/database.h"
#include "pagedb/transaction.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(ConcurrentMultiThreadedReaders) {
  test::TempDir tmp("pagedb_conc_readers");
  const auto db_dir = tmp.path() / "db";

  std::map<std::string, std::string, UnsignedByteLess> model;

  // Pre-populate with 300 keys
  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    for (int i = 0; i < 300; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k_%04d", i);
      std::string k = std::string(kbuf) + std::string(60, 'r');
      std::string v(140, static_cast<char>('A' + (i % 26)));
      PAGEDB_ASSERT_OK(db.put(k, v));
      model[k] = v;
    }
    PAGEDB_ASSERT_OK(db.close());
  }

  // Open with minimum 4 frames to ensure heavy cache eviction and reloading
  // under concurrent reader threads.
  DatabaseOptions opts{};
  opts.buffer_pool_frames = kMinBufferPoolFrames;
  opts.create_if_missing = false;

  auto db_res = Database::open(db_dir, opts);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  constexpr int kNumThreads = 8;
  constexpr int kOpsPerThread = 80;
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  std::atomic<bool> all_ok{true};

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&db, &model, &all_ok, t]() {
      std::mt19937 rng(1337U + static_cast<unsigned>(t));
      for (int i = 0; i < kOpsPerThread; ++i) {
        const int id = static_cast<int>(rng() % 300U);
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "k_%04d", id);
        std::string k = std::string(kbuf) + std::string(60, 'r');

        auto get_res = db.get(k);
        if (!get_res.ok()) {
          all_ok.store(false, std::memory_order_relaxed);
          return;
        }
        auto it = model.find(k);
        if (it == model.end() || it->second != get_res.value()) {
          all_ok.store(false, std::memory_order_relaxed);
          return;
        }

        // Periodic range scan
        if (i % 10 == 0) {
          const int end_id = std::min(299, id + 20);
          char ebuf[32];
          std::snprintf(ebuf, sizeof(ebuf), "k_%04d", end_id);
          std::string ek = std::string(ebuf) + std::string(60, 'r');

          auto scan_res = db.scan(k, ek);
          if (!scan_res.ok()) {
            all_ok.store(false, std::memory_order_relaxed);
            return;
          }
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  PAGEDB_ASSERT_TRUE(all_ok.load());
  PAGEDB_ASSERT_OK(db.verify());
  PAGEDB_ASSERT_OK(db.close());
}

PAGEDB_TEST(ConcurrentReadersWithSerialWriter) {
  test::TempDir tmp("pagedb_conc_rw");
  const auto db_dir = tmp.path() / "db";

  DatabaseOptions opts{};
  opts.buffer_pool_frames = kMinBufferPoolFrames;

  auto db_res = Database::open(db_dir, opts);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  // Populate base keys
  for (int i = 0; i < 50; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "base_%03d", i);
    PAGEDB_ASSERT_OK(db.put(kbuf, "initial_value"));
  }

  std::atomic<bool> running{true};
  std::atomic<bool> readers_ok{true};

  // Launch 4 reader threads
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&db, &running, &readers_ok, r]() {
      std::mt19937 rng(42U + static_cast<unsigned>(r));
      while (running.load(std::memory_order_relaxed)) {
        const int id = static_cast<int>(rng() % 50U);
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "base_%03d", id);
        auto res = db.get(kbuf);
        if (!res.ok() && res.code() != ErrorCode::kAlreadyOpen) {
          readers_ok.store(false, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    });
  }

  // Writer thread performs 60 mutations
  for (int w = 0; w < 60; ++w) {
    const int id = w % 50;
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "base_%03d", id);
    std::string v = "writer_value_" + std::to_string(w);
    PAGEDB_ASSERT_OK(db.put(kbuf, v));
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }

  running.store(false, std::memory_order_relaxed);
  for (auto& th : readers) {
    th.join();
  }

  PAGEDB_ASSERT_TRUE(readers_ok.load());
  PAGEDB_ASSERT_OK(db.verify());
  PAGEDB_ASSERT_OK(db.close());
}

PAGEDB_TEST(SingleWriterExclusionAcrossThreads) {
  test::TempDir tmp("pagedb_conc_exclusion");
  const auto db_dir = tmp.path() / "db";

  auto db_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  PAGEDB_ASSERT_OK(db.put("key", "val"));

  // Main thread opens an explicit transaction
  auto txn_res = db.begin_transaction();
  PAGEDB_ASSERT_OK(txn_res);

  std::atomic<bool> thread2_checked{false};
  std::atomic<bool> thread2_saw_already_open{false};

  std::thread th([&db, &thread2_checked, &thread2_saw_already_open]() {
    auto res = db.begin_transaction();
    if (!res.ok() && res.code() == ErrorCode::kAlreadyOpen) {
      thread2_saw_already_open.store(true);
    }
    thread2_checked.store(true);
  });

  th.join();
  PAGEDB_ASSERT_TRUE(thread2_checked.load());
  PAGEDB_ASSERT_TRUE(thread2_saw_already_open.load());

  // Main thread commits transaction
  PAGEDB_ASSERT_OK(txn_res.value()->commit());

  // Another thread can now begin and commit cleanly
  std::atomic<bool> thread3_success{false};
  std::thread th3([&db, &thread3_success]() {
    auto res = db.begin_transaction();
    if (res.ok()) {
      if (res.value()->put("k3", "v3").ok() && res.value()->commit().ok()) {
        thread3_success.store(true);
      }
    }
  });

  th3.join();
  PAGEDB_ASSERT_TRUE(thread3_success.load());
  PAGEDB_ASSERT_EQ(db.get("k3").value(), "v3");
  PAGEDB_ASSERT_OK(db.close());
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
