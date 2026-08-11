# AnnPocketDb — API Reference

Public surface for the kernel and feature modules. Types live under
`namespace annpocketdb` unless noted. All fallible operations return
`Status` or `Result<T>` (`util/status.hpp`).

---

## 1. Environment (entry point)

**Headers:** `api/environment.hpp`

Open a database file and obtain transactions, convenience KV helpers, and
feature handles.

```cpp
#include "annpocketdb/api/environment.hpp"

using annpocketdb::api::Environment;
using annpocketdb::api::EnvironmentOptions;

EnvironmentOptions opts;
opts.pageSize = 4096;                 // 512 … 65536
opts.maxDatabaseSize = 1ull << 40;    // virtual reservation ceiling
opts.initialFileSize = 0;             // 0 → pageSize * 16
opts.readOnly = false;
opts.multiProcess = true;          // false => no .lock file (single-process)
opts.noSync = false;               // MDB_NOSYNC
opts.noMetaSync = false;           // MDB_NOMETASYNC
opts.mapAsync = false;             // MDB_MAPASYNC
opts.maxReaders = 256;             // mdb_env_set_maxreaders (capped at 256 multi-process)
opts.maxNamedDatabases = 0;        // 0 = unlimited; else reject create beyond limit
opts.durableCrc = true;
opts.defaultSyncMode = core::SyncMode::Durable;
opts.enableWal = false;               // sibling "*.wal" + recovery on open
opts.walPath = {};                    // empty → path + ".wal"
opts.enableChangefeed = false;        // durable change log + live feed
opts.changefeedAsync = false;         // AsyncChangeFeed backend

auto envR = Environment::open("/data/app.apdb", opts);
Environment env = envR.moveValue();
```

Opening a database also opens the sidecar lock file `<path>.lock` (created if
missing). That file holds the exclusive writer lock and the shared-memory
reader-slot table used for multi-process MVCC. Do not delete it while any
process has the database open.

### Multi-process concurrency

AnnPocketDb supports **safe multi-process access** to the same file
(LMDB-style single-writer / multiple-reader):

| Mechanism | Detail |
|-----------|--------|
| Writer exclusion | `flock` (POSIX) / `LockFileEx` (Windows) on `<path>.lock` |
| Shared reader table | 256 slots in a mmap of the lock file; each slot stores `(txnId, pid)` |
| Free-list reclaim | Uses the **global** oldest reader snapshot across all processes |
| Dead-process recovery | Slots with dead PIDs are cleared on register / oldest-snapshot scan |
| Table full | `beginRead()` returns `Status::Busy` (LMDB `MDB_READERS_FULL` equivalent) |

Threads within one process are also serialized on the writer path by an
in-process mutex held together with the OS lock.

### Transactions

| Method | Description |
|--------|-------------|
| `beginRead()` | Snapshot-isolated `ReadTransaction` |
| `beginWrite()` | Single-writer `WriteTransaction` (mutex held until commit/abort) |

### Default keyspace (master root)

| Method | Description |
|--------|-------------|
| `get(key)` | Read value (own short-lived read txn) |
| `put(key, value, overwrite=true, mode=Durable)` | Write + commit |
| `remove(key, mode=Durable)` | Delete + commit |



### Cursor API

| Method | LMDB analogue |
|--------|----------------|
| `first` / `last` / `next` / `prev` | FIRST / LAST / NEXT / PREV |
| `seek` / `seekExact` | SET_RANGE / SET |
| `seekKeyValue` | GET_BOTH |
| `firstDup` / `lastDup` / `nextDup` / `prevDup` | FIRST_DUP / LAST_DUP / NEXT_DUP / PREV_DUP |
| `nextNoDup` / `prevNoDup` | NEXT_NODUP / PREV_NODUP |
| `key` / `valueView` / `value` | current record |
| `del` | cursor del (write-bound) |
| `put(key, value, flags)` | cursor put; flags: `None`, `NoOverwrite`, `AppendDup`, `Current` |

Write-bound cursors come from `WriteTransaction::openCursor()` (non-const).
After `del`/`put` the cursor is re-positioned on the affected entry (or next).

