# PageDB Milestone M7 — Write-Ahead Logging (WAL) and Crash Recovery Design Proposal

## 1. Current Storage Architecture Audit (Pre-M7 Baseline)

Before introducing `pagedb.wal`, we audited the interactions between `PageStore`, `PageAllocator`, `BufferPool`, and `BTree` in Milestones M3–M6:

1. **Direct Writes Bypassing `BufferPool` in M3–M6**:
   - In M3–M6, `PageAllocator::allocate_page`, `PageAllocator::free_page`, and `PageAllocator::flush_superblock` call `PageStore::write_page` **directly** on `pagedb.data` during the middle of a `BTree::put()` or `BTree::del()` call!
   - Specifically:
     - When a leaf splits during `BTree::put()`, `allocator_.allocate_page` immediately appends the new page to `pagedb.data` and overwrites `PageId(0)` (`Superblock::next_page_id` or `free_list_head`) in `pagedb.data` **before** the parent internal node or root page is updated.
     - Similarly, if `BufferPool` has a small capacity (e.g., 4 frames) and evicts a dirty page during a multi-level split or merge, `BufferPool::find_victim_frame` writes that dirty page to `pagedb.data` **before** the overall `put()` or `del()` operation finishes.
2. **Why Direct Data-File Mutation Violates No-Steal (`W-1`) and Atomic Crash Recovery**:
   - If the process crashes midway through a multi-page `put()` or `del()`:
     - If uncommitted dirty pages or uncommitted superblock updates were already written to `pagedb.data`, and there are no undo records in the WAL, `pagedb.data` is left in a half-split / half-merged state!
   - Furthermore, if `pagedb.data` is extended by a partial page write during `allocate_page` and crashes before `COMMIT`, `pagedb.data` could have a non-multiple-of-4096 trailing partial page or extra unreferenced pages beyond the last committed `Superblock::next_page_id`.

---

## 2. Proposed M7 Architecture: Staged No-Steal Whole-Page After-Image WAL

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

### 2.2 Enforcing Strict No-Steal (`W-1`) Across Allocator, Superblock, and BufferPool

To guarantee that **zero uncommitted bytes ever touch `pagedb.data`** before WAL `COMMIT`:

1. **In-Memory Transaction Write-Set / Dirty-Page Staging**:
   - During an active mutation (auto-commit `put`/`del` in M7, or explicit multi-op `Transaction` in M8), neither `PageAllocator` nor `BufferPool` may write uncommitted dirty pages to `pagedb.data`!
   -Wait: What if a single transaction modifies more pages than `buffer_pool_frames` (or what if `buffer_pool_frames == 4` and a transaction touches 5 pages during a 3-level split + free-list pop)?
   - **Critical Design Choice for Opus Review**:
     - **Option A (Uncommitted Dirty Pages Locked in BufferPool + Minimum Pool Capacity)**: Disallow evicting any frame with `is_uncommitted_dirty == true` in `BufferPool::find_victim_frame()`, and stage newly allocated pages (`allocate_page`/`free_page`) in `BufferPool` + in-memory `Superblock` state rather than writing directly to `PageStore`. If an uncommitted transaction exhausts all `buffer_pool_frames`, return `ErrorCode::kResourceExhausted` and roll back the uncommitted transaction cleanly!
     - **Option B (Transaction-Local Shadow Page Map in `PageStore` / `WriteBatch`)**: Hold uncommitted modified 4096-byte page images (including `PageId(0)` superblock and newly allocated/freed pages) in an in-memory transaction write-back map (`std::unordered_map<PageId, PageBuffer>`) seated between `BufferPool`/`PageAllocator` and `PageStore`!
       - Look at what **Option B** gives us:
         1. When `BufferPool` evicts a dirty page during a transaction, or when `PageAllocator` writes a newly allocated page, free page, or updated `Superblock` (`PageId(0)`), the write is intercepted by the in-memory **Transaction Staging Buffer** (`staged_pages_[page_id] = page_image`) instead of calling `pwrite` on `pagedb.data`!
         2. When `BufferPool` or `PageAllocator` reads a page during the transaction, it checks `staged_pages_` first before reading `pagedb.data`!
         3. `pagedb.data` on disk is **100% untouched** during the entire transaction — even if `BufferPool` only has 4 frames and evicts frames 100 times!
         4. If any error occurs before commit (or on explicit `rollback()` in M8!), aborting the transaction simply discards `staged_pages_`, restores the committed `Superblock` state, and invalidates dirty frames in `BufferPool`!
         5. At `commit()`:
            - For each `(page_id, page_buf)` in `staged_pages_` (sorted by `PageId` for determinism):
              - Assign the next `lsn`, stamp `page_lsn = lsn` into `page_buf` (recomputing its page CRC32C), and append a `PAGE_IMAGE(lsn, txn_id, page_id, page_buf)` record to `pagedb.wal`.
            - Append `COMMIT(commit_lsn, txn_id)` to `pagedb.wal`.
            - Call `wal_file_.sync_data()` (`fdatasync`/`fsync`).
            - Once `wal_file_.sync_data()` succeeds, the transaction is **durably committed**!
            - Write all committed `staged_pages_` to `pagedb.data` (or flush dirty frames) and clear `staged_pages_`.

---

### 2.3 Crash Recovery & Torn-Superblock Bootstrap Protocol (Phase 12)

When `Database::open(db_dir)` runs in M7+:

1. Acquire `pagedb.lock` first.
2. Check if `pagedb.wal` exists:
   - Scan `pagedb.wal` sequentially from offset `32`:
     - Read each record header + payload and verify `record_crc32c` (and for `PAGE_IMAGE`, verify the embedded 4096-byte page's own `PageCodec` checksum and `page_lsn == record.lsn`).
     - If a record at the tail of `pagedb.wal` is truncated (fewer bytes remaining than header/payload) or fails `record_crc32c` due to an interrupted write before `COMMIT` sync:
       - Treat it as an **incomplete tail write** from a crash, ignore the uncommitted tail transaction, and truncate `pagedb.wal` back to the end of the last valid record.
     - Collect all transactions that have a valid `COMMIT` record in `pagedb.wal`.
3. **Redo Replay (Repairing Torn Data Pages, Torn Page 0, and Truncated File Extensions)**:
   - For every committed transaction in ascending `lsn` order, replay each of its `PAGE_IMAGE(page_id, image)` records into `pagedb.data` at offset `page_id * 4096`!
   - Notice:
     - Even if `PageId(0)` (Superblock) in `pagedb.data` was torn mid-write during a crash after WAL commit, replaying the latest committed `PAGE_IMAGE(PageId(0))` repairs `PageId(0)` **before** `PageStore` validates `PageId(0)`!
     - What if `pagedb.data` had an uncommitted or torn trailing write beyond the committed `Superblock::next_page_id`? After replaying all committed `PAGE_IMAGE` records and decoding the repaired `PageId(0)` (`Superblock`), truncate `pagedb.data` via `ftruncate` to `superblock.next_page_id * 4096` bytes!
4. Call `data_file_.sync_data()` so all replayed pages and any file-length truncation are durable on disk.
5. Verify that re-running recovery if a crash occurs at any step of recovery is **100% idempotent**.
