#include "pagedb/file_handle.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "pagedb/types.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(CreateWriteReadAndPermissions0600) {
  test::TempDir tmp("pagedb_fh_basic");
  const auto sub_dir = tmp.path() / "dbdir";
  PAGEDB_ASSERT_OK(FileHandle::ensure_directory_0700(sub_dir));

  struct stat dir_st {};
  PAGEDB_ASSERT_EQ(::stat(sub_dir.c_str(), &dir_st), 0);
  PAGEDB_ASSERT_EQ(dir_st.st_mode & 0777, static_cast<mode_t>(0700));

  const auto file_path = sub_dir / "pagedb.data";
  PAGEDB_ASSERT_EQ(FileHandle::open(file_path, OpenMode::kReadOnly).code(),
                   ErrorCode::kNotFound);

  auto created = FileHandle::open(file_path, OpenMode::kCreateNew);
  PAGEDB_ASSERT_OK(created);

  struct stat file_st {};
  PAGEDB_ASSERT_EQ(::stat(file_path.c_str(), &file_st), 0);
  PAGEDB_ASSERT_EQ(file_st.st_mode & 0777, static_cast<mode_t>(0600));

  // Exclusive creation on existing file must fail with AlreadyExists.
  PAGEDB_ASSERT_EQ(FileHandle::open(file_path, OpenMode::kCreateNew).code(),
                   ErrorCode::kAlreadyExists);

  std::array<std::byte, kPageSize> page0{};
  std::array<std::byte, kPageSize> page1{};
  for (std::size_t i = 0; i < kPageSize; ++i) {
    page0[i] = static_cast<std::byte>((i * 7U) & 0xFFU);
    page1[i] = static_cast<std::byte>((i * 13U + 5U) & 0xFFU);
  }

  FileHandle fh = std::move(created.value());
  PAGEDB_ASSERT_OK(fh.write_exact_at(0, page0));
  PAGEDB_ASSERT_OK(fh.write_exact_at(kPageSize, page1));
  PAGEDB_ASSERT_OK(fh.sync_data());
  PAGEDB_ASSERT_OK(fh.sync_all());
  PAGEDB_ASSERT_OK(FileHandle::sync_directory(sub_dir));

  auto sz = fh.size_bytes();
  PAGEDB_ASSERT_OK(sz);
  PAGEDB_ASSERT_EQ(sz.value(), 2ULL * kPageSize);
  PAGEDB_ASSERT_OK(fh.close());
  PAGEDB_ASSERT_FALSE(fh.is_open());
  // Double close is a safe no-op.
  PAGEDB_ASSERT_OK(fh.close());

  auto ro = FileHandle::open(file_path, OpenMode::kReadOnly);
  PAGEDB_ASSERT_OK(ro);
  std::array<std::byte, kPageSize> read0{};
  std::array<std::byte, kPageSize> read1{};
  PAGEDB_ASSERT_OK(ro.value().read_exact_at(0, read0));
  PAGEDB_ASSERT_OK(ro.value().read_exact_at(kPageSize, read1));
  PAGEDB_ASSERT_TRUE(read0 == page0);
  PAGEDB_ASSERT_TRUE(read1 == page1);

  // Reading past EOF returns Corruption.
  std::array<std::byte, 16> tail{};
  PAGEDB_ASSERT_EQ(ro.value().read_exact_at(2ULL * kPageSize, tail).code(),
                   ErrorCode::kCorruption);
}

PAGEDB_TEST(SymlinkAndDirectoryPathsRejected) {
  test::TempDir tmp("pagedb_fh_security");
  const auto real_file = tmp.path() / "real.data";
  {
    auto fh = FileHandle::open(real_file, OpenMode::kCreateNew);
    PAGEDB_ASSERT_OK(fh);
  }

  const auto symlink_path = tmp.path() / "link.data";
  PAGEDB_ASSERT_EQ(::symlink(real_file.c_str(), symlink_path.c_str()), 0);

  auto sym_res = FileHandle::open(symlink_path, OpenMode::kReadOnly);
  PAGEDB_ASSERT_FALSE(sym_res.ok());
  PAGEDB_ASSERT_EQ(sym_res.code(), ErrorCode::kInvalidArgument);

  auto dir_res = FileHandle::open(tmp.path(), OpenMode::kReadOnly);
  PAGEDB_ASSERT_FALSE(dir_res.ok());
}

int g_pread_calls = 0;
int g_pwrite_calls = 0;
bool g_fail_io = false;