### Statistics & readers

```cpp
auto st = env.stat().moveValue();       // master tree: depth, pages, entries
auto info = env.info();                 // map size, last txn, readers, …
std::size_t cleared = env.readerCheck(); // reclaim dead-PID slots
auto readers = env.readerList();         // {slot, pid, snapshotTxnId}
```

| API | LMDB analogue |
|-----|----------------|
| `stat()` / `stat(root)` | `mdb_stat` |
| `info()` | `mdb_env_info` |
| `readerCheck()` | `mdb_reader_check` |
| `readerList()` | `mdb_reader_list` |

### Durability flags & map size

| Flag / API | LMDB | Effect |
|------------|------|--------|
| `noSync` | `MDB_NOSYNC` | Never fsync on commit |
| `noMetaSync` | `MDB_NOMETASYNC` | Skip durable fsync of the meta page |
| `mapAsync` | `MDB_MAPASYNC` | Prefer async flush (`Flush`) when Durable was requested |
| `SyncMode::Durable/Flush/NoSync` | per-commit | Fine control on `commit(mode)` |
| `env.setMapSize(n)` | `mdb_env_set_mapsize` | Grow virtual map ceiling (no active readers; Windows may need reopen) |

### Nested write transactions (LMDB-style)

```cpp
auto w = env.beginWrite().moveValue();
w.put("a", "1");
w.beginNested();
  w.put("b", "2");       // staged in child
  w.abortNested();       // discards "b"; "a" remains
w.beginNested();
  w.put("c", "3");
  w.commitNested();      // merges "c" into parent
w.commit();              // durable: a, c
```

| Method | Behavior |
|--------|----------|
| `beginNested()` | Push savepoint (master root + retirement-log mark) |
| `commitNested()` | Pop savepoint; keep current roots (merge into parent) |
| `abortNested()` | Restore roots; undo page retires since the savepoint |
| `nestingDepth()` | Current stack depth |
| Top-level `commit()` | Refuses if `nestingDepth() > 0` |
| Top-level `abort()` | Aborts all nested frames, then the parent |

Only the **top-level** commit makes changes durable. Nested commit never fsyncs
or flips meta. Auxiliary roots (`put(PageId&, ...)`) are caller-managed: capture
and restore them yourself around nested regions if needed.

### Zero-copy reads

| API | Behavior |
|-----|----------|
| `Cursor::key()` | `ByteView` into the mapped leaf (zero-copy) |
| `Cursor::valueView()` | `ByteView` into the mapped leaf when the value is **inline** |
| `Cursor::value()` | Owned `ByteBuffer` (always; materializes overflow) |
| `txn.getView(key)` | Zero-copy inline value; `InvalidArgument` if overflow-chained |
| `txn.get(key)` | Owned buffer (safe for any value size) |

`valueView` / `getView` pointers remain valid only for the lifetime of the
read (or write) transaction that observed the page. Do not use them after
`commit`/`abort` or after the transaction is destroyed.

### DUPSORT (duplicate keys)

B+tree leaves may hold multiple values per key, ordered by `(key, value)`.

| Method | Description |
|--------|-------------|
| `putDup(key, value, noDupData=false)` | Insert a (key, value) pair; other values for `key` are kept |
| `removeDup(key, value)` | Delete one exact pair |
| `remove(key)` | Delete **all** values for `key` |
| `get(key)` | First value for `key` |
| `getAll(key)` | All values for `key` (vector) |
| Cursor | Iterates each (key, value) in order |

Constraints: DUPSORT values must fit **inline** (no overflow chains) so leaf
ordering can compare without I/O. `noDupData=true` returns `KeyExists` when the
exact pair is already present (LMDB `MDB_NODUPDATA`).

Secondary indexes use DUPSORT natively: index key = field value, index value =
primary key (replacing the earlier composite-key encoding).

### DbFlags (per-tree, LMDB-style)

