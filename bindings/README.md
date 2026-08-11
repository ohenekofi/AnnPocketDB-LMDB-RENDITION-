# AnnPocketDb Language Bindings

This directory contains the official language bindings for AnnPocketDb.

## Architecture: The Stable C API Layer

To support multiple languages efficiently and reliably, AnnPocketDb avoids binding directly to its C++ kernel. Instead, it exposes a single, stable C API (`libannpocketdb_c`). All official language bindings (Go, Python, Dart, Node.js, Java) are thin wrappers over this shared C layer.

### The FFI Contract
- **Opaque Handles:** All internal resources (Environments, Transactions, Cursors) are represented as opaque pointers.
- **Error Handling:** Functions return an `APDB_rc` integer code (`0` indicates success). Bindings evaluate this code and throw native exceptions/errors based on the result.
- **Buffer Lifetimes (Zero-Copy Reads):** Pointers returned by read operations (`apdb_get`, `apdb_cursor_value`, etc.) point directly to memory-mapped scratch areas. These pointers are only valid until the next operation on the same handle. **Bindings perform an eager copy** into their native heap (e.g., `ctypes.string_at` in Python, `C.GoBytes` in Go) immediately after retrieving the value to guarantee memory safety.

## Supported Languages

| Language | FFI Mechanism | Location |
|---|---|---|
| **Go** | `cgo` | `bindings/go` |
| **Python** | `ctypes` | `bindings/python` |
| **Dart** | `dart:ffi` | `bindings/dart` |
| **Node.js** | `N-API` (C++) | `bindings/nodejs` |
| **Java** | `JNI` | `bindings/java` |

## Exposed Features

The C API and all associated language bindings expose the full range of both core and advanced database functionalities:

### Core CRUD & ACID Transactions
- **Environments (`Env`):** Open, Close, Sync, Stats.
- **Transactions (`ReadTxn`, `WriteTxn`):** Full snapshot isolation for reads and serialized writes.
- **Key-Value Operations:** `put`, `get`, `delete`.
- **Cursors (`Cursor`):** Forward and backward traversal, exact seeking, range scans, and in-place mutations.

### Advanced Features
- **Blob Storage (Large Objects)**
  - Out-of-band storage for values that exceed standard B+Tree page size limits.
  - Methods: `Env.put_blob()`, `Env.get_blob()`, `Env.delete_blob()`
- **Two-Phase Commit (2PC)**
  - Allows distributed coordination by durably persisting changes to the WAL without publishing them to readers until commit.
  - Methods: `WriteTxn.prepare()`, `Env.rollback_prepared()`
- **Changefeed (Polling)**
  - A durable append-only log of modifications across the database. Used to replicate data, build indexes, or trigger external events. 
  - Exposed as an iterator (`ChangeFeedIterator`) yielding `ChangeEvent` records detailing the sequence, txn ID, and the old/new values.
  - Methods: `Env.open_changefeed(readTxn, afterToken)`
