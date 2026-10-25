# ADR 0005: Security Hardening and Mandatory Advisory Process Locking

- **Status**: Accepted
- **Date**: 2026-10-25

## Context
A storage engine handling untrusted binary files or shared local directories must prevent symlink attacks, world-readable database files, uninitialized heap memory leakage into disk slack space, and silent multi-process corruption.

## Decision
1. Open database files with `O_NOFOLLOW`, create files with mode `0600` (`0700` for directories), and verify `S_ISREG` via `fstat()` immediately upon open.
2. Enforce the single-process invariant via an exclusive non-blocking POSIX advisory lock (`fcntl(fd, F_SETLK, ...)`) on `pagedb.lock`, returning `ErrorCode::AlreadyOpen` on contention.
3. Zero-initialize all 4096-byte page buffers and scrub vacated payload bytes on deletion/compaction so `[lower_offset, upper_offset)` is always `0x00`.
4. Validate CRC32C before parsing page offsets, and enforce the non-overlapping slot invariant on every decoded slotted page.

## Consequences
- Eliminates on-disk heap disclosure ("slack-space bleed"), symlink redirection, and accidental dual-process corruption.
