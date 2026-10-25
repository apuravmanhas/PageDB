# PageDB

**PageDB** is an embedded, single-process, disk-backed key-value storage engine built from scratch in modern C++20.

It provides ACID transaction semantics, crash-safe Write-Ahead Logging (WAL) with physiological after-images, slotted-page B+ tree indexing with variable-length keys and values, a CLOCK-eviction buffer pool with RAII page guards, safe online fuzzy checkpointing, and Single-Writer Multiple-Reader (SWMR) concurrency.

---

## 1. Architecture Overview

```
+-------------------------------------------------------------------------+
|                        Application / Client API                         |
|     Database (get, put, del, scan, checkpoint, verify) / Transaction    |
+-------------------------------------------------------------------------+
                                    |
   Level 1: Database RWLock (std::shared_mutex) [SWMR Serialized Writes]
                                    |
+-------------------------------------------------------------------------+
|                     Slotted B+ Tree Engine (BTree)                      |
|  - Byte-balanced splits, separator promotions, merges, redistribution  |
|  - Sorted variable-length keys (up to 512B) and values (up to 2048B)    |
+-------------------------------------------------------------------------+
                                    |
   Level 2: BufferPool Internal Mutex (std::mutex + condition_variable)
                                    |
+-----------------------------------+-------------------------------------+
|        Buffer Pool Manager        |       Write-Ahead Log (WAL)         |
|  - CLOCK page eviction policy     |  - Append-only physiological images |
|  - Move-only RAII PageGuard       |  - Staged-pages No-Steal buffer     |
|  - Double-checked hit cache wait  |  - REDO recovery on startup         |
+-----------------------------------+-------------------------------------+
                                    |
+-------------------------------------------------------------------------+
|                     Storage Engine (PageStore)                          |
|  - 4096-byte Slotted Page codec with Castagnoli CRC32C per page         |
|  - Free-list page allocation & header tracking (PageAllocator)          |
|  - PosixIoOps positional I/O (pread/pwrite) & process locking           |
+-------------------------------------------------------------------------+
                                    |
                            File System (Disk)
                    [pagedb.data | pagedb.wal | pagedb.lock]
```

---

## 2. Key Engine Features

- **Binary Slotted Pages (`4096 bytes`)**: Fixed 4KB pages with 32-byte headers containing `magic` (`PAGE`), `page_id`, `page_lsn`, `page_type`, `slot_count`, `free_space_offset`, and Castagnoli CRC32C checksums verified on every physical read.
- **Dynamic B+ Tree Index**: Full support for variable-length keys and values with byte-balanced splits, sibling redistribution, leaf merging, and root collapse. Verified against `std::map` differential oracle.
- **Buffer Pool Manager**: Configurable frame capacity with CLOCK eviction, dirty-page tracking, move-only single-unpin `PageGuard` handles, and condition-variable synchronization to prevent frame starvation under heavy concurrency.
- **Write-Ahead Logging (WAL) & Crash Recovery**: Physiological page after-image WAL format enforcing No-Steal (`W-1`). Recovers committed transactions automatically on startup, repairs torn superblocks, and cleanly truncates uncommitted/torn tails.
- **ACID Transactions & Rollback**: RAII `Transaction` handle supporting multi-operation atomic mutations, read-your-own-writes isolation, single-writer serialization, and immediate zero-disk-mutation rollback.
- **Online Checkpointing & Log Reclamation**: 4-phase non-blocking checkpoint protocol with mathematical safety proof guaranteeing that no log records required for crash recovery are ever reclaimed.
- **SWMR Concurrency**: Single-Writer Multiple-Reader concurrency via `std::shared_mutex` (shared locks for readers, unique locks for writers). Two-level lock hierarchy ($L_1 \rightarrow L_2$) provably eliminates circular wait and deadlocks.
- **Deep Invariant Verification**: `Database::verify()` traverses every allocated page on disk, validates page CRC32Cs, verifies free-list acyclicity, checks buffer pool invariants, and verifies B+ tree separator key fences and uniform leaf depths.
- **Deterministic Fault Injection**: Modular `PosixIoOps` seam enabling controlled fault injection of torn sector writes, simulated power outages, disk full (`ENOSPC`), and mid-log bit flips.

---

## 3. Storage Format and Specifications

Complete formal specifications, invariant proofs, and design documents are located in `docs/`:

| Document | Description |
| :--- | :--- |
| [`docs/SPEC.md`](docs/SPEC.md) | Database functional requirements and API contract |
| [`docs/FORMAT.md`](docs/FORMAT.md) | Byte-by-byte binary specification of pages, superblocks, and WAL records |
| [`docs/INVARIANTS.md`](docs/INVARIANTS.md) | Formal system invariants (`G-1..4`, `P-1..4`, `A-1..2`, `B-1..4`, `T-1..5`, `W-1..4`, `C-1..3`) |
| [`docs/FAILURE_MODEL.md`](docs/FAILURE_MODEL.md) | Crash windows, storage assumptions, and fault handling |
| [`docs/BTREE_DESIGN.md`](docs/BTREE_DESIGN.md) | Variable-length B+ tree algorithms, split policies, and redistribution |
| [`docs/WAL_RECOVERY_DESIGN.md`](docs/WAL_RECOVERY_DESIGN.md) | Staging buffer, LSN rules, torn-tail detection, and REDO recovery |
| [`docs/CHECKPOINT_DESIGN.md`](docs/CHECKPOINT_DESIGN.md) | 4-phase checkpoint protocol and log truncation safety theorem |
| [`docs/CONCURRENCY_DESIGN.md`](docs/CONCURRENCY_DESIGN.md) | SWMR architecture, 2-level lock hierarchy, and deadlock-freedom proof |
| [`docs/SECURITY.md`](docs/SECURITY.md) | File permissions, process exclusion, and adversarial input bounds |

---

## 4. Building and Testing

PageDB uses modern C++20 and standard CMake presets (tested on GCC 13+ / Clang 16+ on Linux / WSL2).

### Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake g++ libpthread-stubs0-dev
```

### Build Presets

1. **Developer Debug Build** (strict `-Werror` and assertions enabled):
   ```bash
   cmake --preset dev
   cmake --build --preset dev -j4
   ctest --preset dev --output-on-failure
   ```

2. **Sanitizer Build** (AddressSanitizer + UndefinedBehaviorSanitizer):
   ```bash
   cmake --preset asan-ubsan
   cmake --build --preset asan-ubsan -j4
   ctest --preset asan-ubsan --output-on-failure
   ```

3. **Optimized Release Build** (`-O3 / RelWithDebInfo`):
   ```bash
   cmake --preset release
   cmake --build --preset release -j4
   ctest --preset release --output-on-failure
   ```

---

## 5. Performance Benchmarks

Run the built-in microbenchmark suite:

```bash
./build/release/pagedb_bench
```

### Microbenchmark Results (Linux WSL2 / x86_64, 4KB Pages)

| Benchmark | Operations | Elapsed | Throughput | Latency (p50 / p99) |
| :--- | :---: | :---: | :---: | :---: |
| **Batch Put (Txn 500 records)** | 25,000 ops | 2.32 s | **10,770 ops/sec** (1.19 MB/s) | - |
| **Point Get (Random Cache Hits)** | 50,000 ops | 4.64 s | **10,767 ops/sec** | p50 = 85.0 μs / p99 = 176.6 μs |
| **Range Scan (50 items/scan)** | 50,000 ops | 0.19 s | **260,276 ops/sec** (28.79 MB/s) | - |
| **Concurrent Get (1 thread)** | 20,000 ops | 1.85 s | **10,800 ops/sec** | - |
| **Concurrent Get (2 threads)** | 40,000 ops | 3.32 s | **12,032 ops/sec** | - |
| **Concurrent Get (4 threads)** | 80,000 ops | 7.30 s | **10,960 ops/sec** | - |
| **Concurrent Get (8 threads)** | 160,000 ops | 13.20 s | **12,125 ops/sec** | - |

*All benchmarks conclude with complete deep invariant verification (`Database::verify()`) confirming 100% database consistency.*

---

## 6. Interactive Command-Line Tool (`pagedb_cli`)

PageDB includes a full-featured CLI for administrative operations, data inspection, and scripting:

```bash
# Interactive REPL mode:
./build/release/pagedb_cli /tmp/mydb

