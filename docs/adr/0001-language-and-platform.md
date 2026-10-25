# ADR 0001: C++20 and Linux/WSL2 Positional I/O Baseline

- **Status**: Accepted
- **Date**: 2026-10-25

## Context
PageDB requires low-level control over binary layout, memory ownership, and synchronous positional file I/O (`pread`, `pwrite`, `fdatasync`, `fcntl`).

## Decision
1. Use **C++20** (`CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`).
2. Target **Linux x86-64** (including **WSL2 Ubuntu** with GCC 13+ / Clang 17+) as the primary platform.
3. Implement custom `Status` and `Result<T>` error types rather than relying on `std::expected` (C++23) or throwing exceptions on expected operational errors.
4. Keep the core engine free of third-party runtime dependencies.
5. Run filesystem-sensitive integration tests on a native Linux filesystem path (`/tmp`) when running inside WSL2 so POSIX advisory locks and `fsync` behave authentically.

## Consequences
- Zero external library supply-chain risk for the core database engine.
- Predictable error propagation across all layers without exception-unwinding overhead.
