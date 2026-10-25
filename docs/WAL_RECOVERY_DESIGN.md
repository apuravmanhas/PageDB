# PageDB Milestone M7 — Write-Ahead Logging (WAL) and Crash Recovery Design Specification

## 1. Storage Architecture Audit (Pre-M7 Baseline)

Before introducing `pagedb.wal`, we audited the interactions between `PageStore`, `PageAllocator`, `BufferPool`, and `BTree` in Milestones M3–M6:

1. **Direct Writes Bypassing `BufferPool` in M3–M6**:
   - In M3–M6, `PageAllocator::allocate_page`, `PageAllocator::free_page`, and `PageAllocator::flush_superblock` call `PageStore::write_page` **directly** on `pagedb.data` during the middle of a `BTree::put()` or `BTree::del()` call.
   - Similarly, if `BufferPool` has a small capacity (e.g., 4 frames) and evicts a dirty page during a multi-level split or merge, `BufferPool::find_victim_frame` writes that dirty page to `pagedb.data` **before** the overall `put()` or `del()` operation finishes.
2. **Why Direct Data-File Mutation Violates No-Steal (`W-1`) and Atomic Crash Recovery**:
   - If the process crashes midway through a multi-page `put()` or `del()` when uncommitted dirty pages or uncommitted superblock updates have already been written to `pagedb.data`, and there are no undo records in the WAL, `pagedb.data` is left in an inconsistent half-split or half-merged state.
   - Furthermore, if `pagedb.data` is extended by a partial page write during `allocate_page` and crashes before `COMMIT`, `pagedb.data` could have a non-multiple-of-4096 trailing partial page or extra unreferenced pages beyond the last committed `Superblock::next_page_id`.

---

## 2. M7 Architecture: Staged No-Steal Whole-Page After-Image WAL

To satisfy Invariants **W-1 (`No-Steal`)**, **W-2 (`WAL-Before-Data`)**, **W-3 (`Commit Durability`)**, and **W-4 (`Idempotent Redo`)**:

### 2.1 Binary Format of `pagedb.wal`

#### WAL File Header (32 Bytes, Offset `0x000..0x01F`)
| Offset | Size | Type | Field | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | 8 | `u64` | `wal_magic` | Constant `0x5041474544425731` (`"PAGEDBW1"`) |
| `0x08` | 2 | `u16` | `format_major` | `1` |
| `0x0A` | 2 | `u16` | `format_minor` | `0` |
| `0x0C` | 4 | `u32` | `page_size` | `4096` |
| `0x10` | 8 | `u64` | `base_lsn` | First valid LSN in this WAL file (`1` in M7–M8) |
| `0x18` | 4 | `u32` | `header_crc32c` | CRC32C over bytes `0x00..0x17` and `0x1C..0x1F` (with `0x18..0x1B` zeroed) |
| `0x1C` | 4 | `u32` | `reserved` | `0` |

#### WAL Header Validation Rules
- If `pagedb.wal` exists on open and has `0 < file_size < 32`, its creation was interrupted before any transaction record could be written; recovery truncates/removes the partial header file and proceeds with an empty WAL.
- If `file_size >= 32`:
  - `wal_magic != 0x5041474544425731` $\rightarrow$ `ErrorCode::kCorruption`.
  - `format_major != 1` or `format_minor != 0` $\rightarrow$ `ErrorCode::kUnsupportedFormatVersion`.
  - `page_size != 4096` or `reserved != 0` or `base_lsn == 0` $\rightarrow$ `ErrorCode::kCorruption`.
  - `header_crc32c` mismatch $\rightarrow$ `ErrorCode::kCorruption`.

#### WAL Record Framing (36-Byte Record Header + Payload)
Every WAL record starts at a byte offset $\ge 32$ and has:

