#include "pagedb/wal.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "pagedb/byte_codec.h"
#include "pagedb/database.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(WalCodecRoundTripAndCorruptionDetection) {
  WalFileHeader fhdr{};
  fhdr.base_lsn = Lsn(1);
  std::array<std::byte, kWalFileHeaderSize> fhdr_buf{};
  PAGEDB_ASSERT_OK(WalCodec::encode_file_header(fhdr, fhdr_buf));

  auto dec_fhdr = WalCodec::decode_file_header(fhdr_buf);
  PAGEDB_ASSERT_OK(dec_fhdr);
  PAGEDB_ASSERT_EQ(dec_fhdr.value().base_lsn, Lsn(1));
  PAGEDB_ASSERT_EQ(dec_fhdr.value().page_size,
                   static_cast<std::uint32_t>(kPageSize));

  // Corrupt a byte in file header -> CRC32C mismatch.
  fhdr_buf[16] ^= std::byte{0x01};
  PAGEDB_ASSERT_EQ(WalCodec::decode_file_header(fhdr_buf).code(),
                   ErrorCode::kCorruption);
  fhdr_buf[16] ^= std::byte{0x01};

  // Encode and verify PAGE_IMAGE record.
  SlottedPage leaf;
  PAGEDB_ASSERT_OK(leaf.init(PageId(1), PageType::kLeaf));
  PageBuffer page_buf{};
  PAGEDB_ASSERT_OK(leaf.encode(page_buf));
  PAGEDB_ASSERT_OK(PageCodec::stamp_lsn(page_buf, Lsn(2)));

  WalRecordHeader rhdr{};
  rhdr.record_type = WalRecordType::kPageImage;
  rhdr.lsn = Lsn(2);
  rhdr.txn_id = TxnId(1);
  rhdr.page_id = PageId(1);
  rhdr.payload_len = static_cast<std::uint32_t>(kPageSize);

  std::vector<std::byte> rec_buf(kWalRecordHeaderSize + kPageSize);
  PAGEDB_ASSERT_OK(WalCodec::encode_record(rhdr, page_buf, rec_buf));

  std::span<const std::byte, kWalRecordHeaderSize> rhdr_span(
      rec_buf.data(), kWalRecordHeaderSize);
  auto dec_rhdr = WalCodec::decode_record_header(rhdr_span);
  PAGEDB_ASSERT_OK(dec_rhdr);
  PAGEDB_ASSERT_OK(WalCodec::verify_record(
      dec_rhdr.value(),
      std::span<const std::byte>(rec_buf).subspan(kWalRecordHeaderSize)));

  // Flip a bit in the payload -> verify_record must fail with Corruption.
  rec_buf[kWalRecordHeaderSize + 100] ^= std::byte{0x80};
  PAGEDB_ASSERT_EQ(
      WalCodec::verify_record(
          dec_rhdr.value(),
          std::span<const std::byte>(rec_buf).subspan(kWalRecordHeaderSize))
          .code(),
      ErrorCode::kCorruption);
}