| Flag | Meaning |
|------|--------|
| `DupSort` | Multiple values per key, sorted by value |
| `DupFixed` | All dups for a key share the same value length (implies DupSort) |
| `ReverseKey` | Keys compared end-to-start (DNS-style) |
| `ReverseDup` | Dup values compared end-to-start (implies DupSort) |
| `IntegerKey` | Keys as native-endian u32/u64 (`MDB_INTEGERKEY`) |
| `IntegerDup` | Dup values as native-endian integers (`MDB_INTEGERDUP`) |

Set on a cursor with `cursor.setFlags(flags)` before seek/put/del so ordering
matches how the tree was written. `putDup` uses `DupSort` automatically.

### Sub-databases

| Method | Description |
|--------|-------------|
| `createSubDatabase(name, mode)` | Allocate empty leaf root, register in catalog, commit |
| `openSubDatabase(name)` | Load catalog → `SubDatabase` handle |
| `listSubDatabases()` | Vector of names |
| `dropSubDatabase(name, mode)` | Retire reachable pages + remove catalog entry |

### Blobs

| Method | Description |
|--------|-------------|
| `putBlob(key, data, mode)` | Overflow-chain write + store `BlobRef` under `key` |
| `getBlob(key)` | Resolve `BlobRef` (or return inline value) |
| `removeBlob(key, mode)` | Retire chain + delete key |
| `blobStore()` | Stateless `BlobStore` bound to this env’s `PageManager` |

### Verify / dump (online)

Requires `ANNPOCKETDB_WITH_BACKUP` (default ON). Full types in
`backup/maintenance.hpp`.

| Method | Description |
|--------|-------------|
| `verify()` | Walk reachable pages; return `backup::VerifyReport` |
| `dumpToFile(path, opts={})` | Write portable `APDUMP01` archive of the master keyspace |

See **§15 Verify, repair, dump, load** for repair, offline CLI, and report fields.

### Other handles

| Method | Description |
|--------|-------------|
| `pageManager()` | Underlying `core::PageManager` |
| `wal()` | Optional `wal::Wal*` (null if disabled) |
| `changeFeed()` | Optional `changefeed::DurableChangeFeed*` |
| `close()` | Detach WAL, close mapping |

**Move semantics:** move-constructible, **not** move-assignable (`BTree` binds a `PageManager&`).

---

## 2. Transactions

**Headers:** `txn/transaction.hpp`

### ReadTransaction

```cpp
auto rt = env.beginRead().moveValue();
auto v  = rt.get(key);           // Result<ByteBuffer>
auto ok = rt.contains(key);      // Result<bool>
auto c  = rt.openCursor();       // btree::Cursor on snapshot root
TxnId id = rt.txnId();
```

- Bound to a fixed root at construction (master or sub-database).
- Registers with the reader table so free-list reclaim cannot steal its pages.

### WriteTransaction

```cpp
auto wt = env.beginWrite().moveValue();

// Master keyspace
wt.put(key, value, /*overwrite=*/true);
wt.remove(key);
wt.get(key);                     // read-your-own-writes
wt.openCursor();

// Explicit root (sub-dbs / indexes) — updates rootPageId in place
PageId root = …;
wt.put(root, key, value);
wt.remove(root, key);
wt.get(root, key);
wt.openCursor(root);

wt.commit(core::SyncMode::Durable);  // or Flush / NoSync
// wt.abort();  // or let destructor abort
```

| SyncMode | Meaning |
|----------|---------|
| `Durable` | fsync data + meta (and WAL if attached) |
| `Flush` | flush buffers, no full fsync guarantee |
| `NoSync` | no forced durability |

**Changefeed:** if the environment enabled it, `beginWrite` attaches a
`ChangeSink`. Master-keyspace put/remove record events; `commit` flushes the
durable log **before** the meta flip and notifies subscribers after success.
Keys under `__sys_*` are never emitted.

**Cursor lifetime:** a cursor from a write txn must not outlive a moved or
destroyed transaction.

---

## 3. Sub-databases

**Headers:** `api/sub_database.hpp`

Named independent B+trees. Catalog key: `__sys_subdb_catalog__` in the master
keyspace. Only the master root is published in the meta page.

### Lifecycle (via Environment)

