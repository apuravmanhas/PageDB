#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pagedb/btree.h"
#include "pagedb/buffer_pool.h"
#include "pagedb/file_handle.h"
#include "pagedb/page_allocator.h"
#include "pagedb/page_store.h"
#include "pagedb/status.h"
#include "pagedb/types.h"
#include "pagedb/wal.h"

namespace pagedb {

inline constexpr std::size_t kMinBufferPoolFrames = 4;
inline constexpr std::size_t kDefaultBufferPoolFrames = 64;

struct DatabaseOptions {
  bool create_if_missing{true};
  bool error_if_exists{false};
  std::size_t buffer_pool_frames{kDefaultBufferPoolFrames};
  // Automatically triggers a checkpoint when pagedb.wal reaches this size.
  // 0 disables auto-checkpointing.
  std::uint64_t wal_auto_checkpoint_bytes{4ULL * 1024ULL * 1024ULL};
  const PosixIoOps* io_ops{&PosixIoOps::default_ops()};
};

class Transaction;

class Database {
 public:
  Database() = default;
  ~Database() noexcept;

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<Database>> open(
      const std::filesystem::path& db_dir,
      const DatabaseOptions& options = {});

  // Begins an explicit multi-operation ACID transaction.
  [[nodiscard]] Result<std::unique_ptr<Transaction>> begin_transaction();

  [[nodiscard]] Status put(std::span<const std::byte> key,
                           std::span<const std::byte> value);
  [[nodiscard]] Status put(std::string_view key, std::string_view value);

  [[nodiscard]] Result<std::string> get(std::span<const std::byte> key);
  [[nodiscard]] Result<std::string> get(std::string_view key);

  [[nodiscard]] Result<bool> del(std::span<const std::byte> key);
  [[nodiscard]] Result<bool> del(std::string_view key);

  [[nodiscard]] Result<std::vector<KeyValue>> scan(
      std::optional<std::string_view> start_key = std::nullopt,
      std::optional<std::string_view> end_key = std::nullopt);
  [[nodiscard]] Result<std::vector<KeyValue>> scan_bytes(
      std::optional<std::span<const std::byte>> start_key = std::nullopt,
      std::optional<std::span<const std::byte>> end_key = std::nullopt);

  [[nodiscard]] Status verify() const;
  [[nodiscard]] Status sync();

  // Performs a full durable checkpoint: flushes all dirty buffer pool frames to
  // pagedb.data, fsyncs pagedb.data, durably advances Superblock::checkpoint_lsn
  // on disk, appends a CHECKPOINT record to pagedb.wal, and reclaims/resets
  // pagedb.wal back to 32 bytes with base_lsn = next_lsn.
  [[nodiscard]] Status checkpoint();

  [[nodiscard]] Status close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return is_open_; }
  [[nodiscard]] Result<std::uint32_t> tree_height() const;
  [[nodiscard]] std::uint32_t page_count() const noexcept;
  [[nodiscard]] PageId root_page_id() const noexcept;
  [[nodiscard]] PageId free_list_head() const noexcept;
  [[nodiscard]] Lsn max_committed_lsn() const noexcept;
  [[nodiscard]] Lsn checkpoint_lsn() const noexcept;
  [[nodiscard]] Result<std::uint64_t> wal_size_bytes() const;
  [[nodiscard]] BufferPoolStats buffer_pool_stats() const noexcept;

 private:
  friend class Transaction;

  [[nodiscard]] Status ensure_open() const noexcept;
  [[nodiscard]] Status commit_active_transaction();
  void abort_active_transaction() noexcept;

  [[nodiscard]] Status txn_put(std::span<const std::byte> key,
                               std::span<const std::byte> value);
  [[nodiscard]] Result<std::string> txn_get(std::span<const std::byte> key);
  [[nodiscard]] Result<bool> txn_del(std::span<const std::byte> key);
  [[nodiscard]] Result<std::vector<KeyValue>> txn_scan_bytes(
      std::optional<std::span<const std::byte>> start_key,
      std::optional<std::span<const std::byte>> end_key);
  [[nodiscard]] Status txn_commit(TxnId txn_id);
  [[nodiscard]] Status txn_rollback(TxnId txn_id) noexcept;

  std::unique_ptr<PageStore> store_;
  std::unique_ptr<Wal> wal_;
  std::unique_ptr<PageAllocator> allocator_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<BTree> tree_;
  DatabaseOptions options_{};
  bool is_open_{false};
  bool has_active_transaction_{false};
};

}  // namespace pagedb
