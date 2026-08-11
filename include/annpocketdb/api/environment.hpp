#pragma once
//
// AnnPocketDb :: api/environment.hpp
//
// Environment is the top-level handle applications open: it owns the
// memory-mapped data file (via core::PageManager) and the single default
// keyspace's B+tree, and issues ReadTransaction / WriteTransaction handles.
//
// Scope note: the default keyspace is fully implemented and tested. Named
// sub-databases use a catalog in the master keyspace (see sub_database.hpp)
// and multi-root WriteTransaction overloads.
//
// Move semantics: Environment is move-constructible but NOT move-assignable.
// BTree holds a PageManager& bound at construction; rebinding that reference
// on move-assignment would be unsafe, so operator=(Environment&&) is deleted.
//
// WAL: when EnvironmentOptions::enableWal is true, open() also opens a
// sibling "*.wal" file, runs recovery (replayInto) *before* attaching the WAL
// to PageManager, then attaches it so subsequent commits are group-logged.
//
// fork() (POSIX): never share an open Environment across a fork() the way
// you must never share an open LMDB handle across one -- the memory
// mapping, the .lock file's shared-memory reader-table slot(s), and every
// open file descriptor are only valid in the process that created them; a
// child inheriting them can corrupt the database or crash. Close the
// Environment before forking and open a fresh one in the child, or (safer
// and simpler, and the pattern this project's own mplay fuzzer test uses)
// spawn the child via fork()+exec() / CreateProcess so it never inherits a
// live handle at all.
//
#include <array>
#include <filesystem>
#include <memory>

#include "annpocketdb/api/stats.hpp"
#include "annpocketdb/api/sub_database.hpp"
#include "annpocketdb/backup/maintenance.hpp"
#include "annpocketdb/blob/blob_store.hpp"
#include "annpocketdb/btree/btree.hpp"
#include "annpocketdb/changefeed/changefeed.hpp"
#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/document/document.hpp"
#include "annpocketdb/index/index.hpp"
#include "annpocketdb/txn/transaction.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"
#include "annpocketdb/wal/wal.hpp"

namespace annpocketdb::api {

struct EnvironmentOptions {
    std::uint32_t pageSize = core::kDefaultPageSize;
    std::uint64_t maxDatabaseSize = 1ull << 40;  // virtual address-space reservation ceiling
    std::uint64_t initialFileSize = 0;           // 0 => pageSize * 16
    bool readOnly = false;
    // When false, no .lock file is created — safe only for single-process use.
    bool multiProcess = true;
    // LMDB-style durability knobs (also in core::DurabilityFlags).
    bool noSync = false;       // MDB_NOSYNC
    bool noMetaSync = false;   // MDB_NOMETASYNC
    bool mapAsync = false;     // MDB_MAPASYNC
    // Configurable limits (LMDB mdb_env_set_maxreaders / maxdbs).
    // Multi-process readers are capped at min(maxReaders, 256) shared slots.
    std::uint32_t maxReaders = 256;
    // 0 = unlimited named sub-databases.
    std::uint32_t maxNamedDatabases = 0;
    bool durableCrc = true;
    core::SyncMode defaultSyncMode = core::SyncMode::Durable;
    // When true, open a write-ahead log beside the data file and attach it to
    // PageManager so commits are logged before the meta-page flip.
    bool enableWal = false;
    std::filesystem::path walPath;  // empty => data path + ".wal" (directory of segments)
    // When true, Environment owns a DurableChangeFeed; beginWrite attaches
    // its sink so master-keyspace put/remove are logged and emitted on commit.
    bool enableChangefeed = false;
    bool changefeedAsync = false;  // AsyncChangeFeed delivery backend

