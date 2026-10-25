#include "pagedb/file_handle.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "pagedb/byte_codec.h"

namespace pagedb {
namespace {

std::string errno_message(const char* prefix, int err_num) {
  std::string msg(prefix);
  msg.append(": ");
  msg.append(std::strerror(err_num));
  return msg;
}

std::mutex& active_locks_mutex() {
  static std::mutex m;
  return m;
}

std::set<std::pair<dev_t, ino_t>>& active_locks_set() {
  static std::set<std::pair<dev_t, ino_t>> s;
  return s;
}

}  // namespace

const PosixIoOps& PosixIoOps::default_ops() noexcept {
  static const PosixIoOps kDefault{
      .pread_fn = &::pread,
      .pwrite_fn = &::pwrite,
      .fdatasync_fn = &::fdatasync,
      .fsync_fn = &::fsync,
      .fstat_fn = &::fstat,
      .ftruncate_fn = &::ftruncate,
      .close_fn = &::close,
  };
  return kDefault;
}

FileHandle::~FileHandle() noexcept { (void)close(); }

FileHandle::FileHandle(FileHandle&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), ops_(other.ops_) {}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    (void)close();
    fd_ = std::exchange(other.fd_, -1);
    ops_ = other.ops_;
  }
  return *this;
}

Result<FileHandle> FileHandle::open(const std::filesystem::path& path,
                                    OpenMode mode, const PosixIoOps* ops) {
  if (ops == nullptr) {
    return Status::InvalidArgument("PosixIoOps pointer must not be null");
  }

  int flags = O_NOFOLLOW | O_CLOEXEC;
  switch (mode) {
    case OpenMode::kReadOnly:
      flags |= O_RDONLY;
      break;
    case OpenMode::kReadWrite:
      flags |= O_RDWR;
      break;
    case OpenMode::kReadWriteCreate:
      flags |= O_RDWR | O_CREAT;
      break;
    case OpenMode::kCreateNew:
      flags |= O_RDWR | O_CREAT | O_EXCL;
      break;
  }

  int fd = -1;
  do {
    fd = ::open(path.c_str(), flags, static_cast<mode_t>(S_IRUSR | S_IWUSR));
  } while (fd < 0 && errno == EINTR);

  if (fd < 0) {
    const int err = errno;
    if (err == ENOENT) {
      return Status::NotFound(errno_message("open failed", err));
    }
    if (err == EEXIST) {
      return Status::AlreadyExists(errno_message("open failed", err));
    }
    if (err == ELOOP) {
      return Status::InvalidArgument(
          errno_message("symlink rejected by O_NOFOLLOW", err));
    }
    return Status::IoError(errno_message("open failed", err));
  }

  FileHandle handle(fd, ops);
  struct stat st {};
  if (ops->fstat_fn(fd, &st) != 0) {
    const int err = errno;
    (void)handle.close();
    return Status::IoError(errno_message("fstat failed after open", err));
  }

  if (!S_ISREG(st.st_mode)) {
    (void)handle.close();
    return Status::InvalidArgument("target path is not a regular file");
  }

  return Result<FileHandle>::Ok(std::move(handle));
}

Status FileHandle::read_exact_at(std::uint64_t offset,
                                 std::span<std::byte> buffer) const {
  if (!is_open()) {
    return Status::InvalidArgument("read_exact_at on closed FileHandle");
  }
  if (buffer.empty()) {
    return Status::Ok();
  }

  auto end_offset = checked_add_u64(offset, buffer.size());
  if (!end_offset.ok()) {
    return end_offset.status();
  }
  if (end_offset.value() >
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return Status::InvalidArgument("read offset exceeds off_t maximum");
  }

  std::size_t total_read = 0;
  while (total_read < buffer.size()) {
    const off_t cur_off = static_cast<off_t>(offset + total_read);
    const std::size_t remaining = buffer.size() - total_read;
    const ssize_t rc =
        ops_->pread_fn(fd_, buffer.data() + total_read, remaining, cur_off);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IoError(errno_message("pread failed", errno));
    }
    if (rc == 0) {
      return Status::Corruption("unexpected EOF during positional read");
    }
    total_read += static_cast<std::size_t>(rc);
  }
  return Status::Ok();
}

