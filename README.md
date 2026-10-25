# PageDB

PageDB is a single-process, embedded, disk-backed key-value storage engine built from scratch in C++20.

## Project Stage

- **Completed Milestones**: **M0, M1, M2, M3, M4** (Phases 0 through 7)
- **Next Milestone (Opus Review Gate)**: **M5 (Phase 8 — B+ Tree Core Algorithms & Variable-Length Split/Merge Invariants)**
- **Implemented Subsystems**:
  - **Specification & Architecture (`M0`)**:
    - v1 Functional Specification ([`docs/SPEC.md`](docs/SPEC.md))
    - Binary Page & Superblock Format ([`docs/FORMAT.md`](docs/FORMAT.md))
    - Failure Model & Durability Contract ([`docs/FAILURE_MODEL.md`](docs/FAILURE_MODEL.md))
    - System Invariants ([`docs/INVARIANTS.md`](docs/INVARIANTS.md))
    - Security Hardening Model ([`docs/SECURITY.md`](docs/SECURITY.md))
    - Architecture Decision Records ([`docs/adr/`](docs/adr/))
  - **Build & CI Foundation (`M1`)**:
    - C++20 CMake build with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wformat=2 -Werror -fstack-protector-strong`, CTest presets (`dev`, `asan-ubsan`, `release`), and GitHub Actions CI.
  - **Safe Core Primitives (`M2`)**:
    - Strong identifiers (`PageId`, `Lsn`, `TxnId`), `Status` / `Result<T>`, overflow-checked arithmetic, explicit little-endian `ByteWriter`/`ByteReader`, unsigned-byte lexicographic comparator, and RFC 3720-verified Castagnoli CRC32C.
  - **POSIX Storage & Slotted-Page Layer (`M3`)**:
    - Move-only `FileHandle` (`O_NOFOLLOW`, `0600` permissions, `S_ISREG` verification, `EINTR`/short-I/O retry loops, `PosixIoOps` fault-injection seam).
    - Exclusive single-process `DatabaseLock` (`fcntl(F_SETLK)` + in-process `(st_dev, st_ino)` registry).
    - 4096-byte `PageCodec`, `Superblock`, and `SlottedPage` (non-overlapping slot validation, zero-scrubbed slack space, automatic compaction) + `PageStore` and `PageAllocator`.
  - **Buffer Pool & RAII Page Guards (`M4`)**:
    - Fixed-capacity `BufferPool` with CLOCK replacement policy, move-only `PageGuard` single-unpin RAII, dirty-page tracking, and I/O failure safety.

## Build and Test (Linux / WSL2 Ubuntu)

Configure, build, and run tests using CMake presets:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

To run with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

## Architecture Overview

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the layered subsystem design and [`docs/ROADMAP.md`](docs/ROADMAP.md) for the milestone progression and exit gates.