```cpp
auto users = env.createSubDatabase("users").moveValue();
auto h     = env.openSubDatabase("users").moveValue();
auto names = env.listSubDatabases().moveValue();
env.dropSubDatabase("users");
```

### Single-op helpers

```cpp
users.put(key, value);
users.get(key);
users.remove(key);
```

Each helper opens a write txn, mutates the sub-root, upserts the catalog, and
commits once.

### Multi-op writer (preferred)

```cpp
auto w = users.beginWriteTxn().moveValue();
w.put("u1", "Ada");
w.put("u2", "Grace");
w.remove("u0");
w.get("u1");
w.openCursor();
w.commit();    // catalog upsert + meta commit
// destructor aborts if neither commit nor abort was called
```

| Method | Description |
|--------|-------------|
| `beginRead()` | Read txn bound to this sub-root |
| `beginWriteTxn()` | `SubDatabaseWriter` |
| `beginWrite()` | Raw env write txn (easy to misuse — prefer `beginWriteTxn`) |
| `name()` / `rootPageId()` | Identity |

### Catalog helpers

```cpp
loadSubDbCatalog(txn, entries);
saveSubDbCatalog(txn, entries);
upsertSubDbCatalogEntry(txn, name, rootPageId);
removeSubDbCatalogEntry(txn, name);
```

---

## 4. Secondary indexes

**Headers:** `index/index.hpp`

Composite keys: `[secondary_key][0x00][primary_key]`. Catalog key:
`__sys_idx_catalog__` (v2: name → `{ fieldPath, rootPageId }`).

```cpp
#include "annpocketdb/index/index.hpp"

index::IndexManager indexes(tree, pm);
indexes.loadCatalog(readTxn);

// Create (field path used by document maintenance)
indexes.createIndex("by_email", "user.email", writeTxn);
indexes.saveCatalog(writeTxn);
writeTxn.commit();
```
```cpp
// Projection pushdown
q.project = {"user.name", "age"};
// QueryResult::projected filled; doc.raw left empty
// Parallel residual evaluation
q.parallelWorkers = 4;  // thread pool over candidate primary keys
// Index maintenance (dependency)
// Index paths only help if secondaries are updated on write:
indexes.maintainOnPut(wtxn, pk, oldDoc, newDoc, *codec);
indexes.saveCatalog(wtxn);
```
### Mutations (catalog roots stay in sync)

```cpp
indexes.insertEntry(wtxn, "by_email", secKey, primaryKey);
indexes.removeEntry(wtxn, "by_email", secKey, primaryKey);
indexes.saveCatalog(wtxn);   // always before commit after catalog/root changes
```

Uses `WriteTransaction::put/remove(PageId&)`.

### Automatic maintenance (with documents)

```cpp
indexes.maintainOnPut(wtxn, primaryKey, oldDoc, newDoc, codec);
indexes.maintainOnRemove(wtxn, primaryKey, oldDoc, codec);
indexes.saveCatalog(wtxn);
wtxn.commit();
```

Extracts each index’s `fieldPath` via `IDocumentCodec::extractIndexKey`.

### Reads

```cpp
auto idx = indexes.openIndex("by_email").moveValue();
auto cur = idx.find(secKey);   // cursor at first matching composite key

// Iteration pattern:
for (auto& c = cur.value(); c.valid(); ) {
    auto k = c.key();
    ByteView sec, pri;
    index::splitIndexKey(k.value(), sec, pri);
    if (compareBytes(sec, secKey) != 0) break;
    // use pri …
    auto n = c.next();
    if (!n || !n.value()) break;
}
```

| Method | Description |
|--------|-------------|
| `createIndex(name, fieldPath, wtxn)` | Empty leaf root + catalog entry |
| `dropIndex(name, wtxn)` | Full reachable-page walk + retire |
| `openIndex` / `hasIndex` / `listIndexes` / `meta` | Lookup |
| `collectReachablePages(pm, root, out)` | Shared tree walk helper |

Helpers: `makeIndexKey`, `splitIndexKey`.

---

## 5. Documents

