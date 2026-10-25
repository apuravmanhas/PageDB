#include "pagedb/status.h"

#include <string>

#include "pagedb/types.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(StrongTypesSentinelAndComparison) {
  PAGEDB_ASSERT_FALSE(kInvalidPageId.is_valid());
  PAGEDB_ASSERT_TRUE(kSuperblockPageId.is_valid());
  PAGEDB_ASSERT_TRUE(kInitialRootPageId.is_valid());
  PAGEDB_ASSERT_EQ(kSuperblockPageId.value, 0U);
  PAGEDB_ASSERT_EQ(kInitialRootPageId.value, 1U);
  PAGEDB_ASSERT_TRUE(kSuperblockPageId < kInitialRootPageId);

  PAGEDB_ASSERT_FALSE(kInvalidLsn.is_valid());
  PAGEDB_ASSERT_TRUE(Lsn(1).is_valid());
  PAGEDB_ASSERT_TRUE(Lsn(1) < Lsn(2));

  PAGEDB_ASSERT_FALSE(kInvalidTxnId.is_valid());
  PAGEDB_ASSERT_TRUE(TxnId(42).is_valid());
}

PAGEDB_TEST(StatusConstructionAndFormatting) {
  Status ok = Status::Ok();
  PAGEDB_ASSERT_TRUE(ok.ok());
  PAGEDB_ASSERT_EQ(ok.code(), ErrorCode::kOk);
  PAGEDB_ASSERT_EQ(ok.to_string(), "Ok");

  Status err = Status::Corruption("bad crc32c");
  PAGEDB_ASSERT_FALSE(err.ok());
  PAGEDB_ASSERT_EQ(err.code(), ErrorCode::kCorruption);
  PAGEDB_ASSERT_EQ(err.message(), "bad crc32c");
  PAGEDB_ASSERT_EQ(err.to_string(), "Corruption: bad crc32c");
}

PAGEDB_TEST(ResultSuccessAndErrorPaths) {
  Result<int> r_ok = Result<int>::Ok(123);
  PAGEDB_ASSERT_TRUE(r_ok.ok());
  PAGEDB_ASSERT_EQ(r_ok.code(), ErrorCode::kOk);
  PAGEDB_ASSERT_EQ(r_ok.value(), 123);
  PAGEDB_ASSERT_TRUE(r_ok.status().ok());

  Result<std::string> r_err = Status::NotFound("missing key");
  PAGEDB_ASSERT_FALSE(r_err.ok());
  PAGEDB_ASSERT_EQ(r_err.code(), ErrorCode::kNotFound);
  PAGEDB_ASSERT_EQ(r_err.status().message(), "missing key");
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
