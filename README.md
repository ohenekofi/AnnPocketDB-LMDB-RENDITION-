# AnnPocketDb (LMDB C++ RENDITION)

An LMDB-inspired, embedded, single-file, memory-mapped storage engine in
C++20. Single writer / multiple readers, copy-on-write MVCC, no server
process, no background compaction thread.

This README is deliberately precise about what is **implemented and
tested** versus **designed and scaffolded** so it's usable as an honest
status page, not marketing copy.

## What's implemented and tested right now

The kernel plus the full transaction/environment layer are complete, have
zero third-party dependencies, and are covered by real (non-mocked) tests
that exercise splits, merges/redistribution, cursors, MVCC isolation, and
concurrent-writer serialization:

| Module | Files | What it does |
|---|---|---|
| `util` | `util/status.hpp`, `util/types.hpp`, `util/crc32c.*` | `Status`/`Result<T>` error handling; byte-view helpers; hardware-accelerated (SSE4.2 / ARMv8-CRC) CRC32C with software fallback |
| `platform` | `platform/mapped_file.*` | Cross-platform mmap wrapper. Reserves the full `maxDatabaseSize` as virtual address space at open time so **growing the file is a pointer-stable `ftruncate`, never a remap** — this is what makes it safe for a reader to hold a pointer into the mapping while another thread grows the file |
| `core` | `core/page.hpp`, `core/meta_page.hpp`, `core/free_list.*`, `core/reader_table.hpp`, `core/page_manager.*` | Page header format, double-buffered meta pages (atomic commit = "pick the valid meta page with the higher committed txn id"), two-stage MVCC-safe free list (retired → reclaimable once no active reader could still see it), page allocation/CRC validation |
| `btree` | `btree/node_codec.*`, `btree/btree.*`, `btree/cursor.*` | Slotted-page leaf/branch node format (binary-search reads, compact-rewrite-on-write), COW insert with leaf/branch splits, **COW delete with merge/redistribute rebalancing that cascades through every ancestor level** (not just a single-level patch), large-value overflow chains, and a stack-based `Cursor` for ordered forward/backward iteration and seeks |
| `txn` | `txn/transaction.hpp/.cpp` | `ReadTransaction` (MVCC snapshot + reader registration) and `WriteTransaction` (single-writer mutex, COW mutation, commit/abort with correct RAII cleanup on early return) |
| `api` | `api/environment.hpp/.cpp` | `Environment`: opens the file, issues transactions, and offers single-op `get`/`put`/`remove` convenience wrappers |

Run the suite:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

ctest --test-dir build -C Debug --output-on-failure #run test
#===============================
cmake -S . -B build -DANNPOCKETDB_WITH_ENCRYPTION=ON && cmake --build build
ctest --test-dir build -C Debug --output-on-failure

