# PageDB v1 Functional Specification

## 1. Overview and Scope

PageDB is an embedded, single-process, disk-backed key-value storage engine written in C++20. Clients link against the `pagedb` library or interact with a database directory through the command-line tools.

PageDB v1 provides ordered binary key-value storage backed by fixed-size 4096-byte pages. It does not include a network server, SQL parser, query optimizer, secondary indexes, or multi-process concurrency.

## 2. Database Directory and Single-Process Ownership

A PageDB instance manages a single filesystem directory containing:

| File | Purpose | Phase Introduced |
| :--- | :--- | :--- |
| `pagedb.lock` | Exclusive OS-level process lock file (`fcntl` write lock) | M3 (Phase 4) |
| `pagedb.data` | Fixed-size 4096-byte page file (Page 0 = superblock, Page 1+ = tree/free pages) | M3 (Phase 5) |
| `pagedb.wal` | Append-only write-ahead log | M7 (Phase 11) |

### Single-Process Invariant
- At most one `Database` instance may open a given directory at a time.
- Opening a directory creates or opens `pagedb.lock` with mode `0600` (`O_NOFOLLOW`) and acquires a non-blocking exclusive advisory lock (`fcntl(fd, F_SETLK, ...)`).
- If the lock is already held by another process or handle, `Database::open` returns `ErrorCode::AlreadyOpen` without modifying any file.
- All files created by PageDB use mode `0600` (`S_IRUSR | S_IWUSR`); created directories use mode `0700`.

## 3. Data Model and Key Ordering

### Binary Keys and Values
- Keys and values are arbitrary byte sequences (`std::span<const std::byte>` / `std::string_view` / `std::string`).
- Embedded NUL (`0x00`) bytes are permitted anywhere in keys and values.
- Both empty keys (`size == 0`) and empty values (`size == 0`) are valid and distinct from an absent key.

### Key Comparator
- Keys are ordered by **unsigned-byte lexicographic comparison**:
  - For two keys $A$ and $B$, compare byte-by-byte at index $i < \min(|A|, |B|)$ as `static_cast<std::uint8_t>(A[i]) <=> static_cast<std::uint8_t>(B[i])`.
  - If all prefix bytes match, the shorter key is strictly less than the longer key (`|A| <=> |B|`).
  - Comparison never depends on `char` signedness, C locale, or UTF-8 validation.

### Version 1 Size Limits
- `kPageSize = 4096` bytes (compile-time and format-version-1 constant).
- `kMaxKeySize = 256` bytes.
- `kMaxValueSize = 512` bytes.
- Any `put`, `get`, `del`, or `scan` bound whose key exceeds `kMaxKeySize`, or any `put` whose value exceeds `kMaxValueSize`, is rejected before mutation with `ErrorCode::InvalidArgument`.

## 4. Public Operations and Semantics

### `Database::open(path, options) -> Result<std::unique_ptr<Database>>`
- Validates `options` and opens or initializes the database directory at `path`.
- If `create_if_missing` is false and `pagedb.data` does not exist, returns `ErrorCode::NotFound`.
- If `error_if_exists` is true and `pagedb.data` already exists, returns `ErrorCode::AlreadyExists` (or `ErrorCode::InvalidArgument`).
- Verifies that `pagedb.data` has a byte size that is a non-zero multiple of `4096` (minimum 2 pages: Page 0 superblock and Page 1 initial root leaf). A partial trailing page is rejected with `ErrorCode::Corruption`.

### `put(key, value) -> Status`
- Validates `key.size() <= kMaxKeySize` and `value.size() <= kMaxValueSize`.
- Inserts `(key, value)` if `key` is absent, or replaces the existing value associated with `key`.
- Returns `Status::Ok()` on success.

### `get(key) -> Result<std::string>`
- Validates `key.size() <= kMaxKeySize`.
- If `key` exists, returns `Result<std::string>::Ok(value)`. An empty string `""` indicates the key is present with a zero-length value.
- If `key` does not exist, returns `ErrorCode::NotFound`.

### `del(key) -> Result<bool>`
- Validates `key.size() <= kMaxKeySize`.
- If `key` exists, removes it and returns `Result<bool>::Ok(true)`.
- If `key` does not exist, leaves the database unchanged and returns `Result<bool>::Ok(false)`.

### `scan(start_key, end_key) -> Result<Iterator>`
- `start_key`: inclusive lower bound (`std::optional<std::string_view>`; `std::nullopt` starts at the minimum key in the database).
- `end_key`: exclusive upper bound (`std::optional<std::string_view>`; `std::nullopt` scans to the end of the database).
- Yields `(key, value)` pairs strictly ordered by the unsigned-byte comparator across the half-open interval `[start_key, end_key)`.
- If both `start_key` and `end_key` are present and `compare(start_key, end_key) >= 0`, the scan yields zero entries and succeeds.

### `close() -> Status`
- Flushes dirty pages (according to the active milestone's persistence rules), synchronizes `pagedb.data`, releases the `pagedb.lock` file lock, and closes all file descriptors.
- Subsequent operations on a closed `Database` return `ErrorCode::InvalidArgument`.

## 5. Error Codes

All operational failures are reported through `Status` and `Result<T>` without throwing C++ exceptions:

| `ErrorCode` | Meaning |
| :--- | :--- |
| `Ok` | Operation succeeded |
| `NotFound` | Target key or file does not exist |
| `AlreadyExists` | Exclusive creation requested for an existing file/database |
| `InvalidArgument` | Key/value exceeds size limit, invalid option, or invalid API usage |
| `AlreadyOpen` | Database directory lock is held by another handle or process |
| `IoError` | Underlying OS file or directory I/O failure (`pread`, `pwrite`, `fsync`, `open`, `fstat`) |
| `Corruption` | Checksum mismatch, invalid magic, partial page, invalid slot offset, or structural invariant violation |
| `UnsupportedFormatVersion` | On-disk format major/minor version is unknown to this binary |
| `ResourceExhausted` | All buffer pool frames are pinned or transaction memory limit reached |
| `TransactionAborted` | Explicit or internal transaction abort |
| `DatabasePoisoned` | Unrecoverable durability or writeback failure occurred; further mutations are forbidden |

## 6. Persistence Guarantees by Milestone

1. **Milestones M3–M6 (Pre-WAL)**:
   - Changes are persisted across a **clean `close()` and reopen**.
   - Crash atomicity is **not** provided prior to Milestone M7. An unclean process termination during a multi-page B+ tree modification may leave `pagedb.data` inconsistent.
2. **Milestones M7+ (WAL Enabled)**:
   - Single-writer, no-steal, whole-page after-image write-ahead logging.
   - Uncommitted dirty pages are never written to `pagedb.data`.
   - A mutation is reported committed only after all modified page images and the `COMMIT` record are written and synchronized (`fdatasync`/`fsync`) to `pagedb.wal`.
   - On reopen after a crash, recovery replays committed page images from `pagedb.wal` and repairs any torn data pages (including Page 0).

## 7. Explicit Non-Goals for v1

- SQL query language or relational schemas.
- Multi-process access to the same database directory.
- Key sizes > 256 bytes or value sizes > 512 bytes (overflow pages are deferred to post-v1 extensions).
- Network server protocols or distributed replication.
- Unaligned `O_DIRECT` or `mmap`-based storage management.
