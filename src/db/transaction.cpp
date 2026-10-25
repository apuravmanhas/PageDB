#include "pagedb/transaction.h"

#include <utility>

#include "pagedb/database.h"

namespace pagedb {

Transaction::Transaction(Database& db, TxnId txn_id) noexcept
    : db_(&db), txn_id_(txn_id), is_active_(true) {}

Transaction::~Transaction() noexcept {
  if (is_active_) {
    (void)rollback();
  }
}

Transaction::Transaction(Transaction&& other) noexcept
    : db_(other.db_),
      txn_id_(other.txn_id_),
      is_active_(std::exchange(other.is_active_, false)) {}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
  if (this != &other) {
    if (is_active_) {
      (void)rollback();
    }
    db_ = other.db_;
    txn_id_ = other.txn_id_;
    is_active_ = std::exchange(other.is_active_, false);
  }
  return *this;
}

Status Transaction::put(std::span<const std::byte> key,
                         std::span<const std::byte> value) {
  if (!is_active_ || db_ == nullptr) {
    return Status::InvalidArgument("put called on inactive Transaction");
  }
  return db_->txn_put(key, value);
}

Status Transaction::put(std::string_view key, std::string_view value) {
  const std::span<const std::byte> k(
      reinterpret_cast<const std::byte*>(key.data()), key.size());
  const std::span<const std::byte> v(
      reinterpret_cast<const std::byte*>(value.data()), value.size());
  return put(k, v);
}

Result<std::string> Transaction::get(std::span<const std::byte> key) {
  if (!is_active_ || db_ == nullptr) {
    return Status::InvalidArgument("get called on inactive Transaction");
  }
  return db_->txn_get(key);
}

Result<std::string> Transaction::get(std::string_view key) {
  const std::span<const std::byte> k(
      reinterpret_cast<const std::byte*>(key.data()), key.size());
  return get(k);
}

Result<bool> Transaction::del(std::span<const std::byte> key) {
  if (!is_active_ || db_ == nullptr) {
    return Status::InvalidArgument("del called on inactive Transaction");
  }
  return db_->txn_del(key);
}

Result<bool> Transaction::del(std::string_view key) {
  const std::span<const std::byte> k(
      reinterpret_cast<const std::byte*>(key.data()), key.size());
  return del(k);
}

Result<std::vector<KeyValue>> Transaction::scan(
    std::optional<std::string_view> start_key,
    std::optional<std::string_view> end_key) {
  std::optional<std::span<const std::byte>> start_bytes;
  std::optional<std::span<const std::byte>> end_bytes;
  if (start_key.has_value()) {
    start_bytes = std::as_bytes(
        std::span<const char>(start_key->data(), start_key->size()));
  }
  if (end_key.has_value()) {
    end_bytes =
        std::as_bytes(std::span<const char>(end_key->data(), end_key->size()));
  }
  return scan_bytes(start_bytes, end_bytes);
}

Result<std::vector<KeyValue>> Transaction::scan_bytes(
    std::optional<std::span<const std::byte>> start_key,
    std::optional<std::span<const std::byte>> end_key) {
  if (!is_active_ || db_ == nullptr) {
    return Status::InvalidArgument("scan called on inactive Transaction");
  }
  return db_->txn_scan_bytes(start_key, end_key);
}

Status Transaction::commit() {
  if (!is_active_ || db_ == nullptr) {
    return Status::InvalidArgument("commit called on inactive Transaction");
  }
  is_active_ = false;
  return db_->txn_commit(txn_id_);
}

Status Transaction::rollback() noexcept {
  if (!is_active_ || db_ == nullptr) {
    return Status::Ok();
  }
  is_active_ = false;
  return db_->txn_rollback(txn_id_);
}

}  // namespace pagedb
