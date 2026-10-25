#include "pagedb/database.h"

#include <utility>

namespace pagedb {

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
  store_opts.io_ops = options.io_ops;

  auto store_res = PageStore::open(db_dir, store_opts);
  if (!store_res.ok()) {
    return store_res.status();
  }

  auto db = std::make_unique<Database>();
  db->store_ = std::move(store_res.value());
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

Status Database::put(std::span<const std::byte> key,
                     std::span<const std::byte> value) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->put(key, value);
}

Status Database::put(std::string_view key, std::string_view value) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->put(key, value);
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
  return tree_->del(key);
}

Result<bool> Database::del(std::string_view key) {
  Status s = ensure_open();
  if (!s.ok()) {
    return s;
  }
  return tree_->del(key);
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
  Status close_s = store_->close();
  store_.reset();
  is_open_ = false;
  if (!flush_s.ok()) {
    return flush_s;
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

BufferPoolStats Database::buffer_pool_stats() const noexcept {
  return is_open_ ? pool_->stats() : BufferPoolStats{};
}

}  // namespace pagedb