Status FileHandle::write_exact_at(std::uint64_t offset,
                                  std::span<const std::byte> buffer) const {
  if (!is_open()) {
    return Status::InvalidArgument("write_exact_at on closed FileHandle");
  }
  if (buffer.empty()) {
    return Status::Ok();
  }

  auto end_offset = checked_add_u64(offset, buffer.size());
  if (!end_offset.ok()) {
    return end_offset.status();
  }
  if (end_offset.value() >
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return Status::InvalidArgument("write offset exceeds off_t maximum");
  }

  std::size_t total_written = 0;
  while (total_written < buffer.size()) {
    const off_t cur_off = static_cast<off_t>(offset + total_written);
    const std::size_t remaining = buffer.size() - total_written;
    const ssize_t rc = ops_->pwrite_fn(fd_, buffer.data() + total_written,
                                       remaining, cur_off);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IoError(errno_message("pwrite failed", errno));
    }
    if (rc == 0) {
      return Status::IoError("pwrite wrote 0 bytes");
    }
    total_written += static_cast<std::size_t>(rc);
  }
  return Status::Ok();
}

Result<std::uint64_t> FileHandle::size_bytes() const {
  if (!is_open()) {
    return Status::InvalidArgument("size_bytes on closed FileHandle");
  }
  struct stat st {};
  int rc = 0;
  do {
    rc = ops_->fstat_fn(fd_, &st);
  } while (rc < 0 && errno == EINTR);

  if (rc != 0) {
    return Status::IoError(errno_message("fstat failed", errno));
  }
  if (st.st_size < 0) {
    return Status::IoError("negative file size reported by fstat");
  }
  return Result<std::uint64_t>::Ok(static_cast<std::uint64_t>(st.st_size));
}

Status FileHandle::truncate(std::uint64_t new_size) const {
  if (!is_open()) {
    return Status::InvalidArgument("truncate on closed FileHandle");
  }
  if (new_size >
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return Status::InvalidArgument("truncate size exceeds off_t maximum");
  }
  int rc = 0;
  do {
    rc = ops_->ftruncate_fn(fd_, static_cast<off_t>(new_size));
  } while (rc < 0 && errno == EINTR);

  if (rc != 0) {
    return Status::IoError(errno_message("ftruncate failed", errno));
  }
  return Status::Ok();
}

Status FileHandle::sync_data() const {
  if (!is_open()) {
    return Status::InvalidArgument("sync_data on closed FileHandle");
  }
  int rc = 0;
  do {
    rc = ops_->fdatasync_fn(fd_);
  } while (rc < 0 && errno == EINTR);

  if (rc != 0) {
    return Status::IoError(errno_message("fdatasync failed", errno));
  }
  return Status::Ok();
}

Status FileHandle::sync_all() const {
  if (!is_open()) {
    return Status::InvalidArgument("sync_all on closed FileHandle");
  }
  int rc = 0;
  do {
    rc = ops_->fsync_fn(fd_);
  } while (rc < 0 && errno == EINTR);

  if (rc != 0) {
    return Status::IoError(errno_message("fsync failed", errno));
  }
  return Status::Ok();
}

Status FileHandle::close() noexcept {
  if (fd_ < 0) {
    return Status::Ok();
  }
  const int fd_to_close = std::exchange(fd_, -1);
  int rc = ops_->close_fn(fd_to_close);
  if (rc < 0 && errno != EINTR) {
    return Status::IoError(errno_message("close failed", errno));
  }
  return Status::Ok();
}