ssize_t short_eintr_pread(int fd, void* buf, std::size_t count, off_t offset) {
  ++g_pread_calls;
  if (g_fail_io) {
    errno = EIO;
    return -1;
  }
  if (g_pread_calls == 1) {
    errno = EINTR;
    return -1;
  }
  const std::size_t chunk = count > 251 ? 251 : count;
  return ::pread(fd, buf, chunk, offset);
}

ssize_t short_eintr_pwrite(int fd, const void* buf, std::size_t count,
                           off_t offset) {
  ++g_pwrite_calls;
  if (g_fail_io) {
    errno = EIO;
    return -1;
  }
  if (g_pwrite_calls == 1) {
    errno = EINTR;
    return -1;
  }
  const std::size_t chunk = count > 317 ? 317 : count;
  return ::pwrite(fd, buf, chunk, offset);
}

int failing_fdatasync(int fd) {
  (void)fd;
  if (g_fail_io) {
    errno = EIO;
    return -1;
  }
  return ::fdatasync(fd);
}

PAGEDB_TEST(SimulatedShortIoEintrRetryAndFaultInjection) {
  test::TempDir tmp("pagedb_fh_shortio");
  PosixIoOps custom_ops = PosixIoOps::default_ops();
  custom_ops.pread_fn = &short_eintr_pread;
  custom_ops.pwrite_fn = &short_eintr_pwrite;
  custom_ops.fdatasync_fn = &failing_fdatasync;

  g_pread_calls = 0;
  g_pwrite_calls = 0;
  g_fail_io = false;

  const auto file_path = tmp.path() / "shortio.data";
  auto opened = FileHandle::open(file_path, OpenMode::kCreateNew, &custom_ops);
  PAGEDB_ASSERT_OK(opened);

  std::array<std::byte, kPageSize> payload{};
  for (std::size_t i = 0; i < kPageSize; ++i) {
    payload[i] = static_cast<std::byte>((i * 29U + 3U) & 0xFFU);
  }

  PAGEDB_ASSERT_OK(opened.value().write_exact_at(0, payload));
  PAGEDB_ASSERT_TRUE(g_pwrite_calls > 10);

  std::array<std::byte, kPageSize> read_back{};
  PAGEDB_ASSERT_OK(opened.value().read_exact_at(0, read_back));
  PAGEDB_ASSERT_TRUE(g_pread_calls > 10);
  PAGEDB_ASSERT_TRUE(read_back == payload);

  // Inject EIO on write, read, and sync_data.
  g_fail_io = true;
  PAGEDB_ASSERT_EQ(opened.value().write_exact_at(0, payload).code(),
                   ErrorCode::kIoError);
  PAGEDB_ASSERT_EQ(opened.value().read_exact_at(0, read_back).code(),
                   ErrorCode::kIoError);
  PAGEDB_ASSERT_EQ(opened.value().sync_data().code(), ErrorCode::kIoError);
  g_fail_io = false;
}

PAGEDB_TEST(DatabaseLockInProcessAndCrossProcessExclusion) {
  test::TempDir tmp("pagedb_lock_test");
  const auto lock_path = tmp.path() / "pagedb.lock";

  auto lock1 = DatabaseLock::acquire(lock_path);
  PAGEDB_ASSERT_OK(lock1);
  PAGEDB_ASSERT_TRUE(lock1.value().is_held());

  // Second acquire in the same process must fail with AlreadyOpen.
  auto lock2 = DatabaseLock::acquire(lock_path);
  PAGEDB_ASSERT_FALSE(lock2.ok());
  PAGEDB_ASSERT_EQ(lock2.code(), ErrorCode::kAlreadyOpen);

  // Fork a child process to verify kernel-level fcntl(F_SETLK) lock exclusion.
  const pid_t pid = ::fork();
  PAGEDB_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    // Child process: clear in-process set inherits copy, but test raw fcntl via
    // opening the file directly to prove the OS kernel lock is active.
    int child_fd = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    if (child_fd < 0) {
      std::_Exit(2);
    }
    struct flock fl {};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    const int rc = ::fcntl(child_fd, F_SETLK, &fl);
    const int err = errno;
    (void)::close(child_fd);
    if (rc == -1 && (err == EACCES || err == EAGAIN)) {
      std::_Exit(0);
    }
    std::_Exit(1);
  }

  int status = 0;
  PAGEDB_ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  PAGEDB_ASSERT_TRUE(WIFEXITED(status));
  PAGEDB_ASSERT_EQ(WEXITSTATUS(status), 0);

  // Release lock1 and verify re-acquisition succeeds.
  PAGEDB_ASSERT_OK(lock1.value().release());
  PAGEDB_ASSERT_FALSE(lock1.value().is_held());

  auto lock3 = DatabaseLock::acquire(lock_path);
  PAGEDB_ASSERT_OK(lock3);
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
