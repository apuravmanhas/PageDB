#pragma once

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace pagedb::test {

class TempDir {
 public:
  explicit TempDir(std::string_view prefix = "pagedb_test") {
    static std::atomic<std::uint64_t> counter{0};
    const auto id = counter.fetch_add(1);
    std::string dir_name(prefix);
    dir_name.append("_");
    dir_name.append(std::to_string(::getpid()));
    dir_name.append("_");
    dir_name.append(std::to_string(id));
    path_ = std::filesystem::path("/tmp") / dir_name;
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> fn) {
    registry().push_back(TestCase{std::move(name), std::move(fn)});
  }
};

inline void fail(const char* file, int line, const char* expr,
                 const std::string& msg = {}) {
  std::cerr << "[FAIL] " << file << ":" << line << ": assertion failed: " << expr;
  if (!msg.empty()) {
    std::cerr << " (" << msg << ")";
  }
  std::cerr << "\n";
  std::exit(1);
}

inline int run_all_tests() {
  std::size_t passed = 0;
  for (const auto& tc : registry()) {
    std::cout << "[ RUN      ] " << tc.name << "\n";
    tc.fn();
    std::cout << "[       OK ] " << tc.name << "\n";
    ++passed;
  }
  std::cout << "[==========] " << passed << " test(s) passed.\n";
  return 0;
}

}  // namespace pagedb::test

#define PAGEDB_TEST(test_name)                                                 \
  static void test_fn_##test_name();                                           \
  static ::pagedb::test::Registrar registrar_##test_name(#test_name,           \
                                                         test_fn_##test_name); \
  static void test_fn_##test_name()

#define PAGEDB_ASSERT_TRUE(expr)                            \
  do {                                                      \
    if (!(expr)) {                                          \
      ::pagedb::test::fail(__FILE__, __LINE__, #expr);      \
    }                                                       \
  } while (false)

#define PAGEDB_ASSERT_FALSE(expr)                           \
  do {                                                      \
    if (expr) {                                             \
      ::pagedb::test::fail(__FILE__, __LINE__, "!(" #expr ")"); \
    }                                                       \
  } while (false)

#define PAGEDB_ASSERT_EQ(lhs, rhs)                                     \
  do {                                                                 \
    const auto& _lhs_val = (lhs);                                      \
    const auto& _rhs_val = (rhs);                                      \
    if (!(_lhs_val == _rhs_val)) {                                     \
      ::pagedb::test::fail(__FILE__, __LINE__, #lhs " == " #rhs);      \
    }                                                                  \
  } while (false)

#define PAGEDB_ASSERT_NE(lhs, rhs)                                     \
  do {                                                                 \
    const auto& _lhs_val = (lhs);                                      \
    const auto& _rhs_val = (rhs);                                      \
    if (!(_lhs_val != _rhs_val)) {                                     \
      ::pagedb::test::fail(__FILE__, __LINE__, #lhs " != " #rhs);      \
    }                                                                  \
  } while (false)

#define PAGEDB_ASSERT_OK(status_or_result)                                      \
  do {                                                                          \
    const auto& _sr = (status_or_result);                                       \
    if (!_sr.ok()) {                                                            \
      ::pagedb::test::fail(__FILE__, __LINE__, #status_or_result ".ok()");      \
    }                                                                           \
  } while (false)