PAGEDB_TEST(TornTailTruncatedAndMidLogCorruptionRejected) {
  test::TempDir tmp("pagedb_wal_torntail");
  const auto wal_path = tmp.path() / "pagedb.wal";

  std::uint64_t txn1_end_offset = 0;
  {
    auto wal_res = Wal::open(wal_path);
    PAGEDB_ASSERT_OK(wal_res);
    Wal& wal = *wal_res.value();

    // Transaction 1: committed.
    const TxnId t1 = wal.allocate_txn_id();
    PAGEDB_ASSERT_OK(wal.append_begin(t1));

    SlottedPage leaf;
    PAGEDB_ASSERT_OK(leaf.init(PageId(1), PageType::kLeaf));
    PageBuffer p1{};
    PAGEDB_ASSERT_OK(leaf.encode(p1));
    PAGEDB_ASSERT_OK(wal.append_page_image(t1, PageId(1), p1));
    PAGEDB_ASSERT_OK(wal.append_commit(t1));
    PAGEDB_ASSERT_OK(wal.sync());
    txn1_end_offset = wal.write_offset();

    // Transaction 2: uncommitted, followed by a torn partial PAGE_IMAGE write.
    const TxnId t2 = wal.allocate_txn_id();
    PAGEDB_ASSERT_OK(wal.append_begin(t2));
    PAGEDB_ASSERT_OK(wal.append_page_image(t2, PageId(1), p1));
    PAGEDB_ASSERT_OK(wal.close());
  }

  // Simulate crash mid-write of Transaction 2's PAGE_IMAGE by truncating 500
  // bytes off the end of pagedb.wal.
  {
    auto fh_res = FileHandle::open(wal_path, OpenMode::kReadWrite);
    PAGEDB_ASSERT_OK(fh_res);
    const std::uint64_t cur_sz = fh_res.value().size_bytes().value();
    PAGEDB_ASSERT_OK(fh_res.value().truncate(cur_sz - 500ULL));
    PAGEDB_ASSERT_OK(fh_res.value().close());
  }

  // Reopen WAL and run recover(): must truncate uncommitted Transaction 2 back
  // to txn1_end_offset and return Transaction 1's committed update.
  {
    auto wal_res = Wal::open(wal_path);
    PAGEDB_ASSERT_OK(wal_res);
    Wal& wal = *wal_res.value();

    auto rec_res = wal.recover();
    PAGEDB_ASSERT_OK(rec_res);
    const WalRecoveryReport& rep = rec_res.value();
    PAGEDB_ASSERT_TRUE(rep.truncated_uncommitted_tail);
    PAGEDB_ASSERT_EQ(rep.valid_end_offset, txn1_end_offset);
    PAGEDB_ASSERT_EQ(rep.committed_updates.size(), 1U);
    PAGEDB_ASSERT_EQ(rep.max_committed_lsn, Lsn(3));
    PAGEDB_ASSERT_EQ(rep.next_lsn, Lsn(4));
    PAGEDB_ASSERT_EQ(rep.next_txn_id, TxnId(2));

    // Append and commit Transaction 2 cleanly now.
    const TxnId t2 = wal.allocate_txn_id();
    PAGEDB_ASSERT_EQ(t2, TxnId(2));
    PAGEDB_ASSERT_OK(wal.append_begin(t2));
    SlottedPage leaf;
    PAGEDB_ASSERT_OK(leaf.init(PageId(1), PageType::kLeaf));
    PageBuffer p1{};
    PAGEDB_ASSERT_OK(leaf.encode(p1));
    PAGEDB_ASSERT_OK(wal.append_page_image(t2, PageId(1), p1));
    PAGEDB_ASSERT_OK(wal.append_commit(t2));
    PAGEDB_ASSERT_OK(wal.sync());
    PAGEDB_ASSERT_OK(wal.close());
  }

  // Now inject mid-log corruption inside Transaction 1's PAGE_IMAGE payload
  // (which precedes Transaction 1's COMMIT and Transaction 2's records).
  {
    auto fh_res = FileHandle::open(wal_path, OpenMode::kReadWrite);
    PAGEDB_ASSERT_OK(fh_res);
    std::array<std::byte, 1> b{};
    const std::uint64_t mid_offset =
        kWalFileHeaderSize + kWalRecordHeaderSize + 128ULL;
    PAGEDB_ASSERT_OK(fh_res.value().read_exact_at(mid_offset, b));
    b[0] ^= std::byte{0xFF};
    PAGEDB_ASSERT_OK(fh_res.value().write_exact_at(mid_offset, b));
    PAGEDB_ASSERT_OK(fh_res.value().close());
  }

  // Wal::recover() must detect mid-log corruption and return kCorruption!
  {
    auto wal_res = Wal::open(wal_path);
    PAGEDB_ASSERT_OK(wal_res);
    auto rec_res = wal_res.value()->recover();
    PAGEDB_ASSERT_FALSE(rec_res.ok());
    PAGEDB_ASSERT_EQ(rec_res.code(), ErrorCode::kCorruption);
  }
}

int g_fdatasync_count = 0;
int g_fail_on_fdatasync_call = -1;

int fault_injecting_fdatasync(int fd) {
  ++g_fdatasync_count;
  if (g_fail_on_fdatasync_call > 0 &&
      g_fdatasync_count == g_fail_on_fdatasync_call) {
    errno = EIO;
    return -1;
  }
  return ::fdatasync(fd);
}

