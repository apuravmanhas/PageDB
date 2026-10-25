# PageDB System Invariants

Every change to PageDB must preserve the invariants listed below. Diagnostic verifiers (`Verifier`) and unit/model tests check these properties directly.

## 1. Global Safety and Security Invariants

- **G-1 (Checked Arithmetic)**: Every byte offset, slot bound, record length, and `PageId * kPageSize` multiplication uses checked arithmetic. Integer overflow returns `ErrorCode::Corruption` or `ErrorCode::InvalidArgument` and never wraps silently.
- **G-2 (No Uninitialized Disk Leaks)**: Every 4096-byte page buffer and WAL record buffer starts zero-initialized (`0x00`). Unused slotted-page space `[lower_offset, upper_offset)` and reserved fields are always zeroed before computing CRC32C and writing to disk.
- **G-3 (Strict File Permissions & Symlink Defense)**: Database files are opened with `O_NOFOLLOW` and mode `0600`, verified via `fstat` to be regular files (`S_ISREG`), and guarded by an exclusive `fcntl(F_SETLK)` advisory lock on `pagedb.lock`.
- **G-4 (Fail-Closed on I/O Error)**: No operation reports `Status::Ok()` if an underlying read, write, or sync failed. Unrecoverable write/sync failures mark the database `DatabasePoisoned`.

## 2. File and Page Invariants

- **P-1 (Whole-Page File Length)**: The byte length of `pagedb.data` on a clean open is $N \times 4096$ where $N \ge 2$ (`PageId(0)` = Superblock, `PageId(1)` = initial root leaf). Any non-multiple of 4096 is rejected with `ErrorCode::Corruption`.
- **P-2 (Checksum-First Decode)**: On every page read, CRC32C is verified over the full 4096-byte image (with bytes `0x24..0x27` zeroed) *before* reading `num_slots`, `lower_offset`, `upper_offset`, or any slot entry.
- **P-3 (Self-Identifying Page Header)**: A decoded page's header `page_id` must match the physical `PageId` (`offset / 4096`) requested from `PageStore`.
- **P-4 (Slotted-Page Bounds)**:
  - `lower_offset == 48 + num_slots * 4`
  - `48 <= lower_offset <= upper_offset <= 4096`
  - Every slot $i$ satisfies `upper_offset <= slot[i].offset` and `slot[i].offset + slot[i].length <= 4096` and `slot[i].length > 0`.
  - No two slots overlap in `[upper_offset, 4096)`.
  - $\sum \text{slot}[i].\text{length} + \text{fragmented\_free\_bytes} == 4096 - \text{upper\_offset}$.

## 3. Page Allocator Invariants

- **A-1 (Monotonic Allocation Bound)**: `superblock.next_page_id >= 2` and `1 <= superblock.root_page_id < superblock.next_page_id`.
- **A-2 (No Out-of-Range Reads)**: Reading or pinning `PageId(p)` where `p == INVALID_PAGE_ID` or `p >= next_page_id` is rejected with `ErrorCode::Corruption` or `ErrorCode::InvalidArgument`.

## 4. Buffer Pool Invariants

- **B-1 (Unique Residency)**: At most one frame in `BufferPool` may hold a given `PageId` at any point in time.
- **B-2 (Pin Protection)**: A frame with `pin_count > 0` is never selected for eviction by the replacement policy. If all frames have `pin_count > 0` and an uncached page is requested, `fetch_page` returns `ErrorCode::ResourceExhausted`.
- **B-3 (Single-Unpin RAII)**: Each `PageGuard` instance owns at most one pin and decrements `pin_count` at most once across move construction, move assignment, explicit `release()`, and destruction.
- **B-4 (Clean Frame Replacement)**: A dirty frame (`is_dirty == true`) is never overwritten by a newly fetched page until its contents have been successfully written to `PageStore` (and, once WAL is enabled in M7, the WAL has been synced through the frame's `page_lsn`).

## 5. B+ Tree Invariants (Milestones M5–M6)

- **T-1 (Key Ordering & Uniqueness)**: Within every leaf and internal page, keys are strictly sorted according to the unsigned-byte lexicographic comparator (`key[0] < key[1] < ...`).
- **T-2 (Subtree Interval Routing)**: In an internal page with leftmost child $C_0$ and slots $(K_0, C_1), \dots, (K_{m-1}, C_m)$, every key in subtree $C_0$ is $< K_0$, every key in subtree $C_i$ ($1 \le i < m$) is in $[K_{i-1}, K_i)$, and every key in subtree $C_m$ is $\ge K_{m-1}$.
- **T-3 (Uniform Leaf Depth)**: All leaf pages reachable from `root_page_id` lie at the exact same depth from the root.
- **T-4 (Forward Leaf Chain)**: Starting at the leftmost leaf and following `next_leaf_page_id` visits every leaf page in the tree in strictly increasing key order and terminates at `kInvalidPageId (0xFFFFFFFF)` without cycles.
- **T-5 (Variable-Length Parent Split Safety)**: Because keys have variable length (`0..256` bytes), replacing a separator key in an internal parent during leaf redistribution or child merge may increase the parent's byte occupancy; the parent must compact or split safely if the replacement separator exceeds remaining parent free space.

## 6. WAL and Recovery Invariants (Milestones M7+)

- **W-1 (No-Steal)**: No frame modified by an uncommitted transaction is ever written to `pagedb.data`.
- **W-2 (WAL-Before-Data)**: A dirty page with `page_lsn = L` is never written to `pagedb.data` until `pagedb.wal` has been durably synchronized through at least `L`.
- **W-3 (Commit Durability)**: A transaction is acknowledged as committed only after all its `PAGE_IMAGE` records and its `COMMIT` record are durably synchronized in `pagedb.wal`.
- **W-4 (Idempotent Redo)**: Replaying committed `PAGE_IMAGE` records from `pagedb.wal` in LSN order produces the exact same page images regardless of how many times recovery is interrupted and restarted.
