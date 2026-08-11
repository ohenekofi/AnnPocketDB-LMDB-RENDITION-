#pragma once
//
// AnnPocketDb :: txn/transaction.hpp
//
// ReadTransaction and WriteTransaction are the public mutation/query
// surface built on top of core::PageManager + btree::BTree:
//
//   - ReadTransaction snapshots the currently-committed root at
//     construction and registers itself with PageManager's reader table so
//     the free list knows not to reclaim any page that snapshot can still
//     reach. Its view never changes for its whole lifetime, however many
//     writes commit concurrently -- this is what MVCC snapshot isolation
//     means in practice here, and it's covered by
//     tests/test_btree.cpp's snapshot-isolation case.
//
//   - WriteTransaction acquires PageManager's single-writer mutex for its
//     entire lifetime (so there is at most one in flight at a time,
//     matching the architecture's single-writer model), applies puts/
//     removes against btree::BTree (which copy-on-write allocates new
//     pages without touching anything a reader might see), and either
//     commit()s (atomically publishing the new *master* root through
//     PageManager::commit) or abort()s (simply never publishing -- the
//     pages it allocated are naturally forgotten on next reopen; see
//     PageManager's growth-model comments for why that's safe and requires
//     no explicit rollback bookkeeping).
//
// Multi-root model (sub-databases / secondary indexes):
//   The meta page stores a single masterRootPageId (the default keyspace,
//   which also holds system catalogs). Named sub-databases and indexes each
//   have their own B+tree root page ids, persisted as catalog entries *inside*
//   the master keyspace. A WriteTransaction therefore:
//     - always tracks masterRootPageId_ (what commit() publishes to meta);
//     - offers put/remove/get that operate on the master root by default;
//     - offers overloads that take PageId& root so callers can mutate an
//       auxiliary tree and receive the updated root back;
//     - requires the caller to write any updated auxiliary roots into the
//       master-keyspace catalog (via the default put path) *before* commit()
//       so the whole change set becomes durable atomically.
//
// Both expose openCursor() for ordered iteration via btree::Cursor.
//
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>
#include <mutex>

#include "annpocketdb/btree/btree.hpp"
#include "annpocketdb/btree/cursor.hpp"
#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/core/writer_gate.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::changefeed { class ChangeSink; }

namespace annpocketdb::txn {

// ---------------------------------------------------------------------------
// Named sub-database support (LMDB's mt_dbs/mt_dbflags/mt_dbiseqs, TXN_DBI_CHANGED)
// ---------------------------------------------------------------------------
//
// Dependency-inversion seam: ReadTransaction/WriteTransaction must consult and
// update the named-sub-database handle table (api::SubDatabaseManager) to
// track per-transaction root/liveness state, but api:: is built on top of
// txn:: (sub_database.hpp includes transaction.hpp), so txn:: cannot include
// api::'s headers without a cycle. txn:: therefore depends only on this
// narrow interface; api::SubDatabaseManager implements it. This keeps the
// existing include direction (api:: -> txn::) intact even though the
// transaction classes now own literal per-transaction sub-database state,
// matching MDB_txn owning mt_dbs/mt_dbiseqs directly in LMDB.
class ISubDbCatalog {
public:
    virtual ~ISubDbCatalog() = default;

    // Validates that handle `h` is currently live and its sequence matches
    // `openSeq` (LMDB's MDB_BAD_DBI check via TXN_DBI_CHANGED). On success,
    // returns the master-keyspace catalog key whose value holds this
    // handle's current root -- the caller reads it through its OWN
    // transaction view (get()), so the root is always consistent with that
    // transaction's snapshot instead of a separately-synchronized cache.
    virtual Result<ByteBuffer> validateHandle(SubDbHandle h, std::uint32_t openSeq) = 0;
    // Cheap re-check against the live sequence only (no key lookup) -- used
    // to re-validate an already-cached txn-local entry on every subsequent
    // operation against the same handle within one transaction.
    virtual bool sequenceMatches(SubDbHandle h, std::uint32_t seq) const = 0;

