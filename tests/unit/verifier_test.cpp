#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pagedb/byte_codec.h"
#include "pagedb/crc32c.h"
#include "pagedb/database.h"
#include "pagedb/page_codec.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(VerifyCleanDatabaseLifecycle) {
  test::TempDir tmp("pagedb_verify_clean");
  const auto db_dir = tmp.path() / "db";

  auto db_res = Database::open(db_dir);
  PAGEDB_ASSERT_OK(db_res);
  Database& db = *db_res.value();

  // Fresh DB should verify cleanly
  PAGEDB_ASSERT_OK(db.verify());

  // Insert 120 keys to cause multiple tree splits
  for (int i = 0; i < 120; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key_%05d", i);
    std::string v(100, static_cast<char>('A' + (i % 26)));
    PAGEDB_ASSERT_OK(db.put(kbuf, v));
  }
  PAGEDB_ASSERT_OK(db.verify());

  // Delete 40 keys to cause redistribution or merges
  for (int i = 0; i < 40; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key_%05d", i * 3);
    PAGEDB_ASSERT_OK(db.del(kbuf));
  }
  PAGEDB_ASSERT_OK(db.verify());

  // Checkpoint and verify again
  PAGEDB_ASSERT_OK(db.checkpoint());
  PAGEDB_ASSERT_OK(db.verify());

  PAGEDB_ASSERT_OK(db.close());
}

PAGEDB_TEST(DetectCorruptPage0Magic) {
  test::TempDir tmp("pagedb_verify_magic");
  const auto db_dir = tmp.path() / "db";

  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();
    PAGEDB_ASSERT_OK(db.put("k1", "v1"));
    PAGEDB_ASSERT_OK(db.checkpoint());
    PAGEDB_ASSERT_OK(db.close());
  }

  // Corrupt the magic bytes in pagedb.data (offset 0)
  const auto data_file = db_dir / "pagedb.data";
  int fd = ::open(data_file.c_str(), O_RDWR);
  PAGEDB_ASSERT_TRUE(fd >= 0);
  const char bad_magic[8] = {'B', 'A', 'D', 'M', 'A', 'G', 'I', 'C'};
  PAGEDB_ASSERT_EQ(::pwrite(fd, bad_magic, 8, 0), 8);
  ::close(fd);

  // Opening the DB must detect the corruption
  auto reopen_res = Database::open(db_dir);
  PAGEDB_ASSERT_FALSE(reopen_res.ok());
  PAGEDB_ASSERT_EQ(reopen_res.status().code(), ErrorCode::kCorruption);
}

PAGEDB_TEST(DetectCorruptPage0Checksum) {
  test::TempDir tmp("pagedb_verify_sb_crc");
  const auto db_dir = tmp.path() / "db";

  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();
    PAGEDB_ASSERT_OK(db.put("k1", "v1"));
    PAGEDB_ASSERT_OK(db.checkpoint());
    PAGEDB_ASSERT_OK(db.close());
  }

  // Tamper with superblock body at offset 40 without updating CRC
  const auto data_file = db_dir / "pagedb.data";
  int fd = ::open(data_file.c_str(), O_RDWR);
  PAGEDB_ASSERT_TRUE(fd >= 0);
  std::uint8_t tamper = 0xFF;
  PAGEDB_ASSERT_EQ(::pwrite(fd, &tamper, 1, 40), 1);
  ::close(fd);

  auto reopen_res = Database::open(db_dir);
  PAGEDB_ASSERT_FALSE(reopen_res.ok());
  PAGEDB_ASSERT_EQ(reopen_res.status().code(), ErrorCode::kCorruption);
}