**Headers:** `document/document.hpp`

Typed values + codecs for structured documents stored as KV values.

### Value model

```cpp
using document::Value;

Value::null();
Value::boolean(true);
Value::integer(-1);
Value::uinteger(42);
Value::floating(1.5);
Value::string("ada");
Value::bytes(buf);
Value::array({…});
Value::map({{"name", Value::string("ada")}});
```

### Paths

```cpp
auto path = document::parseFieldPath("user.address.city");  // or "tags.0"
Value out;
document::getByPath(root, path, out);
```

### Codecs

| Codec | Notes |
|-------|--------|
| `MsgPackCodec` | Default; self-contained MessagePack |
| `PlainTextCodec` | `key=value\n` lines (tests / migration) |

```cpp
auto codec = document::createDocumentCodec().moveValue();  // MsgPack

Document doc = document::encodeDocument(*codec, value).moveValue();
Value v = document::decodeDocument(*codec, doc).moveValue();

std::string field;
codec->extractField(doc.raw(), path, field);
codec->extractValue(doc.raw(), path, value);
codec->extractIndexKey(doc.raw(), path, indexBytes);

document::collectIndexEntries(*codec, doc.raw(), paths, entries);
```

### CRUD helpers

```cpp
document::putDocument(wtxn, primaryKey, doc);
document::getDocument(rtxn, primaryKey);
document::removeDocument(wtxn, primaryKey);
```

### DocumentStore (auto index maintenance)

```cpp
auto codec = document::createDocumentCodec().moveValue();
index::IndexManager indexes(tree, pm);
indexes.loadCatalog(rtxn);

document::DocumentStore docs(*codec, &indexes);
// or: env.documentStore(*codec, &indexes);

auto wtxn = env.beginWrite().moveValue();
docs.put(wtxn, pk, valueTree);   // encodes + maintainOnPut + saveCatalog
docs.remove(wtxn, pk);          // maintainOnRemove + saveCatalog
wtxn.commit();

// Multikey: path ending at an array indexes each element.
indexes.createIndex("by_tag", "tags", wtxn);
```
```cpp
auto codec = document::createDocumentCodec().moveValue();
index::IndexManager indexes(tree, pm);
indexes.loadCatalog(rtxn);

document::DocumentStore docs(*codec, &indexes);
// or env.documentStore(*codec, &indexes);

auto w = env.beginWrite().moveValue();
docs.put(w, pk, valueTree);   // encode + put + maintainOnPut + saveCatalog
docs.remove(w, pk);           // maintainOnRemove + saveCatalog
w.commit();
```
```cpp
//environment
env.documentStore(codec, &indexes);

docs.put(wtxn, id, Value::map({
  {"tags", Value::array({Value::string("a"), Value::string("b")})},
  {"age", Value::integer(30)},
}));
// indexes "tags" → two secondary keys; "age" → one

query::Query q;
q.predicates.push_back({"age", query::Op::Ge, "18"});
eng->execute(rtxn, q);  // can use IndexRange when by_age is maintained
```
---

## 6. Query engine

**Headers:** `query/query.hpp`

```cpp
#include "annpocketdb/query/query.hpp"

auto eng = query::createQueryEngine(*codec, &indexes).moveValue();
// or: createQueryEngine() → PlainTextCodec, no indexes

query::Query q;
q.predicates.push_back({"age", query::Op::Ge, "18"});
q.predicates.push_back({"age", query::Op::Lt, "65"});
q.predicates.push_back({"status", query::Op::Eq, "active"});  // residual
q.project = {"user.name", "age"};   // projection pushdown
q.parallelWorkers = 4;              // parallel residual checks
q.limit = 50;

auto plan = eng->explain(q);  // path, index, bounds, cost
// plan.path: TableScan | IndexPoint | IndexPrefix | IndexRange
auto rows = eng->execute(rtxn, q);
auto n    = eng->countEstimate(rtxn, q);
```

### Operators

`Eq`, `Ne`, `Lt`, `Le`, `Gt`, `Ge`, `Prefix`, `Exists`

### Cost-based planning

