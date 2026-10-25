#include "pagedb/status.h"

namespace pagedb {

std::string_view error_code_to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk:
      return "Ok";
    case ErrorCode::kNotFound:
      return "NotFound";
    case ErrorCode::kAlreadyExists:
      return "AlreadyExists";
    case ErrorCode::kInvalidArgument:
      return "InvalidArgument";
    case ErrorCode::kAlreadyOpen:
      return "AlreadyOpen";
    case ErrorCode::kIoError:
      return "IoError";
    case ErrorCode::kCorruption:
      return "Corruption";
    case ErrorCode::kUnsupportedFormatVersion:
      return "UnsupportedFormatVersion";
    case ErrorCode::kResourceExhausted:
      return "ResourceExhausted";
    case ErrorCode::kTransactionAborted:
      return "TransactionAborted";
    case ErrorCode::kDatabasePoisoned:
      return "DatabasePoisoned";
  }
  return "UnknownError";
}

std::string Status::to_string() const {
  if (ok()) {
    return "Ok";
  }
  std::string out(error_code_to_string(code_));
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  return out;
}

}  // namespace pagedb
