#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "pagedb/database.h"
#include "pagedb/transaction.h"

namespace pagedb::bench {

using Clock = std::chrono::steady_clock;

struct BenchmarkResult {
  std::string name;
  std::uint64_t operations{0};
  double duration_sec{0.0};
  double ops_per_sec{0.0};
  double mb_per_sec{0.0};
  double p50_micros{0.0};
  double p99_micros{0.0};
};

void print_result(const BenchmarkResult& r) {
  std::cout << std::left << std::setw(38) << r.name << " | "
            << std::right << std::setw(10) << r.operations << " ops | "
            << std::setw(8) << std::fixed << std::setprecision(2) << r.duration_sec << " s | "
            << std::setw(10) << std::fixed << std::setprecision(0) << r.ops_per_sec << " ops/s | ";
  if (r.mb_per_sec > 0.0) {
    std::cout << std::setw(8) << std::fixed << std::setprecision(2) << r.mb_per_sec << " MB/s";
  } else {
    std::cout << "        -";
  }
  if (r.p50_micros > 0.0) {
    std::cout << " | p50=" << std::setw(6) << std::fixed << std::setprecision(1) << r.p50_micros << "us"
              << " p99=" << std::setw(6) << std::fixed << std::setprecision(1) << r.p99_micros << "us";
  }
  std::cout << "\n";
}

void run_benchmarks() {
  const std::filesystem::path bench_dir = "/tmp/pagedb_benchmark_data";
  std::error_code ec;
  std::filesystem::remove_all(bench_dir, ec);
  std::filesystem::create_directories(bench_dir);

  std::cout << "========================================================================================\n";
  std::cout << "                     PageDB Storage Engine Microbenchmark Suite                         \n";
  std::cout << "========================================================================================\n\n";

  DatabaseOptions opts{};
  opts.buffer_pool_frames = 256;  // 1 MB buffer pool
  opts.wal_auto_checkpoint_bytes = 16 * 1024 * 1024;  // 16 MB checkpoint threshold

  auto db_res = Database::open(bench_dir, opts);
  if (!db_res.ok()) {
    std::cerr << "Failed to open benchmark database: " << db_res.status().message() << "\n";
    return;
  }
  std::unique_ptr<Database> db = std::move(db_res.value());

  constexpr std::uint64_t kNumBatchPuts = 25000;
  constexpr std::size_t kBatchSize = 500;
  constexpr std::size_t kKeySize = 16;
  constexpr std::size_t kValSize = 100;
  const std::string sample_val(kValSize, 'X');

  // 1. Transactional Batch Put
  {
    const auto t_start = Clock::now();
    for (std::uint64_t b = 0; b < kNumBatchPuts / kBatchSize; ++b) {
      auto txn_res = db->begin_transaction();
      if (!txn_res.ok()) break;
      auto& txn = *txn_res.value();
      for (std::size_t i = 0; i < kBatchSize; ++i) {
        const std::uint64_t id = b * kBatchSize + i;
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "k_%010llu", static_cast<unsigned long long>(id));
        (void)txn.put(kbuf, sample_val);
      }
      (void)txn.commit();
    }
    const auto t_end = Clock::now();
    const double dur = std::chrono::duration<double>(t_end - t_start).count();
    const double mb = static_cast<double>(kNumBatchPuts * (kKeySize + kValSize)) / (1024.0 * 1024.0);
    BenchmarkResult res{
        .name = "Batch Put (Txn 500 records)",
        .operations = kNumBatchPuts,
        .duration_sec = dur,
        .ops_per_sec = static_cast<double>(kNumBatchPuts) / dur,
        .mb_per_sec = mb / dur,
    };
    print_result(res);
  }