For each index whose `fieldPath` (or name) matches predicate fields, build a
candidate and score it:

| Path | Typical cost |
|------|----------------|
| `IndexPoint` (Eq) | 10 |
| `IndexPrefix` | 100 |
| `IndexRange` closed (lower+upper) | 500 |
| `IndexRange` open | 2000 |
| `TableScan` | 100000 |

Lowest cost wins. Range bounds on the same field combine (e.g. `Ge` + `Lt`).

Index paths require indexes to be **maintained on write**
(`IndexManager::maintainOnPut` / `maintainOnRemove`). Without that, use table
scan (still correct, slower).

### Projection

When `Query::project` is non-empty, `QueryResult::projected` is filled and
`doc` raw body is left empty.

### Parallelism

`parallelWorkers > 1` evaluates residual predicates over candidate keys on a
thread pool (safe for concurrent `ReadTransaction::get`).

---

## 7. Blobs

**Headers:** `blob/blob_store.hpp`

Overflow-page chains for large objects. Head payload:
`[magic 'BLOB'][u64 totalLen][data…]`.

### Streaming

```cpp
blob::BlobStore store = env.blobStore();

auto wt = env.beginWrite().moveValue();
auto writer = store.openWriter(wt.txnId()).moveValue();
writer.write(chunk1);
writer.write(chunk2);
BlobId id = writer.finalize().moveValue();

// Or one-shot:
blob::BlobRef ref = store.put(data, wt.txnId()).moveValue();
wt.commit();

auto reader = store.openReader(ref.id).moveValue();
ByteBuffer buf(4096);
reader.read(buf);
reader.seek(offset);
store.get(ref.id);                 // full materialization
store.remove(ref.id, txnId);       // retire entire chain
```

### KV BlobRef

Wire value: `"BR1\0" + pageId(u64) + totalLen(u64)`.

```cpp
env.putBlob(key, bytes);
env.getBlob(key);
env.removeBlob(key);

blob::isBlobRef(value);
blob::encodeBlobRef(ref);
blob::decodeBlobRef(value, ref);
```

If `finalize()` is never called, `BlobWriter`’s destructor retires orphan pages.

---

## 8. Changefeed

**Headers:** `changefeed/changefeed.hpp`

Durable ordered log in the master keyspace:

- Events: `__sys_cf_log__/<16-hex-seq>`
- Meta: `__sys_cf_meta__` (last sequence)

```cpp
opts.enableChangefeed = true;
opts.changefeedAsync = false;   // true → background delivery thread
auto env = Environment::open(path, opts).moveValue();

// Live (in-process)
env.changeFeed()->subscribe([](const changefeed::ChangeEvent& e) {
    // e.sequence, e.type (Insert/Update/Delete), e.txnId, e.key, e.oldValue, e.newValue
});

// After commits, resume / catch-up (any process)
auto rtxn = env.beginRead().moveValue();
ResumeToken token = 0;
auto batch = env.changeFeed()->pollFrom(rtxn, token, /*limit=*/100);
token = batch.value().back().sequence;

auto latest = env.changeFeed()->latestSequence(rtxn);
```

Factories:

```cpp
createChangeFeed(async);           // delivery only
createDurableChangeFeed(async);    // log + delivery
```

---

## 9. WAL

**Headers:** `wal/wal.hpp`, `wal/record.hpp`

Enabled with `opts.enableWal = true`. WAL path is a **directory**
(`data.apdb.wal/` by default) holding numbered segment files
`00000001.seg`, `00000002.seg`, …

**Protocol:** page images logged on finalize → `Commit` + fsync → meta flip →
`checkpoint` deletes segments fully covered by `upToTxnId` (keeps at least
`minSegmentsToKeep`). Segments rotate when they reach `logFileMaxSize`.

**Recovery** streams every segment in order (chunked reads); incomplete
trailing records without a `Commit` are discarded. After installing a recovered
meta, Environment calls `truncateAfterRecovery()` to start a fresh segment set.

