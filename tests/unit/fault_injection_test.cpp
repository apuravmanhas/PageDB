#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "pagedb/crc32c.h"
#include "pagedb/database.h"
#include "pagedb/file_handle.h"
#include "pagedb/wal.h"
#include "test_harness.h"

namespace pagedb {
namespace {

// Thread-safe fault injection control state
struct FaultContext {
  std::atomic<bool> inject_enospc_on_wal{false};
  std::atomic<bool> inject_data_write_failure{false};
  std::atomic<int> wal_fd{-1};
  std::atomic<int> data_fd{-1};
};

static FaultContext g_fault{};

ssize_t FaultPread(int fd, void* buf, std::size_t count, off_t offset) {
  return PosixIoOps::default_ops().pread_fn(fd, buf, count, offset);
}

ssize_t FaultPwrite(int fd, const void* buf, std::size_t count, off_t offset) {
  if (g_fault.inject_enospc_on_wal.load(std::memory_order_relaxed)) {
    // If this write is to the WAL file, inject ENOSPC
    if (fd == g_fault.wal_fd.load(std::memory_order_relaxed)) {
      errno = ENOSPC;
      return -1;
    }
  }
  if (g_fault.inject_data_write_failure.load(std::memory_order_relaxed)) {
    // If this write is to the data file, inject EIO
    if (fd == g_fault.data_fd.load(std::memory_order_relaxed)) {
      errno = EIO;
      return -1;
    }
  }
  return PosixIoOps::default_ops().pwrite_fn(fd, buf, count, offset);
}

int FaultFsync(int fd) {
  return PosixIoOps::default_ops().fsync_fn(fd);
}

int FaultFdatasync(int fd) {
  return PosixIoOps::default_ops().fdatasync_fn(fd);
}

int FaultFstat(int fd, struct stat* st) {
  return PosixIoOps::default_ops().fstat_fn(fd, st);
}

int FaultFtruncate(int fd, off_t length) {
  return PosixIoOps::default_ops().ftruncate_fn(fd, length);
}

int FaultClose(int fd) {
  return PosixIoOps::default_ops().close_fn(fd);
}

const PosixIoOps g_fault_ops{
    .pread_fn = &FaultPread,
    .pwrite_fn = &FaultPwrite,
    .fdatasync_fn = &FaultFdatasync,
    .fsync_fn = &FaultFsync,
    .fstat_fn = &FaultFstat,
    .ftruncate_fn = &FaultFtruncate,
    .close_fn = &FaultClose,
};

PAGEDB_TEST(CrashAfterWalSyncBeforePageInstall) {
  test::TempDir tmp("pagedb_fault_wal_sync");
  const auto db_dir = tmp.path() / "db";

  // 1. Initialize database with initial data
  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();
    PAGEDB_ASSERT_OK(db.put("committed_key_1", "initial_val_1"));
    PAGEDB_ASSERT_OK(db.checkpoint());
    PAGEDB_ASSERT_OK(db.close());
  }

  // 2. Open DB with fault injection harness
  g_fault.inject_enospc_on_wal.store(false);
  g_fault.inject_data_write_failure.store(false);

  DatabaseOptions opts{};
  opts.io_ops = &g_fault_ops;

  auto db_res = Database::open(db_dir, opts);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  // Inspect open file descriptors to distinguish WAL from data
  // Open data file to get its dev/ino or match fd
  struct stat data_st{};
  ::stat((db_dir / "pagedb.data").c_str(), &data_st);
  struct stat wal_st{};
  ::stat((db_dir / "pagedb.wal").c_str(), &wal_st);

  // Scan fds from 3 to 1024
  for (int fd = 3; fd < 256; ++fd) {
    struct stat s{};
    if (::fstat(fd, &s) == 0) {
      if (s.st_ino == data_st.st_ino && s.st_dev == data_st.st_dev) {
        g_fault.data_fd.store(fd);
      } else if (s.st_ino == wal_st.st_ino && s.st_dev == wal_st.st_dev) {
        g_fault.wal_fd.store(fd);
      }
    }
  }

  // Now arm fault injector to fail data writes
  // Commit will sync to WAL, then fail when installing to pagedb.data
  g_fault.inject_data_write_failure.store(true);
  Status put_s = db.put("committed_key_2", "wal_durable_value_2");
  // The put must fail because data page install failed
  PAGEDB_ASSERT_FALSE(put_s.ok());

  // Database is poisoned due to failed data install
  g_fault.inject_data_write_failure.store(false);
  db_res.value().reset();

  // 3. Re-open database with default ops: WAL recovery must replay the committed page
  auto reopen_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(reopen_res);
  Database& recovered_db = *reopen_res.value();

  // The value committed in WAL must now be present!
  auto get_res = recovered_db.get("committed_key_2");
  PAGEDB_ASSERT_OK(get_res);
  PAGEDB_ASSERT_EQ(get_res.value(), "wal_durable_value_2");