  // 2. Point Get (Random, Cache Resident)
  constexpr std::uint64_t kNumGets = 50000;
  std::vector<double> get_latencies;
  get_latencies.reserve(kNumGets);
  {
    std::mt19937 rng(42);
    const auto t_start = Clock::now();
    for (std::uint64_t i = 0; i < kNumGets; ++i) {
      const std::uint64_t id = rng() % kNumBatchPuts;
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k_%010llu", static_cast<unsigned long long>(id));

      const auto op_start = Clock::now();
      auto get_res = db->get(kbuf);
      const auto op_end = Clock::now();

      const double micros = std::chrono::duration<double, std::micro>(op_end - op_start).count();
      get_latencies.push_back(micros);
    }
    const auto t_end = Clock::now();
    const double dur = std::chrono::duration<double>(t_end - t_start).count();
    std::sort(get_latencies.begin(), get_latencies.end());

    BenchmarkResult res{
        .name = "Point Get (Random Cache Hits)",
        .operations = kNumGets,
        .duration_sec = dur,
        .ops_per_sec = static_cast<double>(kNumGets) / dur,
        .mb_per_sec = 0.0,
        .p50_micros = get_latencies[get_latencies.size() / 2],
        .p99_micros = get_latencies[(get_latencies.size() * 99) / 100],
    };
    print_result(res);
  }

  // 3. Range Scan Throughput
  constexpr std::uint64_t kNumScans = 1000;
  constexpr std::uint64_t kScanRange = 50;
  std::uint64_t total_scanned_keys = 0;
  {
    std::mt19937 rng(12345);
    const auto t_start = Clock::now();
    for (std::uint64_t i = 0; i < kNumScans; ++i) {
      const std::uint64_t start_id = rng() % (kNumBatchPuts - kScanRange - 1);
      const std::uint64_t end_id = start_id + kScanRange;
      char sbuf[32], ebuf[32];
      std::snprintf(sbuf, sizeof(sbuf), "k_%010llu", static_cast<unsigned long long>(start_id));
      std::snprintf(ebuf, sizeof(ebuf), "k_%010llu", static_cast<unsigned long long>(end_id));

      auto scan_res = db->scan(sbuf, ebuf);
      if (scan_res.ok()) {
        total_scanned_keys += scan_res.value().size();
      }
    }
    const auto t_end = Clock::now();
    const double dur = std::chrono::duration<double>(t_end - t_start).count();
    BenchmarkResult res{
        .name = "Range Scan (50 items/scan)",
        .operations = total_scanned_keys,
        .duration_sec = dur,
        .ops_per_sec = static_cast<double>(total_scanned_keys) / dur,
        .mb_per_sec = static_cast<double>(total_scanned_keys * (kKeySize + kValSize)) / (dur * 1024.0 * 1024.0),
    };
    print_result(res);
  }

  // 4. Concurrent Reader Scaling (1, 2, 4, 8 threads)
  const std::vector<int> thread_counts = {1, 2, 4, 8};
  constexpr std::uint64_t kOpsPerThread = 20000;
  for (int num_th : thread_counts) {
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(num_th));
    const auto t_start = Clock::now();

    for (int t = 0; t < num_th; ++t) {
      workers.emplace_back([&db, t]() {
        std::mt19937 rng(7777U + static_cast<unsigned>(t));
        for (std::uint64_t i = 0; i < kOpsPerThread; ++i) {
          const std::uint64_t id = rng() % kNumBatchPuts;
          char kbuf[32];
          std::snprintf(kbuf, sizeof(kbuf), "k_%010llu", static_cast<unsigned long long>(id));
          (void)db->get(kbuf);
        }
      });
    }

    for (auto& w : workers) {
      w.join();
    }
    const auto t_end = Clock::now();
    const double dur = std::chrono::duration<double>(t_end - t_start).count();
    const std::uint64_t total_ops = static_cast<std::uint64_t>(num_th) * kOpsPerThread;
    char name_buf[64];
    std::snprintf(name_buf, sizeof(name_buf), "Concurrent Get (%d threads)", num_th);
    BenchmarkResult res{
        .name = name_buf,
        .operations = total_ops,
        .duration_sec = dur,
        .ops_per_sec = static_cast<double>(total_ops) / dur,
        .mb_per_sec = 0.0,
    };
    print_result(res);
  }

  // 5. Verification after benchmark load
  auto v_res = db->verify();
  if (!v_res.ok()) {
    std::cerr << "Post-benchmark invariant verification FAILED: " << v_res.message() << "\n";
  } else {
    std::cout << "\nPost-benchmark deep invariant verification: PASSED (All invariants verified cleanly)\n";
  }

  (void)db->close();
  std::filesystem::remove_all(bench_dir, ec);
  std::cout << "========================================================================================\n\n";
}

}  // namespace pagedb::bench

int main() {
  pagedb::bench::run_benchmarks();
  return 0;
}
