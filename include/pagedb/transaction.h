#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pagedb/btree.h"
#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

class Database;

class Transaction {
 public:
  Transaction(Database& db, TxnId txn_id) noexcept;
  ~Transaction() noexcept;

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  Transaction(Transaction&& other) noexcept;
  Transaction& operator=(Transaction&& other) noexcept;

  [[nodiscard]] TxnId id() const noexcept { return txn_id_; }
  [[nodiscard]] bool is_active() const noexcept { return is_active_; }

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

  // Durably commits all staged mutations to pagedb.wal and installs them into
  // pagedb.data. Inactive on return.
  [[nodiscard]] Status commit();

  // Discards all staged mutations in this transaction, drops dirty buffer pool
  // frames, and restores committed metadata. Inactive on return.
  [[nodiscard]] Status rollback() noexcept;

 private:
  Database* db_{nullptr};
  TxnId txn_id_{kInvalidTxnId};
  bool is_active_{false};
};

}  // namespace pagedb
