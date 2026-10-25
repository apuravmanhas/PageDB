#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace pagedb {

enum class ErrorCode : std::uint8_t {
  kOk = 0,
  kNotFound = 1,
  kAlreadyExists = 2,
  kInvalidArgument = 3,
  kAlreadyOpen = 4,
  kIoError = 5,
  kCorruption = 6,
  kUnsupportedFormatVersion = 7,
  kResourceExhausted = 8,
  kTransactionAborted = 9,
  kDatabasePoisoned = 10,
};

[[nodiscard]] std::string_view error_code_to_string(ErrorCode code) noexcept;

class Status {
 public:
  Status() noexcept : code_(ErrorCode::kOk) {}

  Status(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {
    assert(code_ != ErrorCode::kOk);
  }

  [[nodiscard]] static Status Ok() noexcept { return Status(); }

  [[nodiscard]] static Status NotFound(std::string msg = {}) {
    return Status(ErrorCode::kNotFound, std::move(msg));
  }
  [[nodiscard]] static Status AlreadyExists(std::string msg = {}) {
    return Status(ErrorCode::kAlreadyExists, std::move(msg));
  }
  [[nodiscard]] static Status InvalidArgument(std::string msg = {}) {
    return Status(ErrorCode::kInvalidArgument, std::move(msg));
  }
  [[nodiscard]] static Status AlreadyOpen(std::string msg = {}) {
    return Status(ErrorCode::kAlreadyOpen, std::move(msg));
  }
  [[nodiscard]] static Status IoError(std::string msg = {}) {
    return Status(ErrorCode::kIoError, std::move(msg));
  }
  [[nodiscard]] static Status Corruption(std::string msg = {}) {
    return Status(ErrorCode::kCorruption, std::move(msg));
  }
  [[nodiscard]] static Status UnsupportedFormatVersion(std::string msg = {}) {
    return Status(ErrorCode::kUnsupportedFormatVersion, std::move(msg));
  }
  [[nodiscard]] static Status ResourceExhausted(std::string msg = {}) {
    return Status(ErrorCode::kResourceExhausted, std::move(msg));
  }
  [[nodiscard]] static Status TransactionAborted(std::string msg = {}) {
    return Status(ErrorCode::kTransactionAborted, std::move(msg));
  }
  [[nodiscard]] static Status DatabasePoisoned(std::string msg = {}) {
    return Status(ErrorCode::kDatabasePoisoned, std::move(msg));
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::kOk; }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  [[nodiscard]] std::string to_string() const;

 private:
  ErrorCode code_;
  std::string message_;
};

template <typename T>
class Result {
 public:
  // Implicit construction from a Status (must not be Ok)
  Result(Status status) : storage_(std::move(status)) {
    assert(!std::get<Status>(storage_).ok());
  }

  // Implicit construction from value
  Result(T value) : storage_(std::move(value)) {}

  [[nodiscard]] static Result<T> Ok(T value) {
    return Result<T>(std::move(value));
  }

  [[nodiscard]] bool ok() const noexcept {
    return std::holds_alternative<T>(storage_);
  }

  [[nodiscard]] ErrorCode code() const noexcept {
    if (ok()) {
      return ErrorCode::kOk;
    }
    return std::get<Status>(storage_).code();
  }

  [[nodiscard]] const Status& status() const noexcept {
    static const Status kOkStatus = Status::Ok();
    if (ok()) {
      return kOkStatus;
    }
    return std::get<Status>(storage_);
  }

  [[nodiscard]] T& value() & noexcept {
    assert(ok());
    return std::get<T>(storage_);
  }

  [[nodiscard]] const T& value() const& noexcept {
    assert(ok());
    return std::get<T>(storage_);
  }

  [[nodiscard]] T&& value() && noexcept {
    assert(ok());
    return std::move(std::get<T>(storage_));
  }

 private:
  std::variant<Status, T> storage_;
};

}  // namespace pagedb
