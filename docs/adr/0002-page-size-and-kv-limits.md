# ADR 0002: Fixed 4096-Byte Page Size and Explicit v1 Key/Value Size Limits

- **Status**: Accepted
- **Date**: 2026-10-25

## Context
Variable-size pages complicate buffer pool management, free-space reuse, and WAL after-image recovery. Similarly, unbounded key and value sizes require multi-page overflow chains before the core index is proven.

## Decision
1. Fix the format-version-1 page size to `4096` bytes (`kPageSize = 4096`).
2. Enforce explicit version-1 limits at the API boundary prior to mutation:
   - `kMaxKeySize = 256` bytes (`0 <= key.size() <= 256`)
   - `kMaxValueSize = 512` bytes (`0 <= value.size() <= 512`)
3. With a 48-byte page header, the maximum leaf record + slot overhead is `776` bytes, guaranteeing that at least 5 maximum-sized records fit on a single leaf page (`5 * 776 = 3880 <= 4048`) and at least 15 maximum-sized separator records (`266` bytes including 4-byte slot) fit on an internal page.

## Consequences
- Every leaf and internal page can always split cleanly into two valid non-empty nodes without requiring overflow pages in v1.
- Overflow pages remain cleanly decoupled as a post-v1 feature extension.