  // Old key must also be intact
  auto get_1 = recovered_db.get("committed_key_1");
  PAGEDB_ASSERT_OK(get_1);
  PAGEDB_ASSERT_EQ(get_1.value(), "initial_val_1");

  PAGEDB_ASSERT_OK(recovered_db.verify());
  PAGEDB_ASSERT_OK(recovered_db.close());
}

PAGEDB_TEST(DiskFullDuringWalAppend) {
  test::TempDir tmp("pagedb_fault_enospc");
  const auto db_dir = tmp.path() / "db";

  // Create DB
  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    PAGEDB_ASSERT_OK(db_res.value()->put("safe_key", "safe_val"));
    PAGEDB_ASSERT_OK(db_res.value()->close());
  }

  // Re-open with fault ops
  DatabaseOptions opts{};
  opts.io_ops = &g_fault_ops;

  auto db_res = Database::open(db_dir, opts);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  struct stat wal_st{};
  ::stat((db_dir / "pagedb.wal").c_str(), &wal_st);
  for (int fd = 3; fd < 256; ++fd) {
    struct stat s{};
    if (::fstat(fd, &s) == 0 && s.st_ino == wal_st.st_ino && s.st_dev == wal_st.st_dev) {
      g_fault.wal_fd.store(fd);
      break;
    }
  }

  // Inject ENOSPC on WAL writes
  g_fault.inject_enospc_on_wal.store(true);
  Status s = db.put("doomed_key", "doomed_val");
  PAGEDB_ASSERT_FALSE(s.ok());
  g_fault.inject_enospc_on_wal.store(false);
  db_res.value().reset();

  // Subsequent open with clean ops must be consistent
  auto reopen_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(reopen_res);
  auto safe_get = reopen_res.value()->get("safe_key");
  PAGEDB_ASSERT_OK(safe_get);
  PAGEDB_ASSERT_EQ(safe_get.value(), "safe_val");

  auto doomed_get = reopen_res.value()->get("doomed_key");
  PAGEDB_ASSERT_FALSE(doomed_get.ok());
  PAGEDB_ASSERT_EQ(doomed_get.status().code(), ErrorCode::kNotFound);

  PAGEDB_ASSERT_OK(reopen_res.value()->verify());
  PAGEDB_ASSERT_OK(reopen_res.value()->close());
}

PAGEDB_TEST(RecoveryTornTailRejection) {
  test::TempDir tmp("pagedb_fault_torn_tail");
  const auto db_dir = tmp.path() / "db";

  // 1. Populate DB
  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    for (int i = 0; i < 20; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k_%04d", i);
      PAGEDB_ASSERT_OK(db_res.value()->put(kbuf, "v"));
    }
    PAGEDB_ASSERT_OK(db_res.value()->close());
  }

  // 2. Append 17 bytes of trailing garbage (simulating torn record write) to pagedb.wal
  const auto wal_file = db_dir / "pagedb.wal";
  int fd = ::open(wal_file.c_str(), O_WRONLY | O_APPEND);
  PAGEDB_ASSERT_TRUE(fd >= 0);
  const char torn_garbage[17] = "partial_torn_rec";
  PAGEDB_ASSERT_EQ(::write(fd, torn_garbage, sizeof(torn_garbage)),
                   static_cast<ssize_t>(sizeof(torn_garbage)));
  ::close(fd);

  // 3. Re-open DB: WAL recovery should detect the torn tail, truncate it, and succeed
  auto reopen_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(reopen_res);
  Database& db = *reopen_res.value();

  for (int i = 0; i < 20; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "k_%04d", i);
    auto g = db.get(kbuf);
    PAGEDB_ASSERT_OK(g);
    PAGEDB_ASSERT_EQ(g.value(), "v");
  }

  PAGEDB_ASSERT_OK(db.verify());
  PAGEDB_ASSERT_OK(db.close());
}

PAGEDB_TEST(MidWalCorruptionRejection) {
  test::TempDir tmp("pagedb_fault_mid_wal");
  const auto db_dir = tmp.path() / "db";

  // 1. Write several committed records
  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    for (int i = 0; i < 15; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "valid_k_%04d", i);
      PAGEDB_ASSERT_OK(db_res.value()->put(kbuf, "data"));
    }
    PAGEDB_ASSERT_OK(db_res.value()->close());
  }

  // 2. Corrupt a byte in the middle of pagedb.wal (at offset 100)
  const auto wal_file = db_dir / "pagedb.wal";
  int fd = ::open(wal_file.c_str(), O_RDWR);
  PAGEDB_ASSERT_TRUE(fd >= 0);
  std::uint8_t tamper = 0xAA;
  PAGEDB_ASSERT_EQ(::pwrite(fd, &tamper, 1, 100), 1);
  ::close(fd);

  // 3. Reopen DB: recovery must fail with Corruption rather than replaying garbage
  auto reopen_res = Database::open(db_dir);
  PAGEDB_ASSERT_FALSE(reopen_res.ok());
  PAGEDB_ASSERT_EQ(reopen_res.status().code(), ErrorCode::kCorruption);
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