| Relative Offset | Size | Type | Field | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | 4 | `u32` | `record_magic` | Constant `0x574C5243` (`"WLRC"`) |
| `0x04` | 1 | `u8` | `record_type` | `1` = `BEGIN`, `2` = `PAGE_IMAGE`, `3` = `COMMIT`, `4` = `ABORT`, `5` = `CHECKPOINT` |
| `0x05` | 3 | `u8[3]` | `flags_reserved` | All `0x00` |
| `0x08` | 8 | `u64` | `lsn` | Strictly monotonic record LSN ($1, 2, 3, \dots$) |
| `0x10` | 8 | `u64` | `txn_id` | Transaction ID ($1, 2, 3, \dots$) |
| `0x18` | 4 | `u32` | `page_id` | Target `PageId` for `PAGE_IMAGE` (`0` for `Superblock`, $\ge 1$ for tree/free pages; `0xFFFFFFFF` for non-page records) |
| `0x1C` | 4 | `u32` | `payload_len` | `4096` for `PAGE_IMAGE`; `0` for `BEGIN`/`COMMIT`/`ABORT` |
| `0x20` | 4 | `u32` | `record_crc32c` | CRC32C over header bytes `0x00..0x1F` + `payload[0..payload_len)` |
| `0x24` | `payload_len` | `u8[]` | `payload` | For `PAGE_IMAGE`: full 4096-byte encoded page with `header.page_lsn == lsn` and valid page CRC32C |

---

### 2.2 Enforcing Strict No-Steal (`W-1`) via Transaction Staging in `PageStore`

As confirmed by the M7 Recovery Reviewer Audit (Finding F-1 & F-2), PageDB uses **Option B: Transaction-Local Shadow Page Map (`staged_pages_`)** inside `PageStore`:

1. **Full Read/Write Interception During Active Transactions**:
   - When `PageStore::begin_transaction()` is called:
     - `in_transaction_ = true`
     - `staged_pages_.clear()` (`std::map<PageId, PageBuffer>`, ordered by `PageId`)
     - `staged_page_count_ = page_count_`
     - `staged_superblock_ = superblock_`
   - While `in_transaction_ == true`:
     - `PageStore::write_page(page_id, page_image)` validates `page_image` and writes it into `staged_pages_[page_id]`, updating `staged_page_count_` and `staged_superblock_` in memory. **Zero bytes are written to `pagedb.data`**.
     - `PageStore::read_page(page_id, out_page)` checks `staged_pages_` first and falls back to `data_file_.read_exact_at(...)` only if `page_id` has not been modified in the active transaction.
     - If `BufferPool` evicts a dirty frame mid-transaction (e.g., under a 4-frame pool during a deep split/merge), `BufferPool` writes the dirty frame to `PageStore::write_page`, which stages it in `staged_pages_` rather than touching `pagedb.data`. If the page is re-fetched later in the same transaction, `PageStore::read_page` serves the staged image.
2. **Abort / Rollback Semantics**:
   - If an error occurs during `put()` or `del()` before WAL commit:
     - `BufferPool::discard_all_pages()` drops any uncommitted frames from the buffer pool.
     - `PageStore::abort_transaction()` discards `staged_pages_` and restores `staged_page_count_` and `staged_superblock_` to the committed state.
     - `PageAllocator::reload_superblock()` restores `allocator.superblock_` from `store.superblock()`.
     - Because `pagedb.data` was never modified, rollback is instantaneous and leaves zero residual state.
