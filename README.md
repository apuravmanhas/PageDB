# PageDB

PageDB is a single-process, embedded, disk-backed key-value storage engine built from scratch in C++20.

## Project Stage

- **Current Milestone**: Milestone M1 (Build & Test Foundation) / M0 (Specification & Format Contract)
- **Implemented**:
  - v1 Functional Specification ([`docs/SPEC.md`](docs/SPEC.md))
  - Binary Page & Superblock Format ([`docs/FORMAT.md`](docs/FORMAT.md))
  - Failure Model & Durability Contract ([`docs/FAILURE_MODEL.md`](docs/FAILURE_MODEL.md))
  - System Invariants ([`docs/INVARIANTS.md`](docs/INVARIANTS.md))
  - Security Hardening Model ([`docs/SECURITY.md`](docs/SECURITY.md))
  - Architecture Decision Records ([`docs/adr/`](docs/adr/))
  - Core strong types (`PageId`, `Lsn`, `TxnId`) and `Status` / `Result<T>` error handling
- **Planned (Next Milestones)**:
  - **M2**: Little-endian byte codec, checked arithmetic, unsigned byte comparator, Castagnoli CRC32C
  - **M3**: POSIX `FileHandle`, `DatabaseLock` (`fcntl`), `PageCodec` (`Superblock` & `SlottedPage`), `PageStore`, `PageAllocator`
  - **M4**: Fixed-capacity `BufferPool`, RAII `PageGuard`, CLOCK eviction policy
  - **M5–M6**: Variable-length slotted-page B+ tree index with differential model verification
  - **M7–M9**: Single-writer no-steal `PAGE_IMAGE` write-ahead log (WAL), crash recovery, multi-op transactions, checkpointing

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