PAGEDB_TEST(NoStealRollbackOnPreCommitWalSyncFailure) {
  test::TempDir tmp("pagedb_wal_nosteal");
  const auto db_dir = tmp.path() / "db";

  PosixIoOps custom_ops = PosixIoOps::default_ops();
  custom_ops.fdatasync_fn = &fault_injecting_fdatasync;
  g_fdatasync_count = 0;
  g_fail_on_fdatasync_call = -1;

  DatabaseOptions opts{};
  opts.buffer_pool_frames = kMinBufferPoolFrames;
  opts.io_ops = &custom_ops;

  {
    auto db_res = Database::open(db_dir, opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    // Insert 40 records cleanly (forcing multiple splits).
    for (int i = 0; i < 40; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k_%03d", i);
      PAGEDB_ASSERT_OK(db.put(std::string(kbuf) + std::string(100, 'a'),
                              std::string(200, 'v')));
    }
    PAGEDB_ASSERT_OK(db.verify());

    // Fail the very next fdatasync (which is the WAL commit sync for the 41st
    // put).
    g_fail_on_fdatasync_call = g_fdatasync_count + 1;
    Status fail_s =
        db.put("k_uncommitted_" + std::string(100, 'z'), std::string(200, 'x'));
    PAGEDB_ASSERT_EQ(fail_s.code(), ErrorCode::kIoError);

    // Subsequent mutations on the poisoned instance must fail with
    // kDatabasePoisoned.
    PAGEDB_ASSERT_EQ(db.put("another", "val").code(),
                     ErrorCode::kDatabasePoisoned);

    g_fail_on_fdatasync_call = -1;
    (void)db.close();
  }

  // Reopen the database: the failed 41st transaction must be completely absent,
  // all 40 committed records must be intact, and verify() must succeed.
  {
    DatabaseOptions reopen_opts = opts;
    reopen_opts.create_if_missing = false;
    auto db_res = Database::open(db_dir, reopen_opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_EQ(
        db.get("k_uncommitted_" + std::string(100, 'z')).code(),
        ErrorCode::kNotFound);

    auto scanned = db.scan().value();
    PAGEDB_ASSERT_EQ(scanned.size(), 40U);
    PAGEDB_ASSERT_OK(db.close());
  }
}

PAGEDB_TEST(CrashRecoveryRepairsTornSuperblockTornDataPagesAndTrailingBytes) {
  test::TempDir tmp("pagedb_wal_crash_repair");
  const auto db_dir = tmp.path() / "db";

  DatabaseOptions opts{};
  opts.buffer_pool_frames = kMinBufferPoolFrames;

  std::map<std::string, std::string, UnsignedByteLess> expected;
  std::uint32_t committed_pages = 0;
  {
    auto db_res = Database::open(db_dir, opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    for (int i = 0; i < 80; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "item_%04d", i);
      std::string k = std::string(kbuf) + std::string(90, 'k');
      std::string v(220, static_cast<char>('A' + (i % 26)));
      PAGEDB_ASSERT_OK(db.put(k, v));
      expected[k] = v;
    }
    for (int i = 0; i < 30; i += 2) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "item_%04d", i);
      std::string k = std::string(kbuf) + std::string(90, 'k');
      PAGEDB_ASSERT_OK(db.del(k));
      expected.erase(k);
    }

    PAGEDB_ASSERT_OK(db.verify());
    committed_pages = db.page_count();
    PAGEDB_ASSERT_OK(db.close());
  }

  // Simulate a violent crash mid-writeback on pagedb.data after WAL commit:
  // 1. Corrupt PageId(0) (Superblock) in pagedb.data so its checksum & magic
  //    are destroyed (simulating a torn Page 0 write).
  // 2. Corrupt PageId(1) and PageId(2) in pagedb.data (simulating torn tree
  //    node writes).
  // 3. Append 1337 garbage bytes past the end of pagedb.data (simulating a
  //    partial page allocation write before crash).
  const auto data_path = db_dir / "pagedb.data";
  {
    auto fh_res = FileHandle::open(data_path, OpenMode::kReadWrite);
    PAGEDB_ASSERT_OK(fh_res);
    FileHandle& fh = fh_res.value();

    std::array<std::byte, 256> garbage{};
    garbage.fill(std::byte{0xDE});
    // Tear PageId(0) (Superblock).
    PAGEDB_ASSERT_OK(fh.write_exact_at(0, garbage));
    // Tear PageId(1) and PageId(2).
    PAGEDB_ASSERT_OK(fh.write_exact_at(kPageSize + 64, garbage));
    PAGEDB_ASSERT_OK(fh.write_exact_at(2ULL * kPageSize + 128, garbage));
    // Append 1337 trailing partial-page bytes at EOF.
    const std::uint64_t orig_size = fh.size_bytes().value();
    std::vector<std::byte> trailing(1337, std::byte{0xAB});
    PAGEDB_ASSERT_OK(fh.write_exact_at(orig_size, trailing));
    PAGEDB_ASSERT_OK(fh.sync_data());
    PAGEDB_ASSERT_OK(fh.close());
  }

  // Reopen Database: WAL recovery must repair PageId(0), repair PageId(1) and
  // PageId(2), truncate the 1337 trailing bytes back to committed_pages * 4096,
  // and pass full B+ tree & allocator verification!
  for (int recovery_run = 0; recovery_run < 2; ++recovery_run) {
    // On run 1, corrupt PageId(1) again before reopening to prove recovery is
    // 100% idempotent when interrupted and re-executed (Invariant W-4).
    if (recovery_run == 1) {
      auto fh_res = FileHandle::open(data_path, OpenMode::kReadWrite);
      PAGEDB_ASSERT_OK(fh_res);
      std::array<std::byte, 64> bad{};
      bad.fill(std::byte{0x77});
      PAGEDB_ASSERT_OK(fh_res.value().write_exact_at(kPageSize, bad));
      PAGEDB_ASSERT_OK(fh_res.value().close());
    }

    DatabaseOptions reopen_opts = opts;
    reopen_opts.create_if_missing = false;
    auto db_res = Database::open(db_dir, reopen_opts);
    PAGEDB_ASSERT_OK(db_res);
    Database& db = *db_res.value();

    PAGEDB_ASSERT_OK(db.verify());
    PAGEDB_ASSERT_EQ(db.page_count(), committed_pages);

    auto scanned = db.scan().value();
    PAGEDB_ASSERT_EQ(scanned.size(), expected.size());
    std::size_t idx = 0;
    for (const auto& [k, v] : expected) {
      PAGEDB_ASSERT_EQ(scanned[idx].key, k);
      PAGEDB_ASSERT_EQ(scanned[idx].value, v);
      ++idx;
    }
    PAGEDB_ASSERT_OK(db.close());
  }
}

