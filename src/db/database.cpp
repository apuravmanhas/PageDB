#include "pagedb/database.h"

#include <utility>

namespace pagedb {
namespace {

constexpr const char* kWalFileName = "pagedb.wal";

}  // namespace

Database::~Database() noexcept {
  if (is_open_) {
    (void)close();
  }
}

Result<std::unique_ptr<Database>> Database::open(
    const std::filesystem::path& db_dir, const DatabaseOptions& options) {
  if (options.buffer_pool_frames < kMinBufferPoolFrames) {
    return Status::InvalidArgument(
        "DatabaseOptions::buffer_pool_frames must be >= 4");
  }
  if (options.io_ops == nullptr) {
    return Status::InvalidArgument("DatabaseOptions::io_ops must not be null");
  }

  PageStoreOptions store_opts{};
  store_opts.create_if_missing = options.create_if_missing;
  store_opts.error_if_exists = options.error_if_exists;
  store_opts.defer_validation_for_recovery = true;
  store_opts.io_ops = options.io_ops;

  auto store_res = PageStore::open(db_dir, store_opts);
  if (!store_res.ok()) {
    return store_res.status();
  }
  std::unique_ptr<PageStore> store = std::move(store_res.value());

  auto wal_res = Wal::open(db_dir / kWalFileName, options.io_ops);
  if (!wal_res.ok()) {
    return wal_res.status();
  }
  std::unique_ptr<Wal> wal = std::move(wal_res.value());

  auto rec_res = wal->recover();
  if (!rec_res.ok()) {
    return rec_res.status();
  }
  const WalRecoveryReport& report = rec_res.value();

  if (!report.committed_updates.empty() || report.truncated_uncommitted_tail) {
    for (const WalPageUpdate& upd : report.committed_updates) {
      Status ws =
          store->write_raw_page_for_recovery(upd.page_id, upd.page_image);
      if (!ws.ok()) {
        return ws;
      }
    }

    auto sb_res = store->read_raw_superblock_for_recovery();
    if (!sb_res.ok()) {
      return sb_res.status();
    }
    Status ts = store->truncate_to_page_count(sb_res.value().next_page_id.value);
    if (!ts.ok()) {
      return ts;
    }
    Status ss = store->sync();
    if (!ss.ok()) {
      return ss;
    }
  }

  const bool has_wal_records = report.max_committed_lsn.is_valid() ||
                               report.valid_end_offset > kWalFileHeaderSize;
  Status vs = store->validate_and_load_metadata(report.max_committed_lsn,
                                                has_wal_records);
  if (!vs.ok()) {
    return vs;
  }

  auto db = std::make_unique<Database>();
  db->store_ = std::move(store);
  db->wal_ = std::move(wal);
  db->allocator_ = std::make_unique<PageAllocator>(*db->store_);
  db->pool_ =
      std::make_unique<BufferPool>(*db->store_, options.buffer_pool_frames);
  db->tree_ = std::make_unique<BTree>(*db->pool_, *db->allocator_);
  db->is_open_ = true;

  return Result<std::unique_ptr<Database>>::Ok(std::move(db));
}

Status Database::ensure_open() const noexcept {
  if (!is_open_) {
    return Status::InvalidArgument("operation on closed Database");
  }
  if (store_->is_poisoned()) {
    return Status::DatabasePoisoned("Database is poisoned after I/O failure");
  }
  return Status::Ok();
}

void Database::abort_active_transaction() noexcept {
  pool_->discard_all_pages();
  store_->abort_transaction();
  allocator_->reload_superblock();
}

Status Database::commit_active_transaction() {
  Status flush_s = pool_->flush_dirty_pages();
  if (!flush_s.ok()) {
    abort_active_transaction();
    return flush_s;
  }

  if (store_->staged_pages().empty()) {
    store_->abort_transaction();
    return Status::Ok();
  }

  // Ensure PageId(0) (Superblock) is included in every committing transaction's
  // WAL write-set so that Superblock::page_lsn advances monotonically and a
  // torn Page 0 can always be repaired from pagedb.wal even before the first
  // B+ tree split.
  if (!store_->staged_pages().contains(kSuperblockPageId)) {
    Status sb_s = allocator_->flush_superblock();
    if (!sb_s.ok()) {
      abort_active_transaction();
      return sb_s;
    }
  }

  const std::uint64_t txn_wal_offset = wal_->write_offset();
  const Lsn txn_start_lsn = wal_->peek_next_lsn();
  const Lsn prev_max_committed_lsn = wal_->max_committed_lsn();
  const TxnId txn_id = wal_->allocate_txn_id();

  auto rollback_wal_and_txn = [&]() noexcept {
    wal_->discard_unsynced_from(txn_wal_offset, txn_start_lsn, txn_id,
                                prev_max_committed_lsn);
    store_->poison();
    abort_active_transaction();
  };

  auto begin_res = wal_->append_begin(txn_id);
  if (!begin_res.ok()) {
    rollback_wal_and_txn();
    return begin_res.status();
  }

  for (auto& [page_id, page_buf] : store_->staged_pages()) {
    auto img_res = wal_->append_page_image(txn_id, page_id, page_buf);
    if (!img_res.ok()) {
      rollback_wal_and_txn();
      return img_res.status();
    }
    if (page_id == kSuperblockPageId) {
      store_->set_staged_superblock_lsn(img_res.value());
    } else {
      pool_->update_resident_frame(page_id, page_buf);
    }
  }

  auto commit_res = wal_->append_commit(txn_id);
  if (!commit_res.ok()) {
    rollback_wal_and_txn();
    return commit_res.status();
  }

  // Durable commit point (Invariants W-2, W-3): synchronize WAL before any
  // staged page image is installed into pagedb.data.
  Status wal_sync_s = wal_->sync();
  if (!wal_sync_s.ok()) {
    rollback_wal_and_txn();
    return wal_sync_s;
  }

  Status install_s = store_->install_staged_pages();
  if (!install_s.ok()) {
    return install_s;
  }
  allocator_->reload_superblock();
  return Status::Ok();
}

Status Database::put(std::span<const std::byte> key,
                     std::span<const std::byte> value) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }

  store_->begin_transaction();
  Status op_s = tree_->put(key, value);
  if (!op_s.ok()) {
    abort_active_transaction();
    return op_s;
  }
  return commit_active_transaction();
}