Status FileHandle::sync_directory(const std::filesystem::path& dir_path) {
  int dir_fd = -1;
  do {
    dir_fd = ::open(dir_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (dir_fd < 0 && errno == EINTR);

  if (dir_fd < 0) {
    return Status::IoError(errno_message("open directory failed", errno));
  }

  int sync_rc = 0;
  do {
    sync_rc = ::fsync(dir_fd);
  } while (sync_rc < 0 && errno == EINTR);
  const int sync_err = errno;

  (void)::close(dir_fd);
  if (sync_rc != 0) {
    return Status::IoError(errno_message("fsync directory failed", sync_err));
  }
  return Status::Ok();
}

Status FileHandle::ensure_directory_0700(
    const std::filesystem::path& dir_path) {
  struct stat st {};
  if (::lstat(dir_path.c_str(), &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      return Status::InvalidArgument("database path exists and is not a directory");
    }
    return Status::Ok();
  }
  if (errno != ENOENT) {
    return Status::IoError(errno_message("lstat directory failed", errno));
  }
  if (::mkdir(dir_path.c_str(), static_cast<mode_t>(S_IRWXU)) != 0) {
    return Status::IoError(errno_message("mkdir failed", errno));
  }
  return Status::Ok();
}

DatabaseLock::~DatabaseLock() noexcept { (void)release(); }

DatabaseLock::DatabaseLock(DatabaseLock&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      dev_(std::exchange(other.dev_, 0)),
      ino_(std::exchange(other.ino_, 0)) {}

DatabaseLock& DatabaseLock::operator=(DatabaseLock&& other) noexcept {
  if (this != &other) {
    (void)release();
    fd_ = std::exchange(other.fd_, -1);
    dev_ = std::exchange(other.dev_, 0);
    ino_ = std::exchange(other.ino_, 0);
  }
  return *this;
}

Result<DatabaseLock> DatabaseLock::acquire(
    const std::filesystem::path& lock_file_path) {
  std::lock_guard<std::mutex> guard(active_locks_mutex());

  // Check if lock file already exists and is held in-process before calling
  // open()/close(), because closing any fd for an inode in the same process
  // releases POSIX fcntl locks on that inode.
  struct stat existing_st {};
  if (::lstat(lock_file_path.c_str(), &existing_st) == 0) {
    if (!S_ISREG(existing_st.st_mode)) {
      return Status::InvalidArgument("lock file path is not a regular file");
    }
    const auto key = std::make_pair(existing_st.st_dev, existing_st.st_ino);
    if (active_locks_set().contains(key)) {
      return Status::AlreadyOpen("database lock already held in this process");
    }
  }

  int fd = -1;
  do {
    fd = ::open(lock_file_path.c_str(),
                O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
                static_cast<mode_t>(S_IRUSR | S_IWUSR));
  } while (fd < 0 && errno == EINTR);

  if (fd < 0) {
    const int err = errno;
    if (err == ELOOP) {
      return Status::InvalidArgument("lock file cannot be a symlink");
    }
    return Status::IoError(errno_message("failed to open lock file", err));
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    (void)::close(fd);
    return Status::InvalidArgument("lock file is not a regular file");
  }

  const auto key = std::make_pair(st.st_dev, st.st_ino);
  if (active_locks_set().contains(key)) {
    // Do not close(fd) before knowing it wasn't held; if we reached here via
    // race, it wasn't in existing_st.
    (void)::close(fd);
    return Status::AlreadyOpen("database lock already held in this process");
  }

  struct flock fl {};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;

  if (::fcntl(fd, F_SETLK, &fl) == -1) {
    const int err = errno;
    (void)::close(fd);
    if (err == EACCES || err == EAGAIN) {
      return Status::AlreadyOpen("database lock held by another process");
    }
    return Status::IoError(errno_message("fcntl F_SETLK failed", err));
  }

  active_locks_set().insert(key);
  return Result<DatabaseLock>::Ok(DatabaseLock(fd, st.st_dev, st.st_ino));
}

Status DatabaseLock::release() noexcept {
  if (fd_ < 0) {
    return Status::Ok();
  }
  std::lock_guard<std::mutex> guard(active_locks_mutex());
  const int fd_to_close = std::exchange(fd_, -1);
  const auto key = std::make_pair(std::exchange(dev_, 0), std::exchange(ino_, 0));
  active_locks_set().erase(key);

  struct flock fl {};
  fl.l_type = F_UNLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  (void)::fcntl(fd_to_close, F_SETLK, &fl);

  if (::close(fd_to_close) < 0 && errno != EINTR) {
    return Status::IoError(errno_message("close lock file failed", errno));
  }
  return Status::Ok();
}

}  // namespace pagedb