PAGEDB_TEST(DetectCorruptPageChecksumOnDisk) {
  test::TempDir tmp("pagedb_verify_page_crc");
  const auto db_dir = tmp.path() / "db";

  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();
    // Put enough keys to occupy Page 1
    for (int i = 0; i < 50; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k_%04d", i);
      PAGEDB_ASSERT_OK(db.put(kbuf, std::string(120, 'z')));
    }
    PAGEDB_ASSERT_OK(db.checkpoint());
    PAGEDB_ASSERT_OK(db.close());
  }

  // Corrupt a byte in Page 1 body (offset 4096 + 100)
  const auto data_file = db_dir / "pagedb.data";
  int fd = ::open(data_file.c_str(), O_RDWR);
  PAGEDB_ASSERT_TRUE(fd >= 0);
  std::uint8_t tamper = 0x55;
  PAGEDB_ASSERT_EQ(::pwrite(fd, &tamper, 1, kPageSize + 100), 1);
  ::close(fd);

  // Database::open or verify() should detect the checksum mismatch
  auto reopen_res = Database::open(db_dir);
  if (reopen_res.ok()) {
    // If open succeeded, verify() must catch the corrupted checksum
    Status vs = reopen_res.value()->verify();
    PAGEDB_ASSERT_FALSE(vs.ok());
    PAGEDB_ASSERT_EQ(vs.code(), ErrorCode::kCorruption);
  } else {
    PAGEDB_ASSERT_EQ(reopen_res.status().code(), ErrorCode::kCorruption);
  }
}

PAGEDB_TEST(DetectFreeListTampering) {
  test::TempDir tmp("pagedb_verify_freelist");
  const auto db_dir = tmp.path() / "db";

  {
    auto db_res = Database::open(db_dir);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();
    // Create multiple pages and delete to free pages
    for (int i = 0; i < 80; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "key_%04d", i);
      PAGEDB_ASSERT_OK(db.put(kbuf, std::string(150, 'x')));
    }
    for (int i = 0; i < 80; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "key_%04d", i);
      PAGEDB_ASSERT_OK(db.del(kbuf));
    }
    PAGEDB_ASSERT_OK(db.checkpoint());
    PAGEDB_ASSERT_OK(db.close());
  }

  // Read superblock from pagedb.data to find free_list_head
  const auto data_file = db_dir / "pagedb.data";
  int fd = ::open(data_file.c_str(), O_RDWR);
  PAGEDB_ASSERT_TRUE(fd >= 0);

  PageBuffer sb_buf{};
  PAGEDB_ASSERT_EQ(::pread(fd, sb_buf.data(), kPageSize, 0),
                   static_cast<ssize_t>(kPageSize));
  auto sb_res = PageCodec::decode_superblock(sb_buf);
  PAGEDB_ASSERT_OK(sb_res);
  const Superblock sb = sb_res.value();

  if (sb.free_list_head.is_valid()) {
    // Read the free page
    PageBuffer free_buf{};
    const off_t free_offset =
        static_cast<off_t>(sb.free_list_head.value) * static_cast<off_t>(kPageSize);
    PAGEDB_ASSERT_EQ(::pread(fd, free_buf.data(), kPageSize, free_offset),
                     static_cast<ssize_t>(kPageSize));

    // Tamper with the next_free_page_id to point to 99999 (out of bounds)
    // In PageCodec::encode_free_page:
    // offset 0..3: magic (PAGE)
    // offset 4..7: page_id
    // offset 8..15: page_lsn
    // offset 16..17: page_type (kFree)
    // offset 18..21: next_free_page_id
    // offset 22..27: reserved
    // offset 28..31: crc32c
    std::uint32_t bad_next_id = 99999;
    PAGEDB_ASSERT_OK(write_u32_le_at(free_buf, 18, bad_next_id));
    // Recompute CRC32C
    PAGEDB_ASSERT_OK(write_u32_le_at(free_buf, 28, 0));
    const std::uint32_t new_crc = crc32c(std::span<const std::byte>(free_buf));
    PAGEDB_ASSERT_OK(write_u32_le_at(free_buf, 28, new_crc));

    PAGEDB_ASSERT_EQ(::pwrite(fd, free_buf.data(), kPageSize, free_offset),
                     static_cast<ssize_t>(kPageSize));
    ::close(fd);

    auto reopen_res = Database::open(db_dir);
    if (reopen_res.ok()) {
      Status vs = reopen_res.value()->verify();
      PAGEDB_ASSERT_FALSE(vs.ok());
      PAGEDB_ASSERT_EQ(vs.code(), ErrorCode::kCorruption);
    } else {
      PAGEDB_ASSERT_EQ(reopen_res.status().code(), ErrorCode::kCorruption);
    }
  } else {
    ::close(fd);
  }
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