    virtual Status decodeCatalogRoot(ByteView entryBytes, PageId& outRoot) const = 0;
    // Inverse of decodeCatalogRoot -- used by putSubDb/removeSubDb to
    // publish an updated root into the handle's catalog record on every
    // mutating call (staged into the caller's own transaction, so it
    // becomes durable atomically with everything else on commit). Takes the
    // handle (not just the root) so the implementation can preserve that
    // handle's already-persisted flags word instead of clobbering it.
    virtual Status encodeCatalogRoot(SubDbHandle h, PageId root, ByteBuffer& out) const = 0;

    // Persisted DbFlags for this handle's named database (DupSort/DupFixed/
    // ReverseKey/IntegerKey/etc, stored in the catalog record at create()
    // time) -- used to pick allowDups/the right built-in comparator for
    // every put/get/remove against this handle.
    virtual btree::DbFlags flagsOf(SubDbHandle h) const = 0;
    // Fully-formed comparator for this handle: a caller-registered custom
    // comparator (in-memory/per-process only, see
    // api::SubDatabaseManager::setKeyComparator) if one was set, otherwise
    // the built-in comparator derived from flagsOf(h). Passed as BTree's
    // `customOrder` override so a custom comparator actually governs
    // storage order, not just a Cursor's own read-side navigation.
    virtual btree::KeyOrder keyOrderOf(SubDbHandle h) const = 0;

    // Called once per touched handle when the owning transaction actually
    // finishes -- never speculatively:
    //   - confirmDrop: the handle's catalog record was durably removed by a
    //     just-committed transaction; invalidate the handle for everyone.
    //   - rollbackPendingCreate: a handle provisionally registered by
    //     SubDatabaseManager::create() must be freed because the
    //     transaction that would have made it durable never committed.
    // Ordinary (non-drop, non-new) root updates need no hook here: the
    // catalog record's new value is already durable via the normal
    // transaction commit, exactly like any other key in the master keyspace.
    virtual void confirmDrop(SubDbHandle h) = 0;
    virtual void rollbackPendingCreate(SubDbHandle h) = 0;
};

// Per-transaction cache entry for one bound sub-database handle -- LMDB's
// mt_dbs[dbi] (root) + mt_dbiseqs[dbi] (staleness) + mt_dbflags[dbi]
// (DB_NEW/DB_STALE) rolled into one struct.
struct SubDbTxnEntry {
    PageId root = kInvalidPageId;
    std::uint32_t seq = 0;
    bool isNew = false;   // DB_NEW: created by SubDatabaseManager::create() in this txn
    bool dropped = false; // marked for catalog removal by this txn
    // Master-keyspace catalog key for this handle, cached from
    // ISubDbCatalog::validateHandle so putSubDb/removeSubDb can publish an
    // updated root without a second catalog lookup.
    ByteBuffer catalogKey;
};

// Thread affinity (LMDB's MDB_NOTLS-mode contract): a ReadTransaction is
// move-only and holds no thread-local state anywhere in this class or its
// Cursor -- every field here is either owned data or an ordinary (non
// thread-affine) reader-table handle -- so it is safe to move to a
// different thread than the one that created it (matching LMDB's
// documented "a read-only Transaction can move across threads"). It is
// NOT safe to use concurrently from two threads at once; exactly one
// thread should be driving it (and any Cursor opened from it) at a time.
class ReadTransaction {
public:
    // `snapshot.masterRootPageId` is the root this reader is bound to.
    // For the default keyspace that is the environment master root; for a
    // named sub-database the caller (SubDatabase::beginRead) overrides it
    // with the sub-database's root before constructing the transaction, via
    // rebindToSubDb -- which also carries over that handle's flags/custom
    // comparator, so these plain methods below are comparator-correct for a
    // rebound sub-database, not just the default keyspace.
    ReadTransaction(core::PageManager& pm, btree::BTree& tree, core::Snapshot snapshot, core::ReaderHandle handle);
    ~ReadTransaction();

    ReadTransaction(const ReadTransaction&) = delete;
    ReadTransaction& operator=(const ReadTransaction&) = delete;
    ReadTransaction(ReadTransaction&& other) noexcept;
    ReadTransaction& operator=(ReadTransaction&& other) noexcept;

    Result<ByteBuffer> get(ByteView key) const;
    Result<ByteView> getView(ByteView key) const;
    Result<bool> contains(ByteView key) const;
    Result<std::vector<ByteBuffer>> getAll(ByteView key) const;
    btree::Cursor openCursor() const;

