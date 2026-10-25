#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "pagedb/status.h"
#include "pagedb/types.h"

namespace pagedb {

enum class OpenMode : std::uint8_t {
  kReadOnly = 0,
  kReadWrite = 1,
  kReadWriteCreate = 2,
  kCreateNew = 3,
};

// Narrow system-call seam for deterministic I/O fault injection in tests.
struct PosixIoOps {
  using PreadFn = ssize_t (*)(int fd, void* buf, std::size_t count,
                              off_t offset);
  using PwriteFn = ssize_t (*)(int fd, const void* buf, std::size_t count,
                               off_t offset);
  using FsyncFn = int (*)(int fd);
  using FstatFn = int (*)(int fd, struct stat* st);
  using CloseFn = int (*)(int fd);

  PreadFn pread_fn;
  PwriteFn pwrite_fn;
  FsyncFn fdatasync_fn;
  FsyncFn fsync_fn;
  FstatFn fstat_fn;
  CloseFn close_fn;

  [[nodiscard]] static const PosixIoOps& default_ops() noexcept;
};

class FileHandle {
 public:
  FileHandle() noexcept = default;
  ~FileHandle() noexcept;

  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;

  [[nodiscard]] static Result<FileHandle> open(
      const std::filesystem::path& path, OpenMode mode,
      const PosixIoOps* ops = &PosixIoOps::default_ops());

  // Construct from an existing descriptor (for unit testing with pipes/custom ops).
  explicit FileHandle(int fd,
                      const PosixIoOps* ops = &PosixIoOps::default_ops()) noexcept
      : fd_(fd), ops_(ops) {}

  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int native_fd() const noexcept { return fd_; }

  [[nodiscard]] Status read_exact_at(std::uint64_t offset,
                                     std::span<std::byte> buffer) const;
  [[nodiscard]] Status write_exact_at(std::uint64_t offset,
                                      std::span<const std::byte> buffer) const;

  [[nodiscard]] Result<std::uint64_t> size_bytes() const;
  [[nodiscard]] Status sync_data() const;
  [[nodiscard]] Status sync_all() const;
  [[nodiscard]] Status close() noexcept;

  [[nodiscard]] static Status sync_directory(
      const std::filesystem::path& dir_path);
  [[nodiscard]] static Status ensure_directory_0700(
      const std::filesystem::path& dir_path);

 private:
  int fd_{-1};
  const PosixIoOps* ops_{&PosixIoOps::default_ops()};
};

// Enforces the single-process exclusive database directory invariant.
// Combines an in-process (st_dev, st_ino) registry (to avoid POSIX fcntl
// close-drops-all-locks semantics within the same process) with an OS-level
// non-blocking fcntl(F_SETLK, F_WRLCK) lock across processes.
class DatabaseLock {
 public:
  DatabaseLock() noexcept = default;
  ~DatabaseLock() noexcept;

  DatabaseLock(const DatabaseLock&) = delete;
  DatabaseLock& operator=(const DatabaseLock&) = delete;

  DatabaseLock(DatabaseLock&& other) noexcept;
  DatabaseLock& operator=(DatabaseLock&& other) noexcept;

  [[nodiscard]] static Result<DatabaseLock> acquire(
      const std::filesystem::path& lock_file_path);

  [[nodiscard]] bool is_held() const noexcept { return fd_ >= 0; }
  [[nodiscard]] Status release() noexcept;

 private:
  DatabaseLock(int fd, dev_t dev, ino_t ino) noexcept
      : fd_(fd), dev_(dev), ino_(ino) {}

  int fd_{-1};
  dev_t dev_{0};
  ino_t ino_{0};
};

}  // namespace pagedb
