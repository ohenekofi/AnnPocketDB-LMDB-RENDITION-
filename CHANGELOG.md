# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

### Fixed

#### Fix: non-ASCII data paths on Windows (sidecar files, errors, C API)

Several places turned the data path into a narrow `std::string` with
`path.string()` and built new paths from it. On Windows that conversion goes
through the ANSI code page, so for a data folder such as
`C:\Users\Kwabena Ọbẹ\AppData\...` the `.wal`, `.lock`, `.sha256` and `.enc`
sidecars pointed at a different (mangled) file, or the conversion threw
`std::system_error` out of an API that returns `Status`. Linux and macOS were
not affected (`string()` is UTF-8 there), and behaviour there is unchanged.

- Every path now stays a `std::filesystem::path` down to the OS call. Sidecar
  paths are derived with the new `util::withSuffix()` (`path +=`), never via
  `std::string` (`environment.cpp`, `interprocess_lock.cpp`,
  `checksum_sidecar.cpp`, `encryption_sidecar.cpp`, and the `forceOverwrite`
  `.lock` cleanup in `loadFromFile` / `repair`).
- `Status` messages that include a path use `util::pathToUtf8()`: always
  UTF-8 and never throws (`backup.cpp`, `mapped_file.cpp`).
- The WAL segment scan no longer throws on a non-ASCII file in the WAL
  directory; such files are skipped.
- C API: `apdb_env_open`'s `path` is now defined as **UTF-8 on every
  platform** and converted explicitly (it was read as ANSI on Windows).
  Ill-formed UTF-8 returns `APDB_INVALID_ARG`. The Python, Dart and Go
  bindings already pass UTF-8, so they now work with Unicode paths on Windows
  too.
- `apdb` CLI: on Windows, arguments are read as UTF-16
  (`GetCommandLineW` + `CommandLineToArgvW`), and the console output code page
  is set to UTF-8 while the tool runs.
- On-disk formats and file names are unchanged.

New tests: `test_unicode_paths` (sidecars, backup/dump/load/repair/compact
copy, error messages, WAL directory scan) and `capi_unicode`.

#### Fix: WAL replay cut multi-page overflow chains (blobs, large values, free list)

The WAL `PageImage` record stored a page's type, payload size, item count and
payload, but not `PageHeader::overflowNext` (or `flags`). Replay rebuilds each
logged page from a zeroed header, so every logged overflow page came back as
end-of-chain. Since recovery replays up to the last commit on *every*
WAL-enabled open, not just after a crash, a clean close + reopen truncated any
B-tree value or blob spanning more than one page (and multi-page free-list
chains) to its first page on disk. Values that fit in one page were
unaffected, which is why the existing WAL tests (7–9 byte values) never caught
it; `test_unicode_paths` (WAL + a 20 KB blob) did.

- New record type `PageImageV2` (= 5) carries `flags` and `overflowNext`;
  `logPageImage` writes only V2, and replay restores both fields.
- Legacy `PageImage` (= 1) records are still decoded, so a WAL left behind by
  an older build is replayed exactly as before. The data-file format is
  unchanged.
- **Downgrade note:** an older build reading a WAL written by this one skips
  the unknown V2 records instead of replaying them. Close cleanly (so the data
  file is current) before downgrading.
- `test_wal_replay` case 6: multi-page B-tree values and a multi-page blob
  survive two clean reopens.

## [0.1.1] - 2026-09-03

### Fixed

#### Circular `annpocketdb_kernel` / `annpocketdb_features` static-library dependency

`annpocketdb_features` `PUBLIC`-links `annpocketdb_kernel`, but several
kernel-tier `.cpp` files held direct (non-virtual) symbol references into
features-tier `.cpp` files. GNU ld's single left-to-right archive pass can't
satisfy that in either direction, so any plain (ungrouped)
`target_link_libraries(... AnnPocketDb::features AnnPocketDb::kernel)` link
failed — including the project's own `apdb` tool.

Four call sites were found and fixed, each with the treatment that fit its
call site:

- **`src/api/environment.cpp` (`Environment::open`)** called
  `changefeed::createDurableChangeFeed`. Fixed by moving changefeed's
  delivery backends, `DurableChangeFeed`, and factory functions into the
  kernel tier (still gated behind `ANNPOCKETDB_WITH_CHANGEFEED`) — the
  module is self-contained, so this created no new cascade.
- **`src/api/environment.cpp` (5 sub-database methods)** called
  `blob::BlobStore`/`isBlobRef`/`decodeBlobRef`/`encodeBlobRef` and
  constructed `SubDatabaseManager` directly. Fixed by extracting
  `putBlob`/`getBlob`/`removeBlob` into a new `src/api/environment_blob.cpp`
  and the five sub-database methods into a new
  `src/api/environment_subdatabase.cpp`, both compiled into the features
  tier. `Environment::subDbManager_` construction was made lazy
  (`Environment::ensureSubDbManager()`, constructed on first actual
  sub-database use) instead of happening unconditionally in `open()`, since
  `SubDatabaseManager`'s constructor does no I/O or fallible work — this is
  fully behavior-preserving. Sub-database couldn't move wholesale into the
  kernel tier like changefeed/WAL because `sub_database.cpp` itself depends
  on `index::` (which depends on `document::`) — moving it would have
  cascaded two more feature modules into the kernel for no benefit.