    TxnId txnId() const { return snapshot_.txnId; }
    PageId rootPageId() const { return snapshot_.masterRootPageId; }

    // LMDB's mdb_txn_reset/mdb_txn_renew: release the reader-table slot
    // while keeping this object (and its last snapshot_) around, then
    // re-acquire a slot later for a long-lived polling reader without a
    // fresh construct/destruct cycle. NOTE: does not remember any
    // rebindToSubDb() applied before reset() -- renew() reverts to the
    // default keyspace root; call rebindToSubDb() again afterward if a
    // sub-database view is still needed.
    Status reset();
    Status renew();

    // ---- Named sub-database support ----
    void attachSubDbCatalog(ISubDbCatalog* catalog) { subDbCatalog_ = catalog; }

    // Binds `h` for use within this transaction: first use reads the
    // catalog record through this transaction's own snapshot (get()), so
    // the resolved root is always consistent with everything else this
    // transaction sees; later calls for the same handle re-validate the
    // cached sequence only (TXN_DBI_CHANGED) without another catalog lookup.
    Result<PageId> useSubDb(SubDbHandle h, std::uint32_t openSeq) const;

    // Convenience for a single-sub-database view (api::SubDatabase::beginRead):
    // resolves `h` and rebinds this transaction's own root to it, so that
    // plain get()/contains()/openCursor() calls above operate on that
    // sub-database for the rest of this transaction's lifetime.
    Status rebindToSubDb(SubDbHandle h, std::uint32_t openSeq);

    Result<ByteBuffer> getSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) const;
    Result<bool> containsSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) const;
    btree::Cursor openCursorSubDb(SubDbHandle h, std::uint32_t openSeq) const;

private:
    core::PageManager* pm_;
    btree::BTree* tree_;
    core::Snapshot snapshot_;
    core::ReaderHandle handle_;

    ISubDbCatalog* subDbCatalog_ = nullptr;
    mutable std::unordered_map<SubDbHandle, SubDbTxnEntry> subDbs_;

    // Set by rebindToSubDb so the plain get()/contains()/openCursor() above
    // stay comparator-correct after a rebind, not just getSubDb/etc; left at
    // defaults (plain byte compare) for the ordinary default-keyspace case.
    btree::DbFlags reboundFlags_ = btree::DbFlags::None;
    std::function<int(ByteView, ByteView)> reboundKeyCmp_;
    std::function<int(ByteView, ByteView)> reboundValueCmp_;
    btree::KeyOrder reboundOrder() const {
        btree::KeyOrder order = btree::KeyOrder::fromFlags(reboundFlags_);
        order.customKeyCmp = reboundKeyCmp_;
        order.customValueCmp = reboundValueCmp_;
        return order;
    }
};

// Thread affinity (matches LMDB's documented contract for write
// transactions): a WriteTransaction must be used only from the thread that
// created it. There is no runtime guard enforcing this -- WriterGate's
// mutex serializes concurrent *writer* acquisition across threads/processes
// but does not track or check ownership of an already-open transaction, so
// driving the same WriteTransaction (or a Cursor opened from it) from two
// threads at once is undefined behavior the way it would be for any
// unsynchronized shared mutable object, not something this class detects.
class WriteTransaction {
public:
    // Constructed only by Environment::beginWrite() (or equivalent);
    // acquires pm.writerMutex() for the lifetime of the transaction.
    // Starts with masterRootPageId_ equal to the currently committed master
    // root; default put/remove/get operate on that root.
    WriteTransaction(core::PageManager& pm, btree::BTree& tree);
    ~WriteTransaction();

    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;
    WriteTransaction(WriteTransaction&& other) noexcept;
    WriteTransaction& operator=(WriteTransaction&& other) noexcept;

