# PageDB v1 Binary File and Page Format Specification

## 1. Encoding Principles

1. **Endianness**: All multi-byte integers are encoded in **little-endian** byte order (`u16`, `u32`, `u64`).
2. **No Native Struct Serialization**: On-disk structures are encoded and decoded strictly through explicit byte-level readers and writers (`byte_codec`). No C++ struct is ever cast via `reinterpret_cast` to or from a disk buffer.
3. **Zeroed Slack and Reserved Bytes**: Every reserved header field and every unallocated gap inside a 4096-byte page (`[lower_offset, upper_offset)`) must be filled with `0x00` prior to checksum computation. Non-zero reserved fields in v1 are rejected with `ErrorCode::Corruption`.
4. **Fixed Page Size**: `kPageSize = 4096` bytes (`0x1000`). The byte offset of `PageId(p)` in `pagedb.data` is `static_cast<std::uint64_t>(p) * 4096ULL`, computed with overflow validation.

---

## 2. Common Page Header (48 Bytes, Offsets `0x000..0x02F`)

Every page in `pagedb.data` (including Page 0) starts with the following 48-byte header:

| Offset | Size | Type | Field | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | 4 | `u32` | `page_magic` | Constant `0x50474442` (`"PGDB"`) |
| `0x04` | 2 | `u16` | `format_major` | Format major version (`1`) |
| `0x06` | 2 | `u16` | `format_minor` | Format minor version (`0`) |
| `0x08` | 1 | `u8` | `page_type` | `1` = Superblock, `2` = Leaf, `3` = Internal, `4` = Free |
| `0x09` | 1 | `u8` | `flags` | Page flags (`0` in v1) |
| `0x0A` | 2 | `u16` | `num_slots` | Number of active directory slots (`0` for Superblock and Free pages) |
| `0x0C` | 4 | `u32` | `page_id` | Logical page identifier; must match `file_offset / 4096` (`!= 0xFFFFFFFF`) |
| `0x10` | 8 | `u64` | `page_lsn` | LSN of the WAL record that last modified this page (`0` before WAL) |
| `0x18` | 2 | `u16` | `lower_offset` | End of slot directory (`48 + num_slots * 4` for slotted pages; `112` for Superblock; `56` for Free) |
| `0x1A` | 2 | `u16` | `upper_offset` | Start of payload region growing downward from `4096` (`4096` for non-slotted pages) |
| `0x1C` | 4 | `u32` | `fragmented_free_bytes` | Reclaimable dead payload bytes inside `[upper_offset, 4096)` |
| `0x20` | 4 | `u32` | `aux_page_id` | Leaf: `next_leaf_page_id` (`0xFFFFFFFF` = none). Internal: `leftmost_child_page_id`. Free: `next_free_page_id` (`0xFFFFFFFF` = none). Superblock: `0xFFFFFFFF`. |
| `0x24` | 4 | `u32` | `checksum` | CRC32C (Castagnoli) over all 4096 bytes with bytes `0x24..0x27` set to `0` |
| `0x28` | 8 | `u64` | `reserved` | Must be `0` |

### Header Validation Order on Decode
1. Verify buffer length is exactly `4096` bytes.
2. Read `page_magic`. If `!= 0x50474442`, return `ErrorCode::Corruption`.
3. Read `format_major` and `format_minor`. If `format_major != 1` or `format_minor != 0`, return `ErrorCode::UnsupportedFormatVersion`.
4. Read `checksum` at `0x24..0x27`, compute CRC32C over the 4096-byte page with `0x24..0x27` treated as zero, and verify equality. If mismatched, return `ErrorCode::Corruption` **before** trusting any offset or slot field.
5. Verify `page_id` equals the expected `PageId` requested by the caller.
6. Verify `flags == 0` and `reserved == 0`.
7. Validate `page_type`-specific invariants.

---

## 3. Superblock Layout (`PageId(0)`, Offsets `0x030..0x06F`)

Page 0 has `page_type = 1` (`Superblock`), `num_slots = 0`, `lower_offset = 112` (`0x070`), `upper_offset = 4096`, `fragmented_free_bytes = 0`, `aux_page_id = 0xFFFFFFFF`.

Its 64-byte payload occupies bytes `0x030..0x06F`:

| Offset | Size | Type | Field | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x30` | 8 | `u64` | `sb_magic` | Constant `0x5041474544425631` (`"PAGEDBV1"`) |
| `0x38` | 4 | `u32` | `page_size` | Must equal `4096` |
| `0x3C` | 4 | `u32` | `root_page_id` | Page ID of B+ tree root (`>= 1` and `< next_page_id`) |
| `0x40` | 4 | `u32` | `next_page_id` | Next unallocated page ID (`>= 2`; equals file page count when append-only) |
| `0x44` | 4 | `u32` | `free_list_head` | Head `PageId` of free list (`0xFFFFFFFF` = empty free list) |
| `0x48` | 8 | `u64` | `checkpoint_lsn` | Last completed checkpoint LSN (`0` before checkpoint support) |
| `0x50` | 4 | `u32` | `max_key_size` | Must equal `256` |
| `0x54` | 4 | `u32` | `max_value_size` | Must equal `512` |
| `0x58` | 24 | `u8[24]` | `sb_reserved` | All 24 bytes must be `0x00` |

Bytes `0x070..0xFFF` (3984 bytes) must be all `0x00`.

---

## 4. Slotted-Page Layout (Leaf and Internal Pages)

Leaf pages (`page_type = 2`) and Internal pages (`page_type = 3`) use a slotted-page structure inside bytes `0x030..0xFFF` (4048 usable bytes after the 48-byte header):

```
+-----------------------------------------------------------------------+ 0x000 (0)
| Common Page Header (48 bytes)                                         |
+-----------------------------------------------------------------------+ 0x030 (48)
| Slot Directory: num_slots * 4 bytes (grows upward ->)                 |
|   Slot[i]: u16 payload_offset | u16 payload_length                    |
+-----------------------------------------------------------------------+ lower_offset
| Contiguous Free Space (all bytes 0x00)                                |
+-----------------------------------------------------------------------+ upper_offset
| Payload Records (grows downward <-)                                   |
+-----------------------------------------------------------------------+ 0x1000 (4096)
```

### Slotted-Page Structural Invariants
1. `lower_offset == 48 + num_slots * 4`.
2. `48 <= lower_offset <= upper_offset <= 4096`.
3. Every slot $i \in [0, \text{num\_slots})$ has:
   - `payload_length > 0`
   - `upper_offset <= payload_offset`
   - `payload_offset + payload_length <= 4096` (checked without 16-bit wraparound)
4. **Non-Overlapping Slot Invariant**: No two slots may reference overlapping byte ranges in `[upper_offset, 4096)`.
5. **Exact Space Accounting Invariant**:
   $$\sum_{i=0}^{\text{num\_slots}-1} \text{Slot}[i].\text{length} + \text{fragmented\_free\_bytes} == 4096 - \text{upper\_offset}$$
6. **Zeroed Free Space Invariant**: All bytes in `[lower_offset, upper_offset)` are `0x00`. After page compaction (`fragmented_free_bytes == 0`), payloads are tightly packed in `[upper_offset, 4096)`.
7. **Free-Space Calculation**:
   - `contiguous_free_space() = upper_offset - lower_offset`
   - `usable_free_space() = (upper_offset - lower_offset) + fragmented_free_bytes`
   - If an insertion or update fits in `usable_free_space()` but not in `contiguous_free_space()`, the page compacts its active records in-place (resetting `fragmented_free_bytes = 0`) before allocating the new payload slice.

---

## 5. B+ Tree Record Formats Inside Slotted Pages

### 5.1 Leaf Page (`page_type = 2`)
- `aux_page_id` in the header holds `next_leaf_page_id` (`0xFFFFFFFF` if this is the rightmost leaf).
- Each slot $i$ points to a Leaf Record with length `4 + key_len + val_len`:

| Relative Offset | Size | Type | Field |
| :--- | :--- | :--- | :--- |
| `0x00` | 2 | `u16` | `key_len` (`0 <= key_len <= 256`) |
| `0x02` | 2 | `u16` | `val_len` (`0 <= val_len <= 512`) |
| `0x04` | `key_len` | `u8[]` | `key_bytes` |
| `0x04 + key_len` | `val_len` | `u8[]` | `val_bytes` |

- Maximum Leaf Record size: `4 + 256 + 512 = 772` bytes (+ 4-byte slot = `776` bytes).
- Slots $0 \dots \text{num\_slots}-1$ are stored in strictly ascending key order (`key[0] < key[1] < ...`).

### 5.2 Internal Page (`page_type = 3`)
- `aux_page_id` in the header holds `leftmost_child_page_id` ($C_0$, must be a valid tree page ID $\ge 1$).
- Each slot $i \in [0, \text{num\_slots})$ stores separator key $K_i$ and right child pointer $C_{i+1}$, with record length `6 + key_len`:

| Relative Offset | Size | Type | Field |
| :--- | :--- | :--- | :--- |
| `0x00` | 2 | `u16` | `key_len` (`0 <= key_len <= 256`) |
| `0x02` | 4 | `u32` | `right_child_page_id` ($C_{i+1} \ge 1$) |
| `0x06` | `key_len` | `u8[]` | `separator_key_bytes` ($K_i$) |

- Routing rule:
  - Keys strictly less than $K_0$ route to $C_0$ (`leftmost_child_page_id`).
  - Keys in $[K_i, K_{i+1})$ route to $C_{i+1}$.
  - Keys $\ge K_{\text{num\_slots}-1}$ route to $C_{\text{num\_slots}}$.
- Separator invariant: $K_i$ equals the minimum key present in the subtree rooted at $C_{i+1}$ (or a valid tight separator bound on internal splits/merges).

### 5.3 Free Page (`page_type = 4`)
- `num_slots = 0`, `lower_offset = 48`, `upper_offset = 4096`, `fragmented_free_bytes = 0`.
- `aux_page_id` in the header stores `next_free_page_id` (`0xFFFFFFFF` = end of free list).
- Bytes `0x030..0xFFF` must be all `0x00`.
