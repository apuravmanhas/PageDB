# PageDB Failure Model and Durability Contract

## 1. Purpose

This document defines what failures PageDB handles safely, which failure modes are detected vs. repaired, and which guarantees apply at each milestone.

## 2. Guarantees by Project Stage

### Stage 1: Clean-Shutdown Persistence Only (Milestones M3–M6)
- **Supported**:
  - Clean `Database::close()` flushes all dirty buffer pool frames to `pagedb.data` and invokes `FileHandle::sync_data()`.
  - Subsequent `Database::open()` restores the exact committed logical key-value map.
  - Corrupt headers, invalid magic constants, unknown format versions, truncated trailing bytes, and CRC32C mismatches are deterministically detected and rejected with `ErrorCode::Corruption` or `ErrorCode::UnsupportedFormatVersion`.
- **Not Yet Implemented (Explicit Non-Guarantee in M3–M6)**:
  - Crash atomicity during multi-page mutations (such as B+ tree splits, merges, or root updates). If the process terminates abnormally before `close()`, `pagedb.data` may contain partially updated tree nodes.

### Stage 2: Single-Writer Transactional Page-Image WAL (Milestones M7–M8)
- **Supported**:
  - **No-Steal Policy**: Dirty pages modified by an uncommitted transaction are pinned/locked against writeback and are never flushed to `pagedb.data` before the transaction commits.
  - **Write-Ahead Logging (WAL-Before-Data)**: A transaction is acknowledged as committed only after all modified 4096-byte page after-images (`PAGE_IMAGE` records) and its `COMMIT` record have been appended to `pagedb.wal` and synchronized (`fdatasync`/`fsync`).
  - **Torn-Page Repair (Including Page 0 Superblock)**: Because each committed `PAGE_IMAGE` record carries a full 4096-byte checksummed page image and the WAL is initially untruncated, recovery repairs any torn data page (including `PageId(0)`) whose on-disk checksum is invalid by replaying committed WAL images in LSN order.
  - **Superblock Bootstrap Rule**: During recovery in M7+, if `PageId(0)` in `pagedb.data` fails checksum verification due to an interrupted writeback, `Database::open()` does **not** abort before inspecting `pagedb.wal`; it scans `pagedb.wal` for the latest committed `PAGE_IMAGE` of `PageId(0)` first.
  - **Idempotent Recovery**: Re-running recovery after a crash during recovery produces the identical logical and physical state.

### Stage 3: Checkpointing and Log Reclamation (Milestone M9)
- Old WAL segments are retained until all dirty pages up to the checkpoint LSN are flushed to `pagedb.data`, `pagedb.data` is synced, the checkpoint record is synced in `pagedb.wal`, and the superblock `checkpoint_lsn` is durably updated.

## 3. Fail-Closed Poison Semantics

When a durability-critical or consistency-critical I/O operation fails (for example, `pwrite` or `fdatasync` fails during page writeback or WAL synchronization):
1. The operation returns `ErrorCode::IoError`.
2. The database instance transitions to a **poisoned** state (`DatabasePoisoned`).
3. All subsequent mutating operations (`put`, `del`, `commit`, `flush`) immediately fail with `ErrorCode::DatabasePoisoned` rather than continuing over potentially inconsistent on-disk state.

## 4. Environmental and Hardware Assumptions

PageDB's durability guarantees rely on the underlying operating system and storage stack honoring POSIX semantics:
- **Supported Assumptions**:
  - `pread` and `pwrite` are atomic with respect to the file offset parameter and may return short counts or `EINTR`, which `FileHandle` retries cleanly.
  - `fdatasync`/`fsync` flushes volatile kernel buffers to stable media when the storage controller honors cache-flush commands.
  - On WSL2, unit and crash tests use a native Linux filesystem directory (e.g., `/tmp` backed by `ext4`) rather than Windows DrvFS (`/mnt/c`) when testing strict POSIX advisory locks and directory `fsync` semantics.
- **Out-of-Scope / Unsupported Hardware Failures**:
  - Storage controllers that report `fsync` completion while keeping data in volatile non-battery-backed DRAM.
  - Silent bit-rot inside both `pagedb.data` and `pagedb.wal` simultaneously (detected via CRC32C and reported as `ErrorCode::Corruption`, not silently ignored).
  - Multi-process concurrent modification after manually deleting `pagedb.lock` while a process is running.