    // Page-level XChaCha20-Poly1305 encryption (see encryption/page_cipher.hpp).
    // When true, every data page (Leaf/Branch/Free/Overflow -- meta pages
    // 0/1 always stay cleartext) is transparently encrypted/decrypted
    // through PageManager. Derive `encryptionKey` yourself via
    // encryption::deriveKeyFromPassphrase (Environment does not manage
    // passphrases/salts -- callers own that storage). Requires
    // ANNPOCKETDB_WITH_ENCRYPTION; open() returns NotSupported otherwise.
    bool enableEncryption = false;
    std::array<std::uint8_t, 32> encryptionKey{};

    // Optional additive SHA-256 checksum bolt-on (core/checksum_sidecar.hpp),
    // independent of enableEncryption -- alongside the always-on CRC32C, not
    // a replacement. Requires ANNPOCKETDB_WITH_ENCRYPTION.
    bool enableSha256Checksum = false;

    // Value/overflow-level LZ4 compression (see compression/lz4_codec.hpp).
    // Only ordinary plain (non-DUPSORT, non-DUPFIXED) values above
    // compressionMinSize are ever compressed; the read path decompresses
    // transparently regardless of this flag whenever a value it encounters
    // was compressed by some earlier write. Requires
    // ANNPOCKETDB_WITH_COMPRESSION; open() returns NotSupported otherwise.
    bool enableCompression = false;
    std::uint32_t compressionMinSize = 256;
};


class Environment {
public:
    static Result<Environment> open(const std::filesystem::path& path, const EnvironmentOptions& options = {});

    Environment(Environment&&) noexcept = default;
    Environment& operator=(Environment&&) = delete;  // BTree holds a stable PageManager& bound at construction
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    ~Environment() = default;

    Result<txn::ReadTransaction> beginRead();
    Result<txn::WriteTransaction> beginWrite();

    // Convenience single-operation helpers: each opens its own transaction,
    // applies one change, and commits/aborts automatically. For anything
    // beyond a single put/get/remove, prefer beginWrite()/beginRead()
    // directly so multiple operations share one transaction.
    Result<ByteBuffer> get(ByteView key);
    Status put(ByteView key, ByteView value, bool overwrite = true, core::SyncMode mode = core::SyncMode::Durable);
    Status remove(ByteView key, core::SyncMode mode = core::SyncMode::Durable);

    // ---- Blob helpers (overflow-chain large values) ----
    // Stores a BlobRef value under `key` and materializes the chain under the
    // same write transaction. getBlob resolves a BlobRef (or returns inline
    // value if the key does not hold a ref). removeBlob retires the chain.
    Result<blob::BlobRef> putBlob(ByteView key, ByteView data,
                                  core::SyncMode mode = core::SyncMode::Durable);
    Result<ByteBuffer> getBlob(ByteView key);
    Status removeBlob(ByteView key, core::SyncMode mode = core::SyncMode::Durable);
    blob::BlobStore blobStore() { return blob::BlobStore(*pm_); }

    // Document + index helpers (caller owns codec / IndexManager lifetime).
    document::DocumentStore documentStore(document::IDocumentCodec& codec,
                                          index::IndexManager* indexes = nullptr) {
        return document::DocumentStore(codec, indexes);
    }

    
    // ---- Online verify / offline dump (see backup/maintenance.hpp) ----
    Result<backup::VerifyReport> verify() { return backup::verify(*this); }
    Result<backup::DumpReport> dumpToFile(const std::filesystem::path& out,
                                          const backup::DumpOptions& opts = {}) {
        return backup::dumpToFile(*this, out, opts);
    }
    // Physical page-level incremental dump -- see backup/maintenance.hpp.
    Result<backup::IncrementalDumpReport> incrementalDumpToFile(
        TxnId baselineTxnId, const std::filesystem::path& out,
        const backup::IncrementalDumpOptions& opts = {}) {
        return backup::incrementalDumpToFile(*this, baselineTxnId, out, opts);
    }
    // Fresh, defragmented copy (LMDB's copy(path, compact=True)) -- see
    // backup/maintenance.hpp.
    Result<backup::CompactCopyReport> compactCopyTo(const std::filesystem::path& destPath,
                                                     bool forceOverwrite = false,
                                                     const backup::CompactCopyOptions& opts = {}) {
        return backup::compactCopy(*this, destPath, forceOverwrite, opts);
    }