    // ---- Default keyspace (master root) ----
    Status put(ByteView key, ByteView value, bool overwrite = true);
    // DUPSORT: insert (key, value) without replacing other values for key.
    // noDupData: fail with KeyExists if this exact pair already exists.
    Status putDup(ByteView key, ByteView value, bool noDupData = false);
    // Remove every value for key (all dups).
    // remove() already deletes all matching keys under DUPSORT semantics.
    Status removeDup(ByteView key, ByteView value);
    Result<std::vector<ByteBuffer>> getAll(ByteView key) const;
    Status remove(ByteView key);
    Result<ByteBuffer> get(ByteView key) const;  // read-your-own-writes
    Result<ByteView> getView(ByteView key) const;
    Result<bool> contains(ByteView key) const;
    // Atomically replace key's value, returning what was there before
    // (nullopt if key didn't exist -- the put still happens). LMDB's
    // Transaction.replace() ("use a temporary cursor to invoke
    // Cursor.replace()" -- see btree::Cursor::replace for the same scoping
    // note re: DUPSORT).
    Result<std::optional<ByteBuffer>> replace(ByteView key, ByteView value);
    // Atomically remove key, returning the value that was there (nullopt if
    // it didn't exist). LMDB's Transaction.pop().
    Result<std::optional<ByteBuffer>> pop(ByteView key);
    // Write-bound cursor (del/put enabled). Captures `this` for live root.
    btree::Cursor openCursor();

    // NOTE: the returned Cursor captures `this` to always see this
    // transaction's *current* master root (so later put()/remove() calls are
    // visible after a re-seek). Do not move or destroy this WriteTransaction
    // while a Cursor obtained from it is still in use -- the same lifetime
    // discipline as any C++ iterator into a container that gets relocated.
    // In debug builds, generation() advances on move/destroy so test code
    // can assert the cursor's captured generation still matches.

    // Debug aid: monotonic generation; advances on move-from and destroy.
    // Cursor factories capture the value at open time.
    std::uint64_t generation() const { return generation_; }

    // ---- Explicit-root operations (sub-databases, secondary indexes) ----
    //
    // Mutate the B+tree rooted at `rootPageId`. On success, `rootPageId` is
    // updated in-place to the new root produced by the COW path. The caller
    // is responsible for persisting that new root into a catalog entry in
    // the master keyspace before commit(); otherwise the change is lost when
    // the process exits (commit only publishes masterRootPageId_).
    Status put(PageId& rootPageId, ByteView key, ByteView value, bool overwrite = true);
    Status putDup(PageId& rootPageId, ByteView key, ByteView value, bool noDupData = false);
    Status removeDup(PageId& rootPageId, ByteView key, ByteView value);
    Result<std::vector<ByteBuffer>> getAll(PageId rootPageId, ByteView key) const;
    Status remove(PageId& rootPageId, ByteView key);
    Result<ByteBuffer> get(PageId rootPageId, ByteView key) const;
    Result<ByteView> getView(PageId rootPageId, ByteView key) const;
    Result<bool> contains(PageId rootPageId, ByteView key) const;
    btree::Cursor openCursor(PageId& rootPageId);  // write-bound
    btree::Cursor openCursor(PageId rootPageId) const;  // read-only fixed root

    // Publishes masterRootPageId_ atomically via PageManager::commit and
    // releases the writer lock. The transaction is finished after this
    // call; further put/remove/commit/abort calls return Status::Internal.
    //
    // If prepare() was called first, this instead durably flips the
    // already-prepared meta slot from Prepared to Committed and publishes
    // it -- see prepare()'s comment.
    Status commit(core::SyncMode mode = core::SyncMode::Durable);

    // Releases the writer lock without publishing anything. Idempotent;
    // also invoked automatically by the destructor if commit() was never
    // called, so ordinary scope-exit (including via an early return) is
    // always safe.
    //
    // If prepare() was called first, this instead durably invalidates the
    // already-prepared meta slot (it's already on disk and structurally
    // valid, so simply forgetting it -- like an ordinary abort -- would not
    // be safe) before releasing the lock.
    Status abort();

    // ---- Two-phase commit (LMDB-inspired prepare/commit/abort split) ----
    // Durably writes this transaction's data + meta as Prepared (same
    // durability guarantee as commit()) WITHOUT publishing it and WITHOUT
    // releasing the writer lock -- the lock stays held until the external
    // coordinator's decision arrives via a subsequent commit() or abort()
    // call on this same object. A crash between prepare() and that decision
    // leaves the prepared transaction invisible to any process that reopens
    // the file (see core::PageManager::loadExistingFile's slot-selection
    // rule) -- no coordinator involvement is required for that safety
    // property, only for explicitly resolving the lingering slot (see
    // api::Environment::rollbackPrepared).
    Status prepare(core::SyncMode mode = core::SyncMode::Durable);
    bool prepared() const { return prepared_; }

