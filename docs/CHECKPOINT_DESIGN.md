# PageDB Milestone M9 — Checkpointing & Safe Log Reclamation Design

## 1. Executive Summary & Problem Formulation

In Milestones M7 and M8, `pagedb.wal` is strictly append-only: every committed transaction appends its full 4096-byte `PAGE_IMAGE` records and a `COMMIT` record. While this guarantees durability and crash atomicity, it creates two production issues:
1. **Unbounded Storage Growth**: An active database processing writes will grow `pagedb.wal` indefinitely.
2. **Prolonged Crash Recovery Time**: On `Database::open()`, recovery must scan and replay the entire history of committed transactions from LSN 1.

The objective of **Milestone M9 (Checkpointing & Safe Log Reclamation)** is to establish a verified protocol that:
- Periodically flushes all modified dirty pages to `pagedb.data`.
- Durably records a checkpoint watermark ($LSN_{ckpt}$) in `pagedb.data` (`Superblock::checkpoint_lsn`).
- Safely truncates/reclaims the prefix of `pagedb.wal` containing records prior to $LSN_{ckpt}$.
- Proves mathematically that **zero records necessary for crash recovery are ever discarded**.

---

## 2. Invariants & Mathematical Proof of Safe Log Reclamation

### 2.1 Invariant Definitions

- **C-1 (Flushed Data Watermark)**: At checkpoint completion, every allocated page $P$ in `pagedb.data` must satisfy:
  $$LSN_{disk}(P) \ge \max \{ LSN(T) \mid T \text{ committed}, T \text{ modified } P, LSN(T) \le LSN_{ckpt} \}$$
  Furthermore, `pagedb.data` must have been durably synchronized via `fdatasync`/`fsync`.

- **C-2 (Superblock Checkpoint Watermark)**: `Superblock::checkpoint_lsn` in `pagedb.data` on disk equals $LSN_{ckpt}$, and is synchronized to disk **after** all data pages in C-1 are synchronized.

- **C-3 (Safe Reclamation Invariant)**: A WAL record $R$ with $LSN(R) \le LSN_{ckpt}$ may be reclaimed (deleted or truncated) if and only if:
  1. $R$ belongs to a transaction $T$ that committed prior to or at $LSN_{ckpt}$.
  2. Every page modified by $T$ satisfies Invariant C-1 and C-2.

### 2.2 Correctness Proof

**Theorem**: *Under Invariants C-1, C-2, and C-3, discarding all WAL records with $LSN \le LSN_{ckpt}$ introduces zero loss of committed state and preserves crash recovery correctness.*

**Proof**:
1. Let $S_{committed}(L)$ denote the committed database state after applying all transactions up to LSN $L$.
2. At the time of checkpoint $C$ with watermark $LSN_{ckpt}$, all transactions $T$ with $LSN(T) \le LSN_{ckpt}$ have committed.
3. Because PageDB uses a whole-page after-image WAL with no uncommitted dirty pages written to disk (No-Steal **W-1**), every page modified by $T \le LSN_{ckpt}$ is flushed to `pagedb.data` and synchronized before the checkpoint completes (Invariant C-1).
4. Therefore, the physical contents of `pagedb.data` on disk exactly reflect $S_{committed}(LSN_{ckpt})$.
5. If the database crashes after checkpoint $C$:
   - For any page $P$ that was **not** modified by any transaction $T' > LSN_{ckpt}$, $P$'s on-disk image is valid, un-torn, and has valid CRC32C matching $S_{committed}(LSN_{ckpt})$.
   - For any page $P'$ that **was** modified by a subsequent transaction $T' > LSN_{ckpt}$:
     - If $T'$ committed, its `PAGE_IMAGE` record has $LSN(T') > LSN_{ckpt}$ and therefore resides in the retained portion of `pagedb.wal`. Redo replay will overwrite $P'$ with its latest committed state.
     - If $T'$ did not commit, No-Steal (**W-1**) guarantees that $T'$'s uncommitted modifications never touched `pagedb.data`.
6. Thus, no WAL records with $LSN \le LSN_{ckpt}$ are required to reconstruct the exact committed state. The reclamation is sound and safe. $\blacksquare$

---

## 3. Four-Phase Checkpoint Protocol