    // ---- Named sub-databases ----
    // create: allocates an empty leaf root, registers it in the catalog
    // (with `flags` persisted alongside the root -- DupSort/DupFixed/
    // ReverseKey/IntegerKey/etc, honored by every subsequent put/get/remove
    // against this handle from any process), commits.
    Result<SubDatabase> createSubDatabase(const std::string& name,
                                          btree::DbFlags flags = btree::DbFlags::None,
                                          core::SyncMode mode = core::SyncMode::Durable);
    // open: load catalog and return a handle (NotFound if missing).
    Result<SubDatabase> openSubDatabase(const std::string& name);
    // Idempotent get-or-create (LMDB's open_db(): creates on first call,
    // opens the existing one on every later call). `flags` are only applied
    // at creation -- ignored (existing persisted flags win) if the name
    // already exists, matching LMDB's documented immutability of
    // dupsort/integerkey/integerdup/dupfixed once a database is created.
    Result<SubDatabase> openOrCreateSubDatabase(const std::string& name,
                                                btree::DbFlags flags = btree::DbFlags::None,
                                                core::SyncMode mode = core::SyncMode::Durable);
    // list names currently in the catalog.
    Result<std::vector<std::string>> listSubDatabases();
    // drop: remove catalog entry; pages are retired (best-effort walk).
    Status dropSubDatabase(const std::string& name,
                           core::SyncMode mode = core::SyncMode::Durable);

    Status close();

    // Housekeeping for two-phase commit (see txn::WriteTransaction::prepare):
    // durably invalidates a lingering Prepared meta slot left behind by a
    // process that called prepare() and then crashed before deciding
    // commit() or abort(). Not required for correctness -- a lingering
    // Prepared slot is already ignored automatically on open (see
    // core::PageManager::loadExistingFile) -- this is for an external
    // coordinator to explicitly resolve and reclaim that slot instead of
    // leaving it permanently stale. NotFound if no Prepared slot exists.
    Status rollbackPrepared();

    core::PageManager& pageManager() { return *pm_; }
    const EnvironmentOptions& options() const { return options_; }
    // Grow map ceiling (must have no active readers / writers). Windows may require reopen.
    Status setMapSize(std::uint64_t newMaxBytes);
    std::uint64_t mapSize() const;
    // Explicit out-of-band flush (LMDB's Environment.sync()). Data is always
    // written at commit(), but the OS may still hold it buffered depending
    // on the durability options this environment was opened with; `force`
    // overrides those options for this one call (true => Durable regardless
    // of noSync/mapAsync, matching LMDB's sync(force=True)).
    Status sync(bool force = false);
    // Maximum record key size in bytes (LMDB's max_key_size()) -- a function
    // of pageSize, not a fixed compile-time constant here.
    std::size_t maxKeySize() const;

    // ---- Statistics & reader introspection (LMDB mdb_stat / env_info / reader_*) ----
    Result<DbStat> stat() const;                 // master keyspace tree
    Result<DbStat> stat(PageId rootPageId) const; // any root
    EnvInfo info() const;
    std::size_t readerCheck();                   // reclaim dead-PID slots; returns count
    std::vector<ReaderInfo> readerList() const;

    wal::Wal* wal() { return wal_.get(); }
    changefeed::DurableChangeFeed* changeFeed() { return changeFeed_.get(); }

private:
    Environment() = default;

    std::unique_ptr<core::PageManager> pm_;
    std::unique_ptr<btree::BTree> tree_;
    std::unique_ptr<wal::Wal> wal_;
    std::unique_ptr<changefeed::DurableChangeFeed> changeFeed_;
    std::unique_ptr<SubDatabaseManager> subDbManager_;
    EnvironmentOptions options_;
};

}  // namespace annpocketdb::api
