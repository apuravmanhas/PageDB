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

namespace pagedb {

inline constexpr std::size_t kMinBufferPoolFrames = 4;
inline constexpr std::size_t kDefaultBufferPoolFrames = 64;

struct DatabaseOptions {
  bool create_if_missing{true};
  bool error_if_exists{false};
  std::size_t buffer_pool_frames{kDefaultBufferPoolFrames};
  const PosixIoOps* io_ops{&PosixIoOps::default_ops()};
};

class Database {
 public:
  Database() = default;
  ~Database() noexcept;

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<Database>> open(
      const std::filesystem::path& db_dir,
      const DatabaseOptions& options = {});

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
  [[nodiscard]] Status close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return is_open_; }
  [[nodiscard]] Result<std::uint32_t> tree_height() const;
  [[nodiscard]] std::uint32_t page_count() const noexcept;
  [[nodiscard]] PageId root_page_id() const noexcept;
  [[nodiscard]] PageId free_list_head() const noexcept;
  [[nodiscard]] BufferPoolStats buffer_pool_stats() const noexcept;

 private:
  [[nodiscard]] Status ensure_open() const noexcept;

  std::unique_ptr<PageStore> store_;
  std::unique_ptr<PageAllocator> allocator_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<BTree> tree_;
  bool is_open_{false};
};

}  // namespace pagedb
