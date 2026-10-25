# PageDB Security Model and Hardening Controls

## 1. Threat Model

PageDB is an embedded storage engine designed to run inside a single host process. Its security posture defends against:
1. **Untrusted or Maliciously Crafted Database Files**: Opening a corrupted or adversarially crafted `.data` or `.wal` file must never trigger out-of-bounds memory reads/writes, integer overflow wraparounds, infinite loops, or uninitialized memory disclosure.
2. **Sensitive Heap/Stack Data Leakage (Slack-Space Disclosure)**: Unallocated regions inside 4096-byte pages (`[lower_offset, upper_offset)` and fragmented payload holes) must never persist uninitialized process memory to disk.
3. **Unauthorized Local File Access and Symlink Attacks**: Database files must not be world-readable or vulnerable to local symlink redirection.
4. **Concurrent Multi-Process Corruption**: Two processes opening the same database directory simultaneously must not silently corrupt on-disk pages.

## 2. Implemented Security Controls

### 2.1 Binary Parsing and Checked Arithmetic (`core`, `storage`)
- All integer additions and multiplications on untrusted offsets, lengths, slot indices, and page IDs use explicit overflow-checked helpers (`checked_add_u16`, `checked_add_u32`, `checked_add_u64`, `checked_mul_u64`).
- `PageCodec` verifies the 32-bit Castagnoli CRC32C checksum over the full 4096-byte page *before* interpreting `num_slots`, `lower_offset`, `upper_offset`, or any slot entry.
- `SlottedPage` validates that all slot `[offset, offset + length)` ranges lie strictly within `[upper_offset, 4096)`, that no two slots overlap, and that the sum of active slot lengths plus `fragmented_free_bytes` exactly equals `4096 - upper_offset`.

### 2.2 Zero-Scrubbing of Page Slack Space (`storage`)
- Every in-memory page buffer (`std::array<std::byte, 4096>`) is zero-initialized on creation.
- Whenever a record is deleted, updated in-place, or compacted in a `SlottedPage`, the vacated payload bytes and the contiguous free region `[lower_offset, upper_offset)` are explicitly overwritten with `0x00`.

### 2.3 Filesystem Hardening (`FileHandle`, `DatabaseLock`)
- Files are opened with `O_NOFOLLOW` to reject symbolic links.
- Newly created files use mode `0600` (`S_IRUSR | S_IWUSR`); created directories use mode `0700` (`S_IRWXU`).
- Immediately after `open()`, `FileHandle` calls `fstat()` and verifies `S_ISREG(st.st_mode)`, rejecting FIFOs, device nodes, directories, and sockets.
- `DatabaseLock` acquires an exclusive non-blocking POSIX lock (`fcntl(fd, F_SETLK, &fl)`) on `pagedb.lock` before `pagedb.data` is accessed.

### 2.4 Compiler and Sanitizer Hardening (`CMakeLists.txt`)
- Compiled with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wformat=2 -fno-strict-aliasing -fstack-protector-strong`.
- Dedicated CMake preset (`asan-ubsan`) enables AddressSanitizer (`-fsanitize=address`) and UndefinedBehaviorSanitizer (`-fsanitize=undefined`).
- Strict `.gitignore` rules block accidental commits of database files (`*.data`, `*.wal`, `*.lock`), crash dumps, and environment secrets (`.env`, `*.pem`, `*.key`).