# One-shot command execution:
./build/release/pagedb_cli /tmp/mydb put user:1001 "Alice Smith"
./build/release/pagedb_cli /tmp/mydb get user:1001
./build/release/pagedb_cli /tmp/mydb scan user:1000 user:1010
./build/release/pagedb_cli /tmp/mydb checkpoint
./build/release/pagedb_cli /tmp/mydb stats
./build/release/pagedb_cli /tmp/mydb verify
./build/release/pagedb_cli /tmp/mydb del user:1001
```

### Supported CLI Commands

- `get <key>`: Fetch the value for a key.
- `put <key> <value>`: Insert or update a key-value pair.
- `del <key>`: Delete a key.
- `scan [start] [end]`: Perform a lexicographic range scan over `[start, end)`.
- `stats`: Display page count, tree height, root page ID, free list head, committed LSN, checkpoint LSN, WAL file size, and buffer pool hit/miss counters.
- `checkpoint`: Force an immediate fuzzy checkpoint and log reclamation.
- `verify`: Run deep end-to-end structural and cryptographic invariant verification.
- `help`: Print available commands.
- `exit` / `quit`: Safely close the database and exit.

---

## 7. C++ API Quick Start

```cpp
#include <iostream>
#include "pagedb/database.h"
#include "pagedb/transaction.h"

int main() {
  pagedb::DatabaseOptions options{};
  options.buffer_pool_frames = 128;
  options.wal_auto_checkpoint_bytes = 4 * 1024 * 1024;  // 4MB

  auto db_res = pagedb::Database::open("/tmp/pagedb_demo", options);
  if (!db_res.ok()) {
    std::cerr << "Open failed: " << db_res.status().message() << "\n";
    return 1;
  }
  auto& db = *db_res.value();

  // 1. Auto-commit write
  pagedb::Status s = db.put("user:001", "Alice");
  if (!s.ok()) { /* handle error */ }

  // 2. Point lookup
  auto get_res = db.get("user:001");
  if (get_res.ok()) {
    std::cout << "Value: " << get_res.value() << "\n";
  }

  // 3. Multi-operation atomic transaction
  {
    auto txn_res = db.begin_transaction();
    auto& txn = *txn_res.value();
    (void)txn.put("user:002", "Bob");
    (void)txn.put("user:003", "Charlie");
    (void)txn.del("user:001");
    // Explicit commit (if destroyed without commit, automatically rolls back)
    pagedb::Status cs = txn.commit();
  }

  // 4. Lexicographical range scan
  auto scan_res = db.scan("user:000", "user:999");
  if (scan_res.ok()) {
    for (const auto& kv : scan_res.value()) {
      std::cout << kv.key << " => " << kv.value << "\n";
    }
  }

  // 5. Invariant verification
  pagedb::Status vs = db.verify();
  assert(vs.ok());

  // 6. Graceful close
  return db.close().ok() ? 0 : 1;
}
```

---

## 8. Test Suite Summary

The test suite contains 15 unit targets exercising all layers of the engine:

1. `status_test`: Strong identifiers, error codes, and result propagation.
2. `byte_codec_test`: Overflow-checked arithmetic and little-endian cursor serialization.
3. `crc32c_test`: RFC 3720 Castagnoli CRC32C test vectors and validation.
4. `file_handle_test`: POSIX I/O, file permissions, and exclusive directory locking.
5. `page_codec_test`: Slotted page encoding, decoding, slot offsets, slack zeroing, and compaction.
6. `page_store_test`: Physical page reads, writes, and CRC32C verification.
7. `buffer_pool_test`: Frame table, CLOCK eviction hand, pin invariants, and dirty writes.
8. `btree_test`: B+ tree splits, merges, redistributions, and 2,000-step randomized differential tests vs `std::map`.
9. `database_test`: Multi-session persistence, crash re-open, and API boundary invariants.
10. `wal_test`: WAL record framing, append protocols, LSN sequencing, and crash recovery.
11. `transaction_test`: Atomic transactions, rollback isolation, and single-writer exclusion.
12. `checkpoint_test`: 4-phase checkpoint protocol, WAL truncation, and post-checkpoint crash recovery.
13. `concurrency_test`: 8-thread concurrent reader scaling, buffer pool cache eviction under concurrent load, readers with serial writers, and cross-thread exclusion.
14. `verifier_test`: Deep invariant verification, corruption detection on page 0 magic, superblock CRC32C tampering, body bit-flip detection, and out-of-bounds free list detection.
15. `fault_injection_test`: PosixIoOps fault injection simulating crash-after-wal-sync before data install, disk-full (`ENOSPC`), torn WAL tail truncation, and mid-log corruption rejection.
