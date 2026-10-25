#include "pagedb/btree.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "pagedb/buffer_pool.h"
#include "pagedb/byte_codec.h"
#include "pagedb/page_allocator.h"
#include "pagedb/page_store.h"
#include "test_harness.h"

namespace pagedb {
namespace {

PAGEDB_TEST(EmptyTreeBoundaryAndBinaryKeys) {
  test::TempDir tmp("pagedb_btree_empty");
  auto store_res = PageStore::open(tmp.path() / "db");
  PAGEDB_ASSERT_OK(store_res);
  PageStore& store = *store_res.value();
  PageAllocator allocator(store);
  BufferPool pool(store, 8);
  BTree tree(pool, allocator);

  PAGEDB_ASSERT_OK(tree.verify());
  PAGEDB_ASSERT_EQ(tree.height().value(), 1U);
  PAGEDB_ASSERT_EQ(tree.get("missing").code(), ErrorCode::kNotFound);
  PAGEDB_ASSERT_FALSE(tree.del("missing").value());
  PAGEDB_ASSERT_TRUE(tree.scan(std::nullopt, std::nullopt).value().empty());

  // Size limit validation.
  const std::string max_key(kMaxKeySize, 'K');
  const std::string max_val(kMaxValueSize, 'V');
  const std::string over_key(kMaxKeySize + 1, 'K');
  const std::string over_val(kMaxValueSize + 1, 'V');
  PAGEDB_ASSERT_EQ(tree.put(over_key, "v").code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(tree.put("k", over_val).code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(tree.get(over_key).code(), ErrorCode::kInvalidArgument);
  PAGEDB_ASSERT_EQ(tree.del(over_key).code(), ErrorCode::kInvalidArgument);

  // Empty key ("") and empty value ("") are valid and distinct from absent.
  PAGEDB_ASSERT_OK(tree.put("", ""));
  PAGEDB_ASSERT_EQ(tree.get("").value(), "");
  PAGEDB_ASSERT_OK(tree.put(max_key, max_val));
  PAGEDB_ASSERT_EQ(tree.get(max_key).value(), max_val);

  // Unsigned byte ordering with embedded NUL, 0x7F, 0x80, 0xFF.
  const std::string k_nul("\x00\x01", 2);
  const std::string k_7f("\x7f", 1);
  const std::string k_80("\x80", 1);
  const std::string k_ff("\xff", 1);
  PAGEDB_ASSERT_OK(tree.put(k_ff, "ff"));
  PAGEDB_ASSERT_OK(tree.put(k_80, "80"));
  PAGEDB_ASSERT_OK(tree.put(k_nul, "nul"));
  PAGEDB_ASSERT_OK(tree.put(k_7f, "7f"));
  PAGEDB_ASSERT_OK(tree.verify());

  auto all = tree.scan(std::nullopt, std::nullopt).value();
  PAGEDB_ASSERT_EQ(all.size(), 6U);
  PAGEDB_ASSERT_EQ(all[0].key, "");
  PAGEDB_ASSERT_EQ(all[1].key, k_nul);
  PAGEDB_ASSERT_EQ(all[2].key, max_key);
  PAGEDB_ASSERT_EQ(all[3].key, k_7f);
  PAGEDB_ASSERT_EQ(all[4].key, k_80);
  PAGEDB_ASSERT_EQ(all[5].key, k_ff);

  // Delete empty key and verify it is now NotFound.
  PAGEDB_ASSERT_TRUE(tree.del("").value());
  PAGEDB_ASSERT_FALSE(tree.del("").value());
  PAGEDB_ASSERT_EQ(tree.get("").code(), ErrorCode::kNotFound);
  PAGEDB_ASSERT_OK(tree.verify());
}

PAGEDB_TEST(MultiLevelSplitsUpdateOverflowAndRootCollapse) {
  test::TempDir tmp("pagedb_btree_splits");
  auto store_res = PageStore::open(tmp.path() / "db");
  PAGEDB_ASSERT_OK(store_res);
  PageStore& store = *store_res.value();
  PageAllocator allocator(store);
  // Minimum 4-frame buffer pool to stress eviction during multi-level splits.
  BufferPool pool(store, 4);
  BTree tree(pool, allocator);

  std::map<std::string, std::string, UnsignedByteLess> model;

  // Insert 300 keys with 200-byte keys and 300-byte values to force height >= 3.
  for (int i = 0; i < 300; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%05d_", i);
    std::string k = std::string(buf) + std::string(180, static_cast<char>('a' + (i % 26)));
    std::string v(300, static_cast<char>('A' + (i % 26)));
    PAGEDB_ASSERT_OK(tree.put(k, v));
    model[k] = v;
  }

  PAGEDB_ASSERT_OK(tree.verify());
  PAGEDB_ASSERT_TRUE(tree.height().value() >= 3U);

  // Update every 3rd key with a 512-byte value (exercising update-triggered splits).
  int idx = 0;
  for (auto& [k, v] : model) {
    if (idx % 3 == 0) {
      std::string big_v(kMaxValueSize, 'Z');
      PAGEDB_ASSERT_OK(tree.put(k, big_v));
      v = big_v;
    }
    ++idx;
  }
  PAGEDB_ASSERT_OK(tree.verify());

  // Verify full scan matches std::map exactly.
  auto scanned = tree.scan(std::nullopt, std::nullopt).value();
  PAGEDB_ASSERT_EQ(scanned.size(), model.size());
  std::size_t pos = 0;
  for (const auto& [mk, mv] : model) {
    PAGEDB_ASSERT_EQ(scanned[pos].key, mk);
    PAGEDB_ASSERT_EQ(scanned[pos].value, mv);
    ++pos;
  }

  const std::uint32_t peak_pages = allocator.next_page_id().value;

  // Delete all 300 keys and verify root collapses all the way back to height 1.
  for (const auto& [mk, _] : model) {
    auto del_res = tree.del(mk);
    PAGEDB_ASSERT_OK(del_res);
    PAGEDB_ASSERT_TRUE(del_res.value());
  }
  PAGEDB_ASSERT_OK(tree.verify());
  PAGEDB_ASSERT_EQ(tree.height().value(), 1U);
  PAGEDB_ASSERT_TRUE(tree.scan(std::nullopt, std::nullopt).value().empty());

  // Re-insert 100 keys and verify freed pages are reused without growing file.
  for (int i = 0; i < 100; ++i) {
    std::string k = "reinsert_" + std::to_string(i);
    std::string v(250, 'R');
    PAGEDB_ASSERT_OK(tree.put(k, v));
  }
  PAGEDB_ASSERT_OK(tree.verify());
  PAGEDB_ASSERT_EQ(allocator.next_page_id().value, peak_pages);
}

PAGEDB_TEST(RandomizedDifferentialModelAgainstStdMap) {
  test::TempDir tmp("pagedb_btree_diff");
  auto store_res = PageStore::open(tmp.path() / "db");
  PAGEDB_ASSERT_OK(store_res);
  PageStore& store = *store_res.value();
  PageAllocator allocator(store);
  BufferPool pool(store, 4);
  BTree tree(pool, allocator);

  std::map<std::string, std::string, UnsignedByteLess> model;
  std::mt19937 rng(0xC0FFEE42U);

  auto random_bytes = [&](std::size_t max_len) {
    std::uniform_int_distribution<std::size_t> len_dist(0, max_len);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    const std::size_t len = len_dist(rng);
    std::string s(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
      s[i] = static_cast<char>(byte_dist(rng));
    }
    return s;
  };

  std::vector<std::string> key_universe;
  key_universe.reserve(400);
  for (int i = 0; i < 400; ++i) {
    key_universe.push_back(random_bytes(kMaxKeySize));
  }

  std::uniform_int_distribution<int> op_dist(0, 99);
  std::uniform_int_distribution<std::size_t> key_dist(0, key_universe.size() - 1);

  constexpr int kTotalOps = 2000;
  for (int step = 1; step <= kTotalOps; ++step) {
    const int op = op_dist(rng);
    const std::string& k = key_universe[key_dist(rng)];

    if (op < 50) {
      // 50%: put (insert or overwrite with variable-size value 0..512).
      const std::string v = random_bytes(kMaxValueSize);
      PAGEDB_ASSERT_OK(tree.put(k, v));
      model[k] = v;
    } else if (op < 80) {
      // 30%: del.
      auto del_res = tree.del(k);
      PAGEDB_ASSERT_OK(del_res);
      const bool expected = (model.erase(k) > 0);
      PAGEDB_ASSERT_EQ(del_res.value(), expected);
    } else if (op < 95) {
      // 15%: point get.
      auto get_res = tree.get(k);
      auto it = model.find(k);
      if (it == model.end()) {
        PAGEDB_ASSERT_EQ(get_res.code(), ErrorCode::kNotFound);
      } else {
        PAGEDB_ASSERT_OK(get_res);
        PAGEDB_ASSERT_EQ(get_res.value(), it->second);
      }
    } else {
      // 5%: range scan [k1, k2).
      const std::string& k2 = key_universe[key_dist(rng)];
      auto scan_res =
          tree.scan(std::string_view(k), std::string_view(k2));
      PAGEDB_ASSERT_OK(scan_res);
      const auto& vec = scan_res.value();

      std::vector<KeyValue> expected_vec;
      if (UnsignedByteLess{}(k, k2)) {
        for (auto it = model.lower_bound(k);
             it != model.end() && UnsignedByteLess{}(it->first, k2); ++it) {
          expected_vec.push_back(KeyValue{it->first, it->second});
        }
      }
      PAGEDB_ASSERT_EQ(vec.size(), expected_vec.size());
      for (std::size_t i = 0; i < vec.size(); ++i) {
        PAGEDB_ASSERT_EQ(vec[i].key, expected_vec[i].key);
        PAGEDB_ASSERT_EQ(vec[i].value, expected_vec[i].value);
      }
    }

    if (step % 100 == 0) {
      PAGEDB_ASSERT_OK(tree.verify());
    }
  }

  // Final full scan and structural verification.
  PAGEDB_ASSERT_OK(tree.verify());
  auto final_scan = tree.scan(std::nullopt, std::nullopt).value();
  PAGEDB_ASSERT_EQ(final_scan.size(), model.size());
  std::size_t i = 0;
  for (const auto& [mk, mv] : model) {
    PAGEDB_ASSERT_EQ(final_scan[i].key, mk);
    PAGEDB_ASSERT_EQ(final_scan[i].value, mv);
    ++i;
  }
}

}  // namespace
}  // namespace pagedb

int main() { return pagedb::test::run_all_tests(); }
