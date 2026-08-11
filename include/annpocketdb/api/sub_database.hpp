#pragma once
//
// AnnPocketDb :: api/sub_database.hpp
//
// Named sub-databases, modeled on LMDB's MDB_dbi system:
//   - Each named DB is one record in the master keyspace, keyed by
//     "__sys_subdb__/<name>" (LMDB stores the same info as a record inside
//     MAIN_DBI instead of a separate file) -- see makeSubDbKey/encodeSubDbEntry.
//   - SubDatabaseManager is a lazy, in-process cache of integer handles
//     (SubDbHandle, analogous to MDB_dbi) with a per-slot sequence number
//     (analogous to LMDB's me_dbiseqs) that detects a handle whose
//     underlying named DB was closed/dropped (and possibly recreated,
//     reusing the same slot) out from under it -- the MDB_BAD_DBI
//     equivalent. Unlike LMDB's env-level state, it does NOT cache a root:
//     the root always comes from reading the catalog record through the
//     transaction that's actually using it (txn::WriteTransaction /
//     ReadTransaction's per-transaction table, see transaction.hpp's
//     ISubDbCatalog/SubDbTxnEntry), so a resolved root is always consistent
//     with that transaction's own snapshot -- exactly like LMDB's mt_dbs.
//   - SubDatabase never caches its root permanently; every beginRead()/
//     beginWriteTxn() re-resolves through the attached transaction, mirroring
//     LMDB's mdb_txn_renew0() refreshing txn->mt_dbs on every new transaction.
//
// Hard-to-misuse multi-op API:
//   auto w = sub.beginWriteTxn();     // SubDatabaseWriter
//   w->put(k1, v1);
//   w->put(k2, v2);
//   w->commit();                      // publishes catalog entry + commits once
//
// Single-op helpers (put/get/remove) remain for convenience.
// Environment owns the SubDatabaseManager; create/open/list/drop/close go
// through it. txn::WriteTransaction/ReadTransaction also expose the same
// handle-based operations directly (putSubDb/getSubDb/...), so a single
// transaction can touch multiple named databases at once -- SubDatabase is
// just the single-database convenience wrapper built on top of that.
//
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "annpocketdb/btree/btree.hpp"
#include "annpocketdb/btree/db_flags.hpp"
#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/txn/transaction.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::api {

// Master-keyspace key prefix under which every named sub-database's single
// catalog record lives: "__sys_subdb__/<name>" -> encodeSubDbEntry(rootPageId).
inline constexpr std::string_view kSubDbKeyPrefix = "__sys_subdb__/";

inline ByteBuffer makeSubDbKey(std::string_view name) {
    ByteBuffer key;
    key.reserve(kSubDbKeyPrefix.size() + name.size());
    ByteView prefixView = toByteView(kSubDbKeyPrefix);
    key.insert(key.end(), prefixView.begin(), prefixView.end());
    ByteView nameView = toByteView(name);
    key.insert(key.end(), nameView.begin(), nameView.end());
    return key;
}

// Catalog record value: [u64 rootPageId][u32 DbFlags]. Flags are persisted
// (LMDB stores MDB_db.md_flags the same way) so every caller of a named
// database consistently gets the same DupSort/DupFixed/ReverseKey/
// IntegerKey/etc. behavior without having to independently remember and
// re-pass flags on every call.
Status encodeSubDbEntry(PageId rootPageId, btree::DbFlags flags, ByteBuffer& out);
Status decodeSubDbEntry(ByteView data, PageId& rootPageId, btree::DbFlags& flags);

class SubDatabase;  // defined below

// ---------------------------------------------------------------------------
// SubDatabaseManager
// ---------------------------------------------------------------------------
//
// Owns the in-process handle table (name, sequence, liveness only -- no
// root; see file comment). Populated lazily (like LMDB's mdb_dbi_open): a
// name is only assigned a handle/slot the first time this process opens or
// creates it, never by eagerly scanning the whole catalog at
// environment-open time. listSubDatabases() is the only operation that
// scans the full catalog, and it does so freshly every call.
class SubDatabaseManager : public txn::ISubDbCatalog {
public:
    SubDatabaseManager(core::PageManager& pm, btree::BTree& masterTree,
                       std::uint32_t maxNamedDatabases);

    // Lazy lookup-or-register: checks the in-process cache first; on a miss,
    // looks up the single per-entry catalog key via an internal
    // ReadTransaction. NotFound if no such named database has ever been
    // created.
    Result<SubDbHandle> open(const std::string& name);

