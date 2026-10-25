# PageDB Engineering Roadmap

## 1. Guiding Rule

**Correctness first, durability second, concurrency third, performance fourth, security and verification throughout.**

Every milestone must pass its test and verification gate before moving to the next phase.

---

## 2. Milestone Progression

| Milestone | Phases | Scope | Exit Gate |
| :--- | :--- | :--- | :--- |
| **M0** | Phase 0, 2 | Specification, binary format, failure model, invariants, security model, ADRs | Spec, format offsets, and failure boundaries documented without contradictions |
| **M1** | Phase 1 | CMake C++20 build system, compiler hardening, CTest, presets, CI workflow, `.gitignore`, `README.md` | Clean configure, build, and CTest execution verified |
| **M2** | Phase 3 | Safe primitives: strong IDs (`PageId`, `Lsn`, `TxnId`), `Status`/`Result<T>`, little-endian `byte_codec`, checked math, unsigned byte comparator, Castagnoli `CRC32C` | Unit tests for status, boundary codecs, overflow checks, comparator (`0x00`/`0x7f`/`0x80`/`0xff`), and RFC 3720 CRC32C vectors pass |
| **M3** | Phases 4, 5, 6 | RAII `FileHandle`, `DatabaseLock` (`fcntl`), `IoVfs` fault-injection seam, 4096-byte `PageCodec` (`Superblock`, `SlottedPage`), `PageStore`, `PageAllocator` | Deterministic page encoding, corruption rejection, short-I/O & fault-injection handling, clean close/reopen persistence verified |
| **M4** | Phase 7 | Fixed-capacity `BufferPool`, move-only `PageGuard`, CLOCK eviction, dirty-page tracking, pin exhaustion & I/O error recovery | Capacity-1/2 stress, all-pinned error, dirty eviction, and guard move-semantics tests pass |
| **M5** | Phase 8 | B+ tree core algorithms (insert, byte-aware split, get, range scan, delete, redistribution, merge, root collapse, structural `verify()`) | Randomized differential model tests against `std::map` with custom unsigned comparator and `verify()` after every operation |
| **M6** | Phase 9, 10 | Persistent slotted-page B+ tree over `BufferPool`, free-list page reclamation, and interactive CLI | Reopen persistence under small buffer pool eviction pressure and insert/delete reuse workloads |
| **M7** | Phases 11, 12 | Checksummed `PAGE_IMAGE` + `COMMIT` WAL, no-steal buffer policy, WAL-before-data enforcement, torn-page & torn-superblock recovery | Deterministic crash-point and torn-page fault injection tests pass; recovery is idempotent |
| **M8** | Phase 13 | Explicit multi-operation transactions (`begin`, `put`, `get`, `del`, `scan`, `commit`, `rollback`) with before-image rollback | All-or-nothing commit/rollback and conservation-invariant crash tests pass |
| **M9** | Phase 14 | Crash-safe checkpointing and bounded WAL segment retention | Crash injection across every checkpoint step proves no required WAL image is discarded early |
| **M10** | Phase 15 | Coarse-grained multi-thread safety (shared reader / exclusive writer locks, thread-safe frame pinning) | Stress tests pass cleanly under ThreadSanitizer (TSan) |
| **M11** | Phases 17, 18 | Diagnostic CLI tools (`pagedb_inspect`, `pagedb_verify`, `pagedb_crashtest`) and fuzz/adversarial corpora | Verifier catches all injected corruptions; decoders are total over arbitrary byte inputs |
| **M12** | Phases 19, 20 | Reproducible benchmark suite and profile-driven optimizations | Measured throughput/latency baselines with post-benchmark integrity verification |

---

## 3. Architectural & Security Loopholes Closed in v1

During the Phase 0 engineering audit of the baseline roadmap, six subtle storage and security loopholes were identified and resolved in the specification:

1. **Variable-Length B+ Tree Parent Separator Growth on Delete/Redistribute (M5/M6)**:
   - Because keys range from `0..256` bytes, borrowing an entry from a sibling leaf or merging nodes can replace a short separator key in the parent internal node with a longer separator key. The parent internal node must check `usable_free_space()`, compact if fragmented, and split if necessary even during a deletion path.
2. **OS-Enforced Single-Process Exclusion (M3)**:
   - Rather than relying on caller discipline for the single-process requirement, `DatabaseLock` acquires a non-blocking exclusive POSIX advisory lock (`fcntl(F_SETLK)`) on `pagedb.lock` opened with `O_NOFOLLOW` and `0600` permissions.
3. **Slack-Space Zero Scrubbing and Non-Overlapping Slot Verification (M3)**:
   - All 4096-byte page buffers are zero-initialized and scrubbed on delete/compaction so uninitialized heap or stack memory is never written to disk inside `[lower_offset, upper_offset)`.
   - `SlottedPage::decode` validates both individual slot bounds and pairwise non-overlap, plus exact byte conservation ($\sum \text{slot\_len} + \text{fragmented\_free\_bytes} == 4096 - \text{upper\_offset}$).
4. **Superblock (`PageId(0)`) Torn-Write Recovery Bootstrap (M7)**:
   - If a crash tears `PageId(0)` during a background data-file writeback after WAL commit, `Database::open()` must scan `pagedb.wal` for the latest committed `PAGE_IMAGE` of `PageId(0)` before rejecting a corrupted Page 0 checksum.
5. **Contiguous vs. Total Free Space in Slotted Pages (M3/M5)**:
   - Updating or deleting variable-length records creates fragmented gaps in the payload area (`[upper_offset, 4096)`). Slotted pages track `fragmented_free_bytes` explicitly and compact in-place when `usable_free_space()` is sufficient before triggering a B+ tree split.
6. **WSL2 / Filesystem Test Fidelity (M1–M12)**:
   - Test suites place temporary database directories on native Linux `ext4` (`/tmp`) when running under WSL2 so POSIX advisory locking (`fcntl`) and directory synchronization (`fsync`) are exercised natively rather than through DrvFS emulation.