```

(`cmake`/network weren't available in the environment this was built in,
so each test was also verified by compiling and running it directly with
`g++ -std=c++20 -pthread`; the CMake wiring mirrors that exactly.)

### What the tests actually check

(This list names the test binaries that actually exist under `tests/` today
and are wired into `tests/CMakeLists.txt`/`ctest` right now — it replaced an
earlier draft of this section that had drifted to describe file names and
scenarios from an older design pass that were never actually built.)

- `test_crc32c`: matches the standard CRC32C("123456789") = `0xE3069283` test
  vector; verifies chained computation and the mask/unmask round trip.
- `test_mapped_file`: write → flush → grow well past the initial file size
  and asserts the base pointer **does not move**; confirms growth past the
  reserved ceiling fails cleanly with `MapFull`; confirms persistence across
  close/reopen (read-write and read-only).
- `test_node_codec`: encode/decode round trip for leaf and branch nodes,
  including an overflow-marked entry; binary-search correctness for exact,
  missing, and boundary keys.
- `test_basic_kv`, `test_append`, `test_nested_txn`, `test_2pc`: single-op
  put/get/remove and `KeyExists` semantics; the `MDB_APPEND` fast path and
  its out-of-order rejection; nested-transaction commit/abort and cursor
  generation invalidation; two-phase prepare/commit/abort.
- `test_mvcc`: a `ReadTransaction`'s view is provably frozen — checked after
  a single commit, again through **ten subsequent commits to the same key**,
  and again through a later commit that forces **real COW leaf/branch
  splits** (grows a single-leaf tree to 3,000 keys) while the original
  snapshot is still open; the old snapshot's entry count, values, and
  "key doesn't exist yet" answers are all reverified after the splits.
- `test_btree_rebalance`: **the direct proof the merge/redistribute logic
  works**, not just that keys survive. Directed scenarios (contiguous mass
  deletion forcing merges, sparse deletion forcing redistribution, full
  drain forcing root collapse) verified via full sorted cursor scans and
  `stat()` page-count drops; plus a 6,000-op randomized insert/delete fuzz
  cross-checked against a `std::map` reference (exact key-set equality, not
  just "every expected key found"), followed by `Environment::verify()`'s
  minimum-fill invariant walk (flags any *non-root* node under 25% fill —
  see `VerifyReport::minFillViolations`); plus a dedicated check that after
  deleting 80% of 5,000 sequential keys, average leaf fill stays well above
  the 25% floor (not degrading toward it, which is what a collapse-only, no
  rebalancing policy would do).
- `test_cursor_extras`: `count()`/`replace()`/`pop()`/`getMulti()`/
  `putMulti()`/`seekRangeDup()` — including DUPFIXED and DUPSORT sub-tree
  escalation cases for `count()`, the append-fast-path ordering guarantee for
  `putMulti()`, and `getMulti()`'s missing-key-is-silently-skipped contract.
- `test_dupsort`, `test_subtree_escalation`: DUPSORT/DUPFIXED put/get/cursor
  navigation; escalation of a large dup-group into its own nested B+tree
  sub-page once it crosses the escalation threshold, verified via full
  forward/backward cursor walks over 150 dup values.
- `test_subdatabase`, `test_subdb_flags`, `test_env_extras`: named
  sub-database create/open/drop/close/list lifecycle and stale-handle
  (`BadDbi`) detection; per-database flag persistence across reopen; the
  idempotent `openOrCreateSubDatabase` get-or-create contract.
- `test_page_id_list`: the sorted/unsorted dual-discipline `PageIdList`
  utility backing the free list.
- `test_status_codes`: the LMDB-parity granular `StatusCode` values
  (`BadValSize`, `DbsFull`, `BadDbi`, `InvalidFile`, ...) are reachable from
  real operations, not just declared.
- `test_crypto_primitives`, `test_encrypted_db`, `test_incremental_backup`
  (+ `_encrypted`): XChaCha20-Poly1305 page encryption and Argon2id key
  derivation at the primitive level; full `Environment`-level encrypted
  round-trip including raw-file plaintext-absence and ciphertext-corruption
  detection; the optional SHA-256 checksum bolt-on; physical incremental
  (delta) backup, including applying an increment onto an *encrypted*
  destination.
- `test_compression`: LZ4 value/overflow-level round-trip across the
  size/threshold matrix, an incompressible-value fallback case, a real
  disk-savings assertion (fewer overflow pages with compression on), and
  confirmation DUPSORT/DUPFIXED values are never compressed.
- `test_crash_consistency`: static file corruption/truncation fuzzing, real
  process kills at random timing, and precise crash-window targeting
  (crash right after a WAL commit is durable but before the meta flip;
  crash right after the meta flip but before the WAL checkpoint/data
  fsync) — asserts recovery always lands on a clean prefix of a sequential
  writer's commits, `backup::verify()` stays clean, and (Tier 4) that a
  crashed transaction's retired pages are correctly recovered via WAL
  replay and become reusable again.
- `test_mplay_fuzzer`: spawns real, separate OS processes (`CreateProcess`/
  `fork`+`exec`, never threads) — some readers, some writers — hammering one
  shared database file concurrently, to exercise the `.lock` file's
  shared-memory reader table under genuine cross-process contention. This is
  a real but modest (seconds-scale) CI smoke test on its own — see
  "Sanitizer and soak testing" below for the long-running/instrumented
  counterpart.
- `test_index`: catalog create/drop/persist lifecycle (including the
  `saveCatalog`-before-commit requirement — an index created but never saved
  is gone on reopen), `insertEntry`/`removeEntry` idempotency, cursor
  read-back, and the shared `maxKeySize()` boundary.
- `test_blob`: single-page and multi-page (chunked) round-trips via both
  `BlobStore::get` and streaming `BlobReader`, `BlobWriter`'s
  abandoned-without-`finalize()` destructor safety net, `BlobRef` wire-format
  boundary checks, a hand-crafted oversized-`totalLen` head page rejected
  without an oversized allocation attempt, and `Environment::putBlob`/
  `getBlob`/`removeBlob`'s inline-value and chain-reclamation edge cases.
- `test_document`: every `Value` type round-tripped through the real
  MessagePack codec (including the positive-integer Int/UInt wire-format
  ambiguity — MessagePack's "positive fixint" encoding does not distinguish
  the two), the legacy `PlainTextCodec`, dotted/array field-path resolution,
  and `DocumentStore` + `IndexManager` automatic index maintenance including
  sparse (missing-field) indexing.
- `test_query`: table-scan and index-backed access paths cross-checked
  against each other on identical data for every `Op` (`Eq`/`Ne`/`Lt`/`Le`/
  `Gt`/`Ge`/`Prefix`/`Exists`) — the planner's choice of access path must
  never change the result set — plus `limit`, projection, `parallelWorkers`
  consistency, `countEstimate`, and a worked example of the numeric-vs-lexical
  predicate-comparison quirk (`Predicate::value` is always a string, so
  `score >= "9"` is a lexical, not numeric, comparison).
- `test_changefeed`: `pollFrom` returns Insert/Update/Delete events in commit
  order with correct old/new values, resume-token semantics (no duplicates,
  no gaps), `latestSequence`, `limit`, and a live-subscribe check proving
  `notifyCommitted()` only fires after a real, successful commit.

## Sanitizer and soak testing

Build infrastructure for ThreadSanitizer/AddressSanitizer/UBSan is wired up
via `cmake/Sanitizers.cmake` (`-DANNPOCKETDB_SANITIZE=thread|address|
undefined|address+undefined`, default `none` — fully opt-in, the default
build's flags/behavior are unchanged) and `.github/workflows/sanitizers.yml`
(a fast tier on every push/PR, plus a nightly/manually-dispatched soak tier).
Two dedicated soak tests exist specifically to give a sanitizer enough
scheduling entropy to actually trip on a race, which the fast unit-test
suite's short, fixed scenarios cannot reliably do:

- `test_soak_concurrent`: sustained, randomized, overlapping in-process
  reader/writer/nested-writer activity for a configurable duration
  (`ANNPOCKETDB_SOAK_SECONDS`, default 5s locally / up to an hour in the
  nightly CI job) — a pinned long-lived reader continuously re-verifies its
  snapshot never changes underneath it (the direct stress test of two-stage
  free-list reclaim), alongside short-lived readers and a periodic
  `readerCheck()` maintenance thread.
- `test_mplay_soak`: the cross-process counterpart, using the same
  `mplay_worker` binary as `test_mplay_fuzzer` but run in repeated rounds
  for a configurable duration — the one class of bug a single-process TSan
  run structurally cannot see, since TSan only instruments one process's
  address space.

Both are registered under the ctest label `soak` (excluded from a plain
`ctest` run via `-LE soak`, included via `-L soak`) so the default fast
suite stays fast. See `docs/SANITIZERS.md` for exact commands, how to read a
TSan report, and what this setup does *not* cover (crash-mid-write
consistency is a separate, already-covered concern — see
`test_crash_consistency` above — and MSVC's ThreadSanitizer is still
preview-only, so the `thread` mode requires Clang or GCC on Linux/macOS; a
Windows CI leg covers ASan only).

**Disclosed status**: this closes the "no TSan/soak-instrumented CI" gap
structurally, but no sanitizer run has actually executed against real
scheduling pressure yet to confirm the MVCC/free-list/reader-table
concurrency claims hold up — treat a green fast-tier run as "no regressions
in known scenarios," not as independent verification, until the soak tier
has actually run clean.

## Multi-process access (LMDB-parity)

AnnPocketDb supports **safe concurrent access from multiple OS processes** to the
same database file, matching LMDB's core concurrency model:

- **Single writer / multiple readers** across processes, not only threads.
- A sidecar lock file (`<data-path>.lock`) holds:
  - an exclusive writer lock (`flock` on POSIX, `LockFileEx` on Windows)
  - a shared-memory reader-slot table (256 slots) mapped into every process
- `WriteTransaction` takes the exclusive lock (plus an in-process mutex so
  threads within one process also serialize).
- `ReadTransaction` registers its snapshot txn id in the shared table so free-list
  reclaim never reuses a page still visible to a reader in *any* process.
- Slots record the registering process id; dead processes are opportunistically
  cleared so a crash cannot permanently exhaust the table.
- If all 256 shared slots are occupied, `beginRead()` returns `Status::Busy`
  (LMDB's `MDB_READERS_FULL` equivalent) rather than opening an invisible
  in-process-only reader.

Do **not** delete the `.lock` file while any process has the database open.

## Design notes worth knowing

- **Growth safety**: the address space for `maxDatabaseSize` is reserved
  once at `open()`. Growing the file afterward never moves the mapping, so
  a reader's raw pointer into the mmap is never invalidated by another
  thread's write. Hitting the reservation ceiling returns `Status::MapFull`
  rather than silently attempting an unsafe remap.
- **Compact-on-write node encoding**: because every node mutation already
  copies the page (COW/MVCC), the write path decodes a node's full entry
  set, applies the edit, and re-encodes from scratch rather than doing
  in-place slot-array surgery. This trades a little CPU (already being
  spent on the copy anyway) for a much smaller, more auditable
  implementation. Reads never pay this cost — they binary-search the
  on-disk slot array directly.
- **Free-list reclamation is two-stage**, matching how LMDB avoids
  corrupting a reader's still-in-use snapshot: pages freed by a commit go
  into a `retired` set tagged with the committing txn id; they only become
  reusable once no registered reader's snapshot is old enough to still
  reach them.
- **B+tree delete now merges and redistributes, cascading up the tree**:
  when a node's encoded size falls below 25% of page capacity, it is left
  unpersisted and handed to its parent, which either merges it with a
  sibling (if the combined content fits one page) or redistributes entries
  evenly between them (otherwise) — the same decision a classic B-tree
  delete makes, applied at every ancestor level via the natural recursion,
  not just a single pass. Only the root is allowed to sit below the
  threshold, since it has no sibling to rebalance against.
  `test_btree_rebalance` measures this directly.
- **DUPSORT** — implemented (`putDup` / `removeDup` / `getAll`; secondary indexes use it)
- **Insert-time redistribution** (still open) — see "Roadmap" below.
- **Cursor has no leaf sibling pointers to walk**: adding a prev/next leaf
  pointer to the node format would mean every leaf split touches its
  neighbors too, not just the root-to-leaf path, which conflicts with
  keeping every mutation strictly path-local under COW. Instead `Cursor`
  keeps a stack of (page, index) frames from the root down and re-descends
  through the parent stack when it walks off the end of a leaf — the
  standard approach for B+trees without sibling chaining.
- **WriteTransaction lifetime**: a `Cursor` obtained from a
  `WriteTransaction` captures a live accessor back into that transaction (so
  it can see the transaction's own later writes after a re-seek); moving
  the `WriteTransaction` while such a cursor is alive is undefined, the
  same rule as moving any container out from under a live C++ iterator into
  it. `ReadTransaction`'s cursor has no such constraint (its root is a
  fixed snapshot value captured by copy).

## Dependency philosophy

The kernel (everything above) depends on nothing but the C++20 standard
library and OS syscalls (`mmap`/`ftruncate`/`msync` on POSIX,
`CreateFileMapping`/`MapViewOfFile` on Windows) — no vendored code, no
system packages, maximally portable and auditable.

Feature modules that need real third-party algorithms (compression,
encryption) vendor small, portable, statically-linkable libraries directly
into `thirdparty/` rather than fetching them over the network at configure
time (`FetchContent` was the original plan for compression, but was dropped
in favor of vendoring — see below) — never a system package manager either,
so the project stays trivially shareable across Linux/macOS/Windows with no
"apt install libfoo-dev" step for consumers:

| Concern | Library | Why |
|---|---|---|
| Compression | LZ4 (vendored, `thirdparty/lz4/`, BSD-2-Clause) | Value/overflow-level only (see below) — small, fast, trivially vendored as two files; no network fetch needed, unlike the FetchContent approach this started with |
| Encryption (XChaCha20-Poly1305) | Monocypher (vendored, `thirdparty/monocypher/`, BSD-2-Clause OR CC0-1.0) | Single-file, permissive dual license, no OS crypto API lock-in, no network fetch |
| Document encoding | none — hand-rolled MessagePack subset | `src/document/document.cpp` is self-contained (see its own header comment); no third-party library needed, so it's always built alongside the other feature modules, not gated behind a fetch |

**Compression** (`ANNPOCKETDB_WITH_COMPRESSION`) is implemented: value/overflow-level
LZ4, not page-level. This engine stores every page at a fixed offset
(`id * pageSize`) directly in an mmap, like LMDB — a compressed page would
still occupy its full fixed-size slot, so page-level compression would save
zero disk space without a much larger redesign (variable-length page storage
+ a page-table indirection layer), the same reason real LMDB has never
supported it. Instead, `BTree::maybeCompress` transparently LZ4-compresses
plain (non-DUPSORT, non-DUPFIXED) values at or above `compressionMinSize`
before they're stored inline or chunked into the overflow-chain mechanism,
and decompresses transparently on read — real disk savings with no format
redesign. DUPSORT dup-values and DUPFIXED runs are never compressed (both
have structural requirements — byte-comparable sort order; uniform element
stride — compression would break). LZ4 is vendored directly
(`thirdparty/lz4/lz4.c`/`.h`, no `FetchContent`), the same pattern encryption
established with Monocypher. See `tests/test_compression.cpp`.

**Encryption** (`ANNPOCKETDB_WITH_ENCRYPTION`) is also implemented, not just
scaffolded: page-level XChaCha20-Poly1305 (data pages only — meta pages 0/1
always stay cleartext) plus an optional additive SHA-256 checksum bolt-on,
via vendored Monocypher (`thirdparty/monocypher/`, no `FetchContent`). See
`tests/test_crypto_primitives.cpp`, `test_encrypted_db.cpp`, and
`test_incremental_backup_encrypted.cpp`.

## Formerly untested modules — now covered

`index`, `blob`, `document`, `query`, and `changefeed` all have real,
substantial implementations (hundreds of lines of working logic each) wired
into `CMakeLists.txt`/`Environment`'s public API, but had zero test coverage
until `test_index.cpp`, `test_blob.cpp`, `test_document.cpp`, `test_query.cpp`,
and `test_changefeed.cpp` were added. `wal` was already covered incidentally
by `test_crash_consistency.cpp` (crash/recovery), though there's still no
test of plain, non-crash WAL commit/replay end-to-end.

Writing these tests surfaced two real, previously-invisible correctness bugs,
both now fixed:

- **`query`'s index-accelerated access paths (`IndexPoint`/`IndexPrefix`/
  `IndexRange`) were completely non-functional.** `collectIndexKeys`
  (`src/query/query.cpp`) tried to split a `[secondary][0x00][primary]`
  composite out of each index cursor's *key*, but `Index`'s cursors are
  opened without `DbFlags::DupSort` set, so `cursor.key()` already returns
  the plain secondary key alone (matching `Index::find`'s own reference
  implementation) — there is no separator to split, so every index-backed
  query failed outright with a `Corruption` status the instant it was
  actually exercised. Separately, even once reads were fixed, predicate
  bounds compared raw predicate-value bytes against the index's real
  contents, which `IndexManager::maintainOnPut` populates via
  `Value::toIndexBytes()` — a type-tagged encoding, not raw bytes — so
  comparisons silently mismatched on the leading tag byte. Both are fixed in
  `src/query/query.cpp`.
- Everything else (index catalog persistence, blob chunking/streaming/
  corruption guards, document codec round-trips, changefeed resume tokens)
  matched its documented behavior on the first pass.

- **`index`** (`src/index/index.cpp`) — secondary indexes as auxiliary
  `BTree` instances (DUPSORT-based: secondary key -> primary key as the dup
  value), with a catalog persisted in the master keyspace and
  `maintainOnPut`/`Remove` hooks to stay in sync with primary documents.
- **`blob`** (`src/blob/blob_store.cpp`) — large-object storage via
  streaming `BlobWriter`/`BlobReader` over the overflow-chain mechanism, plus
  `Environment::putBlob`/`getBlob`/`removeBlob` convenience wrappers.
- **`document`** (`src/document/document.cpp`) — hand-rolled MessagePack
  subset codec; always built (no third-party dependency, no CMake gate).
- **`query`** (`src/query/query.cpp`) — a cost-based planner over documents
  and secondary indexes (table scan vs. point/prefix/range index seek),
  projection pushdown, and optional multi-threaded residual-predicate
  evaluation. Real, but narrower than a full query language — see the
  header comment in `include/annpocketdb/query/query.hpp` for exactly which
  predicate/access-path shapes it supports before assuming more than it has.
- **`changefeed`** (`src/changefeed/changefeed.cpp`) — durable, ordered
  change log derived from committed master-keyspace writes, with
  resume-token (`pollFrom`) replay and in-process/async delivery backends.
- **Named sub-databases** (`src/api/sub_database.cpp`) — actually already
  fully implemented *and* tested (`test_subdatabase.cpp`, `test_subdb_flags.cpp`,
  `test_env_extras.cpp`) — this used to be listed here as a future extension;
  it no longer is one.

## Roadmap (genuinely still open)

- **Insert-time sibling redistribution** (B*-tree-style — try moving an
  entry to a sibling before splitting, to keep nodes above the ~50% average
  fill a split-only B-tree guarantees, closer to the ~67% a B*-tree
  guarantees). Delete-time merge/redistribute rebalancing is already
  implemented and tested (`test_btree_rebalance`); this is specifically
  about the insert path, which today only ever splits.
- **libmdbx-style automatic file shrinking** — reclaimed pages are reused
  internally (and, with a WAL attached, recovered automatically after a
  crash — see "Free-list space after crashes" below), but the physical file
  itself never shrinks. The only way to reclaim disk space today is an
  out-of-place rebuild (`backup::compactCopy`), not an automatic in-place
  page-relocation-and-truncate.

## Repository layout

```
include/annpocketdb/<module>/   public headers, mirrored under src/
src/<module>/                   implementation, one module per concern
tests/                          one focused test binary per module, wired into ctest
CMakeLists.txt                  kernel library + optional FetchContent-based feature modules
```

## Operational notes (from review)

### WAL durability
When `EnvironmentOptions::enableWal` is true (and the build enables
`ANNPOCKETDB_WITH_WAL`), commits append page images and a commit record to
the WAL **before** flipping the meta page. Recovery uses `PageManager::writePageAt`
to restore logged page ids (not `allocatePage`, which assigns fresh ids).
Call order on open: open data file → open WAL → `replayInto` → `attachWal`.

Without WAL, durability is double-buffered meta + msync only.

### Free-list space after crashes
With a WAL attached, this is already handled automatically: every retirement
is durably logged as a `FreeListUpdate` record before the commit record
(`PageManager::retirePage`/`persistFreeListAndWal`), and `Wal::recover()`
replays those records back into the retired set on reopen, regardless of
whether the meta page itself needed advancing. The very next ordinary
commit's `FreeList::reclaim()` then promotes and persists them exactly as it
would have if the crash had never happened — no manual compaction needed.
See `test_crash_consistency.cpp`'s Tier 4 for the crash-and-recover proof
(`env.stat().freePages` nonzero immediately after a crash-recovered reopen,
then dropping again as the next commit's writes actually reuse the pool).

**Without** a WAL, this genuinely cannot be automated: there is no durable
record of what a crashed transaction retired, so those pages sit unreachable
until a later `backup::compactCopy`/`repair` cycle. Plan periodic compaction
if you run without a WAL and the workload crashes often with long-lived
readers; enabling the WAL removes the need for that entirely.

### Windows map growth
POSIX reserves `maxDatabaseSize` virtual address space at open (pointer-stable
`ftruncate` growth). Windows remaps on growth under the writer lock — not the
same pointer-stability guarantee. Size `maxDatabaseSize` / `initialFileSize`
generously on Windows, or do not retain raw page pointers across writer ops.

### Feature module build gates
Compression and encryption are both fully implemented (see the dependency
table above), but each still requires its CMake option
(`ANNPOCKETDB_WITH_COMPRESSION` / `ANNPOCKETDB_WITH_ENCRYPTION`, both off by
default) — requesting either at runtime (`enableCompression`/
`enableEncryption`) against a binary built without the matching option
returns `Status::NotSupported` rather than silently ignoring the request.
MessagePack document encoding has no such gate; it has no third-party
dependency, so it's always compiled in.

## Additional confidence tests

This section used to describe planned test binaries (`test_wal_recovery`,
`test_concurrent_readers`, `test_page_size_edges`, `test_overflow_chains`,
`test_btree_rebalance_edges.md`) that were never actually built under those
names — none of them exist in `tests/`. The gaps they were meant to cover are
now addressed differently:

- WAL commit/recovery: `test_crash_consistency.cpp` (crash-and-recover, not
  a plain happy-path replay test — see its Tier 4 for free-list-specific
  recovery). A plain "commit normally with WAL on, reopen, confirm replay"
  smoke test without any crash injection is still missing.
- Concurrent readers under writer churn: `test_mvcc.cpp` (snapshot isolation
  through real COW splits) and `test_mplay_fuzzer.cpp` (real multi-process
  contention) together cover this; no dedicated 4-thread-reader test exists.
- Page-size edges and long overflow chains: not covered by a dedicated test
  today — a real, open gap, not just a renamed one.

## API reference

See [API.md](API.md) for Environment, transactions, sub-databases, indexes, documents, query, blobs, changefeed, and WAL.

## Verify, repair, dump, load

Online (in-process) and offline tooling — a differentiator vs stock LMDB:

```cpp
auto report = env.verify();          // walk every reachable page
auto dump   = env.dumpToFile("out.apdump");
// Salvage a damaged file into a fresh one:
auto repair = annpocketdb::backup::repair("broken.apdb", "fresh.apdb");
```

CLI (`apdb` when `ANNPOCKETDB_WITH_BACKUP=ON`):

```
apdb verify path/to/db
apdb dump   path/to/db out.apdump
apdb load   out.apdump path/to/newdb [--force]
apdb repair path/to/broken path/to/fresh [--force]
```

`verify()` checks CRC (when enabled), page headers, leaf/branch key order,
overflow-chain termination, free-list cycles, and tree depth. `repair()` copies
every key/value that still materializes into a new database; unreadable entries
are skipped with notes rather than aborting the salvage.

## License

MIT — see [LICENSE](LICENSE). The two vendored third-party sources under
`thirdparty/` keep their own permissive licenses in-file (both compatible
with MIT distribution): `lz4/` is BSD-2-Clause, `monocypher/` is dual
BSD-2-Clause/CC0-1.0.