    // Allocates an empty leaf root, provisionally registers the handle (so
    // it is immediately usable within the *same* creating transaction via
    // WriteTransaction::useSubDb, like LMDB's DB_NEW dbi), and stages the
    // catalog-entry write into `txn`. Caller must still commit `txn` --
    // commit()/abort() handle publishing/rolling back the handle
    // automatically (see WriteTransaction::registerNewSubDb). `flags` is
    // persisted into the catalog record (see encodeSubDbEntry) and governs
    // every subsequent put/get/remove against this handle.
    Result<SubDbHandle> create(txn::WriteTransaction& txn, const std::string& name,
                               btree::DbFlags flags = btree::DbFlags::None);

    // Full delete (LMDB's mdb_drop with del=1): stages catalog-record
    // removal and retires the sub-tree's pages into `txn`, and marks the
    // handle so a durable commit invalidates it for everyone
    // (WriteTransaction::markSubDbDropped / confirmDrop).
    Status drop(txn::WriteTransaction& txn, SubDbHandle h);

    // Empties the sub-database's contents but keeps the catalog record and
    // handle valid (LMDB's mdb_drop with del=0): retires the old sub-tree's
    // pages and stages a fresh empty root as the new catalog value. Returns
    // the new root; caller must commit `txn` and then call
    // txn.useSubDb(h, openSeq) again (or rely on the next put/get) to pick
    // it up -- no manager-side confirm needed since, like any ordinary root
    // update, the new value is already durable via the normal commit.
    Result<PageId> empty(txn::WriteTransaction& txn, SubDbHandle h);

    // Invalidates a handle without touching the underlying named database
    // on disk (LMDB's mdb_dbi_close): any SubDatabase/transaction still
    // holding this handle will get BadDbi from useSubDb from now on, and
    // the slot becomes available for a future open()/create() (of this or
    // another name) to reuse. As in LMDB, this is not scoped to a
    // transaction and is not safe to call while another thread may still be
    // using the same handle.
    void close(SubDbHandle h);

    // Full prefix scan over the master keyspace; always fresh, never cached.
    Result<std::vector<std::string>> list(txn::ReadTransaction& txn) const;

    // Sequence this handle currently has (0 if unknown/invalid) -- used by
    // Environment to stamp a newly returned SubDatabase's openSeq_.
    std::uint32_t currentSequence(SubDbHandle h) const;

    // Custom comparator registration (LMDB's mdb_set_compare/mdb_set_dupsort):
    // in-memory/per-process only, never persisted -- an application must
    // re-register after every process restart, exactly like LMDB. Overrides
    // the flags-derived built-in comparator for this handle until close().
    using Comparator = std::function<int(ByteView, ByteView)>;
    void setKeyComparator(SubDbHandle h, Comparator cmp);
    void setValueComparator(SubDbHandle h, Comparator cmp);

    // ---- txn::ISubDbCatalog ----
    Result<ByteBuffer> validateHandle(SubDbHandle h, std::uint32_t openSeq) override;
    bool sequenceMatches(SubDbHandle h, std::uint32_t seq) const override;
    Status decodeCatalogRoot(ByteView entryBytes, PageId& outRoot) const override;
    Status encodeCatalogRoot(SubDbHandle h, PageId root, ByteBuffer& out) const override;
    btree::DbFlags flagsOf(SubDbHandle h) const override;
    btree::KeyOrder keyOrderOf(SubDbHandle h) const override;
    void confirmDrop(SubDbHandle h) override;
    void rollbackPendingCreate(SubDbHandle h) override;

private:
    struct Slot {
        std::string name;
        std::uint32_t sequence = 0;  // bumped on close/drop/reuse -- the MDB_BAD_DBI equivalent
        bool inUse = false;
        btree::DbFlags flags = btree::DbFlags::None;  // persisted (catalog record)
        Comparator keyCmp;                            // NOT persisted -- per-process only
        Comparator valueCmp;                          // NOT persisted -- per-process only
    };

    Result<txn::ReadTransaction> beginInternalRead() const;
    // Caller must hold mutex_ exclusively.
    SubDbHandle allocateSlotLocked(const std::string& name, btree::DbFlags flags = btree::DbFlags::None);
    void invalidateSlotLocked(SubDbHandle h);