    // ---- Nested write transactions (LMDB-style) ----
    // Only one child is active at a time; nesting may be deeper (stack).
    // beginNested captures master root + retirement-log mark.
    // commitNested keeps current roots (merges into parent) and pops the frame.
    // abortNested restores roots and undoes retires recorded after the frame.
    // Top-level commit()/abort() refuse while nestingDepth() > 0.
    Status beginNested();
    Status commitNested();
    Status abortNested();
    std::size_t nestingDepth() const { return savepoints_.size(); }

    TxnId txnId() const { return txnId_; }
    // Root that will be written into the meta page on commit.
    PageId masterRootPageId() const { return masterRootPageId_; }
    // Alias kept for existing call sites that mean "the default keyspace root".
    PageId rootPageId() const { return masterRootPageId_; }
    bool finished() const { return finished_; }

    core::PageManager& pageManager() { return *pm_; }
    btree::BTree& tree() { return *tree_; }

    // Optional changefeed sink (non-owning). When set, put/remove on the master
    // keyspace record user-visible changes; commit() flushes the durable log
    // then notifies subscribers after a successful meta flip.
    void setChangeSink(changefeed::ChangeSink* sink) { changeSink_ = sink; }
    changefeed::ChangeSink* changeSink() const { return changeSink_; }

    // ---- Named sub-database support ----
    void attachSubDbCatalog(ISubDbCatalog* catalog) { subDbCatalog_ = catalog; }

    // Binds `h` for use within this transaction (see ISubDbCatalog for the
    // staleness-check contract). Cheap on repeat calls for the same handle
    // within one transaction: only the cached sequence is re-checked, not
    // the catalog record itself.
    Result<PageId> useSubDb(SubDbHandle h, std::uint32_t openSeq) const;

    // Registers a handle SubDatabaseManager::create() just allocated as
    // usable for the rest of *this* transaction (LMDB's DB_NEW): exported
    // implicitly by the normal commit of its catalog-record put; rolled
    // back via ISubDbCatalog::rollbackPendingCreate if this transaction
    // never durably commits.
    void registerNewSubDb(SubDbHandle h, PageId root, std::uint32_t seq, ByteBuffer catalogKey);
    // Marks `h` so that, once this transaction durably commits,
    // ISubDbCatalog::confirmDrop(h) invalidates the handle for everyone.
    void markSubDbDropped(SubDbHandle h);

    Status putSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key, ByteView value, bool overwrite = true);
    Status removeSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key);
    Result<ByteBuffer> getSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) const;
    Result<bool> containsSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) const;
    btree::Cursor openCursorSubDb(SubDbHandle h, std::uint32_t openSeq) const;
    // Root this transaction currently has cached for `h`, or kInvalidPageId
    // if `h` has not been touched in this transaction yet. Diagnostic only
    // -- does not validate staleness the way useSubDb does.
    PageId currentSubDbRoot(SubDbHandle h) const;

private:
    core::PageManager* pm_;
    btree::BTree* tree_;
    std::unique_lock<core::WriterGate> lock_;
    TxnId txnId_;
    PageId masterRootPageId_;
    SequenceNumber walSequence_;
    bool finished_;
    // Captures a failure from the constructor's refreshFromDiskIfStale()
    // call (only possible if another process's free-list chain is itself
    // corrupt) -- surfaced through commit()/prepare() rather than silently
    // swallowed, since the constructor itself has no Result to fail with.
    Status pendingError_;
    // Bumped on move-from / destroy so debug code can detect use-after-move
    // of a WriteTransaction that still has live cursors capturing `this`.
    std::uint64_t generation_ = 1;
    changefeed::ChangeSink* changeSink_ = nullptr;

    ISubDbCatalog* subDbCatalog_ = nullptr;
    mutable std::unordered_map<SubDbHandle, SubDbTxnEntry> subDbs_;

    bool prepared_ = false;
    core::PageManager::PreparedHandle preparedHandle_;

    struct Savepoint {
        PageId masterRootPageId = kInvalidPageId;
        std::size_t retirementLogMark = 0;
    };
    std::vector<Savepoint> savepoints_;
};

}  // namespace annpocketdb::txn
