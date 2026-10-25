# ADR 0003: Slotted-Page B+ Tree Index with Byte-Aware Split and Merge Rules

- **Status**: Accepted
- **Date**: 2026-10-25

## Context
Keys (`0..256` bytes) and values (`0..512` bytes) are variable-length binary strings. Fixed-fanout B+ tree rules (e.g., "at least $d$ keys per node") do not directly govern byte capacity in a 4096-byte page.

## Decision
1. Use a slotted-page layout where the slot directory grows upward from byte `48` (`lower_offset`) and records grow downward from byte `4096` (`upper_offset`).
2. Compare all keys using unsigned-byte lexicographic order (`std::uint8_t`).
3. Maintain forward sibling pointers (`next_leaf_page_id`) in leaf headers for ordered range scans; omit parent pointers in v1 and track the descent path on the stack during mutations.
4. Distinguish `contiguous_free_space()` (`upper_offset - lower_offset`) from `usable_free_space()` (`contiguous_free_space() + fragmented_free_bytes`). Compact a page in-place when `usable_free_space()` suffices before splitting.
5. Account for variable-length separator key replacement in internal parents during deletion/redistribution so replacing a shorter separator with a longer separator can compact or split the parent safely.

## Consequences
- High space utilization for small keys and values while preserving worst-case split guarantees for maximum-size keys and values.