    // Needed because, unlike IndexManager, this table is read concurrently
    // by many reader threads while a single writer thread may be publishing
    // an update -- writer-vs-writer is already excluded by WriterGate, but
    // writer-vs-reader publication (and close(), which isn't gated by a
    // transaction at all) is not.
    mutable std::shared_mutex mutex_;
    std::vector<Slot> slots_;  // index 0 reserved (kInvalidSubDbHandle)
    std::unordered_map<std::string, SubDbHandle> byName_;
    std::vector<SubDbHandle> freeSlots_;
    std::uint32_t maxNamedDatabases_;
    core::PageManager& pm_;
    btree::BTree& masterTree_;
};

// Multi-operation write handle bound to one sub-database. commit()/abort()
// simply delegate to the underlying WriteTransaction -- all catalog
// bookkeeping happens generically there via the per-transaction sub-database
// table (see transaction.hpp). Destructor aborts if not finished.
class SubDatabaseWriter {
public:
    SubDatabaseWriter(txn::WriteTransaction wtxn, SubDatabase* owner);
    ~SubDatabaseWriter();

    SubDatabaseWriter(const SubDatabaseWriter&) = delete;
    SubDatabaseWriter& operator=(const SubDatabaseWriter&) = delete;
    SubDatabaseWriter(SubDatabaseWriter&&) noexcept;
    SubDatabaseWriter& operator=(SubDatabaseWriter&&) noexcept;

    Status put(ByteView key, ByteView value, bool overwrite = true);
    Status remove(ByteView key);
    Result<ByteBuffer> get(ByteView key) const;
    Result<bool> contains(ByteView key) const;
    btree::Cursor openCursor() const;

    Status commit(core::SyncMode mode = core::SyncMode::Durable);
    void abort();

    bool finished() const { return finished_; }
    PageId rootPageId() const;
    TxnId txnId() const { return wtxn_.txnId(); }

private:
    txn::WriteTransaction wtxn_;
    SubDatabase* owner_ = nullptr;
    bool finished_ = false;
};

class SubDatabase {
public:
    SubDatabase() = default;
    SubDatabase(core::PageManager* pm, btree::BTree* tree, SubDatabaseManager* manager,
                std::string name, SubDbHandle handle, std::uint32_t openSeq)
        : pm_(pm), tree_(tree), manager_(manager), name_(std::move(name)),
          handle_(handle), openSeq_(openSeq) {}

    Result<txn::ReadTransaction> beginRead();

    // Preferred multi-op write path.
    Result<SubDatabaseWriter> beginWriteTxn();

    // Legacy: raw environment WriteTransaction bound to the *default*
    // keyspace, not this sub-database (easy to misuse — prefer beginWriteTxn).
    Result<txn::WriteTransaction> beginWrite();

    Result<ByteBuffer> get(ByteView key);
    Status put(ByteView key, ByteView value, bool overwrite = true,
               core::SyncMode mode = core::SyncMode::Durable);
    Status remove(ByteView key, core::SyncMode mode = core::SyncMode::Durable);

    // Empties this sub-database's contents (LMDB's mdb_drop del=0). This
    // handle and any other still-open handle to the same name remain valid.
    Status clear(core::SyncMode mode = core::SyncMode::Durable);

    // Invalidates this SubDatabase's own handle (LMDB's mdb_dbi_close) --
    // does NOT touch the underlying named database on disk. After this
    // call every method on this object returns BadDbi (LMDB's BadDbiError --
    // "the specified DBI was changed unexpectedly"); other SubDatabase
    // instances holding the same handle are invalidated too (see
    // SubDatabaseManager::close).
    void close();

    const std::string& name() const { return name_; }
    SubDbHandle handle() const { return handle_; }
    std::uint32_t openSeq() const { return openSeq_; }
    SubDatabaseManager* manager() const { return manager_; }
    btree::DbFlags flags() const { return manager_ ? manager_->flagsOf(handle_) : btree::DbFlags::None; }

    bool valid() const { return pm_ != nullptr && tree_ != nullptr && manager_ != nullptr; }

private:
    friend class SubDatabaseWriter;
    core::PageManager* pm_ = nullptr;
    btree::BTree* tree_ = nullptr;
    SubDatabaseManager* manager_ = nullptr;
    std::string name_;
    SubDbHandle handle_ = kInvalidSubDbHandle;
    std::uint32_t openSeq_ = 0;
};

}  // namespace annpocketdb::api