PAGEDB_TEST(RandomizedMultiCrashDifferentialRecoveryTest) {
  test::TempDir tmp("pagedb_wal_rand_crash");
  const auto db_dir = tmp.path() / "db";
  const auto data_path = db_dir / "pagedb.data";
  const auto wal_path = db_dir / "pagedb.wal";

  DatabaseOptions opts{};
  opts.buffer_pool_frames = kMinBufferPoolFrames;

  std::map<std::string, std::string, UnsignedByteLess> model;
  std::mt19937_64 rng(0x5041474544425731ULL);

  for (int cycle = 0; cycle < 12; ++cycle) {
    {
      auto db_res = Database::open(db_dir, opts);
      PAGEDB_ASSERT_OK(db_res);
      Database& db = *db_res.value();
      PAGEDB_ASSERT_OK(db.verify());

      for (int step = 0; step < 20; ++step) {
        const std::uint32_t key_id =
            static_cast<std::uint32_t>(rng() % 90ULL);
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "rk_%03u", key_id);
        std::string key = std::string(kbuf) + std::string(60, 'm');

        if ((rng() % 100ULL) < 65ULL) {
          std::string val(120 + (rng() % 150ULL),
                          static_cast<char>('a' + (step % 26)));
          PAGEDB_ASSERT_OK(db.put(key, val));
          model[key] = val;
        } else {
          auto del_res = db.del(key);
          PAGEDB_ASSERT_OK(del_res);
          const bool erased = (model.erase(key) > 0);
          PAGEDB_ASSERT_EQ(del_res.value(), erased);
        }
      }
      PAGEDB_ASSERT_OK(db. close());
    }

    // Inject a simulated crash artifact between cycles:
    if (cycle % 2 == 0) {
      // Tear PageId(0) or PageId(1) in pagedb.data and append trailing bytes.
      auto fh_res = FileHandle::open(data_path, OpenMode::kReadWrite);
      PAGEDB_ASSERT_OK(fh_res);
      std::array<std::byte, 32> tear{};
      tear.fill(std::byte{0xEE});
      PAGEDB_ASSERT_OK(fh_res.value().write_exact_at(0, tear));
      const std::uint64_t sz = fh_res.value().size_bytes().value();
      PAGEDB_ASSERT_OK(fh_res.value().write_exact_at(sz, tear));
      PAGEDB_ASSERT_OK(fh_res.value().close());
    } else {
      // Append an incomplete/torn uncommitted record at the tail of pagedb.wal.
      auto fh_res = FileHandle::open(wal_path, OpenMode::kReadWrite);
      PAGEDB_ASSERT_OK(fh_res);
      const std::uint64_t sz = fh_res.value().size_bytes().value();
      std::array<std::byte, 20> partial_rec{};
      partial_rec.fill(std::byte{0x57});
      PAGEDB_ASSERT_OK(fh_res.value().write_exact_at(sz, partial_rec));
      PAGEDB_ASSERT_OK(fh_res.value().close());
    }
  }

  // Final reopen and full comparison against reference model.
  auto db_res = Database::open(db_dir, opts);
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
  PAGEDB_ASSERT_OK(db.close());
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
