# ADR 0004: Staged No-Steal Whole-Page After-Image WAL Architecture

- **Status**: Accepted
- **Date**: 2026-10-25

## Context
Implementing physiological ARIES undo/redo logging simultaneously with initial B+ tree persistence increases complexity and makes early storage bugs harder to isolate.

## Decision
1. Deliver clean-close persistence first (Milestones M3–M6) with explicit documentation that crash atomicity begins in Milestone M7.
2. In Milestone M7, implement a single-writer, **no-steal**, **whole-page after-image** write-ahead log (`pagedb.wal`):
   - Uncommitted dirty pages are never evicted or flushed to `pagedb.data`.
   - At commit, each modified page's final 4096-byte image (with `page_lsn` and valid CRC32C) is logged in a `PAGE_IMAGE` record, followed by a `COMMIT` record, and synced (`fdatasync`/`fsync`) before acknowledging commit.
   - Keep `pagedb.wal` untruncated until Milestone M9's checkpoint protocol is proven.
   - During recovery, scan `pagedb.wal` before rejecting a torn `PageId(0)` superblock so a torn superblock can be repaired from its latest committed `PAGE_IMAGE`.

## Consequences
- Recovery is idempotent and repairs torn data pages directly from retained WAL page images.
- Checksum verification detects corruption while retained WAL page images provide the repair source.
