# PageDB B+ Tree Design Document

## 1. Overview

PageDB uses an **order-free, variable-length, byte-aware B+ tree** stored across
4096-byte slotted pages. Keys are `0..256` bytes and values are `0..512` bytes.
The comparator is unsigned byte lexicographic (`UnsignedByteLess`).

### Structural rules

- **Leaf pages** (`PageType::kLeaf`) store `(key, value)` pairs. Adjacent leaves
  are linked via `aux_page_id` (right sibling, or `kInvalidPageId` for the
  rightmost leaf).
- **Internal pages** (`PageType::kInternal`) store separator keys with right-child
  pointers. The `aux_page_id` field holds the `leftmost_child_page_id`.
- Every key in the subtree rooted at `leftmost_child` is strictly less than
  `separator[0]`. Every key in the subtree rooted at `separator[i].right_child`
  satisfies `separator[i] ≤ key < separator[i+1]`.

### Record formats (existing `PageCodec`)

| Record type | Wire layout | Total bytes |
| :--- | :--- | :--- |
| Leaf | `key_len:u16 \| val_len:u16 \| key \| value` | `4 + key_len + val_len` |
| Internal | `key_len:u16 \| right_child:u32 \| key` | `6 + key_len` |

Each record occupies one slot in the slotted page. The slot directory entry adds
4 bytes (`kSlotEntrySize`). Total cost of inserting one leaf record into a page:
`4 + key_len + val_len + 4 = 8 + key_len + val_len`. For an internal record:
`6 + key_len + 4 = 10 + key_len`.

### Page capacity

Usable payload area per page: `kPageSize - kPageHeaderSize = 4048` bytes.

---

## 2. Split Point Selection

### Problem

When an insert overflows a page, we must split it into two pages such that both
fit within 4048 usable bytes. Because records are variable-length, we cannot use
a fixed "split at slot N/2" rule.

### Algorithm: byte-balanced binary split

Given a page with `N` existing slots plus 1 new record to be inserted:

1. Build a virtual sorted sequence of `N + 1` records (the existing slots with
   the new record logically inserted at its sorted position).
2. Compute the **cumulative cost** array `C[i]` for `i = 0..N`, where cost
   includes both the payload bytes and the slot directory entry (4 bytes each).
3. Let `total = C[N+1]` be the total cost of all `N + 1` records.
4. Find the split index `k` (1 ≤ k ≤ N) that minimizes
   `|C[k] - (total - C[k])|` — i.e., the most balanced byte split.
5. For **leaf splits**: records `[0..k-1]` go to the left page, records
   `[k..N]` go to the right page. The separator key promoted to the parent is
   `key[k]` (the first key of the right page) — a *copy* of the key.