```cpp
// Open path (Environment does this):
wal.recover(pm);                   // streaming multi-segment redo
wal.truncateAfterRecovery();       // after successful install
pm.attachWal(&wal);

wal.logPageImage(pageId, header, payload);
wal.commit(txnId, masterRoot, freeListRoot, walSeq, SyncMode::Durable);
wal.checkpoint(upToTxnId, mode); // multi-segment GC, not truncate-to-zero
wal.flush(sync);
wal.segmentCount();
wal.logFileSize();                 // sum of segment sizes
```

`RecoveryResult`: last commit roots, `appliedCommit`, `validLogBytes`,
`segmentsScanned`.

POSIX and Windows use the same logic; Windows I/O is via `CreateFileW` /
`WriteFile` / `ReadFile(OVERLAPPED)` / `FlushFileBuffers` / `DeleteFileW`.

---

## 10. Cursor

**Headers:** `btree/cursor.hpp`

```cpp
auto c = txn.openCursor();
c.first();
c.last();
c.seek(key);          // ≥ key
c.seekExact(key);
c.next();
c.prev();
c.valid();
c.key();              // Result<ByteView>
c.value();            // Result<ByteBuffer> (materializes overflow)
```

No leaf sibling pointers — navigation uses a stack of branch frames.

---

## 11. Status / Result

**Headers:** `util/status.hpp`, `util/types.hpp`

```cpp
Status s = …;
if (!s.ok()) { s.code(); s.toString(); }

Result<T> r = …;
if (r.ok()) T v = r.moveValue();
else        Status err = r.status();
```

Common codes: `OK`, `NotFound`, `KeyExists`, `Corruption`, `IOError`,
`InvalidArgument`, `TxnConflict`, `ReadOnly`, `MapFull`, `Incompatible`,
`Busy`, `NotSupported`, `Internal`.

Byte helpers: `ByteView`, `ByteBuffer`, `toByteView`, `toByteBuffer`,
`toStringView`, `compareBytes`.

---

## 12. Typical recipes

### Indexed document write

```cpp
auto codec = document::createDocumentCodec().moveValue();
index::IndexManager indexes(*env tree access via pageManager/tree…);
// indexes.loadCatalog on open

auto wtxn = env.beginWrite().moveValue();
ByteBuffer old;
if (auto g = wtxn.get(pk); g.ok()) old = g.moveValue();

Document doc = encodeDocument(*codec, valueTree).moveValue();
wtxn.put(pk, doc.raw());
indexes.maintainOnPut(wtxn, pk, view(old), doc.raw(), *codec);
indexes.saveCatalog(wtxn);
wtxn.commit();
```

### Query with index

```cpp
auto eng = query::createQueryEngine(*codec, &indexes).moveValue();
query::Query q;
q.predicates.push_back({"user.email", query::Op::Eq, "ada@ex.com"});
auto rows = eng->execute(rtxn, q);
```

### Sub-database batch

```cpp
auto sub = env.openSubDatabase("orders").moveValue();
auto w = sub.beginWriteTxn().moveValue();
w.put(k1, v1);
w.put(k2, v2);
w.commit();
```

### Large object

```cpp
env.putBlob("video", bytes);
auto data = env.getBlob("video").moveValue();
env.removeBlob("video");
```

### Verify and dump

```cpp
auto v = env.verify().moveValue();
if (!v.ok()) { /* inspect v.issues */ }

env.dumpToFile("backup.apdump");

// Salvage a damaged file (offline / other process):
annpocketdb::backup::RepairOptions ro;
ro.forceOverwrite = true;
auto r = annpocketdb::backup::repair("broken.apdb", "healed.apdb", ro).moveValue();
```

### Changefeed consumer

```cpp
ResumeToken tok = loadCheckpoint();  // your durable cursor
for (;;) {
    auto r = env.beginRead().moveValue();
    auto batch = env.changeFeed()->pollFrom(r, tok, 1000).moveValue();
    if (batch.empty()) { sleep…; continue; }
    for (auto& e : batch) apply(e);
    tok = batch.back().sequence;
    saveCheckpoint(tok);
}
```

---

## 13. Verify, repair, dump, load