Status Database::put(std::string_view key, std::string_view value) {
  const std::span<const std::byte> k(
      reinterpret_cast<const std::byte*>(key.data()), key.size());
  const std::span<const std::byte> v(
      reinterpret_cast<const std::byte*>(value.data()), value.size());
  return put(k, v);
}

Result<std::string> Database::get(std::span<const std::byte> key) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->get(key);
}

Result<std::string> Database::get(std::string_view key) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->get(key);
}

Result<bool> Database::del(std::span<const std::byte> key) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }

  store_->begin_transaction();
  auto del_res = tree_->del(key);
  if (!del_res.ok()) {
    abort_active_transaction();
    return del_res.status();
  }
  Status commit_s = commit_active_transaction();
  if (!commit_s.ok()) {
    return commit_s;
  }
  return del_res;
}

Result<bool> Database::del(std::string_view key) {
  const std::span<const std::byte> k(
      reinterpret_cast<const std::byte*>(key.data()), key.size());
  return del(k);
}

Result<std::vector<KeyValue>> Database::scan(
    std::optional<std::string_view> start_key,
    std::optional<std::string_view> end_key) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->scan(start_key, end_key);
}

Result<std::vector<KeyValue>> Database::scan_bytes(
    std::optional<std::span<const std::byte>> start_key,
    std::optional<std::span<const std::byte>> end_key) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->scan_bytes(start_key, end_key);
}

Status Database::verify() const {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->verify();
}

Status Database::sync() {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  Status ws = wal_->sync();
  if (!ws.ok()) {
    store_->poison();
    return ws;
  }
  return pool_->flush_all();
}

Status Database::close() noexcept {
  if (!is_open_) {
    return Status::Ok();
  }
  Status flush_s = Status::Ok();
  if (!store_->is_poisoned()) {
    flush_s = pool_->flush_all();
  }
  tree_.reset();
  pool_.reset();
  allocator_.reset();
  Status wal_close_s = wal_ ? wal_->close() : Status::Ok();
  wal_.reset();
  Status close_s = store_->close();
  store_.reset();
  is_open_ = false;
  if (!flush_s.ok()) {
    return flush_s;
  }
  if (!wal_close_s.ok()) {
    return wal_close_s;
  }
  return close_s;
}

Result<std::uint32_t> Database::tree_height() const {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->height();
}

std::uint32_t Database::page_count() const noexcept {
  return is_open_ ? store_->page_count() : 0U;
}

PageId Database::root_page_id() const noexcept {
  return is_open_ ? allocator_->root_page_id() : kInvalidPageId;
}

PageId Database::free_list_head() const noexcept {
  return is_open_ ? allocator_->free_list_head() : kInvalidPageId;
}

Lsn Database::max_committed_lsn() const noexcept {
  return (is_open_ && wal_) ? wal_->max_committed_lsn() : kInvalidLsn;
}

BufferPoolStats Database::buffer_pool_stats() const noexcept {
  return is_open_ ? pool_->stats() : BufferPoolStats{};
}

}  // namespace pagedb