- **`src/txn/transaction.cpp`** called
  `changefeed::ChangeSink::record/flush/notifyCommitted` directly.
  `ChangeSink` is a concrete class (not an abstract interface), so this was
  a genuine instance of the same bug. Fixed by splitting `ChangeSink` and
  its encoding helpers out of `changefeed.cpp` into a new, always-compiled
  `src/changefeed/change_sink.cpp` added unconditionally to
  `KERNEL_SOURCES`.
- **`src/core/page_manager.cpp` (`PageManager::commit`)** called
  `wal::Wal::logPageImage/healthy/logFreeListUpdate/commit/checkpoint`
  directly, on the core commit path. Fixed by moving `src/wal/record.cpp`
  and `src/wal/wal.cpp` from the features tier into `KERNEL_SOURCES`, still
  gated behind `ANNPOCKETDB_WITH_WAL`.

No other kernel-tier source file references a features-tier symbol
(confirmed via a full sweep of every `KERNEL_SOURCES` file for
feature-namespace usage and feature-header includes).

**Verified:** `apdb` and every test target link cleanly with a plain,
ungrouped `target_link_libraries` — no `--start-group`/`--end-group` needed
anywhere in the tree.

#### Qt keyword collisions (`slots`, `emit`)

`slots` and `emit` are Qt macros (substituted via `<QObject>` unless
`QT_NO_KEYWORDS` is defined), but were used as ordinary C++ identifiers,
silently corrupting these declarations for any Qt-based consumer:

- `SharedReaderSlot* slots() const` → `readerSlots()` —
  `include/annpocketdb/platform/interprocess_lock.hpp` and all 9 call sites
  in `src/platform/interprocess_lock.cpp`.
- `IChangeFeed::emit` / `InProcessChangeFeed::emit` / `AsyncChangeFeed::emit`
  → `publish` — `include/annpocketdb/changefeed/changefeed.hpp` and
  `src/changefeed/changefeed.cpp`.

A full-tree grep (`include/`, `src/`, `tests/`, `tools/`, `bindings/`,
`*.md`) confirmed these were the only identifier usages of either word, and
that `signals` (the third Qt keyword) does not appear as an identifier
anywhere.

#### `ANNPOCKETDB_WITH_BLOB` / `_CHANGEFEED` / `_SUBDATABASE` = `OFF` didn't compile

The structural fix above left `environment_blob.cpp`,
`environment_subdatabase.cpp`, and `environment.cpp`'s changefeed setup
referencing their feature symbols unconditionally, so disabling any of
these three (independently on-by-default) CMake options broke the build.
Each method now branches internally, matching the pre-existing
`ANNPOCKETDB_WITH_WAL` pattern in `Environment::open()`:

```cpp
#if defined(ANNPOCKETDB_WITH_X)
    // real implementation
#else
    return Status::NotSupported("... support was not built (ANNPOCKETDB_WITH_X)");
#endif
```

`environment_blob.cpp` and `environment_subdatabase.cpp` are now compiled
unconditionally into the features tier (only `blob_store.cpp` /
`sub_database.cpp` themselves stay gated).

#### `ANNPOCKETDB_WITH_ENCRYPTION=OFF` (the project default) didn't compile

Found during build verification of the fixes above, not part of the
original report. `PageManager::open()` / `flushAll()` / `close()`
(`src/core/page_manager.cpp`) called `EncryptionSidecar`/`ChecksumSidecar`
`.open/.flush/.close` unconditionally — only runtime-guarded by
`encryptionEnabled_`/`sha256ChecksumEnabled_`, never preprocessor-guarded —
even though those sidecar classes' definitions only compile in under
`ANNPOCKETDB_WITH_ENCRYPTION` (`OFF` by default). Since those bools can
only be `true` when the macro is defined (enforced earlier in `open()`),
wrapping all three call sites in `#if defined(ANNPOCKETDB_WITH_ENCRYPTION)`
is fully behavior-preserving.

### Verification

Full clean build and test run on the default configuration (all
`ANNPOCKETDB_WITH_*` options at their defaults, `ANNPOCKETDB_WITH_ENCRYPTION`
and `ANNPOCKETDB_WITH_COMPRESSION` off):

```
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build -C Debug --output-on-failure
```

- `apdb.exe` and every test target link with zero unresolved externals.
- **31/31 tests passed** (100%), including `test_blob`, `test_changefeed`,
  `test_subdatabase`, `test_subdb_flags`, `test_env_extras`,
  `test_wal_replay`, `test_crash_consistency`, and `capi_smoke`. Total test
  time: 415.07s real.

## [0.1.0] - Initial commit

Initial import.