**Headers:** `backup/maintenance.hpp`  
**CLI:** `apdb` (built when `ANNPOCKETDB_WITH_BACKUP=ON`)

Online, in-process maintenance — a differentiator vs stock LMDB/libmdbx, which
only ship offline dump/load tools.

### `verify` — live structural check

```cpp
#include "annpocketdb/backup/maintenance.hpp"

auto report = env.verify().moveValue();
// or offline without holding an Environment:
auto report = annpocketdb::backup::verifyFile("/data/app.apdb").moveValue();

if (!report.ok()) {
    for (const auto& issue : report.issues) {
        // issue.pageId, issue.message
    }
}
```

`VerifyReport` fields:

| Field | Meaning |
|-------|---------|
| `pagesScanned` | Pages visited (meta + tree + free-list + overflow) |
| `leafEntries` / `branchEntries` | Entry counts observed |
| `overflowPages` / `freeListPages` | Chain pages walked |
| `treeDepthMax` | Deepest path from root |
| `issues` | `VerifyIssue{ pageId, message }` list; empty ⇒ `ok()` |

Checks include: page-id match, `payloadSize` bounds (and CRC via `getPage`),
leaf/branch key order, empty branch detection, child page-id range, overflow
and free-list cycle/termination bounds (`BoundedWalk`), and max tree depth.

### `repair` — salvage into a fresh file

```cpp
annpocketdb::backup::RepairOptions ropts;
ropts.forceOverwrite = true;
ropts.copyLayout = true;   // inherit pageSize / maxDatabaseSize

auto rep = annpocketdb::backup::repair(
    "/data/broken.apdb", "/data/fresh.apdb", ropts).moveValue();
// rep.salvaged, rep.skipped, rep.notes, rep.sourceVerify
```

Opens the source read-only, creates a new database at the destination, and
copies every key/value that still materializes. Unreadable entries are skipped
with notes rather than aborting the whole salvage.

### Offline dump / load

Portable little-endian archive (`magic = "APDUMP01"`, version 1):

```
keyLen u32 | key bytes | valueLen u32 | value bytes | … | keyLen 0
```

```cpp
// Dump master keyspace
annpocketdb::backup::dumpToFile(env, "out.apdump");
annpocketdb::backup::dumpFile("/data/app.apdb", "out.apdump");

// Load into a new (or --force) database
annpocketdb::backup::LoadOptions lopts;
lopts.forceOverwrite = true;
annpocketdb::backup::loadFromFile("out.apdump", "/data/restored.apdb", lopts);
```

### CLI

```
apdb verify <db-path>
apdb dump   <db-path> <out.apdump>
apdb load   <dump.apdump> <db-path> [--force]
apdb repair <src-db> <dst-db> [--force]
```

Exit code `0` on success; `verify` exits `1` when issues are found.

---

## 14. System keys (reserved)

| Key / prefix | Purpose |
|--------------|---------|
| `__sys_subdb_catalog__` | Sub-database name → root |
| `__sys_idx_catalog__` | Index name → field path + root |
| `__sys_cf_log__/*` | Changefeed event log |
| `__sys_cf_meta__` | Changefeed last sequence |

Do not use `__sys_` prefixes for application data. Changefeed ignores them.

---

## 15. Header map

| Module | Header |
|--------|--------|
| Environment | `api/environment.hpp` |
| Sub-databases | `api/sub_database.hpp` |
| Transactions | `txn/transaction.hpp` |
| Indexes | `index/index.hpp` |
| Documents | `document/document.hpp` |
| Query | `query/query.hpp` |
| Blob | `blob/blob_store.hpp` |
| Changefeed | `changefeed/changefeed.hpp` |
| WAL | `wal/wal.hpp` |
| Verify / repair / dump | `backup/maintenance.hpp` |
| Multi-process lock | `platform/interprocess_lock.hpp` |
| Cursor | `btree/cursor.hpp` |
| Status / bytes | `util/status.hpp`, `util/types.hpp` |

Kernel internals (`core/*`, `btree/btree.hpp`, `platform/*`) are available for
advanced tools but are not required for normal application use.


