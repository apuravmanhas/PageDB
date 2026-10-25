# PageDB Milestone M10 — Concurrency and Synchronization Design Specification

## 1. Concurrency Architecture: Single-Writer, Multiple-Reader (SWMR)

PageDB employs a **Single-Writer, Multiple-Reader (SWMR)** concurrency model, the industry standard for high-performance embedded storage engines (such as LMDB, SQLite in WAL mode, and BoltDB).

### 1.1 Operation Classification

1. **Reader Operations (Concurrent Read-Only Access)**:
   - `Database::get()`
   - `Database::scan()`
   - `Database::scan_bytes()`
   - `Database::tree_height()`
   - `Database::page_count()`
   - `Database::root_page_id()`
   - `Database::free_list_head()`
   - `Database::max_committed_lsn()`
   - `Database::checkpoint_lsn()`
   - Any number of reader threads can execute these operations simultaneously without blocking one another.

2. **Writer Operations (Exclusive Serialized Access)**:
   - `Database::put()`
   - `Database::del()`
   - `Database::begin_transaction()`
   - `Transaction::commit()`
   - `Transaction::rollback()`
   - `Database::checkpoint()`
   - `Database::close()`
   - Writer operations require exclusive access to the database instance, ensuring that B+ tree splits, merges, root adjustments, page allocations, and WAL commits occur with zero write-write race conditions.

---

## 2. Synchronization Architecture & Lock Hierarchy

To guarantee deadlock-freedom and thread safety, PageDB establishes a strict two-level lock hierarchy:

```
+-------------------------------------------------------------------------+
| Level 1: Database Reader-Writer Lock (Database::rwlock_: shared_mutex)  |
|   - Shared ownership (std::shared_lock) held by concurrent readers.     |
|   - Exclusive ownership (std::unique_lock) held by the active writer.   |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| Level 2: BufferPool Internal Mutex (BufferPool::mutex_: std::mutex)     |
|   - Protects frame table mapping (page_table_), CLOCK hand, pin_counts, |
|     and cache statistics.                                               |
|   - Short critical sections; never held across I/O or outside pool.     |
+-------------------------------------------------------------------------+
```

### 2.1 Deadlock-Freedom Proof

**Theorem**: *The synchronization model is provably deadlock-free.*

**Proof**:
1. Let $L_1$ denote the `Database::rwlock_` and $L_2$ denote the `BufferPool::mutex_`.
2. $L_1$ is always acquired at the public `Database` API boundary before invoking any internal storage or index routine.
3. $L_2$ is acquired solely inside `BufferPool` methods (`fetch_page`, `unpin_frame`, `discard_page`, `flush_page`, etc.) and released before the method returns.
4. No code path that holds $L_2$ ever attempts to acquire $L_1$.
5. The resource acquisition graph is a directed acyclic graph ($L_1 \rightarrow L_2$).
6. Therefore, circular wait conditions cannot occur, and the system is deadlock-free. $\blacksquare$

---

## 3. BufferPool Thread-Safety Invariants

Under concurrent read operations, multiple threads will simultaneously invoke `BufferPool::fetch_page()` and destroy `PageGuard`s (calling `BufferPool::unpin_frame()`):

1. **Pin Protection (`B-2`) Under Concurrency**:
   - `pin_count` is modified exclusively under `BufferPool::mutex_`.
   - A frame with `pin_count > 0` is skipped by the CLOCK eviction hand and cannot be evicted while any reader holds a `PageGuard`.
2. **Atomic Cache Hits**:
   - When a page is already resident in `BufferPool`, `page_table_` lookup, `pin_count` increment, and `ref_bit = true` happen atomically under `BufferPool::mutex_`.
3. **Synchronized Cache Misses & Eviction**:
   - When a page is not resident, finding a victim frame via CLOCK sweep, unmapping the old `PageId`, loading the new page from `PageStore`, and mapping the new `PageId` occur with proper synchronization so two threads requesting the same uncached page do not create duplicate resident frames (preserving Invariant **B-1 Unique Residency**).
4. **No-Steal Isolation for Readers**:
   - Readers never observe uncommitted mutations from the active writer because the writer's modifications are held in `PageStore::staged_pages_` and only installed into the buffer pool / data file after durable WAL `COMMIT` while holding the exclusive `unique_lock<std::shared_mutex>`.