6. For **internal splits**: records `[0..k-2]` go to the left page, record
   `k-1` is the **pushed-up separator** (its key goes to the parent, its
   right_child becomes the new right page's `leftmost_child`), and records
   `[k..N]` go to the right page.

### Guarantees

- Both halves contain at least one record (since `k ∈ [1, N]` and `N ≥ 1`).
- The maximum single-record size is `8 + 256 + 512 = 776` bytes (leaf) or
  `10 + 256 = 266` bytes (internal), both well under 4048, so both halves
  always fit.

---

## 3. Deletion: Redistribution vs. Merge Decision

### Low-water threshold

A leaf or internal node is **underfull** when its active payload bytes (sum of
all slot payloads) falls below `kMinPageFillBytes = 1012` bytes (~25% of the
4048 usable bytes).

### Decision rules (conservative no-split-on-delete policy)

After deleting a record from page `P`:

1. If `P` is the root, do nothing (the root may be arbitrarily underfull).
2. If `P.active_bytes() >= kMinPageFillBytes`, done.
3. Choose a sibling `S` (prefer the sibling that shares the same parent
   separator — right sibling first, then left sibling).
4. **Try merge**: if `P.active_bytes() + S.active_bytes()` ≤ 4048 minus the
   cost of the pulled-down parent separator (for internal merges), merge `P`
   and `S`. Merging **always removes** one separator from the parent, strictly
   reducing parent byte usage. This can never overflow the parent.
5. **Try redistribute**: if merge doesn't fit, consider borrowing one record
   from `S` to `P`. Redistribution **replaces** a separator in the parent:
   the old separator `K_old` is removed and a new separator `K_new` is
   inserted. Only redistribute when:
   ```
   parent.usable_free_space() + slot_cost(K_old) >= slot_cost(K_new)
   ```
   where `slot_cost(K) = 10 + len(K)` for internal separators. This ensures
   the parent never overflows during a delete path — **no upward cascading
   splits on delete**.
6. If neither merge nor redistribute is safe, leave `P` underfull. The tree
   remains correct; only fill factor degrades slightly.

### Why this is safe

- **Merge removes a separator from the parent**: parent byte usage strictly
  decreases. The parent may itself become underfull, triggering a recursive
  merge/redistribute check up the tree, but never a split.
- **Redistribution is guarded**: the parent capacity check prevents overflow.
- **Leaving underfull is always safe**: B+ tree search and scan correctness
  depends only on sorted keys and valid child pointers, not on fill factors.

---

## 4. Internal Node Merge: Pull-Down Separator Byte Accounting

When merging internal nodes `L` (left) and `R` (right) separated by parent
key `K_p`:

1. `K_p` is **pulled down** from the parent into `L` as a new separator record
   with `right_child = R.leftmost_child`.
2. All of `R`'s separator slots are appended to `L`.
3. `R` is freed (added to the free list).
4. The parent's separator slot for `K_p` is erased.

**Fit condition** (checked before merging):

$$
\text{active\_bytes}(L) + \text{active\_bytes}(R) + (10 + |K_p|) \le 4048
$$

where `10 + |K_p|` accounts for the pulled-down separator's slot entry (4 bytes)
plus its record payload (6 + |K_p| bytes).

---

## 5. Root Collapse and Empty-Tree Representation

### Root collapse

After a merge reduces the root internal node to zero separator slots (only
`leftmost_child` remains), the root is **collapsed**:

1. Set `superblock.root_page_id = root.leftmost_child`.
2. Free the old root page.

This can cascade: if the promoted child is also an internal node with zero
slots, collapse again. In practice, each merge removes at most one level.

### Empty-tree state

- The initial database has a single empty leaf at `PageId(1)` as the root.
- An empty tree is represented as a leaf root with `num_slots == 0`.
- A leaf root with zero slots is **never** collapsed or freed. It is the
  permanent empty-tree sentinel.
- `get()` on an empty tree returns `kNotFound`.
- `scan()` on an empty tree returns an empty range.

---

## 6. Buffer Pool Capacity Requirement

The B+ tree implementation requires a minimum of **4 buffer pool frames**:

| Operation | Frames needed | Pages held simultaneously |
| :--- | :--- | :--- |
| `get()` | 1 per level | Only 1 at a time (unpin parent before descending) |
| `put()` (no split) | 1 per level | Only 1 at a time |
| `put()` (leaf split) | 3 | parent + old leaf + new sibling |
| `put()` (cascading internal split) | 4 | grandparent + parent + left + right |
| `del()` (merge/redistribute) | 3 | parent + left sibling + right sibling |

With the "unpin before descending" discipline during traversal and "pin at most
3-4 during structural modification", 4 frames is sufficient for all single-
threaded operations on trees of any depth.

### Pin discipline

- **Traversal** (get, scan, finding the target leaf for put/del): pin one page,
  read it, identify the child to descend to, **unpin the current page**, then
  pin the child. Only 1 frame is held at a time during traversal.
- **Structural modification** (split, merge, redistribute): the traversal
  saves the path (page IDs) on a stack. When a modification is needed, re-pin
  the necessary pages (parent + affected children) temporarily.

Actually, a simpler and more robust approach: **re-traverse from the root when a
split is needed**. On the first traversal, detect that the leaf is full. On the
second traversal, split proactively on the way down (any full node encountered
during descent is split before entering it). This eliminates the need to hold
multiple pages simultaneously during traversal.

**Chosen approach**: single-pass with deferred splits. Traverse top-down, saving
page IDs. At the leaf, attempt the insert. If it overflows, split the leaf
(pinning parent + leaf + new sibling = 3 frames), then propagate upward if the
parent overflows (re-pin grandparent + parent + new sibling = 3 frames, releasing
the leaf first). Maximum concurrent pins: **3 frames**.

Minimum `BufferPool` capacity: **4 frames** (3 for modifications + 1 margin for
the `PageAllocator` to write a newly allocated page before it enters the pool).

---

## 7. Scan (Range Query) Design

`scan(start_key, end_key)` returns all `(key, value)` pairs where
`start_key ≤ key < end_key` in sorted order.

1. Traverse the tree to find the leaf containing `start_key` (or the first key
   ≥ `start_key`).
2. Scan forward through the leaf's slots. For each matching key, collect the
   result.
3. Follow `aux_page_id` (next leaf pointer) to the right sibling leaf. Continue
   scanning until a key ≥ `end_key` is found or the leaf chain ends.
4. Unpin each leaf before moving to the next.

---

## 8. Implementation Structure

```
include/pagedb/btree.h     — BTree class declaration
src/index/btree.cpp         — BTree core operations (put, get, del, scan)
tests/unit/btree_test.cpp   — Deterministic + randomized differential tests
```

### BTree class interface

```cpp
class BTree {
 public:
  BTree(BufferPool& pool, PageAllocator& allocator);

  [[nodiscard]] Status put(std::span<const std::byte> key,
                           std::span<const std::byte> value);
  [[nodiscard]] Result<std::string> get(std::span<const std::byte> key);
  [[nodiscard]] Status del(std::span<const std::byte> key);

  struct KeyValue {
    std::string key;
    std::string value;
  };
  [[nodiscard]] Result<std::vector<KeyValue>> scan(
      std::span<const std::byte> start_key,
      std::span<const std::byte> end_key);

  // Structural integrity verifier: walks every reachable page, checks sorted
  // keys, valid child pointers, and parent-child separator invariants.
  [[nodiscard]] Status verify() const;
};
```

---

## 9. Differential Testing Strategy

Every operation is mirrored against a `std::map<std::string, std::string, UnsignedByteLess>` reference model:

- After every `put()`, `del()`, verify the B+ tree `get()` result matches the
  map lookup.
- After every N operations, run `verify()` to check structural invariants.
- After every N operations, run a full `scan("", max_key)` and compare against
  the map's sorted contents.
- Use a seeded PRNG for reproducibility. Generate keys of varying lengths
  (0..256 bytes) including edge cases: empty keys, max-length keys, keys with
  `0x00`/`0xFF` bytes.
