#include "pagedb/page_store.h"

#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#include "pagedb/byte_codec.h"

namespace pagedb {
namespace {

constexpr const char* kDataFileName = "pagedb.data";
constexpr const char* kDataTempFileName = "pagedb.data.tmp";
constexpr const char* kLockFileName = "pagedb.lock";

Status write_initial_pages(const std::filesystem::path& tmp_path,
                           const PosixIoOps* ops) {
  Superblock sb{};
  sb.root_page_id = kInitialRootPageId;
  sb.next_page_id = PageId(2);
  sb.free_list_head = kInvalidPageId;
  sb.checkpoint_lsn = kInvalidLsn;
  sb.page_lsn = kInvalidLsn;

  PageBuffer page0{};
  Status s = PageCodec::encode_superblock(sb, page0);
  if (!s.ok()) {
    return s;
  }

  SlottedPage root_leaf;
  s = root_leaf.init(kInitialRootPageId, PageType::kLeaf);
  if (!s.ok()) {
    return s;
  }
  PageBuffer page1{};
  s = root_leaf.encode(page1);
  if (!s.ok()) {
    return s;
  }

  auto fh_res = FileHandle::open(tmp_path, OpenMode::kCreateNew, ops);
  if (!fh_res.ok()) {
    return fh_res.status();
  }
  FileHandle& fh = fh_res.value();
  s = fh.write_exact_at(0, page0);
  if (!s.ok()) {
    return s;
  }
  s = fh.write_exact_at(kPageSize, page1);
  if (!s.ok()) {
    return s;
  }
  s = fh.sync_data();
  if (!s.ok()) {
    return s;
  }
  return fh.close();
}

// Creates pagedb.data atomically: the two initial pages are written and synced
// into pagedb.data.tmp, which is then renamed into place and the directory is
// synced. A failure or crash at any point leaves either no pagedb.data at all
// or a complete one, never a truncated file that blocks every later open.
// Caller must hold the database lock, which makes removing a stale temp file
// safe.
Status create_data_file_atomically(const std::filesystem::path& db_dir,
                                   const PosixIoOps* ops) {
  const auto data_path = db_dir / kDataFileName;
  const auto tmp_path = db_dir / kDataTempFileName;

  std::error_code ec;
  std::filesystem::remove(tmp_path, ec);
  if (ec) {
    return Status::IoError("failed to remove stale " +
                           std::string(kDataTempFileName) + ": " +
                           ec.message());
  }

  Status s = write_initial_pages(tmp_path, ops);
  if (!s.ok()) {
    std::error_code rm_ec;
    std::filesystem::remove(tmp_path, rm_ec);
    return s;
  }

  std::filesystem::rename(tmp_path, data_path, ec);
  if (ec) {
    std::error_code rm_ec;
    std::filesystem::remove(tmp_path, rm_ec);
    return Status::IoError("failed to rename initial data file: " +
                           ec.message());
  }
  return FileHandle::sync_directory(db_dir);
}

}  // namespace

Result<std::unique_ptr<PageStore>> PageStore::open(
    const std::filesystem::path& db_dir, const PageStoreOptions& options) {
  if (options.io_ops == nullptr) {
    return Status::InvalidArgument("PageStoreOptions::io_ops must not be null");
  }
  const auto data_path = db_dir / kDataFileName;
  const auto lock_path = db_dir / kLockFileName;

  std::error_code ec;
  const bool dir_exists = std::filesystem::exists(db_dir, ec);
  if (ec) {
    return Status::IoError("failed to stat database directory: " +
                           ec.message());
  }
  if (!dir_exists && !options.create_if_missing) {
    return Status::NotFound("database directory does not exist");
  }
  Status dir_s = FileHandle::ensure_directory_0700(db_dir);
  if (!dir_s.ok()) {
    return dir_s;
  }

  auto lock_res = DatabaseLock::acquire(lock_path);
  if (!lock_res.ok()) {
    return lock_res.status();
  }

  const bool data_exists = std::filesystem::exists(data_path, ec);
  if (ec) {
    return Status::IoError("failed to stat pagedb.data: " + ec.message());
  }
  if (data_exists && options.error_if_exists) {
    return Status::AlreadyExists("database file already exists");
  }
  if (!data_exists && !options.create_if_missing) {
    return Status::NotFound("pagedb.data does not exist");
  }

  if (!data_exists) {
    Status init_s = create_data_file_atomically(db_dir, options.io_ops);
    if (!init_s.ok()) {
      return init_s;
    }
  }

  auto fh_res = FileHandle::open(data_path, OpenMode::kReadWrite, options.io_ops);
  if (!fh_res.ok()) {
    return fh_res.status();
  }

  auto store = std::make_unique<PageStore>();
  store->db_dir_ = db_dir;
  store->lock_ = std::move(lock_res.value());
  store->data_file_ = std::move(fh_res.value());

  auto file_size_res = store->data_file_.size_bytes();
  if (!file_size_res.ok()) {
    return file_size_res.status();
  }
  const std::uint64_t file_size = file_size_res.value();

  // Validate existing file length.
  if (file_size < 2ULL * kPageSize || (file_size % kPageSize) != 0ULL) {
    return Status::Corruption(
        "pagedb.data size is not a valid multiple of 4096 bytes (>= 2 pages)");
  }
  const std::uint64_t num_pages = file_size / kPageSize;
  if (num_pages >=
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
    return Status::Corruption("pagedb.data page count overflows PageId");
  }

  store->page_count_ = static_cast<std::uint32_t>(num_pages);

  PageBuffer page0{};
  Status r0 = store->data_file_.read_exact_at(0, page0);
  if (!r0.ok()) {
    return r0;
  }
  auto sb_res = PageCodec::decode_superblock(page0);
  if (!sb_res.ok()) {
    return sb_res.status();
  }
  const Superblock& sb = sb_res.value();
  if (sb.next_page_id.value != store->page_count_) {
    return Status::Corruption(
        "Superblock next_page_id does not match physical file page count");
  }

  // Validate root page header and checksum on open.
  PageBuffer root_buf{};
  Status rr = store->read_page(sb.root_page_id, root_buf);
  if (!rr.ok()) {
    return rr;
  }

  store->superblock_ = sb;
  return Result<std::unique_ptr<PageStore>>::Ok(std::move(store));
}

Status PageStore::read_page(PageId page_id,
                            std::span<std::byte, kPageSize> out_page) const {
  if (!data_file_.is_open()) {
    return Status::InvalidArgument("read_page on closed PageStore");
  }
  if (!page_id.is_valid() || page_id.value >= page_count_) {
    return Status::InvalidArgument("read_page PageId out of allocated range");
  }

  auto off_res = checked_page_offset(page_id, kPageSize);
  if (!off_res.ok()) {
    return off_res.status();
  }

  Status rs = data_file_.read_exact_at(off_res.value(), out_page);
  if (!rs.ok()) {
    return rs;
  }

  auto hdr_res = PageCodec::decode_and_verify_header(out_page, page_id);
  if (!hdr_res.ok()) {
    return hdr_res.status();
  }
  return Status::Ok();
}

Status PageStore::write_page(
    PageId page_id, std::span<const std::byte, kPageSize> page_image) {
  if (poisoned_) {
    return Status::DatabasePoisoned("write_page forbidden on poisoned PageStore");
  }
  if (!data_file_.is_open()) {
    return Status::InvalidArgument("write_page on closed PageStore");
  }
  if (!page_id.is_valid() || page_id.value > page_count_) {
    return Status::InvalidArgument("write_page PageId out of bounds");
  }

  auto hdr_res = PageCodec::decode_and_verify_header(page_image, page_id);
  if (!hdr_res.ok()) {
    return hdr_res.status();
  }

  Superblock updated_sb{};
  if (page_id == kSuperblockPageId) {
    auto sb_res = PageCodec::decode_superblock(page_image);
    if (!sb_res.ok()) {
      return sb_res.status();
    }
    updated_sb = sb_res.value();
  }

  auto off_res = checked_page_offset(page_id, kPageSize);
  if (!off_res.ok()) {
    return off_res.status();
  }

  Status ws = data_file_.write_exact_at(off_res.value(), page_image);
  if (!ws.ok()) {
    poisoned_ = true;
    return ws;
  }

  if (page_id.value == page_count_) {
    ++page_count_;
  }
  if (page_id == kSuperblockPageId) {
    superblock_ = updated_sb;
  }
  return Status::Ok();
}

Status PageStore::sync() {
  if (poisoned_) {
    return Status::DatabasePoisoned("sync forbidden on poisoned PageStore");
  }
  if (!data_file_.is_open()) {
    return Status::InvalidArgument("sync on closed PageStore");
  }
  Status s = data_file_.sync_data();
  if (!s.ok()) {
    poisoned_ = true;
  }
  return s;
}

Status PageStore::close() noexcept {
  Status s1 = data_file_.close();
  Status s2 = lock_.release();
  if (!s1.ok()) {
    return s1;
  }
  return s2;
}

}  // namespace pagedb