To guarantee crash consistency without risking deadlocks or race conditions, checkpointing follows four strictly ordered phases:

```
+-----------------------------------------------------------------------------+
| Phase 1: Quiesce & Flush BufferPool                                         |
|   1. Ensure no transaction is actively writing (or acquire write lock).     |
|   2. Flush all dirty frames in BufferPool to PageStore.                     |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
| Phase 2: Durably Synchronize Data File                                      |
|   1. Call PageStore::sync() (fdatasync on pagedb.data).                     |
|   2. All committed page images up to current LSN are now durable on disk.   |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
| Phase 3: Durably Advance Superblock checkpoint_lsn                          |
|   1. Read current wal->max_committed_lsn() as ckpt_lsn.                    |
|   2. Update Superblock::checkpoint_lsn = ckpt_lsn.                          |
|   3. Write Superblock to PageId(0) in pagedb.data.                          |
|   4. Call PageStore::sync() (fdatasync).                                    |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
| Phase 4: WAL Reclamation & Reset                                            |
|   1. Append CHECKPOINT record to pagedb.wal (for log auditability).         |
|   2. Reset pagedb.wal: rewrite 32-byte header with base_lsn = next_lsn,     |
|      ftruncate to 32 bytes, and wal->sync().                                |
+-----------------------------------------------------------------------------+
```

---

## 4. Crash Window Analysis Across Checkpoint Phases

| Crash Point | On-Disk State | Recovery Action | Guarantee |
| :--- | :--- | :--- | :--- |
| **Crash during Phase 1** (flushing buffer pool) | Some data pages written, others not. `pagedb.wal` fully intact. `Superblock::checkpoint_lsn` stale. | Recovery scans `pagedb.wal` from offset 32, replays all committed `PAGE_IMAGE`s. Partial page writes are completely overwritten. | **Safe** (Zero data loss) |
| **Crash during Phase 2** (data `fdatasync`) | Data pages mostly/fully written, `pagedb.wal` fully intact. `Superblock::checkpoint_lsn` stale. | Recovery scans `pagedb.wal` from offset 32, replays all committed `PAGE_IMAGE`s. Idempotent per **W-4**. | **Safe** (Zero data loss) |
| **Crash during Phase 3** (updating `Superblock::checkpoint_lsn`) | If Page 0 was torn: `pagedb.wal` still contains all historical `PAGE_IMAGE(PageId(0))` records! | Recovery replays latest committed `PAGE_IMAGE(0)` from `pagedb.wal`, repairing Page 0 before open. | **Safe** (Torn Page 0 repaired) |
| **Crash between Phase 3 & 4** (Superblock updated, WAL not yet reset) | `pagedb.data` is fully up-to-date with `checkpoint_lsn = L`. `pagedb.wal` still has old records. | Recovery reads `Superblock::checkpoint_lsn = L`. Replays records with $LSN > L$ (which is empty) or replays all records idempotently. | **Safe** (Zero data loss) |
| **Crash during Phase 4** (WAL header rewrite / truncate) | If WAL is truncated to $< 32$ bytes: open detects incomplete header, reinitializes header with `base_lsn = 1`. `pagedb.data` is already complete with all committed data. | `pagedb.data` validates immediately. No replay needed because WAL was truncated at checkpoint boundary. | **Safe** (Zero data loss) |

---

## 5. Architectural Recommendations for Implementation

1. **`Database::checkpoint()` API**:
   - Public method: `Status Database::checkpoint()` allowing explicit on-demand checkpoints.
   - Automatic checkpoint trigger: when `pagedb.wal` exceeds `wal_auto_checkpoint_bytes` (default: 4MB, configurable in `DatabaseOptions`, 0 = disabled).

2. **`Wal::reset(Lsn new_base_lsn)`**:
   - Add method in `Wal` to rewrite the 32-byte header with `base_lsn = new_base_lsn`, truncate the file to 32 bytes, and call `sync()`.

3. **Validation & Verification**:
   - Add unit test suite `tests/unit/checkpoint_test.cpp`:
     - Test explicit checkpointing truncating a multi-megabyte WAL down to 32 bytes while maintaining 100% data integrity across reopen.
     - Test automatic checkpointing triggered by size threshold during sustained insertions.
     - Test fault-injected crashes during each of the 4 checkpoint phases to verify recovery correctness under torn writes.