3. **Commit Protocol & Crash Windows (Finding F-5 & F-12)**:
   - **Step 1 (Collect BufferPool dirty frames)**: Flush all dirty frames in `BufferPool` into `PageStore` (`staged_pages_`). If `staged_pages_` is empty (a no-op mutation such as deleting a non-existent key), end the transaction without writing WAL records.
   - **Step 2 (WAL Append)**:
     - Allocate `txn_id = wal.next_txn_id()`.
     - Append `BEGIN(lsn, txn_id)` to `pagedb.wal`.
     - For each `(page_id, page_buf)` in `staged_pages_` (iterated in ascending `PageId` order):
       - Assign `lsn = wal.next_lsn()`.
       - Call `PageCodec::stamp_lsn(page_buf, lsn)` to write `page_lsn = lsn` at offset `0x10` and recompute the 4096-byte page CRC32C at offset `0x24`.
       - If `page_id` is currently resident (and unpinned) in `BufferPool`, update the resident frame's buffer with the `page_lsn`-stamped `page_buf` so in-memory cache and disk state remain byte-identical.
       - Append `PAGE_IMAGE(lsn, txn_id, page_id, page_buf)` to `pagedb.wal`.
     - Append `COMMIT(commit_lsn, txn_id)` to `pagedb.wal`.
   - **Step 3 (WAL Sync — Commit Point)**:
     - Call `wal.sync()` (`fdatasync`). Once this returns `Status::Ok()`, the transaction is durably committed (**W-3**).
   - **Step 4 (Install Staged Pages to `pagedb.data`)**:
     - Write all non-superblock pages (`page_id >= 1`) in `staged_pages_` to `pagedb.data`, then write `PageId(0)` (`Superblock`) **last** so that `Superblock::next_page_id` and `Superblock::root_page_id` on disk are updated only after all referenced tree/free pages are in place.
     - Clear `staged_pages_` and mark `in_transaction_ = false`.

---

### 2.3 Crash Recovery & Torn-Tail vs. Mid-Log Corruption Protocol (Finding F-4, F-6, F-9)

When `Database::open(db_dir)` runs:

1. **Acquire Exclusive Lock**: Acquire `pagedb.lock` first.
2. **Torn-Tail vs. Mid-Log Corruption Boundary (Finding F-4)**:
   - Scan `pagedb.wal` sequentially starting at offset `32`:
     - Verify `lsn` is strictly monotonically increasing (`record.lsn == expected_lsn`).
     - For `PAGE_IMAGE`, also verify `PageCodec::decode_and_verify_header(payload, record.page_id)` and `header.page_lsn == record.lsn`.
     - If a record at `offset` has fewer bytes remaining than a 36-byte header or `36 + payload_len`, or fails magic/CRC/header validation:
       - **Distinguishing Torn Tail from Mid-Log Corruption**:
         1. Scan the remaining bytes after `offset` to check whether **any valid WAL record header + payload + CRC** exists later in `pagedb.wal`. If a valid record exists after the corrupt byte range, this is **mid-log corruption**, not an interrupted tail write — abort recovery and return `ErrorCode::kCorruption`!
         2. Otherwise (no valid record follows `offset`), treat `offset..EOF` as an **uncommitted torn tail** from an interrupted write before `COMMIT` sync, discard any open uncommitted transaction records, and `ftruncate` `pagedb.wal` to `offset` ( followed by `wal.sync()`).
3. **Torn-Superblock Bootstrap & Redo Replay (Finding F-6 & F-9)**:
   - Open `pagedb.data` in recovery mode (`PageStore::open` defers `Superblock` validation and whole-page length rejection until after WAL replay).
   - For every committed transaction in `pagedb.wal` (in ascending LSN order), write each `PAGE_IMAGE(page_id, payload)` directly to `pagedb.data` at byte offset `page_id * 4096`.
   - After replaying all committed `PAGE_IMAGE` records:
     - Read and decode `PageId(0)` (`Superblock`) from `pagedb.data`. If `PageId(0)` is still invalid after WAL replay, return `ErrorCode::kCorruption`.
     - Truncate `pagedb.data` via `FileHandle::truncate(superblock.next_page_id * 4096)` to remove any uncommitted or torn trailing file extension beyond `superblock.next_page_id`.
     - Call `data_file_.sync_data()` (`fdatasync`).
     - Validate all `PageStore` invariants (`file_size == next_page_id * 4096`, valid `Superblock`, valid root page header/CRC, and verify no page in `pagedb.data` has `page_lsn > max_committed_lsn` when WAL records exist).
4. **Idempotence (`W-4`)**:
   - Because `pagedb.wal` retains all committed `PAGE_IMAGE` records in M7–M8 and recovery writes full 4096-byte page images followed by `ftruncate` to `superblock.next_page_id * 4096`, interrupting recovery at any point and re-running `Database::open()` produces the exact same physical and logical database state.
